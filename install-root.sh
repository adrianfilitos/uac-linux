#!/usr/bin/env bash
# Instala el puente PAM de uac-linux (requiere root).
#
#   uac-sudo bash install-root.sh [usuario]      # todo
#   uac-sudo bash install-root.sh --files-only   # solo binario y reglas PAM
#
# Lo que hace:
#   1. Copia el puente a /usr/local/libexec/uac-linux/uac-pam (root, 0755).
#   2. Escribe /etc/pam.d/sudo-uac, /etc/pam.d/sudo-uac-login y
#      /etc/pam.d/polkit-1 (para que pkexec también muestre la tarjeta).
#   3. Añade Defaults en /etc/sudoers.d/90-uac-<usuario> para que sudo use ese
#      servicio PAM, sin caché de contraseña y con un solo intento.
#   4. Quita la regla NOPASSWD: a partir de aquí toda elevación pasa por el
#      aviso o por la contraseña, nunca por la puerta de atrás.
#
# Si algo falla en la prueba, deja sudo como estaba.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LIBEXEC="/usr/local/libexec/uac-linux"
CONFIG_DIR="/etc/uac-linux"
PAM_SUDO="/etc/pam.d/sudo-uac"
PAM_LOGIN="/etc/pam.d/sudo-uac-login"
PAM_POLKIT="/etc/pam.d/polkit-1"
STAMP="$(date +%Y%m%d-%H%M%S)"

FILES_ONLY=0
TARGET_USER=""
for arg in "$@"; do
    case "$arg" in
        --files-only) FILES_ONLY=1 ;;
        -*) echo "ERROR: opción desconocida: $arg" >&2; exit 1 ;;
        *) TARGET_USER="$arg" ;;
    esac
done
[ -n "$TARGET_USER" ] || TARGET_USER="${SUDO_USER:-}"

say() { printf '\n==> %s\n' "$*"; }
warn() { printf '\nAVISO: %s\n' "$*" >&2; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "ejecútalo como root: uac-sudo bash $0 [usuario]"
if [ "$FILES_ONLY" -eq 0 ]; then
    [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] \
        || die "no sé para qué usuario instalar (pasa el nombre)."
    id "$TARGET_USER" >/dev/null 2>&1 \
        || die "el usuario '$TARGET_USER' no existe."
fi

# --- 1. puente --------------------------------------------------------------
say "Copiando el puente a $LIBEXEC"
install -d -m 0755 "$LIBEXEC"
install -m 0755 "$ROOT/src/uac_pam.py" "$LIBEXEC/uac-pam"
install -m 0755 "$ROOT/src/uac_denied.sh" "$LIBEXEC/uac-denied"
if [ -f "$ROOT/src/data/uac.wav" ]; then
    install -m 0644 "$ROOT/src/data/uac.wav" "$LIBEXEC/uac.wav"
else
    warn "no encuentro src/data/uac.wav: el aviso saldrá sin sonido."
fi
install -d -m 0755 "$CONFIG_DIR"
install -d -m 0700 /run/uac-pam

# --- 2. reglas PAM ----------------------------------------------------------
write_pam() {
    local path="$1" session="$2" tmp
    if [ -f "$path" ]; then
        cp -a "$path" "$path.uac-backup.$STAMP"
    fi
    tmp="$(mktemp)"
    cat >"$tmp" <<EOF
#%PAM-1.0
# Generado por uac-linux. ./install-root.sh lo reescribe: no lo edites a mano.
#
# La tarjeta va delante de la autenticación normal:
#   Sí        -> card sale 0 -> success=done -> entra sin contraseña
#   No        -> card deja marca -> uac-denied sale 1 -> cae en pam_deny
#   sin aviso -> no hay marca -> uac-denied sale 0 -> sigue la pila -> contraseña
#
# La denegación no la puede dar "card" directamente: pam_exec solo distingue
# "salió 0" de "salió otra cosa", y [success=die] devolvería PAM_SUCCESS (es
# decir, autenticaría). Por eso el «No» pasa por pam_deny.
#
# El segundo paso es un script de shell, no el puente en Python: si alguien
# borra uac-pam, uac-denied sigue diciendo "no hay marca" y la autenticación
# cae en la contraseña en vez de dejar sudo (y pkexec) inservibles.
auth [success=done default=ignore] pam_exec.so seteuid quiet $LIBEXEC/uac-pam card
auth [success=1 default=ignore] pam_exec.so seteuid quiet $LIBEXEC/uac-denied
auth requisite pam_deny.so

# Límites y entorno locales, igual que en /etc/pam.d/sudo
session required pam_limits.so
session required pam_env.so readenv=1 user_readenv=0
session required pam_env.so readenv=1 envfile=/etc/default/locale user_readenv=0

@include common-auth
@include common-account
$session
EOF
    install -m 0644 -o root -g root "$tmp" "$path"
    rm -f "$tmp"
}

say "Escribiendo $PAM_SUDO, $PAM_LOGIN y $PAM_POLKIT"
write_pam "$PAM_SUDO" "@include common-session-noninteractive"
write_pam "$PAM_LOGIN" "@include common-session"
# polkit: es lo que autentica pkexec y los diálogos de "se requieren
# privilegios" de GNOME. Ojo: a partir de aquí pkexec también muestra la
# tarjeta, así que deja de servir como puerta de emergencia (para eso queda
# "su -" desde un TTY o una sesión de recuperación).
write_pam "$PAM_POLKIT" "@include common-session-noninteractive"

# --- 2b. permiso para que root hable con el bus de la sesión ----------------
# El puente corre como root (pam_exec con `seteuid`), y el bus de sesión solo
# admite al usuario dueño del bus. Se le concede a root la conexión y nada
# más: no puede recibir mensajes ni suplantar servicios.
DBUS_LOCAL="/etc/dbus-1/session-local.conf"
if [ -f "$DBUS_LOCAL" ] && ! grep -q "uac-linux" "$DBUS_LOCAL"; then
    warn "$DBUS_LOCAL ya existe y no es mío. Añade a mano dentro de"
    echo '     <policy context="default">:  <allow user="root"/>'
elif [ ! -f "$DBUS_LOCAL" ]; then
    say "Permitiendo que root se conecte al bus de sesión"
    cat >"$DBUS_LOCAL" <<'XML'
<!-- Añadido por uac-linux: el puente de PAM corre como root y necesita poder
     hablar con el bus de la sesión para mostrar la tarjeta. Solo se le
     permite la conexión; no puede recibir mensajes de otros clientes ni
     suplantar ningún servicio. -->
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy context="default">
    <!-- uac-linux: permitir la conexión del puente, que corre como root -->
    <allow user="root"/>
  </policy>
</busconfig>
XML
    chmod 0644 "$DBUS_LOCAL"
fi

# El bus ya arrancado no lee la configuración nueva hasta que se lo piden.
TARGET_UID="$(id -u "$TARGET_USER")"
if [ -S "/run/user/$TARGET_UID/bus" ]; then
    runuser -u "$TARGET_USER" -- env \
        XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$TARGET_UID/bus" \
        dbus-send --session --print-reply --type=method_call \
        --dest=org.freedesktop.DBus / org.freedesktop.DBus.ReloadConfig \
        >/dev/null 2>&1 \
        && echo "  OK: bus de sesión recargado." \
        || warn "no pude recargar el bus de la sesión."
else
    warn "no hay sesión gráfica abierta; el bus se recargará al entrar."
fi

if [ "$FILES_ONLY" -eq 1 ]; then
    say "Comprobando el puente contra la sesión de $TARGET_USER"
    "$LIBEXEC/uac-pam" selftest "$TARGET_USER" || true
    cat <<EOF

Si arriba pone «responde», prueba la tarjeta sin tocar sudo:

    uac-sudo $LIBEXEC/uac-pam card $TARGET_USER

Y cuando funcione, remata con:

    uac-sudo bash $ROOT/install-root.sh $TARGET_USER
EOF
    exit 0
fi

# --- 3. sudoers -------------------------------------------------------------
SUDOERS="/etc/sudoers.d/90-uac-$TARGET_USER"
BACKUP="/etc/sudoers.d/.90-uac-$TARGET_USER.bak.$STAMP"

if [ -e "$SUDOERS" ]; then
    cp -a "$SUDOERS" "$BACKUP"
fi

# Escribe la regla. Formato: dos líneas de Defaults y, si se pide, la regla
# NOPASSWD que solo existe durante la prueba previa.
write_sudoers() {
    local tmp
    tmp="$(mktemp)"
    cat >"$tmp" <<EOF
# Generado por uac-linux para $TARGET_USER.
# Cada elevación pasa por el aviso de consentimiento (/etc/pam.d/sudo-uac).
Defaults:$TARGET_USER pam_service=sudo-uac
Defaults:$TARGET_USER pam_login_service=sudo-uac-login
# Un solo intento: si la tarjeta dice "No", sudo no insiste.
Defaults:$TARGET_USER passwd_tries=1
# Sin caché: la tarjeta sale en cada elevación, como el UAC de Windows.
Defaults:$TARGET_USER timestamp_timeout=0
$1
EOF
    visudo -cf "$tmp" >/dev/null 2>&1 || { rm -f "$tmp"; return 1; }
    install -m 0440 -o root -g root "$tmp" "$SUDOERS"
    rm -f "$tmp"
}

restore_sudoers() {
    if [ -e "$BACKUP" ]; then
        mv -f "$BACKUP" "$SUDOERS"
    else
        rm -f "$SUDOERS"
    fi
}

say "Probando el servicio PAM nuevo (con la regla NOPASSWD todavía puesta)"
write_sudoers "$TARGET_USER ALL=(ALL:ALL) NOPASSWD: ALL" \
    || die "la regla de sudoers generada no es válida."

if runuser -u "$TARGET_USER" -- /usr/bin/sudo -n true 2>/dev/null; then
    echo "  OK: el servicio $PAM_SUDO no estorba a sudo."
else
    restore_sudoers
    die "sudo ha fallado con el servicio nuevo. No se ha cambiado nada."
fi

say "Quitando la regla NOPASSWD"
write_sudoers "" || { restore_sudoers; die "no pude escribir el sudoers final."; }

if runuser -u "$TARGET_USER" -- /usr/bin/sudo -n true 2>/dev/null; then
    warn "todavía se puede usar sudo sin contraseña. Revisa 'sudo -l'."
else
    echo "  OK: ya no hay camino silencioso a root."
fi

say "Instalado."
cat <<EOF

Ahora, en una terminal normal (no como root):

    sudo id        # debe salir la tarjeta; "Sí" ejecuta sin contraseña
    sudo id        # y "No" no debe ejecutar nada ni pedir contraseña
    pkexec id      # también sale la tarjeta (polkit pasa por PAM)

OJO: como pkexec también pide la tarjeta, ya NO sirve como vía de
emergencia. Si alguna vez sudo se quedara roto, entra con:

    su -           # desde un TTY (Ctrl+Alt+F3), con la contraseña de root

Si el aviso no aparece, la contraseña sigue siendo el respaldo.
Para volver atrás del todo:

    uac-sudo rm -f $SUDOERS $PAM_SUDO $PAM_LOGIN $PAM_POLKIT

Registro de diagnóstico del puente: /run/uac-pam/last.log
Auditoría de cada decisión:        journalctl -t uac-linux -n 50
EOF
