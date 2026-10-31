/* uac-linux: aviso de consentimiento en la capa del shell.
 *
 * Se dibuja directamente en Main.uiGroup y hace Main.pushModal con
 * Shell.ActionMode.SYSTEM_MODAL, igual que el bloqueo de pantalla o el
 * agente de polkit. Eso captura teclado y ratón y desactiva Super, Alt+Tab
 * y el resto de atajos del sistema mientras el aviso está abierto.
 */

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import Shell from 'gi://Shell';
import Pango from 'gi://Pango';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const BUS_NAME = 'org.nixer.Uac';
const OBJECT_PATH = '/org/nixer/Uac';
const CARD_WIDTH = 456;
const DEFAULT_MESSAGE =
    '¿Quieres permitir que esta aplicación haga cambios en el dispositivo?';

const UAC_XML = `
<node>
  <interface name="org.nixer.Uac">
    <method name="Ping"/>
    <method name="Request">
      <arg type="s" name="request" direction="in"/>
      <arg type="b" name="approved" direction="out"/>
    </method>
  </interface>
</node>`;

const APP_ICONS = {
    'apt': 'system-software-install',
    'apt-get': 'system-software-install',
    'dpkg': 'system-software-install',
    'snap': 'system-software-install',
    'systemctl': 'applications-system',
    'service': 'applications-system',
};

// St.BoxLayout ya no expone "spacing": hay que dárselo al Clutter.BoxLayout.
function box(params, spacing = 0) {
    const widget = new St.BoxLayout(params);
    if (spacing && widget.layout_manager)
        widget.layout_manager.spacing = spacing;
    return widget;
}

function uiGroup() {
    return Main.uiGroup ?? Main.layoutManager.uiGroup ?? global.stage;
}

class UacDialog {
    constructor(request, dir, onFinished) {
        this._request = request;
        this._dir = dir;
        const shieldFile = typeof dir === 'string'
            ? Gio.File.new_for_path(`${dir}/uac-shield.svg`)
            : dir.get_child('uac-shield.svg');
        this._shieldUri = shieldFile.get_uri();
        this._onFinished = onFinished;
        this._finished = false;
        this._grab = null;
        this._stageKeys = 0;
        this._stageSizeId = 0;
        this._monitorsChangedId = 0;
        this._monitor = null;
        this._focusables = [];
        this._focusIndex = 0;
        this._detailsOpen = false;
        this._entry = null;

        this._build();

        // Si el overview estaba abierto, se cierra: el fondo debe ser el
        // escritorio atenuado, como el secure desktop de Windows.
        try {
            if (Main.overview.visible)
                Main.overview.hide();
        } catch (e) { /* sin overview disponible */ }

        const group = Main.layoutManager.modalDialogGroup ?? uiGroup();
        group.add_child(this._root);
        // Ocupa el escenario entero (todos los monitores). El BindConstraint
        // del ModalDialog nativo no siempre estira una raíz propia, así que
        // se dimensiona a mano y se reajusta si cambia la resolución.
        this._resize();
        this._stageSizeId = global.stage.connect('notify::size',
            () => this._resize());
        // Al conectar un monitor, cambiar el principal o variar la
        // disposición, la tarjeta vuelve al centro del monitor principal.
        this._monitorsChangedId = Main.layoutManager.connect(
            'monitors-changed', () => this._resize());

        try {
            this._grab = Main.pushModal(this._root, {
                actionMode: Shell.ActionMode.SYSTEM_MODAL,
            });
            if (this._grab.get_seat_state() !== Clutter.GrabState.ALL) {
                Main.popModal(this._grab);
                this._grab = null;
                log('uac-linux: el compositor no concedió el bloqueo total');
            } else {
                log(`uac-linux: bloqueo concedido (seat=${this._grab.get_seat_state()}, actionMode=${Main.actionMode})`);
                Main.layoutManager.emit('system-modal-opened');
            }
        } catch (e) {
            logError(e, 'uac-linux: no se pudo capturar el escritorio');
            this._grab = null;
        }

        // Se conecta a la raíz, que es el actor del grab: con el modal activo
        // los eventos se entregan ahí, no al escenario.
        this._stageKeys = this._root.connect('key-press-event',
            (_actor, event) => this._onKey(event));

        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            if (!this._finished) {
                const target = this._entry ?? this._focusables[this._focusIndex];
                if (target)
                    target.grab_key_focus();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _monitorRect() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (monitor && monitor.width > 0 && monitor.height > 0)
            return monitor;
        return {
            x: 0,
            y: 0,
            width: global.stage.width || 1280,
            height: global.stage.height || 720,
        };
    }

    _resize() {
        const monitor = this._monitorRect();
        this._monitor = monitor;
        // El fondo ocupa el escenario entero (todos los monitores), pero la
        // tarjeta se centra solo en el monitor principal. Clutter.BinLayout
        // respeta la posición fija de sus hijos, así que basta con anclar
        // _center a la geometría del principal.
        this._root.set_position(0, 0);
        this._root.set_size(
            global.stage.width || 1280,
            global.stage.height || 720);
        this._center.set_position(monitor.x, monitor.y);
        this._center.set_size(monitor.width, monitor.height);
    }

    // ------------------------------------------------------------- interfaz
    _build() {
        const request = this._request;
        const dark = this._isDark();

        this._root = new St.Widget({
            style_class: dark ? 'uac-root uac-dark' : 'uac-root uac-light',
            reactive: true,
            can_focus: true,
            // Sin layout manager la raíz coloca a sus hijos en (0,0) con su
            // tamaño natural e ignora la expansión; BinLayout ocupa todo.
            layout_manager: new Clutter.BinLayout(),
        });

        // Zona de centrado: _resize() la ancla al monitor principal. Sin
        // x_expand/y_expand para que BinLayout use su tamaño fijo, y centra
        // la tarjeta en ambos ejes recolocándola sola si cambia de alto (por
        // ejemplo al abrir "Mostrar detalles").
        this._center = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
            x_align: Clutter.ActorAlign.START,
            y_align: Clutter.ActorAlign.START,
        });
        this._root.add_child(this._center);

        this._card = new St.BoxLayout({
            vertical: true,
            style_class: 'uac-card',
        });
        this._card.set_width(CARD_WIDTH);
        this._center.add_child(this._card);

        this._buildHeader(request);
        this._buildBody(request);
        this._buildFooter();
    }

    _buildHeader(request) {
        const header = new St.BoxLayout({vertical: true, style_class: 'uac-header'});

        const titlebar = new St.BoxLayout({
            vertical: false,
            style_class: 'uac-titlebar',
        });
        // La tarjeta se arrastra agarrándola por la barra de título, como en
        // Windows. Los eventos se capturan en la raíz porque, con el grab
        // modal, Clutter no los entrega a los hijos no interactivos.
        this._titlebar = titlebar;
        this._root.connect('captured-event', (_a, event) =>
            this._onCaptured(event));

        const title = new St.Label({
            text: 'Control de cuentas de usuario',
            style_class: 'uac-title',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });

        const close = new St.Button({
            style_class: 'uac-close',
            can_focus: false,
            child: new St.Icon({
                icon_name: 'window-close-symbolic',
                icon_size: 14,
            }),
        });
        close.set_size(34, 30);
        close.connect('clicked', () => this.finish(false));
        this._closeButton = close;

        titlebar.add_child(title);
        titlebar.add_child(close);
        header.add_child(titlebar);

        const question = new St.Label({
            text: request.message || DEFAULT_MESSAGE,
            style_class: 'uac-question',
        });
        question.x_align = Clutter.ActorAlign.START;
        question.clutter_text.line_wrap = true;
        question.clutter_text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
        question.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
        header.add_child(question);

        this._card.add_child(header);
    }

    _buildBody(request) {
        const body = new St.BoxLayout({vertical: true, style_class: 'uac-body'});

        const appRow = box({
            vertical: false,
            style_class: 'uac-app-row',
        }, 16);

        const iconBox = new St.Widget({style_class: 'uac-icon'});
        iconBox.set_size(32, 32);

        const appIcon = new St.Icon({
            gicon: Gio.Icon.new_for_string(this._iconName(request.app_name)),
            icon_size: 30,
        });
        appIcon.set_position(1, 1);
        iconBox.add_child(appIcon);

        const badge = new St.Bin({style_class: 'uac-badge'});
        badge.set_child(new St.Icon({
            gicon: Gio.Icon.new_for_string(this._shieldUri),
            icon_size: 13,
        }));
        badge.set_position(17, 17);
        iconBox.add_child(badge);

        const appLabel = new St.Label({
            text: request.app_name || 'Aplicación desconocida',
            style_class: 'uac-app',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        appLabel.clutter_text.line_wrap = true;
        appLabel.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;

        appRow.add_child(iconBox);
        appRow.add_child(appLabel);
        body.add_child(appRow);

        if (request.publisher) {
            const publisher = new St.Label({
                text: request.publisher,
                style_class: 'uac-publisher',
            });
            publisher.clutter_text.line_wrap = true;
            publisher.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
            body.add_child(publisher);
        }

        const rows = this._detailRows(request);
        if (rows.length > 0) {
            this._detailsBox = box({
                vertical: true,
                style_class: 'uac-details-box',
                visible: false,
            }, 6);
            for (const [key, value] of rows) {
                const row = box({
                    vertical: false,
                    style_class: 'uac-detail-row',
                }, 10);
                const keyLabel = new St.Label({
                    text: String(key),
                    style_class: 'uac-detail-key',
                });
                keyLabel.set_width(96);
                const valueLabel = new St.Label({
                    text: String(value),
                    style_class: 'uac-detail-value',
                    x_expand: true,
                });
                valueLabel.clutter_text.line_wrap = true;
                valueLabel.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
                row.add_child(keyLabel);
                row.add_child(valueLabel);
                this._detailsBox.add_child(row);
            }

            const link = new St.Button({
                style_class: 'uac-link',
                can_focus: true,
                child: new St.Label({
                    text: 'Mostrar detalles',
                    style_class: 'uac-link-text',
                }),
            });
            link.x_align = Clutter.ActorAlign.START;
            link.connect('clicked', () => this._toggleDetails());
            this._detailsLabel = link.get_child();
            this._detailsButton = link;
            body.add_child(link);
            body.add_child(this._detailsBox);
            this._focusables.push(link);
        }

        if (request.password_required) {
            const label = new St.Label({
                text: 'Contraseña de tu cuenta',
                style_class: 'uac-field-label',
            });
            body.add_child(label);

            this._entry = new St.Entry({
                style_class: 'uac-password',
                hint_text: 'Contraseña',
                can_focus: true,
            });
            this._entry.clutter_text.password_char = '●';
            this._entry.clutter_text.connect('activate', () => this.finish(true));
            body.add_child(this._entry);
            this._focusables.push(this._entry);
        }

        this._card.add_child(body);
    }

    _buildFooter() {
        const footer = box({
            vertical: false,
            style_class: 'uac-footer',
        }, 8);

        this._yes = new St.Button({
            label: 'Sí',
            style_class: 'uac-button uac-secondary',
            can_focus: true,
            x_expand: true,
        });
        this._yes.connect('clicked', () => this.finish(true));

        this._no = new St.Button({
            label: 'No',
            style_class: 'uac-button uac-primary',
            can_focus: true,
            x_expand: true,
        });
        this._no.connect('clicked', () => this.finish(false));

        footer.add_child(this._yes);
        footer.add_child(this._no);
        this._card.add_child(footer);

        this._focusables.push(this._yes, this._no);
        this._focusIndex = this._focusables.length - 1; // "No", como Windows
    }

    _detailRows(request) {
        const rows = [];
        for (const row of request.details ?? []) {
            if (Array.isArray(row) && row.length >= 2)
                rows.push([row[0], row[1]]);
        }
        if (request.command && !rows.some(([key]) => key === 'Comando'))
            rows.push(['Comando', request.command]);
        return rows;
    }

    _iconName(appName) {
        const name = (appName || '').toLowerCase();
        return APP_ICONS[name] ?? 'utilities-terminal';
    }

    _isDark() {
        try {
            const settings = new Gio.Settings({
                schema_id: 'org.gnome.desktop.interface',
            });
            return settings.get_string('color-scheme') === 'prefer-dark';
        } catch (e) {
            return false;
        }
    }

    _toggleDetails() {
        this._detailsOpen = !this._detailsOpen;
        this._detailsBox.visible = this._detailsOpen;
        this._detailsLabel.text = this._detailsOpen
            ? 'Ocultar detalles'
            : 'Mostrar detalles';
    }

    // -------------------------------------------------------------- teclado
    // ------------------------------------------------------------ arrastre
    _onCaptured(event) {
        const type = event.type();
        if (type === Clutter.EventType.BUTTON_PRESS) {
            if (event.get_button() !== 1 || !this._inTitlebar(event))
                return Clutter.EVENT_PROPAGATE;
            // Un clic en la X debe cerrar, no arrastrar.
            const source = event.get_source();
            if (source && this._closeButton.contains(source))
                return Clutter.EVENT_PROPAGATE;
            const [x, y] = event.get_coords();
            this._drag = {
                x,
                y,
                tx: this._card.translation_x,
                ty: this._card.translation_y,
            };
            return Clutter.EVENT_STOP;
        }

        if (type === Clutter.EventType.MOTION) {
            if (!this._drag)
                return Clutter.EVENT_PROPAGATE;
            const [x, y] = event.get_coords();
            this._card.translation_x =
                this._drag.tx + (x - this._drag.x);
            this._card.translation_y =
                this._drag.ty + (y - this._drag.y);
            this._keepOnScreen();
            return Clutter.EVENT_STOP;
        }

        if (type === Clutter.EventType.BUTTON_RELEASE) {
            if (!this._drag)
                return Clutter.EVENT_PROPAGATE;
            this._drag = null;
            return Clutter.EVENT_STOP;
        }

        return Clutter.EVENT_PROPAGATE;
    }

    _inTitlebar(event) {
        if (!this._card || !this._titlebar)
            return false;
        const [x, y] = event.get_coords();
        const monitor = this._monitor ?? this._monitorRect();
        // BinLayout centra la tarjeta en el monitor principal; el arrastre
        // suma su traslación.
        const left = monitor.x +
            (monitor.width - this._card.width) / 2 +
            this._card.translation_x;
        const top = monitor.y +
            (monitor.height - this._card.height) / 2 +
            this._card.translation_y;
        return x >= left && x <= left + this._card.width &&
            y >= top && y <= top + this._titlebar.height;
    }

    // Evita que al arrastrar la tarjeta se salga del monitor principal. Se
    // calcula la posición base (centrada por BinLayout) a mano porque
    // get_transformed_position() devuelve NaN en algunos entornos.
    _keepOnScreen() {
        const monitor = this._monitor ?? this._monitorRect();
        const width = this._card.width;
        const height = this._card.height;
        // Posición base: centro del monitor principal.
        const baseX = monitor.x + (monitor.width - width) / 2;
        const baseY = monitor.y + (monitor.height - height) / 2;

        const minX = monitor.x - baseX;
        const maxX = monitor.x + monitor.width - width - baseX;
        if (maxX >= minX) {
            this._card.translation_x = Math.min(
                maxX, Math.max(minX, this._card.translation_x));
        } else {
            this._card.translation_x = 0;
        }

        const minY = monitor.y - baseY;
        const maxY = monitor.y + monitor.height - height - baseY;
        if (maxY >= minY) {
            this._card.translation_y = Math.min(
                maxY, Math.max(minY, this._card.translation_y));
        } else {
            this._card.translation_y = 0;
        }
    }

    _onKey(event) {
        if (this._finished)
            return Clutter.EVENT_STOP;

        const symbol = event.get_key_symbol();

        if (symbol === Clutter.KEY_Escape) {
            this.finish(false);
            return Clutter.EVENT_STOP;
        }

        if (symbol === Clutter.KEY_Tab || symbol === Clutter.KEY_ISO_Left_Tab) {
            const backwards = symbol === Clutter.KEY_ISO_Left_Tab ||
                Boolean(event.get_state() & Clutter.ModifierType.SHIFT_MASK);
            this._moveFocus(backwards ? -1 : 1);
            return Clutter.EVENT_STOP;
        }

        const inEntry = this._entry &&
            (this._entry.has_key_focus() ||
             this._entry.clutter_text.has_key_focus());
        if (!inEntry && (symbol === Clutter.KEY_Left || symbol === Clutter.KEY_Right)) {
            this._moveFocus(symbol === Clutter.KEY_Left ? -1 : 1);
            return Clutter.EVENT_STOP;
        }

        return Clutter.EVENT_PROPAGATE;
    }

    _moveFocus(delta) {
        const count = this._focusables.length;
        if (count === 0)
            return;
        this._focusIndex = (this._focusIndex + delta + count) % count;
        this._focusables[this._focusIndex].grab_key_focus();
    }

    // --------------------------------------------------------------- cierre
    finish(approved) {
        if (this._finished)
            return;
        this._finished = true;
        const password = this._entry ? this._entry.get_text() : null;
        this.close();
        this._onFinished(approved, password);
    }

    close() {
        this._finished = true;
        if (this._stageKeys) {
            this._root.disconnect(this._stageKeys);
            this._stageKeys = 0;
        }
        if (this._stageSizeId) {
            global.stage.disconnect(this._stageSizeId);
            this._stageSizeId = 0;
        }
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = 0;
        }
        if (this._grab) {
            try {
                Main.popModal(this._grab);
            } catch (e) {
                logError(e, 'uac-linux: no se pudo liberar el modal');
            }
            this._grab = null;
        }
        this._root.destroy();
    }
}

export default class UacExtension extends Extension {
    enable() {
        this._dialog = null;
        this._timeoutId = 0;
        this._exported = Gio.DBusExportedObject.wrapJSObject(UAC_XML, this);
        this._exported.export(Gio.DBus.session, OBJECT_PATH);
        this._ownerId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            BUS_NAME,
            Gio.BusNameOwnerFlags.NONE,
            null, null, null);
    }

    disable() {
        this._clearCardTimeout();
        if (this._dialog) {
            this._dialog.finish(false);
            this._dialog = null;
        }
        if (this._ownerId) {
            Gio.bus_unown_name(this._ownerId);
            this._ownerId = 0;
        }
        if (this._exported) {
            this._exported.unexport();
            this._exported = null;
        }
    }

    Ping() {
    }

    // Quién pide la tarjeta. El puente de PAM la pide desde un proceso con
    // euid 0 (pam_exec con `seteuid`); un proceso del usuario no puede
    // pintarla. Si la consulta falla se deja pasar: es preferible una tarjeta
    // de más que romper el aviso por un fallo del bus.
    _callerUid(invocation) {
        try {
            const sender = invocation.get_sender();
            if (!sender)
                return null;
            const reply = Gio.DBus.session.call_sync(
                'org.freedesktop.DBus',
                '/org/freedesktop/DBus',
                'org.freedesktop.DBus',
                'GetConnectionUnixUser',
                new GLib.Variant('(s)', [sender]),
                new GLib.VariantType('(u)'),
                Gio.DBusCallFlags.NONE,
                1000,
                null);
            const [uid] = reply.deep_unpack();
            return uid;
        } catch (e) {
            logError(e, 'uac-linux: no pude comprobar el uid del solicitante');
            return null;
        }
    }

    _armCardTimeout(request) {
        const seconds = Number(request.timeout);
        if (!Number.isFinite(seconds) || seconds <= 0)
            return;
        this._clearCardTimeout();
        this._timeoutId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT,
            Math.min(seconds, 600),
            () => {
                this._timeoutId = 0;
                if (this._dialog) {
                    log('uac-linux: la tarjeta caducó sin respuesta');
                    this._dialog.finish(false);
                }
                return GLib.SOURCE_REMOVE;
            });
    }

    _clearCardTimeout() {
        if (this._timeoutId) {
            GLib.Source.remove(this._timeoutId);
            this._timeoutId = 0;
        }
    }

    RequestAsync(requestJSON, invocation) {
        let request;
        try {
            request = JSON.parse(requestJSON);
        } catch (e) {
            invocation.return_error_literal(
                Gio.DBusError, Gio.DBusError.INVALID_ARGS, 'JSON inválido');
            return;
        }

        const caller = this._callerUid(invocation);
        if (caller !== null && caller !== 0) {
            log(`uac-linux: aviso rechazado; lo pide el uid ${caller}`);
            invocation.return_error_literal(
                Gio.DBusError, Gio.DBusError.ACCESS_DENIED,
                'Solo el puente de PAM puede mostrar el aviso');
            return;
        }

        if (request.password_required) {
            // Con contraseña manda el diálogo GTK, que sí puede devolverla.
            invocation.return_error_literal(
                Gio.DBusError, Gio.DBusError.NOT_SUPPORTED,
                'El aviso con contraseña no usa la extensión');
            return;
        }

        if (this._dialog) {
            invocation.return_error_literal(
                Gio.DBusError, Gio.DBusError.FAILED,
                'Ya hay un aviso de UAC abierto');
            return;
        }

        this._dialog = new UacDialog(
            request,
            this.dir,
            (approved) => {
                this._clearCardTimeout();
                this._dialog = null;
                try {
                    invocation.return_value(new GLib.Variant('(b)', [approved]));
                } catch (e) {
                    logError(e, 'uac-linux: no se pudo responder por D-Bus');
                }
            });
        this._armCardTimeout(request);
    }
}
