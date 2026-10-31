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

// Geometría de referencia (píxeles lógicos a escala 1). Se multiplica por el
// "texto grande" del escritorio: GNOME Shell ignora text-scaling-factor en sus
// propios widgets, así que la tarjeta lo aplica a mano. El escalado de pantalla
// (125 %, 150 %…) ya lo compone mutter, así que aquí siempre son píxeles
// lógicos y no hay que volver a multiplicar por él.
const INTERFACE_SCHEMA = 'org.gnome.desktop.interface';
const A11Y_SCHEMA = 'org.gnome.desktop.a11y.applications';
const BASE_CARD_WIDTH = 456;
const MAX_CARD_WIDTH = 900;
const MIN_SCALE = 1.0;
const MAX_SCALE = 2.0;
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

        // La escala y el estado de accesibilidad se leen una sola vez, al
        // abrir: la tarjeta vive un minuto como mucho y no tiene sentido
        // rehacer el diseño a mitad.
        this._settings = new Gio.Settings({schema_id: INTERFACE_SCHEMA});
        this._scale = this._readScale();
        this._a11y = this._readA11y();
        this._monitor = this._monitorRect();
        this._pressed = null;

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

    // ------------------------------------------------------------- escalado
    _readScale() {
        try {
            const raw = this._settings.get_double('text-scaling-factor');
            if (Number.isFinite(raw) && raw > 0)
                return Math.min(MAX_SCALE, Math.max(MIN_SCALE, raw));
        } catch (e) { /* el esquema no tiene la clave: 1.0 */ }
        return 1.0;
    }

    _readA11y() {
        try {
            const a11y = new Gio.Settings({schema_id: A11Y_SCHEMA});
            return this._settings.get_boolean('toolkit-accessibility') ||
                a11y.get_boolean('screen-reader-enabled');
        } catch (e) {
            return false;
        }
    }

    // Redondeo a píxeles enteros: los medios píxeles dan bordes borrosos.
    _px(value) {
        return Math.round(value * this._scale);
    }

    _cardWidth() {
        const monitor = this._monitor ?? this._monitorRect();
        const wanted = this._px(BASE_CARD_WIDTH);
        // Con texto al 200 % en un monitor pequeño, que no se salga: deja un
        // 5 % de margen a cada lado.
        const limit = Math.round(monitor.width * 0.9);
        return Math.max(this._px(280), Math.min(wanted, limit, MAX_CARD_WIDTH));
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
        this._card.set_width(this._cardWidth());
        this._center.add_child(this._card);

        this._buildHeader(request);
        this._buildBody(request);
        this._buildFooter();
    }

    _buildHeader(request) {
        const px = value => this._px(value);
        const header = new St.BoxLayout({vertical: true, style_class: 'uac-header'});

        const titlebar = new St.BoxLayout({
            vertical: false,
            style_class: 'uac-titlebar',
        });
        titlebar.set_style(
            `min-height:${px(48)}px; padding:0 ${px(6)}px 0 ${px(24)}px;`);
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
        title.set_style(`font-size:${px(13)}px;`);

        const close = new St.Button({
            style_class: 'uac-close',
            can_focus: false,
            child: new St.Icon({
                icon_name: 'window-close-symbolic',
                icon_size: px(14),
            }),
        });
        close.set_size(px(34), px(30));
        if (this._a11y)
            close.connect('clicked', () => this.finish(false, 'accesibilidad'));
        this._closeButton = close;

        titlebar.add_child(title);
        titlebar.add_child(close);
        header.add_child(titlebar);

        const question = new St.Label({
            text: request.message || DEFAULT_MESSAGE,
            style_class: 'uac-question',
        });
        question.set_style(
            `font-size:${px(17)}px; ` +
            `margin:${px(4)}px ${px(24)}px ${px(22)}px ${px(24)}px;`);
        question.x_align = Clutter.ActorAlign.START;
        question.clutter_text.line_wrap = true;
        question.clutter_text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
        question.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
        header.add_child(question);

        this._card.add_child(header);
    }

    _buildBody(request) {
        const px = value => this._px(value);
        const body = new St.BoxLayout({vertical: true, style_class: 'uac-body'});
        body.set_style(
            `padding:${px(22)}px ${px(24)}px ${px(26)}px ${px(24)}px;`);

        const appRow = box({
            vertical: false,
            style_class: 'uac-app-row',
        }, px(16));

        const iconBox = new St.Widget({style_class: 'uac-icon'});
        iconBox.set_size(px(32), px(32));

        const appIcon = new St.Icon({
            gicon: Gio.Icon.new_for_string(this._iconName(request.app_name)),
            icon_size: px(30),
        });
        appIcon.set_position(1, 1);
        iconBox.add_child(appIcon);

        const badge = new St.Bin({style_class: 'uac-badge'});
        badge.set_child(new St.Icon({
            gicon: Gio.Icon.new_for_string(this._shieldUri),
            icon_size: px(13),
        }));
        badge.set_position(px(17), px(17));
        iconBox.add_child(badge);

        const appLabel = new St.Label({
            text: request.app_name || 'Aplicación desconocida',
            style_class: 'uac-app',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        appLabel.set_style(`font-size:${px(15)}px;`);
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
            publisher.set_style(
                `font-size:${px(13)}px; margin-top:${px(20)}px;`);
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
            }, px(6));
            this._detailsBox.set_style(`margin-top:${px(12)}px;`);
            for (const [key, value] of rows) {
                const row = box({
                    vertical: false,
                    style_class: 'uac-detail-row',
                }, px(10));
                const keyLabel = new St.Label({
                    text: String(key),
                    style_class: 'uac-detail-key',
                });
                keyLabel.set_style(`font-size:${px(13)}px;`);
                keyLabel.set_width(px(96));
                const valueLabel = new St.Label({
                    text: String(value),
                    style_class: 'uac-detail-value',
                    x_expand: true,
                });
                valueLabel.set_style(`font-size:${px(13)}px;`);
                valueLabel.clutter_text.line_wrap = true;
                valueLabel.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
                row.add_child(keyLabel);
                row.add_child(valueLabel);
                this._detailsBox.add_child(row);
            }

            const linkLabel = new St.Label({
                text: 'Mostrar detalles',
                style_class: 'uac-link-text',
            });
            linkLabel.set_style(`font-size:${px(13)}px;`);
            const link = new St.Button({
                style_class: 'uac-link',
                can_focus: true,
                child: linkLabel,
            });
            link.set_style(`margin-top:${px(20)}px;`);
            link.x_align = Clutter.ActorAlign.START;
            // Sin accesibilidad, este enlace solo responde a eventos reales
            // (ver _onCaptured); con ella, también a AT-SPI.
            if (this._a11y)
                link.connect('clicked', () => this._toggleDetails());
            this._detailsLabel = linkLabel;
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
            label.set_style(`font-size:${px(13)}px; margin-top:${px(18)}px;`);
            body.add_child(label);

            this._entry = new St.Entry({
                style_class: 'uac-password',
                hint_text: 'Contraseña',
                can_focus: true,
            });
            this._entry.set_style(
                `min-height:${px(28)}px; padding:0 ${px(8)}px; ` +
                `margin-top:${px(6)}px;`);
            this._entry.clutter_text.password_char = '●';
            this._entry.clutter_text.connect('activate', () => this.finish(true, 'contraseña'));
            body.add_child(this._entry);
            this._focusables.push(this._entry);
        }

        this._card.add_child(body);
    }

    _buildFooter() {
        const px = value => this._px(value);
        const footer = box({
            vertical: false,
            style_class: 'uac-footer',
        }, px(8));
        footer.set_style(`padding:${px(24)}px;`);

        const buttonStyle =
            `min-height:${px(28)}px; padding:0 ${px(12)}px; ` +
            `font-size:${px(13)}px;`;

        this._yes = new St.Button({
            label: 'Sí',
            style_class: 'uac-button uac-secondary',
            can_focus: true,
            x_expand: true,
        });
        this._yes.set_style(buttonStyle);

        this._no = new St.Button({
            label: 'No',
            style_class: 'uac-button uac-primary',
            can_focus: true,
            x_expand: true,
        });
        this._no.set_style(buttonStyle);

        // Sin accesibilidad, los botones **no** escuchan "clicked": física y
        // teclado se resuelven con eventos reales en _onCaptured, y AT-SPI
        // (DoAction) solo puede emitir "clicked", así que no puede pulsar
        // nada. Con accesibilidad activa se acepta a propósito, para no dejar
        // fuera a quien usa lector de pantalla; queda anotado en el journal.
        if (this._a11y) {
            log('uac-linux: accesibilidad activa; la tarjeta acepta AT-SPI');
            this._yes.connect('clicked', () => this.finish(true, 'accesibilidad'));
            this._no.connect('clicked', () => this.finish(false, 'accesibilidad'));
        }

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
                rows.push([String(row[0]).slice(0, 120), String(row[1]).slice(0, 600)]);
        }
        if (request.command && !rows.some(([key]) => key === 'Comando'))
            rows.push(['Comando', String(request.command).slice(0, 600)]);
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

    // -------------------------------------------------------------- entrada
    // Todo pasa por aquí, en la fase de captura. Sin accesibilidad, los botones
    // no escuchan "clicked": AT-SPI (DoAction) solo puede emitir "clicked", así
    // que un programa no puede aprobar por su cuenta. Ratón y toque se
    // resuelven con eventos reales y se deciden al soltar; el teclado, con
    // Return/Espacio sobre el botón que tenga el foco.
    _onCaptured(event) {
        const type = event.type();

        if (type === Clutter.EventType.KEY_PRESS) {
            const symbol = event.get_key_symbol();
            if (symbol === Clutter.KEY_Return ||
                symbol === Clutter.KEY_KP_Enter ||
                symbol === Clutter.KEY_space) {
                const action = this._focusedAction();
                if (action === 'link') {
                    this._toggleDetails();
                    return Clutter.EVENT_STOP;
                }
                if (action) {
                    this._activate(action, 'teclado');
                    return Clutter.EVENT_STOP;
                }
            }
            return Clutter.EVENT_PROPAGATE;
        }

        if (type === Clutter.EventType.BUTTON_PRESS) {
            if (event.get_button() !== 1)
                return Clutter.EVENT_PROPAGATE;
            const source = event.get_source();
            // Un clic en la X cierra, no arrastra.
            if (this._isInside(this._closeButton, source)) {
                this.finish(false, 'botón X');
                return Clutter.EVENT_STOP;
            }
            const inTitlebar = source
                ? this._isInside(this._titlebar, source)
                : this._inTitlebar(event);
            if (inTitlebar) {
                const [x, y] = event.get_coords();
                this._drag = {
                    x,
                    y,
                    tx: this._card.translation_x,
                    ty: this._card.translation_y,
                };
                return Clutter.EVENT_STOP;
            }
            // Se deja pasar para que el botón se vea pulsado; la decisión se
            // toma al soltar, así que un clic que se sale del botón no aprueba.
            this._pressed = this._actionAtEvent(event);
            return Clutter.EVENT_PROPAGATE;
        }

        if (type === Clutter.EventType.BUTTON_RELEASE) {
            if (this._drag) {
                this._drag = null;
                return Clutter.EVENT_STOP;
            }
            const pressed = this._pressed;
            this._pressed = null;
            if (!pressed || this._actionAtEvent(event) !== pressed)
                return Clutter.EVENT_PROPAGATE;
            if (pressed === 'link') {
                this._toggleDetails();
                return Clutter.EVENT_STOP;
            }
            this._activate(pressed, 'botón');
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

        if (type === Clutter.EventType.TOUCH_BEGIN) {
            this._pressed = this._actionAtEvent(event);
            return Clutter.EVENT_PROPAGATE;
        }

        if (type === Clutter.EventType.TOUCH_END) {
            const pressed = this._pressed;
            this._pressed = null;
            if (!pressed || this._actionAtEvent(event) !== pressed)
                return Clutter.EVENT_PROPAGATE;
            if (pressed === 'link') {
                this._toggleDetails();
                return Clutter.EVENT_STOP;
            }
            this._activate(pressed, 'toque');
            return Clutter.EVENT_STOP;
        }

        return Clutter.EVENT_PROPAGATE;
    }

    // ¿El evento nace en este actor o en un hijo suyo? Ojo: contains() solo
    // mira descendientes, así que hay que comparar también el propio actor
    // (las etiquetas de los botones no son reactivas y el origen del clic es
    // el St.Button, no su hijo).
    _isInside(actor, source) {
        if (!actor || !source)
            return false;
        return actor === source || actor.contains(source);
    }

    // Punto (x, y) dentro del actor, en coordenadas de escenario.
    _pointIn(actor, x, y) {
        if (!actor)
            return false;
        try {
            const [ax, ay] = actor.get_transformed_position();
            if (!Number.isFinite(ax) || !Number.isFinite(ay))
                return false;
            return x >= ax && x <= ax + actor.width &&
                y >= ay && y <= ay + actor.height;
        } catch (e) {
            return false;
        }
    }

    // Qué hay debajo del puntero, mirando el actor de origen del evento.
    _actionAt(source) {
        if (this._isInside(this._yes, source))
            return 'yes';
        if (this._isInside(this._no, source))
            return 'no';
        if (this._isInside(this._detailsButton, source))
            return 'link';
        return null;
    }

    // Igual, pero con respaldo por coordenadas: al soltar, el grab del botón
    // puede reasignar el origen del evento a otro actor.
    _actionAtEvent(event) {
        const bySource = this._actionAt(event.get_source());
        if (bySource)
            return bySource;
        const [x, y] = event.get_coords();
        if (this._pointIn(this._yes, x, y))
            return 'yes';
        if (this._pointIn(this._no, x, y))
            return 'no';
        if (this._pointIn(this._detailsButton, x, y))
            return 'link';
        return null;
    }

    _activate(action, via) {
        if (this._finished)
            return;
        if (action === 'yes')
            this.finish(true, via);
        else if (action === 'no')
            this.finish(false, via);
    }

    _focusedAction() {
        if (this._yes && this._yes.has_key_focus())
            return 'yes';
        if (this._no && this._no.has_key_focus())
            return 'no';
        if (this._detailsButton && this._detailsButton.has_key_focus())
            return 'link';
        return null;
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
            this.finish(false, 'Escape');
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
    finish(approved, via = '?') {
        if (this._finished)
            return;
        this._finished = true;
        const password = this._entry ? this._entry.get_text() : null;
        // Queda en el journal del shell junto a la línea del puente, para
        // poder distinguir un «No» de un cierre por caducidad o por la X.
        log(`uac-linux: ${approved ? 'aprobado' : 'denegado'} (vía ${via})`);
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
            this._dialog.finish(false, 'extensión desactivada');
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
    // pintarla. Si no se puede averiguar el uid se rechaza: el bus siempre
    // responde, así que un fallo aquí solo puede significar que algo va mal.
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
                    this._dialog.finish(false, 'caducó');
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
        if (caller !== 0) {
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
