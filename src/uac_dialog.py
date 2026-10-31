#!/usr/bin/env python3
"""Diálogo UAC (Control de cuentas de usuario) para Linux.

Imita el aviso de consentimiento de Windows 11: una ventana clara y centrada
sobre un escritorio atenuado. Para que Mutter inhiba Super/Alt+Tab la
superficie real es fullscreen, pero lo que se ve es una tarjeta de 456 px con
su barra de título, como en Windows.

Entrada (--request fichero.json):
{
  "app_name": "apt",
  "publisher": "Terminal · sudo",
  "message": "¿Quieres permitir que esta aplicación haga cambios...?",
  "command": "sudo apt install htop",
  "details": [["Usuario", "nixer"]],
  "password_required": false,
  "username": "nixer",
  "error": "Contraseña incorrecta",
  "fullscreen": true,
  "session": {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0"}
}

Salida por stdout:
{"approved": true, "password": null}

Códigos de salida: 0 aprobado, 1 denegado, 2 error.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
CSS_PATH = DATA_DIR / "uac.css"
SHIELD_PATH = DATA_DIR / "uac-shield.svg"

APP_ID = "io.github.nixer.UacDialog"
DEFAULT_MESSAGE = (
    "¿Quieres permitir que esta aplicación haga cambios en tu dispositivo?"
)

# Iconos de aplicación para las órdenes más habituales. El resto usa una
# terminal; el escudo pequeño se superpone siempre como en Windows.
APP_ICONS = {
    "apt": "system-software-install",
    "apt-get": "system-software-install",
    "dpkg": "system-software-install",
    "snap": "system-software-install",
    "systemctl": "applications-system",
    "service": "applications-system",
}


def restore_session_env(session: dict | None) -> None:
    """Recupera variables de sesión que sudo puede haber filtrado.

    El ayudante de contraseña de sudo se ejecuta en un entorno saneado; sin
    estas variables GTK no sabría a qué pantalla conectarse.
    """
    if session:
        for key, value in session.items():
            if value and not os.environ.get(key):
                os.environ[key] = str(value)
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    os.environ.setdefault("XDG_RUNTIME_DIR", runtime)
    os.environ.setdefault(
        "DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime}/bus"
    )
    os.environ.setdefault("XDG_SESSION_TYPE", "wayland")


def load_request(args: argparse.Namespace) -> dict:
    request: dict = {}
    if args.request:
        with open(args.request, "r", encoding="utf-8") as fh:
            request = json.load(fh)

    for key in ("app_name", "publisher", "message", "command", "username", "error"):
        value = getattr(args, key, None)
        if value:
            request[key] = value
    if args.detail:
        request.setdefault("details", []).extend(args.detail)
    if args.password:
        request["password_required"] = True
    if getattr(args, "fullscreen", False):
        request["fullscreen"] = True

    request.setdefault("message", DEFAULT_MESSAGE)
    request.setdefault("password_required", False)
    request.setdefault("fullscreen", False)
    details = request.setdefault("details", [])

    # El comando va en "Mostrar detalles", como la ruta del programa en
    # Windows; en el cuerpo solo se muestra el nombre de la aplicación.
    if request.get("command"):
        has_command = any(
            isinstance(row, (list, tuple))
            and row
            and str(row[0]).lower() in ("comando", "command")
            for row in details
        )
        if not has_command:
            details.append(["Comando", request["command"]])
    return request


class UacDialog:
    """Ventana del diálogo. Se construye aparte para poder probarla."""

    def __init__(self, application, request: dict, result: dict) -> None:
        self.request = request
        self.result = result
        self.password_entry = None
        self.error_label = None
        self.details_button = None
        self.details_revealer = None
        self.fullscreen = bool(request.get("fullscreen", False))
        self._shortcuts_inhibited = False
        self._finished = False

        # Importamos GTK aquí para que restore_session_env() se ejecute antes
        # de que GTK inicialice la conexión con el servidor gráfico desde main().
        import gi

        gi.require_version("Gtk", "4.0")
        gi.require_version("Gdk", "4.0")
        from gi.repository import Gdk, Gio, GLib, Gtk

        self.Gtk = Gtk
        self.Gdk = Gdk
        self.Gio = Gio
        self.GLib = GLib

        self.window = Gtk.ApplicationWindow(
            application=application, title="Control de cuentas de usuario"
        )
        self.window.add_css_class("uac-window")
        self.dark = self._detect_dark_theme()
        self.window.add_css_class("uac-dark" if self.dark else "uac-light")
        self.window.set_modal(True)
        self.window.set_resizable(False)
        self.window.set_decorated(False)
        if self.fullscreen:
            self.window.add_css_class("uac-fullscreen")
        else:
            self.window.add_css_class("uac-windowed")
            self.window.set_default_size(620, 600)

        # Capa que oscurece el escritorio (escritorio seguro simulado) y
        # tarjeta clara centrada, con el aspecto del aviso de Windows.
        overlay = Gtk.Overlay()
        self.window.set_child(overlay)

        backdrop = Gtk.Box()
        backdrop.add_css_class("uac-backdrop")
        backdrop.set_hexpand(True)
        backdrop.set_vexpand(True)
        overlay.set_child(backdrop)

        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        card.add_css_class("uac-card")
        card.set_halign(Gtk.Align.CENTER)
        card.set_valign(Gtk.Align.CENTER)
        card.set_size_request(456, -1)
        overlay.add_overlay(card)

        # --- Cabecera gris: título + pregunta ------------------------------
        header = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        header.add_css_class("uac-header")
        card.append(header)

        titlebar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        titlebar.add_css_class("uac-titlebar")
        header.append(titlebar)

        title = Gtk.Label(label="Control de cuentas de usuario")
        title.add_css_class("uac-title")
        title.set_xalign(0)
        title.set_hexpand(True)
        titlebar.append(title)

        close_button = Gtk.Button()
        close_button.add_css_class("uac-close")
        close_button.set_valign(Gtk.Align.CENTER)
        close_image = Gtk.Image.new_from_icon_name("window-close-symbolic")
        close_image.set_pixel_size(12)
        close_button.set_child(close_image)
        close_button.connect("clicked", self._deny)
        titlebar.append(close_button)

        question = Gtk.Label(label=request["message"])
        question.add_css_class("uac-question")
        question.set_wrap(True)
        question.set_xalign(0)
        # Limita el ancho natural para que la tarjeta mida 456 px y la
        # pregunta se parta en dos líneas, como en el aviso de Windows.
        question.set_max_width_chars(42)
        header.append(question)

        # --- Cuerpo blanco: app, editor, contraseña y detalles -------------
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        body.add_css_class("uac-body")
        card.append(body)

        app_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        app_row.set_valign(Gtk.Align.CENTER)
        body.append(app_row)

        icon_overlay = Gtk.Overlay()
        icon_overlay.set_size_request(32, 32)
        icon_overlay.set_halign(Gtk.Align.START)
        icon_overlay.set_valign(Gtk.Align.START)
        app_icon = Gtk.Image.new_from_icon_name(
            self._app_icon_name(request.get("app_name") or "")
        )
        app_icon.set_pixel_size(30)
        app_icon.set_halign(Gtk.Align.START)
        app_icon.set_valign(Gtk.Align.START)
        app_icon.set_margin_start(1)
        app_icon.set_margin_top(1)
        icon_overlay.set_child(app_icon)

        badge = Gtk.Box()
        badge.add_css_class("uac-badge")
        badge.set_halign(Gtk.Align.START)
        badge.set_valign(Gtk.Align.START)
        badge.set_margin_start(17)
        badge.set_margin_top(17)
        shield = Gtk.Picture.new_for_filename(str(SHIELD_PATH))
        shield.set_content_fit(Gtk.ContentFit.CONTAIN)
        shield.set_size_request(13, 13)
        badge.append(shield)
        icon_overlay.add_overlay(badge)
        app_row.append(icon_overlay)

        app_label = Gtk.Label(
            label=request.get("app_name") or "Aplicación desconocida"
        )
        app_label.add_css_class("uac-app")
        app_label.set_xalign(0)
        app_label.set_valign(Gtk.Align.CENTER)
        app_label.set_vexpand(False)
        app_label.set_hexpand(True)
        app_label.set_max_width_chars(30)
        app_row.append(app_label)

        publisher_text = request.get("publisher")
        if publisher_text:
            lowered = str(publisher_text).lower()
            if not lowered.startswith(("editor", "verified publisher", "origen")):
                publisher_text = f"Editor: {publisher_text}"
            publisher = Gtk.Label(label=str(publisher_text))
            publisher.add_css_class("uac-publisher")
            publisher.set_xalign(0)
            publisher.set_wrap(True)
            publisher.set_max_width_chars(45)
            body.append(publisher)

        if request.get("password_required"):
            pbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            pbox.add_css_class("uac-password-box")
            user = request.get("username") or "tu cuenta"
            label = Gtk.Label(label=f"Contraseña de {user}")
            label.add_css_class("uac-field-label")
            label.set_xalign(0)
            pbox.append(label)

            self.password_entry = Gtk.PasswordEntry()
            self.password_entry.set_show_peek_icon(True)
            self.password_entry.set_property("placeholder-text", "Contraseña")
            self.password_entry.add_css_class("uac-password")
            self.password_entry.connect("activate", self._accept)
            pbox.append(self.password_entry)
            body.append(pbox)

        details = request.get("details") or []
        if details:
            self.details_button = Gtk.Button(label="Mostrar detalles")
            self.details_button.add_css_class("uac-link")
            self.details_button.set_halign(Gtk.Align.START)
            self.details_button.connect("clicked", self._toggle_details)
            body.append(self.details_button)

            self.details_revealer = Gtk.Revealer()
            self.details_revealer.set_transition_type(
                Gtk.RevealerTransitionType.SLIDE_DOWN
            )
            self.details_revealer.set_transition_duration(140)
            dbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            dbox.add_css_class("uac-details-box")
            for key, value in details:
                self._detail_row(dbox, str(key), str(value))
            self.details_revealer.set_child(dbox)
            body.append(self.details_revealer)

        self.error_label = Gtk.Label()
        self.error_label.add_css_class("uac-error")
        self.error_label.set_xalign(0)
        self.error_label.set_wrap(True)
        self.error_label.set_max_width_chars(45)
        self.error_label.set_visible(False)
        if request.get("error"):
            self.error_label.set_text(str(request["error"]))
            self.error_label.set_visible(True)
        body.append(self.error_label)

        # --- Pie gris con Sí / No ------------------------------------------
        footer = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        footer.add_css_class("uac-footer")
        footer.set_homogeneous(True)
        card.append(footer)

        self.btn_yes = Gtk.Button(label="Sí")
        self.btn_yes.add_css_class("uac-button")
        self.btn_yes.add_css_class("uac-secondary")
        self.btn_yes.connect("clicked", self._accept)
        footer.append(self.btn_yes)

        self.btn_no = Gtk.Button(label="No")
        self.btn_no.add_css_class("uac-button")
        self.btn_no.add_css_class("uac-primary")
        self.btn_no.connect("clicked", self._deny)
        footer.append(self.btn_no)

        # En Windows el botón predeterminado es No (el más seguro).
        self.window.set_default_widget(self.btn_no)
        self.window.connect("close-request", self._on_close)
        self.window.connect("map", self._on_map)
        key_controller = Gtk.EventControllerKey()
        key_controller.connect("key-pressed", self._on_key)
        self.window.add_controller(key_controller)

    # ------------------------------------------------------------------ UI
    def _detect_dark_theme(self) -> bool:
        """Sigue el tema del escritorio: oscuro si GNOME lo pide."""
        try:
            interface = self.Gio.Settings.new("org.gnome.desktop.interface")
            scheme = interface.get_string("color-scheme")
            if scheme == "prefer-dark":
                return True
            if scheme == "prefer-light":
                return False
        except Exception:  # noqa: BLE001 - sin esquema, seguimos con el tema
            pass
        try:
            settings = self.Gtk.Settings.get_default()
            if settings is not None:
                theme = (
                    settings.get_property("gtk-theme-name") or ""
                ).lower()
                if "dark" in theme:
                    return True
                if settings.get_property("gtk-application-prefer-dark-theme"):
                    return True
        except Exception:  # noqa: BLE001 - entorno sin GTK settings
            pass
        return False

    def _app_icon_name(self, app_name: str) -> str:
        Gtk = self.Gtk
        icon = APP_ICONS.get(app_name.lower(), "utilities-terminal")
        display = self.Gdk.Display.get_default()
        if display is not None:
            theme = Gtk.IconTheme.get_for_display(display)
            if not theme.has_icon(icon):
                icon = "utilities-terminal"
        return icon

    def _detail_row(self, parent, key: str, value: str) -> None:
        Gtk = self.Gtk
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        key_label = Gtk.Label(label=key)
        key_label.add_css_class("uac-detail-key")
        key_label.set_size_request(110, -1)
        key_label.set_xalign(0)
        row.append(key_label)

        value_label = Gtk.Label(label=value)
        value_label.add_css_class("uac-detail-value")
        value_label.set_xalign(0)
        value_label.set_wrap(True)
        value_label.set_max_width_chars(32)
        value_label.set_selectable(True)
        value_label.set_hexpand(True)
        row.append(value_label)
        parent.append(row)

    def _toggle_details(self, *_args) -> None:
        if self.details_revealer is None:
            return
        revealed = not self.details_revealer.get_reveal_child()
        self.details_revealer.set_reveal_child(revealed)
        self.details_button.set_label(
            "Ocultar detalles" if revealed else "Mostrar detalles"
        )

    # --------------------------------------------------------- Pantalla
    def _on_map(self, *_args) -> None:
        # La superficie ya existe cuando la ventana se mapea: es el momento
        # fiable para pedirle al compositor que inhiba Super, Alt+Tab, etc.
        if self.fullscreen:
            self.GLib.timeout_add(120, self._inhibit_shortcuts)

    def _inhibit_shortcuts(self) -> bool:
        if self._shortcuts_inhibited:
            return False
        surface = self.window.get_surface()
        if surface is None:
            return False
        try:
            surface.inhibit_system_shortcuts(None)
            self._shortcuts_inhibited = True
        except Exception as exc:  # noqa: BLE001 - no debe impedir el diálogo
            print(
                f"uac: no se pudieron inhibir los atajos del sistema: {exc}",
                file=sys.stderr,
            )
        return False

    def _release_shortcuts(self) -> None:
        if not self._shortcuts_inhibited:
            return
        try:
            surface = self.window.get_surface()
            if surface is not None:
                surface.restore_system_shortcuts()
        except Exception:  # noqa: BLE001 - la superficie puede haber muerto
            pass
        self._shortcuts_inhibited = False

    # ------------------------------------------------------------ Acciones
    def _accept(self, *_args) -> None:
        if self._finished:
            return
        if self.password_entry is not None:
            password = self.password_entry.get_text()
            if not password:
                self.error_label.set_text(
                    "Escribe tu contraseña para continuar."
                )
                self.error_label.set_visible(True)
                self.password_entry.grab_focus()
                return
            self.result["password"] = password
        self._finished = True
        self.result["approved"] = True
        self.window.close()

    def _deny(self, *_args) -> None:
        if self._finished:
            return
        self._finished = True
        self.result["approved"] = False
        self.result["password"] = None
        self.window.close()

    def _on_close(self, *_args):
        # Si se cierra con la X, Alt+F4 o Escape, cuenta como "No".
        self._release_shortcuts()
        if not self._finished:
            self._finished = True
            self.result["approved"] = False
            self.result["password"] = None
        return False

    def _on_key(self, _controller, keyval, _keycode, _state) -> bool:
        if keyval == self.Gdk.KEY_Escape:
            self._deny()
            return True
        return False

    def present(self) -> None:
        # Primero se muestra y después se pide fullscreen: pedirlo antes de
        # mapear dejaba la ventana sin tamaño real y la tarjeta fuera de vista.
        self.window.present()
        if self.fullscreen:
            self.GLib.idle_add(self._enter_fullscreen)
        self.GLib.idle_add(self._focus_default)

    def _enter_fullscreen(self) -> bool:
        self.window.fullscreen()
        return False

    def _focus_default(self) -> bool:
        # En Windows el foco inicial está en "No". Con contraseña, en el campo.
        if self.password_entry is not None:
            self.password_entry.grab_focus()
        else:
            self.btn_no.grab_focus()
        return False


def run(request: dict) -> dict:
    """Muestra el diálogo y devuelve el resultado. Bloquea hasta cerrarlo."""
    import gi

    gi.require_version("Gtk", "4.0")
    from gi.repository import Gio, Gtk

    result: dict = {"approved": False, "password": None}

    app = Gtk.Application(
        application_id=APP_ID, flags=Gio.ApplicationFlags.NON_UNIQUE
    )

    def on_activate(application):
        _load_css()
        dialog = UacDialog(application, request, result)
        dialog.present()
        # Mantiene viva la referencia mientras la ventana exista.
        application._uac_dialog = dialog

    app.connect("activate", on_activate)
    app.run(["uac-dialog"])
    return result


def _load_css() -> None:
    import gi

    gi.require_version("Gdk", "4.0")
    from gi.repository import Gdk, GLib, Gtk

    display = Gdk.Display.get_default()
    if display is None:
        return
    provider = Gtk.CssProvider()
    try:
        provider.load_from_path(str(CSS_PATH))
    except GLib.Error as exc:  # CSS roto: seguimos con el tema del sistema.
        print(f"uac: no se pudo cargar el CSS: {exc}", file=sys.stderr)
        return
    Gtk.StyleContext.add_provider_for_display(
        display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Diálogo UAC para Linux (GTK4)."
    )
    parser.add_argument("--request", help="Fichero JSON con la petición.")
    parser.add_argument("--app", dest="app_name", help="Nombre de la aplicación.")
    parser.add_argument("--publisher", help="Editor o procedencia.")
    parser.add_argument("--message", help="Pregunta principal.")
    parser.add_argument("--command", help="Comando que se va a ejecutar.")
    parser.add_argument("--username", help="Usuario que se autentica.")
    parser.add_argument("--error", help="Mensaje de error a mostrar.")
    parser.add_argument(
        "--detail",
        action="append",
        type=lambda value: value.split("=", 1),
        metavar="CLAVE=VALOR",
        help="Fila de detalles (repetible).",
    )
    parser.add_argument(
        "--password",
        action="store_true",
        help="Pedir contraseña.",
    )
    parser.add_argument(
        "--fullscreen",
        action="store_true",
        help="Ocupa la pantalla entera (experimental; el shell ya no lo necesita).",
    )
    parser.add_argument(
        "--windowed",
        action="store_true",
        help="Compatibilidad: la ventana ya es el modo por defecto.",
    )
    parser.add_argument(
        "--demo",
        action="store_true",
        help="Muestra una petición de ejemplo sin argumentos.",
    )
    args = parser.parse_args(argv)

    if args.demo:
        args.app_name = args.app_name or "apt"
        args.publisher = args.publisher or "Terminal · sesión local"
        args.command = args.command or "sudo apt install htop"
        args.detail = args.detail or [["Usuario", "nixer"]]

    try:
        request = load_request(args)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"uac: petición inválida: {exc}", file=sys.stderr)
        return 2

    restore_session_env(request.get("session"))

    # Sin pantalla no hay diálogo posible.
    if not (
        os.environ.get("WAYLAND_DISPLAY")
        or os.environ.get("DISPLAY")
    ):
        print("uac: no hay servidor gráfico disponible", file=sys.stderr)
        return 2

    try:
        result = run(request)
    except Exception as exc:  # noqa: BLE001 - queremos devolver JSON siempre
        print(f"uac: error al mostrar el diálogo: {exc}", file=sys.stderr)
        return 2

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    return 0 if result.get("approved") else 1


if __name__ == "__main__":
    raise SystemExit(main())
