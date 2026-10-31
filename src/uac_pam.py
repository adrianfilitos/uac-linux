#!/usr/bin/python3
"""Puente PAM de uac-linux: la tarjeta de consentimiento, dentro de PAM.

Lo instala ``install-root.sh`` en ``/usr/local/libexec/uac-linux/uac-pam`` y lo
llama ``pam_exec.so`` desde ``/etc/pam.d/sudo-uac``, ``sudo-uac-login`` y
``polkit-1``. Así el aviso sale con **cualquier** ``sudo`` y con cualquier
elevación que autentique polkit (``pkexec`` incluido): desde bash, desde
Python, desde un servicio, con el comando completo y sin depender de ninguna
función ni de ningún shim.

PAM no sabe devolver "Sí" o "No" con un solo módulo externo (``pam_exec`` solo
distingue éxito de fallo), así que se usan tres líneas:

    auth [success=done default=ignore] pam_exec.so seteuid quiet uac-pam card
    auth [success=1    default=ignore] pam_exec.so seteuid quiet uac-denied
    auth requisite pam_deny.so
    @include common-auth

* **Sí** -> ``card`` sale 0 -> ``success=done`` -> ``PAM_SUCCESS`` sin
  contraseña.
* **No** -> ``card`` sale 1 y deja una marca -> ``uac-denied`` sale 1 -> cae en
  ``pam_deny`` -> ``PAM_AUTH_ERR`` inmediato: sudo no llega a pedir nada.
* Sin extensión, sin bus, sin respuesta o con el ayudante roto -> no hay marca
  -> ``uac-denied`` sale 0 -> salta el ``pam_deny`` -> sigue la pila ->
  ``pam_unix`` pide la contraseña.

``uac-denied`` es un script de shell aparte (``src/uac_denied.sh``) y no forma
parte de este fichero a propósito: si alguien borra ``uac-pam``, sigue
existiendo y la autenticación cae en la contraseña en vez de dejar sudo y
pkexec inservibles.

Ojo con el orden: la denegación **no** puede darla ``card`` con
``[success=die]``, porque un módulo que sale 0 devuelve ``PAM_SUCCESS`` y eso
autentica. El «No» tiene que pasar por un módulo que falle.

``pam_exec`` ejecuta el comando con el uid *real* del proceso que autentica,
salvo con ``seteuid``, que conserva el efectivo. Aquí se usa ``seteuid`` a
propósito: como root, el proceso no es ptraceable por tu cuenta y la decisión
("aprobado") no se puede falsificar desde un proceso tuyo.

Cuando el que autentica no es ``sudo`` (``pkexec``, es decir, el ayudante
``polkit-agent-helper-1``, que es setuid root y conserva tu uid real), el
puente pasa a root también en el uid real: si no, el bus de sesión diría que
la petición viene de tu cuenta y la extensión la rechazaría.

Si te llenan de avisos y los vas cerrando con «No», a la tercera denegación en
un minuto se dejan de pintar tarjetas durante un minuto: sin tarjeta no hay
spam, y la petición se deniega al instante. Un «Sí» limpia la cuenta.
"""

from __future__ import annotations

import array
import json
import math
import os
import pwd
import re
import shlex
import shutil
import socket
import subprocess
import sys
import syslog
import time
import wave
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
SOUND_FILE = BASE_DIR / "uac.wav"

# Interfaz D-Bus que expone la extensión de GNOME Shell.
DBUS_NAME = "org.nixer.Uac"
DBUS_PATH = "/org/nixer/Uac"

# Estado compartido entre las dos líneas de PAM. /run es tmpfs y solo root
# puede crear cosas ahí, así que la marca "denegado" no la puede falsificar
# un proceso del usuario.
STATE_DIR = Path("/run/uac-pam")
CONFIG_DIR = Path("/etc/uac-linux")
# Las marcas de denegación caducan: uac-denied (el script de shell del segundo
# paso de PAM) ignora las que tengan más de 300 s.

# Anti-spam: quien te llena la pantalla de avisos lo hace porque los cierras.
# Con SPAM_DENIALS "No" seguidos en SPAM_WINDOW_S segundos se dejan de pintar
# tarjetas durante el resto de la ventana. Un "Sí" reinicia la cuenta, y los
# scripts que apruebas nunca llegan a contar.
SPAM_DENIALS = 3
SPAM_WINDOW_S = 60

# La tarjeta se cierra sola al cabo de un minuto (como el UAC de Windows, que
# considera que no hay respuesta). El puente espera algo más para que gane
# siempre el temporizador de la extensión y no al revés.
CARD_TIMEOUT_S = 60
DBUS_TIMEOUT_MS = (CARD_TIMEOUT_S + 10) * 1000

# Opciones de sudo que consumen el argumento siguiente (solo para adivinar el
# nombre de la aplicación que se muestra en la tarjeta).
VALUE_FLAGS = {
    "-u", "--user", "-g", "--group", "-p", "--prompt", "-C", "--close-from",
    "-T", "--command-timeout", "-R", "--chroot", "-D", "--chdir", "-r",
    "--role", "-t", "--type", "-U", "--other-user", "-h", "--host",
}

# --- aviso sonoro -----------------------------------------------------------
# El WAV original tiene el pico a -10,9 dBFS y el RMS a -28 dBFS: suena
# correctamente en silencio, pero se pierde bajo música o vídeo. Se normaliza
# el pico a -0,5 dBFS y se empujan 10 dB más con un limitador suave, lo que
# sube la energía media unos 6,8 dB sin que el pico llegue a recortar.
TARGET_CEILING = 0.95
EXTRA_GAIN_DB = 10.0
LIMITER_ATTACK = 0.002
LIMITER_RELEASE = 0.12
SOUND_DELAY = 0.35
DUCK_LEVEL = 0.30
DUCK_SECONDS = 2.5


class Unavailable(Exception):
    """No se pudo mostrar la tarjeta (sin sesión, sin extensión, error)."""


# ----------------------------------------------------------------- utilidades
def _log(message: str) -> None:
    """Deja una línea de diagnóstico en /run/uac-pam/last.log."""
    directory = _state_dir()
    if directory is None:
        return
    log = directory / "last.log"
    try:
        if log.exists() and log.stat().st_size > 64_000:
            log.unlink()
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        with log.open("a", encoding="utf-8") as handle:
            handle.write(f"{stamp} [{os.path.basename(sys.argv[0])}] {message}\n")
    except OSError:
        pass


def _state_dir() -> Path | None:
    """Devuelve /run/uac-pam si es de root y solo root puede escribir."""
    try:
        STATE_DIR.mkdir(mode=0o700, exist_ok=True)
        info = STATE_DIR.stat()
        if info.st_uid != 0:
            return None
        if info.st_mode & 0o077:
            os.chmod(STATE_DIR, 0o700)
        if not os.access(STATE_DIR, os.R_OK | os.W_OK | os.X_OK):
            return None
    except OSError:
        return None
    return STATE_DIR


def _root_flag(name: str) -> bool:
    """Marca de configuración en /etc/uac-linux que solo root puede poner."""
    try:
        info = (CONFIG_DIR / name).stat()
    except OSError:
        return False
    return info.st_uid == 0 and not (info.st_mode & 0o022)


def _state_key() -> str | None:
    """Identifica la invocación de PAM: pid + instante de arranque del padre.

    Las dos líneas de ``pam_exec`` (card y uac-denied) son hijas del mismo
    proceso PAM, así que comparten padre. Se añade el campo 22 de
    ``/proc/<pid>/stat`` para que un pid reciclado no arrastre una marca
    vieja.
    """
    pid = os.getppid()
    start = "?"
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
        start = raw.rsplit(") ", 1)[1].split()[19]
    except (OSError, IndexError, ValueError):
        pass
    return f"{pid}-{start}"


def _deny_path() -> Path | None:
    key = _state_key()
    directory = _state_dir()
    if key is None or directory is None:
        return None
    return directory / f"{key}.deny"


def _write_deny() -> None:
    path = _deny_path()
    if path is None:
        return
    try:
        path.write_text("1", encoding="utf-8")
        path.chmod(0o600)
    except OSError:
        pass


# ------------------------------------------------------- auditoría y anti-spam
def _audit(decision: str, **fields) -> None:
    """Deja una línea JSON en el journal: ``journalctl -t uac-linux``.

    Sirve para saber qué se aprobó, cuándo y desde dónde, sin depender de que
    ``last.log`` (que se rota a los 64 KB) siga teniendo la línea.
    """
    record = {"decision": decision}
    for key, value in fields.items():
        if value is not None and value != "":
            record[key] = value
    try:
        syslog.openlog("uac-linux", syslog.LOG_PID, syslog.LOG_AUTH)
        syslog.syslog(syslog.LOG_NOTICE, json.dumps(record, ensure_ascii=False))
    except Exception:  # noqa: BLE001 - la auditoría nunca debe romper el aviso
        pass


def _denials_path(uid: int) -> Path | None:
    directory = _state_dir()
    if directory is None:
        return None
    return directory / f"{uid}.denials"


def _recent_denials(path: Path) -> list[float]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    if not isinstance(raw, list):
        return []
    now = time.time()
    stamps = []
    for item in raw:
        if not isinstance(item, (int, float)) or isinstance(item, bool):
            continue
        age = now - float(item)
        if 0 <= age <= SPAM_WINDOW_S:
            stamps.append(float(item))
    return stamps


def _write_denials(path: Path, stamps: list[float]) -> None:
    tmp = path.with_name(f"{path.name}.{os.getpid()}.part")
    try:
        tmp.write_text(json.dumps(stamps), encoding="utf-8")
        tmp.chmod(0o600)
        tmp.replace(path)
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass


def _spam_blocked(uid: int) -> bool:
    """¿Hay que dejar de pintar tarjetas a este usuario un rato?"""
    path = _denials_path(uid)
    if path is None:
        return False
    return len(_recent_denials(path)) >= SPAM_DENIALS


def _record_denial(uid: int) -> None:
    path = _denials_path(uid)
    if path is None:
        return
    stamps = _recent_denials(path)
    stamps.append(time.time())
    _write_denials(path, stamps[-(SPAM_DENIALS + 2):])


def _clear_denials(uid: int) -> None:
    path = _denials_path(uid)
    if path is None:
        return
    try:
        path.unlink()
    except OSError:
        pass


def _become_root() -> None:
    """Pasa a root en el uid real, no solo en el efectivo.

    Con ``sudo`` el proceso ya llega con todo a 0. Con polkit no: el ayudante
    ``polkit-agent-helper-1`` es setuid root y conserva tu uid real, así que el
    bus de sesión identifica al cliente como tuyo y la extensión rechazaría la
    petición (solo admite uid 0). Como ya somos root efectivos, elevarnos es
    cuestión de un ``setresuid``.
    """
    if os.geteuid() != 0 or os.getuid() == 0:
        return
    for step in (
        lambda: os.setgroups([]),
        lambda: os.setresgid(0, 0, 0),
        lambda: os.setresuid(0, 0, 0),
    ):
        try:
            step()
        except OSError:
            pass


def _read_cmdline(pid: int) -> list[str]:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        return []
    return [part.decode("utf-8", "replace") for part in raw.split(b"\0") if part]


def _guess_app(argv: list[str]) -> str:
    """Adivina el programa principal solo para mostrarlo en la tarjeta."""
    for arg in argv:
        if arg in VALUE_FLAGS:
            continue
        if arg.startswith("-"):
            continue
        if Path(arg).name in ("sudo", "sudoedit"):
            continue
        return Path(arg).name
    return "sudo"


def _session_env_of(uid: int) -> dict[str, str]:
    """Variables que la extensión no necesita, pero ayudan a diagnosticar."""
    return {
        "uid": str(uid),
        "runtime": f"/run/user/{uid}",
    }


# ------------------------------------------------------------------- sonido
def _write_normalized(src: Path, dst: Path) -> None:
    """Escribe dst con el audio de src más alto, limitado al techo."""
    with wave.open(str(src), "rb") as reader:
        channels = reader.getnchannels()
        width = reader.getsampwidth()
        rate = reader.getframerate()
        frames = reader.readframes(reader.getnframes())

    if width != 2:
        raise ValueError("el WAV no es PCM de 16 bits")

    samples = array.array("h")
    samples.frombytes(frames)
    if sys.byteorder == "big":
        samples.byteswap()
    if not samples:
        raise ValueError("el WAV está vacío")

    peak = max(abs(min(samples)), abs(max(samples))) / 32768.0
    if peak <= 0:
        raise ValueError("el WAV está en silencio")
    gain = (TARGET_CEILING / peak) * 10 ** (EXTRA_GAIN_DB / 20.0)
    if gain <= 1.0:
        raise ValueError("el WAV ya está suficientemente alto")

    attack = 1.0 - math.exp(-1.0 / (rate * LIMITER_ATTACK))
    release = 1.0 - math.exp(-1.0 / (rate * LIMITER_RELEASE))
    envelope = 0.0

    for index, value in enumerate(samples):
        scaled = (value / 32768.0) * gain
        level = abs(scaled)
        if level > envelope:
            envelope += (level - envelope) * attack
        else:
            envelope += (level - envelope) * release
        if envelope > TARGET_CEILING:
            scaled *= TARGET_CEILING / envelope
        if scaled > TARGET_CEILING:
            scaled = TARGET_CEILING
        elif scaled < -TARGET_CEILING:
            scaled = -TARGET_CEILING
        samples[index] = int(scaled * 32767)

    if sys.byteorder == "big":
        samples.byteswap()

    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(f"{dst.name}.{os.getpid()}.part")
    try:
        with wave.open(str(tmp), "wb") as writer:
            writer.setnchannels(channels)
            writer.setsampwidth(width)
            writer.setframerate(rate)
            writer.writeframes(samples.tobytes())
        tmp.replace(dst)
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


def _cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or (Path(os.environ.get("HOME", "/tmp")) / ".cache")
    return Path(base) / "uac-linux"


def _alert_file() -> Path:
    """Versión amplificada y cacheada del aviso, o el original si falla."""
    try:
        info = SOUND_FILE.stat()
    except OSError:
        return SOUND_FILE
    if info.st_size <= 0:
        return SOUND_FILE

    cache = _cache_dir()
    cached = cache / f"uac-alert-v3-{int(info.st_mtime)}-{info.st_size}.wav"
    try:
        for stale in cache.glob("uac-alert-*.wav"):
            if stale != cached:
                stale.unlink()
    except OSError:
        pass
    if cached.is_file():
        return cached
    try:
        _write_normalized(SOUND_FILE, cached)
    except (OSError, ValueError, EOFError, wave.Error):
        return SOUND_FILE
    return cached


def _player() -> list[str] | None:
    """Primer reproductor disponible, con sus opciones."""
    for name, extra in (
        ("pw-play", ["--media-role", "alert"]),
        ("paplay", []),
        ("canberra-gtk-play", ["-f"]),
    ):
        found = shutil.which(name)
        if found:
            return [found, *extra]
    return None


def _output_streams() -> list[dict]:
    if shutil.which("pw-dump") is None or shutil.which("wpctl") is None:
        return []
    try:
        dump = json.loads(
            subprocess.check_output(["pw-dump"], stderr=subprocess.DEVNULL, timeout=2)
        )
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    streams = []
    for obj in dump:
        props = obj.get("info", {}).get("props", {})
        if props.get("media.class") != "Stream/Output/Audio":
            continue
        if (
            props.get("application.name") in ("pw-play", "pw-cat")
            or props.get("node.name") in ("pw-play", "pw-cat")
            or props.get("application.process.binary") in ("pw-play", "pw-cat")
        ):
            continue
        name = (
            props.get("application.name")
            or props.get("node.name")
            or str(obj.get("id"))
        )
        streams.append({"id": int(obj["id"]), "name": name})
    return streams


def _stream_volume(node_id: int) -> float | None:
    try:
        out = subprocess.check_output(
            ["wpctl", "get-volume", str(node_id)],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if "MUTED" in out:
        return None
    match = re.search(r"Volume:\s*([0-9.]+)", out)
    if not match:
        return None
    try:
        return float(match.group(1))
    except ValueError:
        return None


def _set_stream_volume(node_id: int, volume: float) -> None:
    try:
        subprocess.run(
            ["wpctl", "set-volume", "--limit", "10", str(node_id), f"{volume:.2f}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass


def _duck() -> list[dict]:
    """Baja el resto de audio y devuelve lo que hay que restaurar."""
    targets = []
    for stream in _output_streams():
        volume = _stream_volume(stream["id"])
        if volume is None or volume <= DUCK_LEVEL:
            continue
        targets.append({"id": stream["id"], "volume": volume})
        _set_stream_volume(stream["id"], DUCK_LEVEL)
    return targets


def _sound_worker(entry: pwd.struct_passwd) -> None:
    """Suena el aviso como el usuario: baja la música y la restaura luego."""
    try:
        os.setsid()
    except OSError:
        pass
    if os.geteuid() == 0:
        try:
            os.setgroups([])
            os.setresgid(entry.pw_gid, entry.pw_gid, entry.pw_gid)
            os.setresuid(entry.pw_uid, entry.pw_uid, entry.pw_uid)
        except OSError:
            return

    runtime = f"/run/user/{entry.pw_uid}"
    os.environ.update(
        {
            "HOME": entry.pw_dir,
            "USER": entry.pw_name,
            "LOGNAME": entry.pw_name,
            "XDG_RUNTIME_DIR": runtime,
            "XDG_CACHE_HOME": f"{entry.pw_dir}/.cache",
            "PATH": "/usr/local/bin:/usr/bin:/bin",
        }
    )
    os.environ.pop("DBUS_SESSION_BUS_ADDRESS", None)
    os.environ.pop("PAM_USER", None)

    player = _player()
    if player is None:
        return
    sound = _alert_file()
    targets = [] if _root_flag("no-ducking") else _duck()
    try:
        subprocess.Popen(
            [*player, str(sound)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError:
        for target in targets:
            _set_stream_volume(int(target["id"]), float(target["volume"]))
        return
    if targets:
        time.sleep(DUCK_SECONDS)
        for target in targets:
            _set_stream_volume(int(target["id"]), float(target["volume"]))


def play_alert(entry: pwd.struct_passwd) -> None:
    """Lanza el aviso sonoro y espera lo justo para que coincida con la tarjeta."""
    if _root_flag("no-sound") or not SOUND_FILE.is_file():
        return
    try:
        pid = os.fork()
    except OSError:
        return
    if pid == 0:
        try:
            _sound_worker(entry)
        except BaseException:  # noqa: BLE001 - el hijo nunca debe volcar trazas
            pass
        finally:
            os._exit(0)
    time.sleep(SOUND_DELAY)


# ------------------------------------------------------------------ D-Bus
def _shell_channel(uid: int):
    """Conexión al bus de la sesión, ya verificada contra el GNOME Shell.

    Devuelve ``(connection, nombre_unico, Gio, GLib)``. Lanza ``Unavailable``
    si no hay bus, si la extensión no está activa o si el nombre lo tiene un
    proceso que no es el shell de esta sesión: sin esa comprobación, un
    proceso del usuario podría registrar ``org.nixer.Uac`` antes que la
    extensión y contestar «sí» por su cuenta.
    """
    try:
        from gi.repository import Gio, GLib
    except Exception as exc:  # noqa: BLE001 - entorno sin PyGObject
        raise Unavailable(f"sin PyGObject: {exc}") from exc

    address = f"unix:path=/run/user/{uid}/bus"
    flags = (
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
    )
    try:
        connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
    except GLib.Error as exc:
        raise Unavailable(f"sin bus de sesión en {address}: {exc.message}") from exc

    def name_owner(name: str) -> str | None:
        try:
            reply = connection.call_sync(
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                "GetNameOwner",
                GLib.Variant("(s)", (name,)),
                GLib.VariantType.new("(s)"),
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
        except GLib.Error:
            return None
        return reply.unpack()[0]

    shell_owner = name_owner("org.gnome.Shell")
    card_owner = name_owner(DBUS_NAME)
    if not card_owner:
        raise Unavailable("la extensión de uac-linux no está activa")
    if shell_owner != card_owner:
        raise Unavailable(
            f"{DBUS_NAME} lo tiene {card_owner}, no el shell de la sesión"
        )
    try:
        pid = connection.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "GetConnectionUnixProcessID",
            GLib.Variant("(s)", (card_owner,)),
            GLib.VariantType.new("(u)"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        ).unpack()[0]
        executable = os.readlink(f"/proc/{pid}/exe")
    except (GLib.Error, OSError) as exc:
        raise Unavailable(f"no pude identificar al shell: {exc}") from exc
    if Path(executable).name != "gnome-shell":
        raise Unavailable(f"el aviso lo sirve {executable}, no GNOME Shell")

    return connection, card_owner, Gio, GLib


def _ask_shell(uid: int, request: dict) -> bool:
    """Muestra la tarjeta en el shell. Devuelve True si se aprueba.

    Lanza ``Unavailable`` si no hay forma de mostrarla, para que PAM caiga a la
    contraseña en vez de denegar por las bravas.
    """
    connection, card_owner, Gio, GLib = _shell_channel(uid)

    # Se llama al nombre único, no al bien conocido: si el nombre cambiara de
    # dueño entre la comprobación y la llamada, el mensaje no le llegaría.
    payload = GLib.Variant("(s)", (json.dumps(request, ensure_ascii=False),))
    try:
        reply = connection.call_sync(
            card_owner,
            DBUS_PATH,
            DBUS_NAME,
            "Request",
            payload,
            GLib.VariantType.new("(b)"),
            Gio.DBusCallFlags.NONE,
            DBUS_TIMEOUT_MS,
            None,
        )
    except GLib.Error as exc:
        raise Unavailable(f"{exc.domain} {exc.code}: {exc.message}") from exc

    values = reply.unpack()
    if not values:
        raise Unavailable("respuesta vacía de la extensión")
    return bool(values[0])


# ----------------------------------------------------------------- tarjeta
def _clip(text: str, head: int = 320, tail: int = 160) -> str:
    """Recorta un texto largo antes de mandarlo a la tarjeta.

    ARG_MAX permite mandar megabytes de argumentos a ``sudo``. Si eso llega
    tal cual a un ``St.Label`` de 456 px dentro de un modal del sistema, el
    shell se pone a maquetar texto sin fin y el escritorio se queda colgado.
    Se conserva el principio (el ejecutable) y el final (los últimos
    argumentos), que es donde suele estar lo que importa.
    """
    text = str(text)
    if len(text) <= head + tail + 1:
        return text
    return f"{text[:head]} … {text[-tail:]}"


def _describe_service(service: str, argv: list[str]) -> tuple[str, str, str]:
    """(aplicación, editor, pregunta) según quién pida la autenticación.

    Con ``sudo`` el comando está en ``/proc/<ppid>/cmdline``. Con polkit el
    padre es ``polkit-agent-helper-1``, que no dice qué acción se autoriza (eso
    lo sabe polkitd, no PAM), así que la tarjeta es genérica y honesta: no se
    inventa el nombre de una aplicación que no podemos comprobar.
    """
    if service.startswith("polkit"):
        return (
            "Aplicación del sistema",
            "polkit",
            "¿Quieres permitir que esta aplicación haga cambios en el dispositivo?",
        )
    return (
        _clip(_guess_app(argv), head=80, tail=20),
        "Terminal · sudo",
        "¿Quieres permitir que esta aplicación ejecute un comando como "
        "administrador?",
    )


def _build_request(entry: pwd.struct_passwd, service: str, tty: str) -> dict:
    parent = os.getppid()
    argv = _read_cmdline(parent)
    try:
        cwd = os.readlink(f"/proc/{parent}/cwd")
    except OSError:
        cwd = ""

    app_name, publisher, message = _describe_service(service, argv)
    command = _clip(" ".join(shlex.quote(arg) for arg in argv) if argv else "sudo")
    details = [
        ["Usuario", entry.pw_name],
        ["Equipo", socket.gethostname()],
        ["Directorio", _clip(cwd) or "?"],
        ["Autenticación", "Un clic"],
        ["Servicio", service],
    ]
    if tty:
        details.append(["Terminal", _clip(tty)])

    return {
        "app_name": app_name,
        "publisher": publisher,
        "message": message,
        "command": command,
        "details": details,
        "password_required": False,
        "username": entry.pw_name,
        "session": _session_env_of(entry.pw_uid),
        "timeout": CARD_TIMEOUT_S,
    }


def _audit_fields(entry: pwd.struct_passwd, service: str, tty: str,
                  request: dict | None = None, **extra) -> dict:
    fields = {
        "user": entry.pw_name,
        "uid": entry.pw_uid,
        "service": service,
        "tty": tty,
        "ppid": os.getppid(),
    }
    if request is not None:
        fields["cmd"] = request.get("command")
        fields["app"] = request.get("app_name")
    fields.update(extra)
    return fields


def _target_entry(args: list[str]) -> pwd.struct_passwd | None:
    """Usuario al que se refiere el aviso.

    En PAM sale de ``PAM_USER``. ``uac-pam card <usuario>`` permite probarlo a
    mano desde root sin tocar PAM.
    """
    name = os.environ.get("PAM_USER") or (args[0] if args else "")
    if not name:
        name = os.environ.get("SUDO_USER") or ""
    if not name:
        return None
    try:
        return pwd.getpwnam(name)
    except KeyError:
        return None


def run_card(args: list[str]) -> int:
    service = os.environ.get("PAM_SERVICE") or "sudo-uac"
    tty = os.environ.get("PAM_TTY") or ""
    entry = _target_entry(args)
    if entry is None:
        _log("card: no sé a qué usuario corresponde la petición")
        return 1
    if entry.pw_uid == 0:
        # root no se autentica: si llega aquí, que siga la pila normal.
        return 1
    if _root_flag("disabled"):
        _log("card: desactivado por /etc/uac-linux/disabled")
        _audit("desactivado", user=entry.pw_name, uid=entry.pw_uid,
               service=service, tty=tty)
        return 1

    _become_root()

    if _spam_blocked(entry.pw_uid):
        _log("card: demasiadas denegaciones seguidas; se deniega sin tarjeta")
        _audit("spam", user=entry.pw_name, uid=entry.pw_uid, service=service,
               tty=tty, ventana=SPAM_WINDOW_S, limite=SPAM_DENIALS)
        _write_deny()
        return 1

    request = _build_request(entry, service, tty)
    _log(
        "card: inicio "
        f"(uid={entry.pw_uid} euid={os.geteuid()} padre={os.getppid()} "
        f"servicio={service} cmd={request['command']!r})"
    )

    started = time.monotonic()
    play_alert(entry)
    try:
        approved = _ask_shell(entry.pw_uid, request)
    except Unavailable as exc:
        _log(f"card: sin tarjeta, se pedirá contraseña ({exc})")
        _audit("sin-tarjeta", motivo=str(exc),
               **_audit_fields(entry, service, tty, request))
        return 1

    ms = int((time.monotonic() - started) * 1000)
    if approved:
        _clear_denials(entry.pw_uid)
        _log("card: aprobado")
        _audit("aprobado", ms=ms, **_audit_fields(entry, service, tty, request))
        return 0

    _record_denial(entry.pw_uid)
    _log("card: denegado por el usuario")
    _audit("denegado", ms=ms, **_audit_fields(entry, service, tty, request))
    _write_deny()
    return 1


def run_selftest(args: list[str]) -> int:
    entry = _target_entry(args)
    if entry is None:
        print("no sé qué usuario comprobar", file=sys.stderr)
        return 2

    print(f"usuario      : {entry.pw_name} (uid {entry.pw_uid})")
    print(f"proceso      : uid {os.getuid()} euid {os.geteuid()}")
    print(f"padre        : {os.getppid()} {' '.join(_read_cmdline(os.getppid()))}")
    print(f"binario      : {Path(sys.argv[0]).resolve()}")
    guard = Path("/usr/local/libexec/uac-linux/uac-denied")
    guard_ok = guard.is_file() and os.access(guard, os.X_OK)
    print(f"denegación   : {'sí' if guard_ok else 'NO'} ({guard})")

    sound = SOUND_FILE.is_file()
    print(f"sonido       : {'sí' if sound else 'no'} ({SOUND_FILE})")
    print(f"reproductor  : {_player() or 'ninguno'}")
    print(f"desactivado  : {'sí' if _root_flag('disabled') else 'no'}")
    print(f"estado       : {_state_dir() or 'NO ESCRIBIBLE'}")
    polkit = Path("/etc/pam.d/polkit-1")
    print(f"polkit       : {'sí' if polkit.is_file() else 'no'} ({polkit})")
    denials = _denials_path(entry.pw_uid)
    count = len(_recent_denials(denials)) if denials is not None else 0
    blocked = count >= SPAM_DENIALS
    print(
        f"anti-spam    : {count}/{SPAM_DENIALS} denegaciones en {SPAM_WINDOW_S} s"
        f"{' -> BLOQUEADO' if blocked else ''}"
    )

    bus = Path(f"/run/user/{entry.pw_uid}/bus")
    print(f"bus de sesión: {'sí' if bus.exists() else 'no'} ({bus})")
    if not bus.exists():
        print("extensión    : sin sesión gráfica -> se pedirá contraseña")
        return 1

    try:
        _, card_owner, _gio, _glib = _shell_channel(entry.pw_uid)
    except Unavailable as exc:
        print(f"extensión    : {exc}")
        return 1

    print(f"extensión    : responde ({card_owner} es GNOME Shell)")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    mode = args[0] if args else "card"
    rest = args[1:]

    if mode == "card":
        return run_card(rest)
    if mode == "selftest":
        return run_selftest(rest)

    print(f"uac-pam: modo desconocido: {mode}", file=sys.stderr)
    print("uso: uac-pam card|selftest [usuario]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
