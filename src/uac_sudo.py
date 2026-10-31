#!/usr/bin/python3
"""Envoltorio de sudo para uac-linux.

Aquí ya no se pinta nada: la tarjeta la muestra el puente PAM
(``/usr/local/libexec/uac-linux/uac-pam``), que se dispara con cualquier
``sudo``, venga de donde venga.

Esto solo sirve para que ``sudo`` funcione **sin terminal** (una app gráfica,
un script lanzado desde el escritorio, el propio instalador). En ese caso sudo
no tiene dónde pedir la contraseña, así que se le da ``uac-askpass``, que abre
una ventana GTK. Si hay terminal, se comporta como el sudo de siempre.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

SUDO = "/usr/bin/sudo"
ASKPASS = Path(__file__).resolve().parent / "uac_askpass.py"


def main() -> int:
    if not Path(SUDO).exists():
        print(f"uac-sudo: no encuentro {SUDO}", file=sys.stderr)
        return 1

    argv = [SUDO]
    if not sys.stdin.isatty():
        argv.append("-A")
    argv.extend(sys.argv[1:])

    env = dict(os.environ)
    env["SUDO_ASKPASS"] = str(ASKPASS)

    try:
        os.execve(SUDO, argv, env)
    except OSError as exc:
        print(f"uac-sudo: no pude ejecutar sudo: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
