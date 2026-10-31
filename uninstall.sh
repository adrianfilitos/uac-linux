#!/usr/bin/env bash
# Desinstala la parte de usuario y el puente PAM de root.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BASHRC="$HOME/.bashrc"
MARK_BEGIN="# >>> uac-linux >>>"
MARK_END="# <<< uac-linux <<<"
SUDOERS_FILE="/etc/sudoers.d/90-uac-${USER}"
LIBEXEC="/usr/local/libexec/uac-linux"
PAM_FILES=("/etc/pam.d/sudo-uac" "/etc/pam.d/sudo-uac-login")

say() { printf '\n==> %s\n' "$*"; }

say "Quitando la función sudo de $BASHRC"
if grep -qF "$MARK_BEGIN" "$BASHRC" 2>/dev/null; then
    cp "$BASHRC" "$BASHRC.uac-backup.$(date +%Y%m%d-%H%M%S)"
    awk -v begin="$MARK_BEGIN" -v end="$MARK_END" '
        $0 == begin { skip = 1; next }
        $0 == end   { skip = 0; next }
        !skip       { print }
    ' "$BASHRC" >"$BASHRC.uac-tmp"
    mv "$BASHRC.uac-tmp" "$BASHRC"
    echo "Hecho (backup guardado al lado de .bashrc)."
else
    echo "No había bloque de uac-linux."
fi

say "Quitando enlaces de ~/.local/bin"
rm -f \
    "$HOME/.local/bin/uac-dialog" \
    "$HOME/.local/bin/uac-sudo" \
    "$HOME/.local/bin/uac-askpass"
echo "Hecho."

say "Quitando la caché del aviso sonoro"
rm -rf "$HOME/.cache/uac-linux"
echo "Hecho."

say "Quitando la extensión de GNOME Shell"
gnome-extensions disable "uac-linux@nixer" 2>/dev/null || true
python3 - <<'PYEXT' || true
import ast, subprocess
uuid = "uac-linux@nixer"
current = subprocess.run(["gsettings", "get", "org.gnome.shell",
                          "enabled-extensions"],
                         capture_output=True, text=True).stdout.strip()
enabled = ast.literal_eval(current) if current else []
if uuid in enabled:
    enabled.remove(uuid)
    value = "[" + ", ".join(f"'{item}'" for item in enabled) + "]"
    subprocess.run(["gsettings", "set", "org.gnome.shell",
                    "enabled-extensions", value], check=True)
PYEXT
rm -rf "$HOME/.local/share/gnome-shell/extensions/uac-linux@nixer"
echo "Hecho."

if [ -e "$SUDOERS_FILE" ] || [ -d "$LIBEXEC" ]; then
    say "Quitando el puente PAM (te pedirá la contraseña una vez)"
    /usr/bin/sudo rm -rf "$LIBEXEC" \
        "${PAM_FILES[@]}" \
        /etc/uac-linux \
        "$SUDOERS_FILE"
    /usr/bin/sudo rm -f /etc/pam.d/sudo-uac.uac-backup.* \
        /etc/pam.d/sudo-uac-login.uac-backup.* 2>/dev/null || true
    /usr/bin/sudo rmdir /run/uac-pam 2>/dev/null || true
    # La política del bus de sesión solo se quita si la pusimos nosotros.
    if /usr/bin/sudo grep -q "uac-linux" /etc/dbus-1/session-local.conf 2>/dev/null; then
        /usr/bin/sudo rm -f /etc/dbus-1/session-local.conf
        systemctl --user reload dbus 2>/dev/null || true
    fi
    echo "Hecho: sin puente y sin Defaults. sudo vuelve a pedir contraseña."
else
    echo "No hay puente PAM instalado."
fi

say "Desinstalado. Abre una terminal nueva para recuperar el sudo normal."
echo "El código sigue en: $ROOT"
