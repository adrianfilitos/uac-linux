/* uac-card: tarjeta de consentimiento de uac-linux para KDE Plasma.
 *
 * En GNOME la tarjeta la pinta la extensión dentro de gnome-shell. En Plasma
 * no hay una capa equivalente accesible desde un script, así que este binario
 * hace lo mismo que un bloqueador de pantalla:
 *
 *   - es dueño de org.nixer.Uac y responde Request(s) -> b;
 *   - abre UNA ventana override-redirect que cubre el escritorio virtual
 *     entero (todos los monitores), con el fondo atenuado;
 *   - hace XGrabKeyboard y XGrabPointer sobre esa ventana, de modo que ni
 *     KWin ni ninguna otra ventana reciben teclas ni clics mientras decide;
 *   - pinta la tarjeta de 456 px centrada en el monitor principal.
 *
 * El binario lo instala install-root.sh en /usr/local/libexec/uac-linux y
 * queda como root, así que el puente de PAM puede comprobar que el servicio
 * D-Bus lo sirve un ejecutable de confianza y no un programa del usuario.
 *
 * Solo entrada real: no se conecta ninguna señal "clicked" ni se usan
 * GtkButton. Los botones se resuelven con gestos (soltar dentro del mismo
 * botón) y con Return/Espacio sobre el que tenga el foco, igual que la
 * extensión de GNOME. AT-SPI no puede fabricar gestos, así que un programa
 * del usuario no puede aprobar por su cuenta.
 */

#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/XKBlib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUS_NAME "org.nixer.Uac"
#define OBJECT_PATH "/org/nixer/Uac"

#define BASE_CARD_WIDTH 456
#define MAX_CARD_WIDTH 900
#define MIN_SCALE 1.0
#define MAX_SCALE 2.0
#define DEFAULT_MESSAGE \
    "¿Quieres permitir que esta aplicación haga cambios en el dispositivo?"
#define DEFAULT_TIMEOUT 60

/* Un clic que ya venía de antes (botón pulsado cuando la ventana aparece) no
 * debe decidir nada: se ignoran los eventos del primer cuarto de segundo. */
#define INPUT_GRACE_US 300000

enum {
    ACTION_NONE = 0,
    ACTION_YES,
    ACTION_NO,
    ACTION_LINK,
};

typedef struct {
    char *app_name;
    char *publisher;
    char *message;
    char *command;
    GPtrArray *details; /* GPtrArray de gchar*[2] */
    gboolean password_required;
    int timeout;
} Request;

typedef struct {
    GtkWidget *window;
    GtkWidget *stage;
    GtkWidget *card;
    GtkWidget *titlebar;
    GtkWidget *close;
    GtkWidget *yes;
    GtkWidget *no;
    GtkWidget *link;
    GtkWidget *link_label;
    GtkWidget *details;
    GtkWidget *app_label;

    GtkWidget *focusables[3];
    int focus_count;
    int focus_index;

    gboolean details_open;
    gboolean dark;
    double scale;
    int card_width;

    /* Geometría: rectángulo del monitor principal dentro del escenario y
     * rectángulo del escenario dentro de la raíz X. */
    int mon_x, mon_y, mon_w, mon_h;
    int bbox_x, bbox_y, bbox_w, bbox_h;
    int card_x, card_y;

    gboolean dragging;
    double drag_x, drag_y;
    int drag_cx, drag_cy;

    int pressed;
    gboolean finished;
    gint64 shown_at;
    gboolean approved;
    const char *via;
    guint timeout_id;

    GMainLoop *loop;
    GMainLoop *main_loop;

    GDBusConnection *bus;
    gboolean card_open;
    GDBusMethodInvocation *pending;

    Display *xdisplay;
    Display *xdisplay2;
    guint xi_source;
    Window xid;
    gboolean grabbed;
    gboolean x11;
    gboolean xi_grab;
    int xi_opcode;
    int xi_devices[16];
    int xi_count;
    gboolean xi_key;
    gboolean xi_pointer;
} Card;

static Card *G = NULL;

static void set_shortcuts_blocked(gboolean blocked);

/* --------------------------------------------------------------- utilidades */

static GtkWidget *make_box(GtkOrientation orientation, int spacing)
{
    GtkWidget *box = gtk_box_new(orientation, spacing);
    return box;
}

static void set_margins(GtkWidget *widget, int top, int right, int bottom,
                        int left)
{
    gtk_widget_set_margin_top(widget, top);
    gtk_widget_set_margin_end(widget, right);
    gtk_widget_set_margin_bottom(widget, bottom);
    gtk_widget_set_margin_start(widget, left);
}

static GtkWidget *make_label(const char *text, const char *css_class,
                             double font_px, gboolean wrap)
{
    GtkWidget *label = gtk_label_new(text ? text : "");
    gtk_widget_add_css_class(label, css_class);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(label), wrap);
    if (wrap)
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);

    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs,
                           pango_attr_size_new((int)(font_px * PANGO_SCALE)));
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
    return label;
}

static int px(double value)
{
    return (int)lround(value * (G ? G->scale : 1.0));
}

/* --------------------------------------------------------------- tema/ajustes */

/* Lee ~/.config/kdeglobals. Plasma no expone el esquema de colores por
 * GSettings, así que se mira el color de fondo de las ventanas: si es oscuro,
 * la tarjeta usa la paleta oscura de Windows 11. */
static void read_theme(gboolean *dark, double *scale)
{
    *dark = FALSE;
    *scale = 1.0;

    char *path = g_build_filename(g_get_user_config_dir(), "kdeglobals", NULL);
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &error)) {
        gchar *bg = g_key_file_get_string(keyfile, "Colors:Window",
                                          "BackgroundNormal", NULL);
        if (bg) {
            int r = 0, g = 0, b = 0;
            if (sscanf(bg, "%d,%d,%d", &r, &g, &b) == 3) {
                double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                *dark = luminance < 128.0;
            }
            g_free(bg);
        }

        gchar *font = g_key_file_get_string(keyfile, "General", "activeFont",
                                            NULL);
        if (font) {
            /* Formato: "Segoe UI Variable,10,-1,5,50,0,..." */
            gchar **parts = g_strsplit(font, ",", -1);
            if (parts[0] && parts[1]) {
                double points = g_ascii_strtod(parts[1], NULL);
                if (points > 0)
                    *scale = points / 10.0;
            }
            g_strfreev(parts);
            g_free(font);
        }
    }

    if (error)
        g_error_free(error);
    g_key_file_unref(keyfile);
    g_free(path);

    if (*scale < MIN_SCALE)
        *scale = MIN_SCALE;
    if (*scale > MAX_SCALE)
        *scale = MAX_SCALE;
}

/* ------------------------------------------------------------------ CSS */

static const char *CARD_CSS =
    ".uac-root { background-color: rgba(0,0,0,0.58); }\n"
    ".uac-card { background-color:#ffffff; border:1px solid #e0e0e0;"
    " border-radius:8px; box-shadow:0 10px 40px rgba(0,0,0,0.45); }\n"
    ".uac-header { background-color:#e7e7e7;"
    " border-radius:8px 8px 0 0; }\n"
    ".uac-title { color:#333333; }\n"
    ".uac-close { border-radius:2px; }\n"
    ".uac-close:hover { background-color:#c42b1c; }\n"
    ".uac-close:hover .uac-close-icon { color:#ffffff; }\n"
    ".uac-close-icon { color:#333333; }\n"
    ".uac-question { color:#1a1a1a; }\n"
    ".uac-body { background-color:#ffffff; }\n"
    ".uac-app { color:#1a1a1a; }\n"
    ".uac-publisher { color:#5f5f5f; }\n"
    ".uac-link { color:#0067c0; }\n"
    ".uac-link:hover { color:#005a9e; }\n"
    ".uac-detail-key { color:#5f5f5f; }\n"
    ".uac-detail-value { color:#1a1a1a; }\n"
    ".uac-footer { background-color:#f3f3f3;"
    " border-radius:0 0 8px 8px; }\n"
    ".uac-button { border-radius:3px; border:2px solid transparent; }\n"
    ".uac-secondary { background-color:#fbfbfb; color:#1a1a1a;"
    " border-color:#cfcfcf; }\n"
    ".uac-secondary:hover { background-color:#f0f0f0; }\n"
    ".uac-secondary.uac-focused { border-color:#000000; }\n"
    ".uac-primary { background-color:#0067c0; color:#ffffff;"
    " border-color:#0067c0; }\n"
    ".uac-primary:hover { background-color:#1975c5;"
    " border-color:#1975c5; }\n"
    ".uac-primary.uac-focused { border-color:#000000; }\n"
    ".uac-badge { background-color:#ffffff; border-radius:3px; }\n"
    ".uac-dark .uac-card { background-color:#202020;"
    " border-color:#3a3a3a; }\n"
    ".uac-dark .uac-header { background-color:#2b2b2b; }\n"
    ".uac-dark .uac-title { color:#cfcfcf; }\n"
    ".uac-dark .uac-close-icon { color:#cfcfcf; }\n"
    ".uac-dark .uac-close:hover { background-color:#c42b1c; }\n"
    ".uac-dark .uac-close:hover .uac-close-icon { color:#ffffff; }\n"
    ".uac-dark .uac-question { color:#ffffff; }\n"
    ".uac-dark .uac-body { background-color:#202020; }\n"
    ".uac-dark .uac-app { color:#ffffff; }\n"
    ".uac-dark .uac-publisher { color:#b0b0b0; }\n"
    ".uac-dark .uac-link { color:#4cc2ff; }\n"
    ".uac-dark .uac-link:hover { color:#6fd0ff; }\n"
    ".uac-dark .uac-detail-key { color:#b0b0b0; }\n"
    ".uac-dark .uac-detail-value { color:#ffffff; }\n"
    ".uac-dark .uac-footer { background-color:#2b2b2b; }\n"
    ".uac-dark .uac-secondary { background-color:#2d2d2d; color:#ffffff;"
    " border-color:#5a5a5a; }\n"
    ".uac-dark .uac-secondary:hover { background-color:#383838; }\n"
    ".uac-dark .uac-secondary.uac-focused { border-color:#ffffff; }\n"
    ".uac-dark .uac-primary { background-color:#0067c0; color:#ffffff;"
    " border-color:#0067c0; }\n"
    ".uac-dark .uac-primary:hover { background-color:#1975c5;"
    " border-color:#1975c5; }\n"
    ".uac-dark .uac-primary.uac-focused { border-color:#ffffff; }\n"
    ".uac-dark .uac-badge { background-color:#202020; }\n";

/* --------------------------------------------------------------- iconos */

static const char *icon_for(const char *app_name)
{
    static const struct {
        const char *app;
        const char *icon;
    } map[] = {
        {"apt", "system-software-install"},
        {"apt-get", "system-software-install"},
        {"dpkg", "system-software-install"},
        {"snap", "system-software-install"},
        {"systemctl", "applications-system"},
        {"service", "applications-system"},
    };

    if (!app_name)
        return "utilities-terminal";
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++) {
        if (g_ascii_strcasecmp(app_name, map[i].app) == 0)
            return map[i].icon;
    }
    return "utilities-terminal";
}

static char *shield_path(void)
{
    /* El SVG se instala junto al binario. */
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return NULL;
    exe[len] = '\0';
    char *dir = g_path_get_dirname(exe);
    char *path = g_build_filename(dir, "uac-shield.svg", NULL);
    g_free(dir);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        return NULL;
    }
    return path;
}

/* --------------------------------------------------------------- disposición */

static int card_width(void)
{
    int wanted = px(BASE_CARD_WIDTH);
    int limit = (int)lround(G->mon_w * 0.9);
    int width = MIN(wanted, limit);
    width = MIN(width, MAX_CARD_WIDTH);
    return MAX(px(280), width);
}

static void place_card(void)
{
    if (!G->card)
        return;

    int width = G->card_width;
    int minimum = 0, natural = 0;
    gtk_widget_measure(G->card, GTK_ORIENTATION_VERTICAL, width, &minimum,
                       &natural, NULL, NULL);
    int height = MAX(minimum, natural);

    G->card_x = G->mon_x + (G->mon_w - width) / 2;
    G->card_y = G->mon_y + (G->mon_h - height) / 2;
    gtk_fixed_move(GTK_FIXED(G->stage), G->card, G->card_x, G->card_y);
}

static void clamp_card(void)
{
    int width = G->card_width;
    int minimum = 0, natural = 0;
    gtk_widget_measure(G->card, GTK_ORIENTATION_VERTICAL, width, &minimum,
                       &natural, NULL, NULL);
    int height = MAX(minimum, natural);

    int min_x = G->mon_x;
    int max_x = G->mon_x + G->mon_w - width;
    int min_y = G->mon_y;
    int max_y = G->mon_y + G->mon_h - height;

    if (max_x < min_x)
        G->card_x = min_x;
    else
        G->card_x = CLAMP(G->card_x, min_x, max_x);
    if (max_y < min_y)
        G->card_y = min_y;
    else
        G->card_y = CLAMP(G->card_y, min_y, max_y);

    gtk_fixed_move(GTK_FIXED(G->stage), G->card, G->card_x, G->card_y);
}

/* Punto (x, y) del escenario dentro de un widget. */
static gboolean point_in(GtkWidget *widget, double x, double y)
{
    graphene_rect_t rect;
    if (!widget)
        return FALSE;
    if (!gtk_widget_compute_bounds(widget, G->stage, &rect))
        return FALSE;
    return x >= rect.origin.x && x <= rect.origin.x + rect.size.width &&
           y >= rect.origin.y && y <= rect.origin.y + rect.size.height;
}

static int action_at(double x, double y)
{
    if (point_in(G->yes, x, y))
        return ACTION_YES;
    if (point_in(G->no, x, y))
        return ACTION_NO;
    if (G->link && point_in(G->link, x, y))
        return ACTION_LINK;
    return ACTION_NONE;
}

/* ------------------------------------------------------------------- foco */

static GtkWidget *focused_widget(void)
{
    if (G->focus_count == 0 || G->focus_index < 0)
        return NULL;
    return G->focusables[G->focus_index];
}

static void refresh_focus(void)
{
    for (int i = 0; i < G->focus_count; i++)
        gtk_widget_remove_css_class(G->focusables[i], "uac-focused");
    GtkWidget *current = focused_widget();
    if (current)
        gtk_widget_add_css_class(current, "uac-focused");
}

static void move_focus(int delta)
{
    if (G->focus_count == 0)
        return;
    G->focus_index =
        (G->focus_index + delta + G->focus_count) % G->focus_count;
    refresh_focus();
}

/* -------------------------------------------------------------- interfaz */

static void add_detail_row(GtkWidget *box, const char *key, const char *value)
{
    GtkWidget *row = make_box(GTK_ORIENTATION_HORIZONTAL, px(10));
    GtkWidget *key_label = make_label(key, "uac-detail-key", px(13), TRUE);
    gtk_widget_set_size_request(key_label, px(96), -1);
    gtk_label_set_xalign(GTK_LABEL(key_label), 0.0);
    GtkWidget *value_label =
        make_label(value, "uac-detail-value", px(13), TRUE);
    gtk_widget_set_hexpand(value_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(value_label), 0.0);

    gtk_box_append(GTK_BOX(row), key_label);
    gtk_box_append(GTK_BOX(row), value_label);
    gtk_box_append(GTK_BOX(box), row);
}

static void build_header(Request *request)
{
    GtkWidget *header = make_box(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(header, "uac-header");

    GtkWidget *titlebar = make_box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(titlebar, -1, px(48));
    set_margins(titlebar, 0, px(6), 0, px(24));
    G->titlebar = titlebar;

    GtkWidget *title =
        make_label("Control de cuentas de usuario", "uac-title", px(13),
                   FALSE);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

    GtkWidget *close = make_box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(close, "uac-close");
    gtk_widget_set_size_request(close, px(34), px(30));
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    GtkWidget *close_icon =
        gtk_image_new_from_icon_name("window-close-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(close_icon), px(14));
    gtk_widget_add_css_class(close_icon, "uac-close-icon");
    gtk_widget_set_halign(close_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(close_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(close_icon, TRUE);
    gtk_box_append(GTK_BOX(close), close_icon);
    G->close = close;

    gtk_box_append(GTK_BOX(titlebar), title);
    gtk_box_append(GTK_BOX(titlebar), close);
    gtk_box_append(GTK_BOX(header), titlebar);

    const char *message =
        (request && request->message && *request->message) ? request->message
                                                           : DEFAULT_MESSAGE;
    GtkWidget *question = make_label(message, "uac-question", px(17), TRUE);
    gtk_label_set_xalign(GTK_LABEL(question), 0.0);
    set_margins(question, px(4), px(24), px(22), px(24));
    gtk_box_append(GTK_BOX(header), question);

    gtk_box_append(GTK_BOX(G->card), header);
}

static void build_body(Request *request)
{
    GtkWidget *body = make_box(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(body, "uac-body");
    set_margins(body, px(22), px(24), px(26), px(24));

    GtkWidget *app_row = make_box(GTK_ORIENTATION_HORIZONTAL, px(16));

    /* Icono de la aplicación con el escudo superpuesto. */
    GtkWidget *icon_area = gtk_fixed_new();
    gtk_widget_set_size_request(icon_area, px(32), px(32));

    GtkWidget *app_icon = gtk_image_new_from_icon_name(
        icon_for(request ? request->app_name : NULL));
    gtk_image_set_pixel_size(GTK_IMAGE(app_icon), px(30));
    gtk_fixed_put(GTK_FIXED(icon_area), app_icon, px(1), px(1));

    char *svg = shield_path();
    if (svg) {
        GtkWidget *badge = make_box(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(badge, "uac-badge");
        GtkWidget *shield = gtk_image_new_from_file(svg);
        gtk_image_set_pixel_size(GTK_IMAGE(shield), px(13));
        gtk_box_append(GTK_BOX(badge), shield);
        gtk_fixed_put(GTK_FIXED(icon_area), badge, px(17), px(17));
        g_free(svg);
    }

    GtkWidget *app_label = make_label(
        (request && request->app_name && *request->app_name)
            ? request->app_name
            : "Aplicación desconocida",
        "uac-app", px(15), TRUE);
    gtk_widget_set_hexpand(app_label, TRUE);
    gtk_widget_set_valign(app_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(app_label), 0.0);
    G->app_label = app_label;

    gtk_box_append(GTK_BOX(app_row), icon_area);
    gtk_box_append(GTK_BOX(app_row), app_label);
    gtk_box_append(GTK_BOX(body), app_row);

    if (request && request->publisher && *request->publisher) {
        GtkWidget *publisher = make_label(request->publisher, "uac-publisher",
                                          px(13), TRUE);
        set_margins(publisher, px(20), 0, 0, 0);
        gtk_box_append(GTK_BOX(body), publisher);
    }

    /* Detalles + enlace para mostrarlos. */
    gboolean has_rows = FALSE;
    GtkWidget *details = make_box(GTK_ORIENTATION_VERTICAL, px(6));
    gtk_widget_add_css_class(details, "uac-details");
    set_margins(details, px(12), 0, 0, 0);
    gtk_widget_set_visible(details, FALSE);

    if (request && request->details) {
        for (guint i = 0; i < request->details->len; i++) {
            gchar **pair = g_ptr_array_index(request->details, i);
            add_detail_row(details, pair[0], pair[1]);
            has_rows = TRUE;
        }
    }
    if (request && request->command && *request->command) {
        add_detail_row(details, "Comando", request->command);
        has_rows = TRUE;
    }

    if (has_rows) {
        GtkWidget *link_label =
            make_label("Mostrar detalles", "uac-link", px(13), FALSE);
        gtk_label_set_xalign(GTK_LABEL(link_label), 0.0);
        GtkWidget *link = make_box(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(link, "uac-link");
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        set_margins(link, px(20), 0, 0, 0);
        gtk_box_append(GTK_BOX(link), link_label);
        G->link = link;
        G->link_label = link_label;
        gtk_box_append(GTK_BOX(body), link);
        gtk_box_append(GTK_BOX(body), details);
        G->details = details;
    }

    gtk_box_append(GTK_BOX(G->card), body);
}

static GtkWidget *make_button(const char *text, const char *css_class)
{
    GtkWidget *button = make_box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(button, "uac-button");
    gtk_widget_add_css_class(button, css_class);
    gtk_widget_set_size_request(button, -1, px(28));
    gtk_widget_set_hexpand(button, TRUE);

    GtkWidget *label = make_label(text, "uac-button-text", px(13), FALSE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(button), label);
    return button;
}

static void build_footer(void)
{
    GtkWidget *footer = make_box(GTK_ORIENTATION_HORIZONTAL, px(8));
    gtk_widget_add_css_class(footer, "uac-footer");
    set_margins(footer, px(24), px(24), px(24), px(24));

    GtkWidget *yes = make_button("Sí", "uac-secondary");
    GtkWidget *no = make_button("No", "uac-primary");
    gtk_box_append(GTK_BOX(footer), yes);
    gtk_box_append(GTK_BOX(footer), no);
    gtk_box_append(GTK_BOX(G->card), footer);

    G->yes = yes;
    G->no = no;
    G->focusables[0] = yes;
    G->focusables[1] = no;
    G->focus_count = 2;
    if (G->link) {
        G->focusables[2] = G->link;
        G->focus_count = 3;
        G->focus_index = 2; /* "No" primero, como Windows */
    } else {
        G->focus_index = 1;
    }
    refresh_focus();
}

static void set_card_classes(void)
{
    if (G->dark)
        gtk_widget_add_css_class(G->window, "uac-dark");
}

/* --------------------------------------------------------------- entrada */

static void finish(gboolean approved, const char *via);

static void toggle_details(void)
{
    G->details_open = !G->details_open;
    gtk_widget_set_visible(G->details, G->details_open);
    gtk_label_set_text(GTK_LABEL(G->link_label),
                       G->details_open ? "Ocultar detalles"
                                       : "Mostrar detalles");
    place_card();
}

static void activate(int action, const char *via)
{
    if (G->finished)
        return;
    if (action == ACTION_YES)
        finish(TRUE, via);
    else if (action == ACTION_NO)
        finish(FALSE, via);
    else if (action == ACTION_LINK)
        toggle_details();
}

static void handle_press(double x, double y)
{
    if (G->finished)
        return;
    if (g_get_monotonic_time() - G->shown_at < INPUT_GRACE_US)
        return;
    if (point_in(G->close, x, y)) {
        finish(FALSE, "botón X");
        return;
    }
    if (point_in(G->titlebar, x, y) && point_in(G->card, x, y)) {
        G->dragging = TRUE;
        G->drag_x = x;
        G->drag_y = y;
        G->drag_cx = G->card_x;
        G->drag_cy = G->card_y;
        return;
    }
    G->pressed = action_at(x, y);
}


static void handle_press(double x, double y);
static void handle_release(double x, double y);
static void handle_motion(double x, double y);
static gboolean handle_keyval(guint keyval, guint state);

static void on_pressed(GtkGestureClick *gesture, int n_press, double x,
                       double y, gpointer user_data)
{
    (void)gesture; (void)n_press; (void)user_data;
    handle_press(x, y);
}

static void on_released(GtkGestureClick *gesture, int n_press, double x,
                        double y, gpointer user_data)
{
    (void)gesture; (void)n_press; (void)user_data;
    handle_release(x, y);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y,
                      gpointer user_data)
{
    (void)controller; (void)user_data;
    handle_motion(x, y);
}

static void handle_release(double x, double y)
{
    if (G->finished)
        return;
    if (G->dragging) {
        G->dragging = FALSE;
        return;
    }
    int pressed = G->pressed;
    G->pressed = ACTION_NONE;
    if (pressed == ACTION_NONE || action_at(x, y) != pressed)
        return;
    activate(pressed, "botón");
}

static void handle_motion(double x, double y)
{
    if (!G->dragging || G->finished)
        return;
    G->card_x = G->drag_cx + (int)lround(x - G->drag_x);
    G->card_y = G->drag_cy + (int)lround(y - G->drag_y);
    clamp_card();
}

static int focused_action(void)
{
    GtkWidget *current = focused_widget();
    if (!current)
        return ACTION_NONE;
    if (current == G->yes)
        return ACTION_YES;
    if (current == G->no)
        return ACTION_NO;
    if (current == G->link)
        return ACTION_LINK;
    return ACTION_NONE;
}

static gboolean handle_keyval(guint keyval, guint state)
{
    if (G->finished)
        return TRUE;
    if (g_get_monotonic_time() - G->shown_at < INPUT_GRACE_US)
        return TRUE;

    if (keyval == GDK_KEY_Escape) {
        finish(FALSE, "Escape");
        return TRUE;
    }

    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    if (keyval == GDK_KEY_Tab) {
        move_focus(shift ? -1 : 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_ISO_Left_Tab) {
        move_focus(-1);
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
        keyval == GDK_KEY_space) {
        int action = focused_action();
        if (action != ACTION_NONE) {
            if (action == ACTION_LINK)
                toggle_details();
            else
                activate(action, "teclado");
            return TRUE;
        }
        return FALSE;
    }

    if (keyval == GDK_KEY_Left) {
        move_focus(-1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Right) {
        move_focus(1);
        return TRUE;
    }

    return FALSE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)user_data;
    return handle_keyval(keyval, (guint)state);
}

/* ------------------------------------------------------------------ cierre */

static void ungrab_xi2(void);

static void release_grab(void)
{
    if (G->xi_source) {
        g_source_remove(G->xi_source);
        G->xi_source = 0;
    }
    if (G->x11 && G->xdisplay2 && G->grabbed) {
        ungrab_xi2();
        XFlush(G->xdisplay2);
        G->grabbed = FALSE;
    }
}

static void finish(gboolean approved, const char *via)
{
    if (G->finished)
        return;
    G->finished = TRUE;
    G->approved = approved;
    G->via = via;

    if (G->timeout_id) {
        g_source_remove(G->timeout_id);
        G->timeout_id = 0;
    }

    release_grab();
    set_shortcuts_blocked(FALSE);

    g_printerr("uac-card: %s (vía %s)\n",
               approved ? "aprobado" : "denegado", via);

    if (G->window) {
        gtk_window_destroy(GTK_WINDOW(G->window));
        G->window = NULL;
        G->stage = NULL;
        G->card = NULL;
    }

    if (G->loop)
        g_main_loop_quit(G->loop);

    if (G->pending) {
        g_dbus_method_invocation_return_value(G->pending,
                                              g_variant_new("(b)", approved));
        G->pending = NULL;
    }
    G->card_open = FALSE;
}

static gboolean on_timeout(gpointer user_data)
{
    (void)user_data;
    G->timeout_id = 0;
    if (G->stage)
        finish(FALSE, "caducó");
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------- X11 */

/* KWin registra sus atajos globales por XInput2, así que un XGrabKeyboard
 * clásico no basta: Super y Alt+Tab seguirían llegando al escritorio. Se hace
 * un grab XI2 (además del core) sobre la ventana. Como GTK4 también habla XI2,
 * los eventos llegan a la misma conexión y la tarjeta los procesa normal.
 *
 * El grab de puntero se hace solo por XI2: con un XGrabPointer core, GTK
 * dejaba de recibir los clics. */
static int ignore_x_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    return 0;
}

static gboolean grab_xi2(Window window)
{
    if (G->xi_opcode < 0 || window == None)
        return FALSE;

    int device_count = 0;
    XIDeviceInfo *devices =
        XIQueryDevice(G->xdisplay2, XIAllDevices, &device_count);
    if (!devices)
        return FALSE;

    XErrorHandler previous = XSetErrorHandler(ignore_x_error);
    G->xi_count = 0;
    G->xi_key = FALSE;
    G->xi_pointer = FALSE;

    for (int i = 0; i < device_count; i++) {
        XIDeviceInfo *device = &devices[i];
        gboolean keyboard = device->use == XIMasterKeyboard;
        gboolean pointer = device->use == XIMasterPointer;
        if (!keyboard && !pointer)
            continue;

        unsigned char mask[XIMaskLen(XI_LASTEVENT)];
        memset(mask, 0, sizeof(mask));
        if (keyboard) {
            XISetMask(mask, XI_KeyPress);
            XISetMask(mask, XI_KeyRelease);
        }
        if (pointer) {
            XISetMask(mask, XI_ButtonPress);
            XISetMask(mask, XI_ButtonRelease);
            XISetMask(mask, XI_Motion);
        }

        XIEventMask event_mask;
        event_mask.deviceid = device->deviceid;
        event_mask.mask_len = sizeof(mask);
        event_mask.mask = mask;

        Status status = XIGrabDevice(
            G->xdisplay, device->deviceid, window, CurrentTime, None,
            XIGrabModeAsync, XIGrabModeAsync, True, &event_mask);
        if (status == GrabSuccess && G->xi_count < 16) {
            G->xi_devices[G->xi_count++] = device->deviceid;
            if (keyboard)
                G->xi_key = TRUE;
            if (pointer)
                G->xi_pointer = TRUE;
        }
    }

    XSync(G->xdisplay2, False);
    XSetErrorHandler(previous);
    XIFreeDeviceInfo(devices);

    G->xi_grab = G->xi_count > 0;
    return G->xi_key && G->xi_pointer;
}

static void ungrab_xi2(void)
{
    if (!G->x11 || !G->xdisplay || !G->xi_grab)
        return;
    XErrorHandler previous = XSetErrorHandler(ignore_x_error);
    for (int i = 0; i < G->xi_count; i++)
        XIUngrabDevice(G->xdisplay2, G->xi_devices[i], CurrentTime);
    XSync(G->xdisplay2, False);
    XSetErrorHandler(previous);
    G->xi_count = 0;
    G->xi_grab = FALSE;
}


static gboolean xi_dispatch(GIOChannel *channel, GIOCondition condition,
                            gpointer data)
{
    (void)channel;
    (void)condition;
    (void)data;
    if (!G->xdisplay2)
        return G_SOURCE_REMOVE;

    while (XPending(G->xdisplay2)) {
        XEvent event;
        XNextEvent(G->xdisplay2, &event);
        if (event.type != GenericEvent)
            continue;
        XGenericEventCookie *cookie = &event.xcookie;
        if (!XGetEventData(G->xdisplay2, cookie))
            continue;
        if (cookie->extension != G->xi_opcode) {
            XFreeEventData(G->xdisplay2, cookie);
            continue;
        }

        XIDeviceEvent *device = cookie->data;
        switch (cookie->evtype) {
        case XI_KeyPress: {
            unsigned state = (unsigned)device->mods.effective;
            int level = (state & ShiftMask) ? 1 : 0;
            KeySym symbol = XkbKeycodeToKeysym(
                G->xdisplay2, (KeyCode)device->detail, 0, level);
            if (symbol != NoSymbol)
                handle_keyval((guint)symbol, state);
            break;
        }
        case XI_ButtonPress:
            if (device->detail == 1)
                handle_press(device->event_x, device->event_y);
            break;
        case XI_ButtonRelease:
            if (device->detail == 1)
                handle_release(device->event_x, device->event_y);
            break;
        case XI_Motion:
            handle_motion(device->event_x, device->event_y);
            break;
        default:
            break;
        }
        XFreeEventData(G->xdisplay2, cookie);
    }
    return G_SOURCE_CONTINUE;
}

static void xi_start(Window window)
{
    if (G->xi_source || !G->xdisplay2)
        return;
    if (!grab_xi2(window)) {
        ungrab_xi2();
        return;
    }
    GIOChannel *channel =
        g_io_channel_unix_new(ConnectionNumber(G->xdisplay2));
    G->xi_source = g_io_add_watch(channel, G_IO_IN, xi_dispatch, NULL);
    g_io_channel_unref(channel);
    G->grabbed = TRUE;
    g_printerr("uac-card: entrada capturada (override-redirect, XI2)\n");
}

static gboolean try_grab(gpointer user_data)
{
    (void)user_data;
    if (!G->window || G->finished || !G->x11 || !G->xdisplay)
        return G_SOURCE_REMOVE;

    if (G->grabbed)
        return G_SOURCE_REMOVE;

    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(G->window));
    if (!surface)
        return G_SOURCE_CONTINUE;
    G->xid = gdk_x11_surface_get_xid(surface);
    if (!G->xid)
        return G_SOURCE_CONTINUE;

    XSetInputFocus(G->xdisplay2, G->xid, RevertToParent, CurrentTime);
    XFlush(G->xdisplay2);
    xi_start(G->xid);
    return G->grabbed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

/* Rellena monitor principal y caja que cubre todos los monitores. */
static void compute_monitors(void)
{
    GdkDisplay *display = gdk_display_get_default();
    GListModel *monitors = gdk_display_get_monitors(display);
    guint count = g_list_model_get_n_items(monitors);

    int bx = 0, by = 0, bw = 0, bh = 0;
    int primary = -1;
    GdkRectangle primary_rect = {0, 0, 1920, 1080};

    /* UAC_CARD_MONITOR=n permite elegir el monitor a mano. */
    const char *wanted = g_getenv("UAC_CARD_MONITOR");
    int preferred = wanted ? atoi(wanted) : -1;

    for (guint i = 0; i < count; i++) {
        GdkMonitor *monitor = g_list_model_get_item(monitors, i);
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        int scale = gdk_monitor_get_scale_factor(monitor);
        if (scale <= 0)
            scale = 1;

        /* GDK da píxeles lógicos; X trabaja en píxeles de dispositivo. */
        int x = geometry.x * scale;
        int y = geometry.y * scale;
        int w = geometry.width * scale;
        int h = geometry.height * scale;

        if (i == 0 || x < bx)
            bx = x;
        if (i == 0 || y < by)
            by = y;
        if (x + w > bx + bw)
            bw = x + w - bx;
        if (y + h > by + bh)
            bh = y + h - by;

        if (primary < 0 && x <= 0 && y <= 0 && x + w > 0 && y + h > 0)
            primary = (int)i;

        g_object_unref(monitor);
    }

    if (preferred >= 0 && (guint)preferred < count)
        primary = preferred;
    if (primary < 0)
        primary = 0;

    if (count > 0) {
        GdkMonitor *monitor = g_list_model_get_item(monitors, primary);
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        int scale = gdk_monitor_get_scale_factor(monitor);
        if (scale <= 0)
            scale = 1;
        primary_rect.x = geometry.x * scale;
        primary_rect.y = geometry.y * scale;
        primary_rect.width = geometry.width * scale;
        primary_rect.height = geometry.height * scale;
        g_object_unref(monitor);
    }

    G->bbox_x = bx;
    G->bbox_y = by;
    G->bbox_w = bw > 0 ? bw : primary_rect.width;
    G->bbox_h = bh > 0 ? bh : primary_rect.height;

    G->mon_x = primary_rect.x - bx;
    G->mon_y = primary_rect.y - by;
    G->mon_w = primary_rect.width;
    G->mon_h = primary_rect.height;
}

static void make_override_redirect(GtkWidget *window)
{
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!surface)
        return;
    G->xid = gdk_x11_surface_get_xid(surface);
    if (!G->xid)
        return;

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    XChangeWindowAttributes(G->xdisplay, G->xid, CWOverrideRedirect, &attrs);

    XMoveResizeWindow(G->xdisplay, G->xid, G->bbox_x, G->bbox_y, G->bbox_w,
                      G->bbox_h);
    XRaiseWindow(G->xdisplay, G->xid);
    XFlush(G->xdisplay);
}

/* ---------------------------------------------------------------- D-Bus */

static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='org.nixer.Uac'>"
    "    <method name='Ping'/>"
    "    <method name='Request'>"
    "      <arg type='s' name='request' direction='in'/>"
    "      <arg type='b' name='approved' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static gboolean sender_is_root(GDBusConnection *connection,
                               const gchar *sender)
{
    if (!sender || !*sender)
        return FALSE;

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixUser",
        g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (!reply) {
        g_printerr("uac-card: no pude comprobar el uid: %s\n",
                   error ? error->message : "?");
        if (error)
            g_error_free(error);
        return FALSE;
    }

    guint32 uid = 65534;
    g_variant_get(reply, "(u)", &uid);
    g_variant_unref(reply);
    return uid == 0;
}

static void free_request(Request *request)
{
    if (!request)
        return;
    g_free(request->app_name);
    g_free(request->publisher);
    g_free(request->message);
    g_free(request->command);
    if (request->details) {
        for (guint i = 0; i < request->details->len; i++) {
            gchar **pair = g_ptr_array_index(request->details, i);
            g_free(pair[0]);
            g_free(pair[1]);
            g_free(pair);
        }
        g_ptr_array_unref(request->details);
    }
    g_free(request);
}

static char *json_text(JsonObject *object, const char *member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    JsonNode *node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;
    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return g_strdup(json_node_get_string(node));
}

static Request *parse_request(const char *json, GError **error)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, error)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "el aviso no es un objeto JSON");
        g_object_unref(parser);
        return NULL;
    }
    JsonObject *object = json_node_get_object(root);

    Request *request = g_new0(Request, 1);
    request->app_name = json_text(object, "app_name");
    request->publisher = json_text(object, "publisher");
    request->message = json_text(object, "message");
    request->command = json_text(object, "command");
    request->timeout = DEFAULT_TIMEOUT;

    if (json_object_has_member(object, "password_required")) {
        JsonNode *node = json_object_get_member(object, "password_required");
        if (node && JSON_NODE_HOLDS_VALUE(node))
            request->password_required = json_node_get_boolean(node);
    }
    if (json_object_has_member(object, "timeout")) {
        JsonNode *node = json_object_get_member(object, "timeout");
        if (node && JSON_NODE_HOLDS_VALUE(node)) {
            int seconds = (int)json_node_get_double(node);
            if (seconds > 0 && seconds <= 600)
                request->timeout = seconds;
        }
    }

    request->details = g_ptr_array_new();
    if (json_object_has_member(object, "details")) {
        JsonNode *node = json_object_get_member(object, "details");
        if (node && JSON_NODE_HOLDS_ARRAY(node)) {
            JsonArray *array = json_node_get_array(node);
            guint length = json_array_get_length(array);
            for (guint i = 0; i < length; i++) {
                JsonNode *item = json_array_get_element(array, i);
                if (!item || !JSON_NODE_HOLDS_ARRAY(item))
                    continue;
                JsonArray *pair = json_node_get_array(item);
                if (json_array_get_length(pair) < 2)
                    continue;
                JsonNode *key_node = json_array_get_element(pair, 0);
                JsonNode *value_node = json_array_get_element(pair, 1);
                if (!JSON_NODE_HOLDS_VALUE(key_node) ||
                    !JSON_NODE_HOLDS_VALUE(value_node))
                    continue;
                gchar **strings = g_new0(gchar *, 2);
                strings[0] = g_strndup(json_node_get_string(key_node), 120);
                strings[1] = g_strndup(json_node_get_string(value_node), 600);
                g_ptr_array_add(request->details, strings);
            }
        }
    }

    g_object_unref(parser);
    return request;
}

static gboolean show_card(Request *request);

static void handle_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data)
{
    (void)connection;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "Ping") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "Request") != 0) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Método desconocido");
        return;
    }

    if (!sender_is_root(connection, sender)) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
            "Solo el puente de PAM puede mostrar el aviso");
        return;
    }

    if (G->card_open) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "Ya hay un aviso de UAC abierto");
        return;
    }

    const gchar *json = NULL;
    g_variant_get(parameters, "(&s)", &json);
    GError *error = NULL;
    Request *request = parse_request(json, &error);
    if (!request) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
            "JSON inválido: %s", error ? error->message : "?");
        if (error)
            g_error_free(error);
        return;
    }

    if (request->password_required) {
        free_request(request);
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
            "El aviso con contraseña no usa esta tarjeta");
        return;
    }

    G->pending = invocation;
    G->card_open = TRUE;
    G->approved = FALSE;
    G->finished = FALSE;
    G->pressed = ACTION_NONE;
    G->details_open = FALSE;
    G->dragging = FALSE;
    G->focus_index = 0;

    if (!show_card(request)) {
        G->pending = NULL;
        G->card_open = FALSE;
        free_request(request);
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "No pude abrir la tarjeta");
        return;
    }
    free_request(request);

    /* Bucle anidado: el Request se queda esperando la decisión mientras GTK
     * sigue procesando eventos. */
    G->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(G->loop);
    g_main_loop_unref(G->loop);
    G->loop = NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call, NULL, NULL, {0}};

/* ------------------------------------------------------------- ventana */

static gboolean show_card(Request *request)
{
    G->finished = FALSE;
    G->shown_at = g_get_monotonic_time();

    compute_monitors();

    GtkWidget *window = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_title(GTK_WINDOW(window), "Control de cuentas de usuario");
    gtk_widget_add_css_class(window, "uac-root");
    G->window = window;
    set_card_classes();

    GtkWidget *stage = gtk_fixed_new();
    gtk_widget_set_size_request(stage, G->bbox_w, G->bbox_h);
    gtk_window_set_child(GTK_WINDOW(window), stage);
    G->stage = stage;

    GtkWidget *card = make_box(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "uac-card");
    G->card = card;
    G->card_width = card_width();
    gtk_widget_set_size_request(card, G->card_width, -1);
    gtk_fixed_put(GTK_FIXED(stage), card, 0, 0);

    build_header(request);
    build_body(request);
    build_footer();

    /* Entrada: un solo gesto sobre el escenario decide qué botón se pulsó.
     * Sin señales "clicked": un clic programático (AT-SPI) no llega aquí. */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_released), NULL);
    gtk_widget_add_controller(stage, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
    gtk_widget_add_controller(stage, GTK_EVENT_CONTROLLER(motion));

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(window, keys);

    place_card();

    if (G->x11) {
        /* Realizar sin gestionar: la ventana nace override-redirect, así que
         * KWin no la decora, no la mueve y no la puede tapar con paneles. */
        gtk_widget_realize(window);
        make_override_redirect(window);
        gtk_window_present(GTK_WINDOW(window));
        XMoveResizeWindow(G->xdisplay, G->xid, G->bbox_x, G->bbox_y,
                          G->bbox_w, G->bbox_h);
        XRaiseWindow(G->xdisplay, G->xid);
        XFlush(G->xdisplay);
        g_timeout_add(50, try_grab, NULL);
    } else {
        /* Wayland: sin grabs reales. Se deja la ventana a pantalla completa
         * como mejor esfuerzo; el puente avisa de que no es un bloqueo. */
        gtk_window_fullscreen(GTK_WINDOW(window));
        gtk_window_present(GTK_WINDOW(window));
    }

    set_shortcuts_blocked(TRUE);
    G->timeout_id = g_timeout_add_seconds(request->timeout, on_timeout, NULL);
    return TRUE;
}

/* --------------------------------------------------------------- arranque */

static void on_name_acquired(GDBusConnection *connection, const gchar *name,
                             gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_printerr("uac-card: sirviendo %s\n", name);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name,
                         gpointer user_data)
{
    (void)connection;
    (void)user_data;
    /* Otra instancia ya tiene el nombre (o se cayó el bus): esta sobra. */
    g_printerr("uac-card: no pude quedarme con %s; salgo\n", name);
    if (G && G->main_loop)
        g_main_loop_quit(G->main_loop);
}

/* KWin atiende Meta y Alt+Tab con eventos XI2 "raw", que ningún grab puede
 * interceptar. KGlobalAccel sí sabe bloquear los atajos globales mientras hay
 * un diálogo modal (es lo que usa el bloqueo de pantalla). Es estado de
 * ejecución: no toca kglobalshortcutsrc. */
static void set_shortcuts_blocked(gboolean blocked)
{
    if (!G || !G->bus)
        return;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        G->bus, "org.kde.kglobalaccel", "/kglobalaccel",
        "org.kde.KGlobalAccel", "blockGlobalShortcuts",
        g_variant_new("(b)", blocked), NULL, G_DBUS_CALL_FLAGS_NONE, 1000,
        NULL, &error);
    if (reply)
        g_variant_unref(reply);
    if (error) {
        g_printerr("uac-card: no pude %s los atajos globales: %s\n",
                   blocked ? "bloquear" : "restaurar", error->message);
        g_error_free(error);
    }
}

static void install_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, CARD_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

int main(int argc, char **argv)
{
    gboolean demo = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--demo") == 0)
            demo = TRUE;
        else if (g_strcmp0(argv[i], "--version") == 0) {
            g_print("uac-card 1.4\n");
            return 0;
        }
    }

    gtk_init();
    XSetErrorHandler(ignore_x_error);

    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    gboolean is_kde = desktop && g_strrstr(desktop, "KDE") != NULL;
    if (!is_kde && !demo && !g_getenv("UAC_CARD_FORCE")) {
        /* En GNOME la tarjeta la pinta la extensión; este proceso sobra. */
        g_printerr("uac-card: escritorio '%s', no es Plasma; salgo\n",
                   desktop ? desktop : "?");
        return 0;
    }

    G = g_new0(Card, 1);
    G->x11 = GDK_IS_X11_DISPLAY(gdk_display_get_default());
    G->main_loop = g_main_loop_new(NULL, FALSE);

    read_theme(&G->dark, &G->scale);
    install_css();

    if (!G->x11)
        g_printerr("uac-card: sin X11 no puedo bloquear la entrada; "
                   "la tarjeta no será modal\n");

    if (G->x11) {
        GdkSurface *surface = NULL;
        (void)surface;
        G->xdisplay = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
        G->xdisplay2 = XOpenDisplay(NULL);
        G->xi_opcode = -1;
        if (G->xdisplay2) {
            int xi_event = 0, xi_error = 0;
            if (XQueryExtension(G->xdisplay2, "XInputExtension",
                                &G->xi_opcode, &xi_event, &xi_error)) {
                int major = 2, minor = 2;
                if (XIQueryVersion(G->xdisplay2, &major, &minor) != Success)
                    G->xi_opcode = -1;
            }
        }
    }

    if (demo) {
        GError *bus_error = NULL;
        G->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &bus_error);
        if (!G->bus && bus_error)
            g_error_free(bus_error);

        Request request = {0};
        request.app_name = g_strdup("apt");
        request.publisher = g_strdup("Terminal · sudo");
        request.message = g_strdup(DEFAULT_MESSAGE);
        request.command = g_strdup("sudo apt install htop");
        request.timeout = DEFAULT_TIMEOUT;
        request.details = g_ptr_array_new();
        gchar **pair = g_new0(gchar *, 2);
        pair[0] = g_strdup("Usuario");
        pair[1] = g_strdup(g_get_user_name());
        g_ptr_array_add(request.details, pair);

        if (!show_card(&request)) {
            free_request(&request);
            return 2;
        }
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        G->loop = loop;
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        G->loop = NULL;

        g_print("{\"approved\": %s, \"via\": \"%s\"}\n",
                G->approved ? "true" : "false", G->via ? G->via : "?");
        return G->approved ? 0 : 1;
    }

    if (!G->x11) {
        /* Sin X11 no hay forma razonable de bloquear; mejor no mentir. */
        return 0;
    }

    GError *error = NULL;
    G->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!G->bus) {
        g_printerr("uac-card: sin bus de sesión: %s\n",
                   error ? error->message : "?");
        if (error)
            g_error_free(error);
        return 1;
    }

    GDBusNodeInfo *info =
        g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!info) {
        g_printerr("uac-card: XML inválido: %s\n",
                   error ? error->message : "?");
        return 1;
    }

    guint registration =
        g_dbus_connection_register_object(G->bus, OBJECT_PATH,
                                          info->interfaces[0],
                                          &interface_vtable, NULL, NULL,
                                          &error);
    if (!registration) {
        g_printerr("uac-card: no pude exportar el objeto: %s\n",
                   error ? error->message : "?");
        return 1;
    }

    /* Si una ejecución anterior murió con los atajos bloqueados, se
     * restauran al arrancar. */
    set_shortcuts_blocked(FALSE);

    g_bus_own_name_on_connection(G->bus, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_name_acquired, on_name_lost, NULL, NULL);

    g_main_loop_run(G->main_loop);

    release_grab();
    g_dbus_node_info_unref(info);
    return 0;
}
