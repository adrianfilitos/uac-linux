#!/usr/bin/env bash
# Instalador de uac-linux (parte de usuario + puente PAM de root).
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HOME/.local/bin"
BASHRC="$HOME/.bashrc"
MARK_BEGIN="# >>> uac-linux >>>"
MARK_END="# <<< uac-linux <<<"

say() { printf '\n==> %s\n' "$*"; }
warn() { printf '\nAVISO: %s\n' "$*" >&2; }

say "Instalando uac-linux desde $ROOT"

command -v python3 >/dev/null || {
    echo "ERROR: falta python3." >&2
    exit 1
}
if ! python3 - <<'PY'
import gi
gi.require_version("Gtk", "4.0")
PY
then
    echo "ERROR: faltan las bindings de GTK4 (python3-gi)." >&2
    exit 1
fi

chmod +x "$ROOT/src/uac_dialog.py" "$ROOT/src/uac_sudo.py" "$ROOT/src/uac_askpass.py"
mkdir -p "$BIN"
ln -sfn "$ROOT/src/uac_dialog.py" "$BIN/uac-dialog"
ln -sfn "$ROOT/src/uac_sudo.py" "$BIN/uac-sudo"
ln -sfn "$ROOT/src/uac_askpass.py" "$BIN/uac-askpass"
say "Enlaces creados en $BIN"

# --- Extensión de GNOME Shell (escritorio seguro) --------------------------
EXT_UUID="uac-linux@nixer"
EXT_DIR="$HOME/.local/share/gnome-shell/extensions/$EXT_UUID"
if command -v gjs >/dev/null && command -v gnome-extensions >/dev/null; then
    # Copia real, no enlace: el shell solo descubre extensiones al arrancar,
    # y así no depende de cómo resuelva los symlinks.
    mkdir -p "$EXT_DIR"
    cp -f "$ROOT/extension/metadata.json" \
          "$ROOT/extension/extension.js" \
          "$ROOT/extension/stylesheet.css" \
          "$ROOT/extension/uac-shield.svg" \
          "$EXT_DIR/"
    say "Extensión copiada en $EXT_DIR"
    # Se activa por gsettings: gnome-extensions enable falla si el shell aún
    # no conoce la extensión, porque solo descubre directorios al arrancar.
    python3 - "$EXT_UUID" <<'PYEXT' || warn "No pude activar la extensión; actívala a mano."
import ast, subprocess, sys
uuid = sys.argv[1]
key = ["gsettings", "get", "org.gnome.shell", "enabled-extensions"]
current = subprocess.run(key, capture_output=True, text=True).stdout.strip()
enabled = ast.literal_eval(current) if current else []
if uuid not in enabled:
    enabled.append(uuid)
    value = "[" + ", ".join(f"'{item}'" for item in enabled) + "]"
    subprocess.run(["gsettings", "set", "org.gnome.shell",
                    "enabled-extensions", value], check=True)
    print(f"Extensión añadida a enabled-extensions: {uuid}")
else:
    print(f"Extensión ya activada: {uuid}")
PYEXT
    if gnome-extensions info "$EXT_UUID" 2>/dev/null | grep -qi "ACTIVE\|Activad"; then
        echo "La extensión ya está activa en esta sesión."
    else
        warn "La extensión se cargará al reiniciar GNOME Shell."
        echo "    Cierra sesión y vuelve a entrar UNA vez."
        echo "    Después comprueba: gnome-extensions info $EXT_UUID"
    fi
else
    warn "No encuentro gjs/gnome-extensions: el aviso usará la ventana GTK."
fi

# --- Tarjeta de Plasma (uac-card) -----------------------------------------
# En Plasma no hay una capa de shell donde dibujar el aviso como en GNOME, así
# que la tarjeta es un binario aparte: se compila aquí y se instala como root
# en /usr/local/libexec/uac-linux/ (el puente comprueba que sea de confianza).
CARD_SRC="$ROOT/plasma/uac_card.c"
CARD_BIN="$ROOT/plasma/uac-card"
BUILD_CARD=0
if [ -f "$CARD_SRC" ]; then
    say "Compilando la tarjeta de Plasma (uac-card)"
    if ! command -v gcc >/dev/null 2>&1; then
        warn "falta gcc: no compilo uac-card. En Plasma se pedirá la contraseña."
    elif ! pkg-config --exists gtk4 json-glib-1.0 x11 xi; then
        warn "faltan dependencias de uac-card."
        echo "    apt install libgtk-4-dev libjson-glib-dev libxi-dev" >&2
    elif gcc -O2 -Wall -o "$CARD_BIN" "$CARD_SRC" \
            $(pkg-config --cflags --libs gtk4 json-glib-1.0 x11 xi) -lm; then
        BUILD_CARD=1
        echo "  OK: $CARD_BIN"
    else
        warn "la compilación de uac-card ha fallado."
    fi
fi

if [ "$BUILD_CARD" -eq 1 ]; then
    mkdir -p "$HOME/.config/autostart"
    cat >"$HOME/.config/autostart/uac-card.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=uac-card
Comment=Tarjeta de consentimiento de uac-linux
Exec=/usr/local/libexec/uac-linux/uac-card
Terminal=false
NoDisplay=true
OnlyShowIn=KDE;
EOF
    say "Autostart de Plasma instalado en ~/.config/autostart/uac-card.desktop"
fi

# --- Puente PAM (requiere root) -------------------------------------------
if [ "${1:-}" != "--no-root" ]; then
    say "Instalando el puente PAM. Te pedirá tu contraseña UNA vez."
    /usr/bin/sudo bash "$ROOT/install-root.sh" "$USER"
else
    warn "Paso de root omitido (--no-root)."
    echo "    Ejecuta esto cuando quieras activar el aviso:"
    echo "    /usr/bin/sudo bash $ROOT/install-root.sh $USER"
fi

# --- Arrancar la tarjeta ahora (si estamos en Plasma) ----------------------
if [ "$BUILD_CARD" -eq 1 ] && [ -x /usr/local/libexec/uac-linux/uac-card ] \
   && [[ "${XDG_CURRENT_DESKTOP:-}" == *KDE* ]] \
   && ! pgrep -x uac-card >/dev/null 2>&1; then
    setsid -f /usr/local/libexec/uac-linux/uac-card >/dev/null 2>&1 || true
    say "Tarjeta de Plasma arrancada en esta sesión"
fi

# --- Función sudo para bash ------------------------------------------------
# Ya no es la que muestra el aviso (eso lo hace PAM con cualquier sudo). Solo
# aporta el respaldo de contraseña cuando no hay terminal donde escribirla.
touch "$BASHRC"
if ! grep -qF "$MARK_BEGIN" "$BASHRC"; then
    cp "$BASHRC" "$BASHRC.uac-backup.$(date +%Y%m%d-%H%M%S)"
    cat >> "$BASHRC" <<'EOF'

# >>> uac-linux >>>
# El aviso lo pone PAM; esto solo añade la ventana de contraseña si no hay tty.
sudo() { command "$HOME/.local/bin/uac-sudo" "$@"; }
# <<< uac-linux <<<
EOF
    say "Función sudo añadida a $BASHRC"
else
    say "La función sudo ya estaba en $BASHRC"
fi

say "Listo."
cat <<EOF

Abre una terminal nueva (o ejecuta: source ~/.bashrc) y prueba:

    sudo true          # "Sí" ejecuta, "No" no ejecuta nada
    uac-dialog --demo

El aviso sale con cualquier sudo (bash, Python, /usr/bin/sudo). Si algo falla
(sin sesión gráfica, extensión caída), sudo pide la contraseña como siempre.

Para apagarlo hace falta root (a propósito):

    uac-sudo touch /etc/uac-linux/disabled
    uac-sudo rm -f /etc/uac-linux/disabled

Documentación y avisos de seguridad: $ROOT/README.md
EOF
