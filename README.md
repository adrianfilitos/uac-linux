# uac-linux

Un aviso de "Control de cuentas de usuario" al estilo de Windows para Ubuntu y
GNOME. Cada vez que algo pide privilegios con `sudo` sale una tarjeta modal con
**Sí** y **No**, sin pedir contraseña, y el escritorio deja de responder
mientras decides.

La referencia es el aviso de consentimiento de administrador de Windows: en modo
de aprobación de administrador, UAC muestra una ventana de consentimiento
(Sí/No) sobre un escritorio atenuado, sin pedir credenciales. Este proyecto
reproduce esa experiencia en Linux.

La diferencia con la primera versión: el aviso ya **no** lo pone un envoltorio
de bash, lo pone **PAM**. Da igual quién llame a `sudo` (bash, Python, un
script, un Makefile, un servicio): PAM es el mismo camino para todos.

Y desde la 1.3 el aviso también sale con **polkit**, así que `pkexec` y los
diálogos de «se requieren privilegios» de GNOME paran en la misma tarjeta.

## Cómo se ve

- Tarjeta de 456 px centrada en el **monitor principal**, con barra de título,
  icono de la aplicación, editor, pregunta, enlace "Mostrar detalles" y botones
  **Sí** / **No**. El fondo atenúa todos los monitores, pero la tarjeta siempre
  aparece en el centro del principal, aunque haya varios conectados. Tiene las
  esquinas redondeadas y se puede arrastrar agarrándola por la barra de título,
  sin poder sacarla de ese monitor.
- Sigue el tema del escritorio: si GNOME está en oscuro, la tarjeta usa la
  paleta oscura de Windows 11; si está en claro, la clara.
- El comando completo aparece dentro de "Mostrar detalles".
- El aviso se dibuja **dentro de GNOME Shell**, no como ventana de una
  aplicación. Mientras está abierto, el shell hace un grab modal del teclado y
  del ratón (`Main.pushModal` con `Shell.ActionMode.SYSTEM_MODAL`), igual que
  la pantalla de bloqueo o el diálogo de polkit. El fondo se atenúa y no se
  puede usar Super, Alt+Tab, el overview ni ninguna otra ventana hasta
  responder.
- Si no respondes en **60 segundos**, la tarjeta se cierra sola y cuenta como
  **No**.
- **Respeta el tamaño de texto del escritorio** (`text-scaling-factor`): con
  «texto grande» al 125 % la tarjeta crece un 125 %, letra, rellenos, icono y
  botones incluidos, hasta el 200 %. GNOME Shell ignora ese ajuste en sus
  propios widgets, así que aquí se aplica a mano. El escalado de pantalla
  (125 %, 150 %…) ya lo compone mutter y no se multiplica dos veces: la tarjeta
  nunca se sale del monitor (tope del 90 % del ancho).
- Solo se aprueba con **entrada de verdad**: soltar el botón del ratón encima
  del botón, tocar, o Return/Espacio sobre el que tenga el foco. Un clic que
  empieza en «Sí» y suelta fuera no aprueba nada. Si `Sí` fuera el botón
  enfocado, un Enter lo activa, igual que en Windows.
- Si tienes la **accesibilidad activada** (lector de pantalla o «tecnologías de
  asistencia»), la tarjeta acepta además el clic que manda AT-SPI, para no
  dejar fuera a quien navega con lector. Queda anotado en el journal y, mientras
  esté activada, un programa del mismo usuario sí podría pulsar por ti (ver
  «Seguridad»).
- **Anti-spam**: si cierras tres tarjetas con «No» en menos de un minuto, deja
  de pintar tarjetas durante ese minuto y las peticiones se deniegan al
  instante. Un «Sí» reinicia la cuenta. Así un bucle de `sudo true` no te
  obliga a pulsar «No» sin fin.
- Suena el aviso de UAC de Windows (`src/data/uac.wav`) justo antes de que
  aparezca la tarjeta, con una espera de 0,35 s para lograr la misma sensación
  que en Windows. El WAV original tiene el pico muy bajo (−10,9 dBFS), así que
  se normaliza el pico a ≈−0,5 dBFS y se le aplican 10 dB extra con un
  limitador suave: sube unos 6,8 dB la energía media sin recortes. El resultado
  se guarda en caché (`~/.cache/uac-linux/`) y se reproduce con `pw-play` (rol
  `alert`), `paplay` o `canberra-gtk-play`, el primero que exista. Mientras
  suena, el resto de audio baja al 30 % y se restaura solo a los 2,5 s (ducking
  con `wpctl`), para que la música no tape el aviso.

### Aviso sobre `src/data/uac.wav`

Ese fichero es el sonido de aviso de Windows y **es propiedad de Microsoft**. No
forma parte de este proyecto ni de su licencia: está aquí como copia local sacada
de la instalación de Windows del autor, para que el aviso suene igual que en
Windows. Si eres titular de los derechos y quieres que lo retire, abre una issue
y se elimina.

El sonido es totalmente opcional: si borras el archivo, el puente lo detecta y
simplemente no suena.

## Requisitos

- Ubuntu 24.04 con GNOME 46 y Wayland (probado en ese entorno).
- Python 3 y `python3-gi` con Gio/GObject (vienen de serie).
- GTK 4 (`gir1.2-gtk-4.0`), solo para la ventana de contraseña de respaldo.
- `gjs` y GNOME Shell (vienen de serie).
- `sudo` y `pam_exec` (paquete `libpam-modules`, instalado por defecto).

## Instalación

```bash
cd ~/uac-linux
chmod +x install.sh install-root.sh uninstall.sh
./install.sh
```

El instalador:

1. Crea enlaces en `~/.local/bin` (`uac-sudo`, `uac-dialog`, `uac-askpass`).
2. Copia la extensión de GNOME Shell a
   `~/.local/share/gnome-shell/extensions/uac-linux@nixer` y la activa.
3. Añade una función `sudo()` a `~/.bashrc`; hoy solo sirve para que `sudo`
   tenga con qué pedir la contraseña cuando no hay terminal.
4. Pide la contraseña **una última vez** y ejecuta `install-root.sh`, que:
   - copia el puente a `/usr/local/libexec/uac-linux/uac-pam`;
   - escribe `/etc/pam.d/sudo-uac`, `/etc/pam.d/sudo-uac-login` y
     `/etc/pam.d/polkit-1` (este último es el que hace que `pkexec` también
     muestre la tarjeta);
   - permite que root se conecte al bus de la sesión
     (`/etc/dbus-1/session-local.conf`);
   - escribe `/etc/sudoers.d/90-uac-<usuario>` apuntando `sudo` a ese servicio
     PAM y **quita el `NOPASSWD`**;
   - comprueba que `sudo` sigue funcionando y que ya no entra sin contraseña.

**GNOME Shell solo descubre extensiones nuevas al arrancar.** Tras instalarla
por primera vez hay que cerrar sesión y volver a entrar (una sola vez). Para
comprobarlo:

```bash
gnome-extensions info uac-linux@nixer
```

Debe decir `Estado: ACTIVE`.

Si prefieres hacer el paso de root por separado:

```bash
./install.sh --no-root
sudo bash install-root.sh "$USER"
```

## Uso

```bash
sudo apt update
sudo systemctl restart NetworkManager
sudo -i
pkexec id
```

Cada orden muestra la tarjeta. **Sí** ejecuta el comando como root; **No**,
Escape o la X lo cancelan, y no se pide nada más. La tarjeta sale también de
dentro de un script:

```bash
sh -c 'sudo id'          # también pasa por el aviso
```

Si el aviso no puede salir (extensión desactivada, sin sesión gráfica, shell
caído), `sudo` pide la contraseña como siempre. Nunca te quedas fuera.

### Apagarlo un rato

Los interruptores son ficheros que solo root puede crear, porque `sudo` borra
las variables de entorno antes de llamar a PAM:

```bash
sudo touch /etc/uac-linux/disabled      # sin tarjeta: solo contraseña
sudo rm /etc/uac-linux/disabled         # otra vez con tarjeta
sudo touch /etc/uac-linux/no-sound      # sin sonido
sudo touch /etc/uac-linux/no-ducking    # sin bajar la música
```

## Cómo funciona

1. Ejecutas `sudo`. Antes de mirar contraseñas, PAM abre
   `/etc/pam.d/sudo-uac` (se lo ha dicho `Defaults:<usuario> pam_service=` en
   `/etc/sudoers.d/90-uac-<usuario>`). Con `pkexec` o un diálogo de polkit, PAM
   abre `/etc/pam.d/polkit-1`, que tiene exactamente las mismas tres líneas y el
   mismo puente.
2. `pam_exec` ejecuta `/usr/local/libexec/uac-linux/uac-pam card` **como root**
   (uid real del usuario, uid efectivo 0 gracias a `seteuid`).
3. El puente lee `PAM_USER`, `PAM_SERVICE` y el comando exacto de
   `/proc/<ppid>/cmdline`, y manda la petición por D-Bus a la extensión del
   shell. Antes de enviarla comprueba que `org.nixer.Uac` pertenece al mismo
   proceso que `org.gnome.Shell` y que ese proceso es `gnome-shell`, y le habla
   por su nombre único para que nadie se cuele en medio.
4. La extensión comprueba que quien pide el aviso es **uid 0** (un proceso
   tuyo no puede pintar la tarjeta) y pinta el aviso en
   `Main.layoutManager.modalDialogGroup` con
   `Main.pushModal(root, {actionMode: Shell.ActionMode.SYSTEM_MODAL})`. Eso
   captura teclado y ratón en el compositor: el escritorio deja de responder
   hasta que eliges **Sí** o **No**.
5. **Sí** → el puente sale con código 0 → `auth [success=done]` → PAM da la
   autenticación por buena. No hay contraseña de por medio.
6. **No** → el puente deja una marca en `/run/uac-pam` (tmpfs, solo root) y
   sale con 1. La línea siguiente, `uac-denied`, ve la marca y sale con 1,
   así que PAM cae en `auth requisite pam_deny.so` y deniega en firme: `sudo`
   no llega ni a pedir la contraseña.
7. Sin marca (el aviso no salió) → `uac-denied` sale con 0, se salta el
   `pam_deny` y sigue la pila normal: `pam_unix` pide la contraseña.

Detalles que importan:

- `pam_exec` solo distingue "salió 0" de "salió otra cosa", así que el "No" no
  se puede expresar con un código de salida. Por eso está la marca y por eso la
  denegación la da `pam_deny`, que es el único que puede devolver un fallo de
  verdad. (`[success=die]` con un módulo que sale 0 devuelve **PAM_SUCCESS**, o
  sea que habría autenticado. Está comprobado en la documentación de `pam.d(5)`
  y es la trampa clásica de esta configuración.)
- `Defaults:<usuario> timestamp_timeout=0`: la tarjeta sale en **cada**
  elevación, como en Windows, sin caché de 15 minutos.
- `Defaults:<usuario> passwd_tries=1`: un "No" o una contraseña mal no se
  repiten tres veces.
- El estado vive en `/run/uac-pam/` (0700 root). Ahí está `last.log`, que es lo
  primero que hay que mirar si algo no sale:

```bash
sudo cat /run/uac-pam/last.log
```

- Cada decisión deja además una línea JSON en el journal
  (`journalctl -t uac-linux`), con usuario, servicio, comando, aplicación y
  milisegundos que tardaste en responder. Es lo que permite auditar después
  quién aprobó qué:

```bash
journalctl -t uac-linux -n 20
```

- El anti-spam guarda en `/run/uac-pam/<uid>.denials` la hora de tus últimas
  denegaciones. Con tres en 60 s, el puente deja de pintar tarjetas el resto de
  la ventana, escribe la marca de «No» y deniega. Vive en tmpfs: se borra al
  reiniciar, y un proceso tuyo no puede escribirlo.
- Con polkit la tarjeta no puede saber **qué acción** se está autorizando (eso
  lo sabe polkitd, no PAM), así que en ese caso es genérica: aplicación del
  sistema y el servicio `polkit-1`. La de `sudo` sí enseña el comando exacto.

## Seguridad: léelo

Esto sigue siendo una **capa de experiencia de usuario**, no una frontera de
seguridad. Dicho eso, es mucho más sólida que antes:

Lo que **sí** consigue:

- Ya no hay `NOPASSWD`: cualquier proceso que pida privilegios por `sudo` pasa
  por el aviso, venga de una shell, de un script, de un Makefile o de un
  servicio. `command sudo` o `/usr/bin/sudo` ya no son una puerta de atrás.
- Tampoco hay puerta por **polkit**: `pkexec` y los diálogos de «se requieren
  privilegios» pasan por `/etc/pam.d/polkit-1` y su tarjeta.
- Un proceso tuyo **no puede pintar la tarjeta** (la extensión exige uid 0 al
  solicitante) ni puede falsificar la respuesta: el puente corre como root y la
  decisión viaja por una conexión que solo acepta a `gnome-shell`.
- **AT-SPI no puede pulsar «Sí»** mientras la accesibilidad esté apagada (que
  es como viene de fábrica). Los botones no escuchan el evento `clicked` que
  dispara `DoAction`: solo cuentan los eventos reales de ratón, toque o teclado.
  Está probado con una sonda que recorre el árbol de accesibilidad y lo
  encuentra vacío.
- La marca de "No" está en tmpfs de root, así que no se puede borrar desde tu
  cuenta para reintentar.
- El **anti-spam** corta el bucle de tarjetas: tres «No» en un minuto y ese
  minuto no sale ni una más (las peticiones se deniegan al instante).
- Cada decisión queda en el journal (`journalctl -t uac-linux`) con usuario,
  comando y tiempo de respuesta.
- Si el aviso falla, se pide contraseña; no hay forma de que un fallo te dé
  root por la cara.

Lo que **no** consigue:

- Un clic es consentimiento, no autenticación. Con la accesibilidad activada
  (lector de pantalla), un programa que ya se ejecuta como tú y sepa pilotar
  AT-SPI sí puede pulsar "Sí" en tu lugar: es el precio de no dejar fuera a
  quien navega con lector. Para cerrar eso del todo haría falta un escritorio
  seguro de verdad, con cambio de VT y entrada leída desde root.
- `su`, `systemctl`, los grupos `docker`/`incus`, los binarios setuid y los
  servicios de systemd siguen su camino. El aviso cubre lo que pasa por PAM
  (`sudo` y polkit), no la configuración del sistema.
- Si alguien borra `/usr/local/libexec/uac-linux/uac-pam`, el paso `card`
  falla, pero `uac-denied` (un script de shell aparte) sigue diciendo "no hay
  marca", así que la autenticación cae en **la contraseña**: sudo y pkexec
  siguen usándose con tu clave de siempre. Si además se borra `uac-denied`,
  PAM deniega en firme y la salida es `su -` desde un TTY o el modo recovery
  del GRUB.
- El comando se muestra **recortado** a unos 480 caracteres (principio y
  final, con `…` en medio) y la extensión corta cualquier texto a 600. No es
  un detalle estético: `ARG_MAX` permite mandar megabytes de argumentos a
  `sudo`, y un `St.Label` de 456 px con dos megas de texto cuelga el shell
  entero. Antes de la 1.2 se podía hacer desde una cuenta normal. La
  contrapartida es que un comando larguísimo puede esconder la parte peligrosa
  en el medio.
- Un programa tuyo todavía puede **encadenar tres tarjetas** por minuto o
  esperar a que se agote el bloqueo (`while :; do sudo true; done`). No consigue
  root; te obliga a pulsar «No» tres veces y a esperar.
- No lo uses en un equipo compartido.

## Volver atrás

```bash
sudo rm -f /etc/sudoers.d/90-uac-"$USER" \
    /etc/pam.d/sudo-uac /etc/pam.d/sudo-uac-login /etc/pam.d/polkit-1
```

A partir de ahí `sudo` vuelve a `/etc/pam.d/sudo` (y polkit a su configuración
de siempre) y piden contraseña. Si `sudo` estuviera roto, esas órdenes hay que
lanzarlas como root **desde un TTY** (`Ctrl+Alt+F3`, entra con tu usuario y
contraseña y luego `su -`), porque `pkexec` también pasa por el puente.

## Desinstalar

```bash
cd ~/uac-linux
./uninstall.sh
```

Quita la función de `~/.bashrc` (con copia de seguridad), los enlaces, el
puente, los ficheros PAM, `/etc/uac-linux`, `/etc/sudoers.d/90-uac-<usuario>`,
las copias `.uac-backup.*` y la política D-Bus. Abre una terminal nueva al
terminar.

## Estructura

```
uac-linux/
├── install.sh          # parte de usuario + llamada al paso de root
├── install-root.sh     # puente, reglas PAM (sudo y polkit) y sudoers
├── uninstall.sh        # revierte todo
├── LICENSE             # MIT (el WAV de Windows no está cubierto)
├── extension/          # extensión de GNOME Shell (la tarjeta y el bloqueo)
│   ├── metadata.json
│   ├── extension.js    # aviso modal con Main.pushModal
│   ├── stylesheet.css
│   └── uac-shield.svg
├── src/
│   ├── uac_pam.py      # puente PAM: tarjeta, sonido, anti-spam y auditoría
│   ├── uac_denied.sh   # segundo paso de PAM: marca de «No» -> denegar (shell)
│   ├── uac_sudo.py     # envoltorio fino: solo añade la ventana de contraseña sin tty
│   ├── uac_dialog.py   # diálogo GTK4 (JSON de entrada/salida)
│   ├── uac_askpass.py  # ayudante SUDO_ASKPASS (respaldo con contraseña)
│   └── data/
│       ├── uac.css
│       ├── uac.wav     # aviso sonoro de UAC (copia local de Windows)
│       └── uac-shield.svg
└── README.md
```

## Diagnóstico

```bash
sudo /usr/local/libexec/uac-linux/uac-pam selftest "$USER"
```

Comprueba usuario, bus de sesión, extensión, sonido, reproductor, si polkit
está enganchado y cuántas denegaciones llevas en el último minuto. Y para ver
solo la tarjeta, sin tocar PAM:

```bash
sudo /usr/local/libexec/uac-linux/uac-pam card "$USER"
```

Cada decisión queda además en el journal:

```bash
journalctl -t uac-linux -n 20        # puente: aprobado, denegado, spam…
journalctl -b -g uac-linux           # shell: por dónde se respondió
```

## Prueba visual sin instalar nada

```bash
./src/uac_dialog.py --demo
```

Por defecto la ventana GTK es normal (no fullscreen). `--fullscreen` la pone a
pantalla completa. Recuerda que solo la extensión del shell bloquea de verdad.

## Integración con otras aplicaciones

`uac_dialog.py` acepta un JSON con `app_name`, `publisher`, `message`,
`command`, `username`, `details`, `password_required`, `fullscreen` y `session`,
y devuelve por stdout `{"approved": bool, "password": str|null}`. Se puede
invocar desde cualquier lanzador o script. El puente de PAM le añade un campo
`timeout` para que la tarjeta se cierre sola.
