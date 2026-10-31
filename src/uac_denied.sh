#!/bin/sh
# uac-linux - segundo paso del aviso de consentimiento.
#
# Se instala como /usr/local/libexec/uac-linux/uac-denied y lo llama la
# segunda línea de pam_exec de /etc/pam.d/sudo-uac (y de sudo-uac-login y
# polkit-1), justo antes de "auth requisite pam_deny.so":
#
#   auth [success=1 default=ignore] pam_exec.so seteuid quiet \
#        /usr/local/libexec/uac-linux/uac-denied
#
# Sale 0 cuando NO hay marca de denegacion (la pila sigue y pedira la
# contrasena) y 1 cuando la hay (PAM salta a pam_deny y falla en firme).
#
# Va aparte del puente en Python a proposito: si alguien borra
# /usr/local/libexec/uac-linux/uac-pam, este script sigue vivo y sudo cae en
# la contrasena en lugar de quedarse inservible (fail-closed). Un "No" sigue
# denegando porque la marca la escribe el puente antes de salir.
#
# La clave es <pid del padre>-<campo 22 de /proc/<pid>/stat>, igual que en
# uac_pam.py: las dos lineas de pam_exec comparten padre, y el campo 22 (el
# instante de arranque) evita que un pid reciclado arrastre una marca vieja.

set -u

STATE_DIR=/run/uac-pam
DENY_TTL=300

[ -d "$STATE_DIR" ] || exit 0
[ -r "/proc/$PPID/stat" ] || exit 0

# El campo "comm" puede llevar espacios y parentesis, asi que se corta por el
# ultimo ") " y se cuenta desde el campo de estado (el 3 del original).
start=$(/usr/bin/sed 's/.*) //' "/proc/$PPID/stat" 2>/dev/null |
    /usr/bin/cut -d' ' -f20)
[ -n "$start" ] || exit 0

marker="$STATE_DIR/$PPID-$start.deny"
[ -e "$marker" ] || exit 0

now=$(/usr/bin/date +%s)
mtime=$(/usr/bin/stat -c %Y "$marker" 2>/dev/null || echo 0)

# La marca se consume siempre: da igual si luego caduca o no.
rm -f "$marker" 2>/dev/null

[ "$mtime" -gt 0 ] || exit 0
[ $((now - mtime)) -le "$DENY_TTL" ] || exit 0

exit 1
