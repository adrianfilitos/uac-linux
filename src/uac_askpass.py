#!/usr/bin/python3
"""Ayudante SUDO_ASKPASS: muestra el diálogo UAC y devuelve la contraseña.

sudo ejecuta este programa cuando se lanza con -A. El contrato de sudo es
simple: lo que se escriba en stdout es la contraseña. Todo lo demás (avisos,
errores) va a stderr para no contaminarla.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
DIALOG = BASE_DIR / "uac_dialog.py"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True, help="Fichero JSON.")
    args = parser.parse_args(argv)

    request_path = Path(args.request)
    rundir = request_path.parent

    # Si el usuario ya dijo "No", no le volvemos a preguntar en los reintentos
    # que hace sudo por su cuenta (passwd_tries=3 por defecto).
    if (rundir / "denied").exists():
        return 1

    attempts_file = rundir / "attempts"
    attempt = 0
    if attempts_file.exists():
        try:
            attempt = int(attempts_file.read_text(encoding="utf-8").strip() or "1")
        except ValueError:
            attempt = 1

    cmd = [sys.executable, str(DIALOG), "--request", str(request_path)]
    if attempt:
        cmd.extend(
            [
                "--error",
                "Contraseña incorrecta. Inténtalo de nuevo.",
            ]
        )

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=600,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"uac: no se pudo mostrar el diálogo: {exc}", file=sys.stderr)
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1

    attempts_file.write_text(str(attempt + 1), encoding="utf-8")

    if proc.stderr:
        sys.stderr.write(proc.stderr)
        sys.stderr.flush()

    if proc.returncode != 0:
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1

    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if not lines:
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1
    try:
        data = json.loads(lines[-1])
    except json.JSONDecodeError:
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1

    if not data.get("approved"):
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1

    password = data.get("password") or ""
    if not password:
        (rundir / "denied").write_text("1", encoding="utf-8")
        return 1

    sys.stdout.write(password + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
