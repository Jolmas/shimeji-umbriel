# UmbrielSupport para wl_shimeji

Plugin para [wl_shimeji](https://github.com/CluelessCatBurger/wl_shimeji) que usa el
socket IPC de [Umbriel](https://github.com/noctalia-dev/umbriel) para decir a las
mascotas dónde está la ventana activa y permitirles moverla.

Es el equivalente al plugin `kwinsupport`, pero para sesiones Umbriel. No depende
de D-Bus ni de KWin: todo se hace con el socket UNIX JSON de Umbriel.

## Qué hace

- Se suscribe a los eventos `windows` y `outputs` de Umbriel.
- Publica en wl_shimeji la geometría global de la ventana elegida como IE
  (`interactive element`) mediante `set_active_ie(...)`, que es lo que permite a
  las mascotas pararse, sentarse y caminar sobre una ventana:
  - ignora ventanas a pantalla completa, minimizadas, ocultas y sin tamaño;
  - qué ventana se elige se controla con `UMBRIEL_IE_MODE` (ver más abajo).
- Avisa con `window_moved_hint()` cuando la ventana activa cambia o se mueve,
  para cancelar acciones de mascotas que estuvieran arrastrándola.
- Implementa los hooks opcionales:
  - `wl_shimeji_plugin_move`: envía `window-move-to` con ancla `top_left`,
    convirtiendo las coordenadas globales de wl_shimeji a coordenadas del área
    útil del monitor;
  - `wl_shimeji_plugin_restore`: vuelve a colocar en `(100, 100)` las ventanas
    flotantes que quedaron fuera de todos los monitores;
  - `wl_shimeji_plugin_activate`: envía `window-focus` para enfocar la ventana
    seleccionada.

La conexión es no bloqueante, se reconecta sola con retroceso exponencial
(1–30 s) y limpia el estado al perder el socket.

## Requisitos

- `wl_shimeji` con API de plugins `0.0.2` (cabeceras
  `wl_shimeji/plugins.h` y `master_header.h`, y
  `libwayland-shimeji-plugins.so`).
- `jansson` (parser JSON): en Debian/Ubuntu, `libjansson-dev`.
- `cc`, `make` y `pkg-config`.

En esta máquina las cabeceras y la librería están en `/usr/local/include` y
`/usr/local/lib`, que es lo que usa el `Makefile` por defecto (`PREFIX=/usr/local`).
Si tu instalación está en otro sitio:

```sh
make PREFIX=/usr WL_SHIMEJI_INCLUDEDIR=/usr/include WL_SHIMEJI_LIBDIR=/usr/lib
```

## Compilación e instalación

```sh
cd ~/shimeji-umbriel
make
make test                 # pruebas offline del parser de snapshots
sudo make install         # instala libumbrielsupport.so en $PREFIX/lib/wl_shimeji/
```

También se puede compilar como subdirectorio de `wl_shimeji/src/plugins/`:

```sh
cp -r ~/wl_shimeji-umbriel /ruta/a/wl_shimeji/src/plugins/umbriel
cd /ruta/a/wl_shimeji && make build-plugins && sudo make install-plugins
```

## Uso

wl_shimeji carga automáticamente los `.so` de `/usr/lib/wl_shimeji/` (o del
directorio que indiques con `plugins_location` en `shimeji-overlayd.conf`).

### Mascotas sobre las ventanas (modo de IE)

La ventana sobre la que pueden pararse las mascotas se elige con la variable de
entorno `UMBRIEL_IE_MODE`, que se lee al cargar el plugin. Debes definirla en el
entorno del proceso `shimeji-overlayd` y reiniciar el overlay:

| Valor              | Ventana que se publica como IE                                      |
| ------------------ | ------------------------------------------------------------------ |
| `active` (default) | foco global (`active`), luego foco del workspace, luego única candidata |
| `focused`          | foco del workspace (`focused`) primero, luego foco global           |
| `largest`          | ventana visible más grande (área `w * h`); útil para ventanas maximizadas |
| `none`             | ninguna: las mascotas no interactúan con ventanas                  |

Se aceptan alias: `seat` = `active`, `workspace` = `focused`,
`biggest`/`max` = `largest`, `off`/`disabled` = `none`. Un valor desconocido
produce un aviso y se usa `active`.

Ejemplo:

```sh
UMBRIEL_IE_MODE=largest shimeji-overlayd ...
```

También puedes guardarlo en `~/.config/wl_shimeji/umbriel-support.conf` (o
`$XDG_CONFIG_HOME/wl_shimeji/umbriel-support.conf`):

```ini
# Ventana en la que las mascotas pueden pararse
ie_mode=largest
```

La variable de entorno tiene prioridad sobre el archivo. Esta opción también
está disponible en `wl-shimeji-gui` → *Ajustes* → *Interacción y
comportamiento* → «Mascotas sobre ventanas (UmbrielSupport)».

Además, wl_shimeji debe tener `ie_interactions=true` en
`shimeji-overlayd.conf`. En wl_shimeji 0.0.2 las acciones IE (`FallWithIe`,
`WalkWithIe`, `ThrowIe`) también comprueban `ie_throwing`, así que déjalo en
`true` si quieres que las mascotas lleguen a la ventana y se muevan por ella.

El plugin elige el socket de Umbriel de esta forma:

1. `UMBRIEL_SOCKET`, si está definido;
2. si no, `$XDG_RUNTIME_DIR/umbriel-$WAYLAND_DISPLAY.sock`;
3. como último recurso, `/run/user/<uid>/umbriel-$WAYLAND_DISPLAY.sock`.

Debe ejecutarse dentro de una sesión Umbriel. En otras sesiones el plugin se
carga pero no recibe eventos.

### Ejemplo de depuración

```sh
# Ver los eventos que publica Umbriel
printf '{"cmd":"subscribe","events":["windows","outputs"]}\n' | socat - "$UMBRIEL_SOCKET"

# Llamar al plugin directamente con un cliente IPC
noctalia msg plugin <entry> <target> ...
```

### Reparar personajes que «no aparecen» (wl_shimeji 0.0.2)

Algunos prototypes convertidos invocan directamente desde un `behavior` una
acción que lee variables locales en su `init`: embebidas (`Fall`, `Dragged`,
`Look`, `Offset`, ...) y acciones `Move` (`walk_left`, `walk_right`,
`climb_left`, `climb_right`, ...). En wl_shimeji 0.0.2 eso provoca un SIGSEGV en
`fall_action_init()` o `move_action_init()`: el primer `actionref` de la mascota
se crea con `overwritten_locals = NULL` y esas acciones lo direferencian. El
overlay se cierra y la mascota nunca llega a dibujarse (o desaparece al elegir
un behavior de movimiento).

`tools/fix-shimeji-embedded-actions` envuelve esas acciones en una `Sequence`,
que es la estructura que ya usan los prototypes que funcionan (Shimeji, Miku,
...). El `behavior` sigue apuntando al mismo nombre. Las acciones originales se
renombran con sufijo `Embedded` o `Inner`.

```sh
# Solo informar: qué personajes tienen el fallo (no modifica nada)
fix-shimeji-embedded-actions --dry-run

# Aplicar a todos los prototypes de ~/.local/share/wl_shimeji/shimejis
fix-shimeji-embedded-actions

# Aplicar solo a un personaje
fix-shimeji-embedded-actions ~/.local/share/wl_shimeji/shimejis/Shimeji.mafuyu-asashina
```

Guarda copias de `actions.json` y `behaviors.json` en
`~/.local/share/wl_shimeji/backups/embedded-actions/<fecha>/` (usa
`--no-backup` para desactivarlo). Es idempotente: si el envoltorio ya existe no
toca el fichero. Después hay que reiniciar `shimeji-overlayd` para que
recargue los prototypes.

Mafuyu también tenía dos problemas adicionales:

- **Aparecía 4 veces más grande**: los sprites eran de 512×512 con ancla
  `(256,512)`, frente a los 128×128 y `(64,128)` habituales.
  `fix-shimeji-sprite-scale` reescala los QOI y ajusta anclas y hotspots.
- **Desaparecía al caer**: la conducta `fall` tenía `frequency=200`, así que se
  elegía al azar y caía con velocidad 10 hasta salir de pantalla. Los
  prototypes que funcionan la tienen a `0` y solo la activan por eventos.
  `fix-shimeji-random-fall` corrige esa frecuencia.

```sh
fix-shimeji-sprite-scale ~/.local/share/wl_shimeji/shimejis/Shimeji.mafuyu-asashina
fix-shimeji-random-fall   ~/.local/share/wl_shimeji/shimejis/Shimeji.mafuyu-asashina
```

Ambos scripts admiten `--dry-run`, backups y son idempotentes. Los tres fixes
también se aplicaron al `.wlshm` de origen, para que una reconversión no los
reintroduzca. Para volver a parchear un `.wlshm` directamente (sin convertirlo
antes) está `tools/fix-wlshm-embedded-actions`:

```sh
fix-wlshm-embedded-actions --dry-run Shimeji.mafuyu-asashina.wlshm
fix-wlshm-embedded-actions            Shimeji.mafuyu-asashina.wlshm
```

Conserva la cabecera WLPK y el resto de entradas del tar, y admite `--dry-run`.

## Limitaciones

- El IPC de Umbriel no expone la posición global del puntero, así que el plugin
  no alimenta `set_cursor_pos_func`. Los comportamientos que siguen al cursor
  necesitan otra fuente.
- `window-move-to` solo funciona sobre ventanas flotantes. En layouts de
  mosaico (scrolling, dwindle, master), Umbriel rechaza el movimiento; las
  mascotas no pueden reordenar esas ventanas.
- Las ventanas sin identificador externo (`id` vacío) no pueden ser objetivo
  de acciones; sí se usan para calcular la ventana activa.

## Estructura

```
src/plugin.c   símbolos exportados y callbacks de wl_shimeji
src/ipc.c      cliente no bloqueante del socket de Umbriel
src/state.c    parseo de snapshots y selección de ventana
tests/         pruebas del parser y cliente IPC simulado
tools/         utilidades para prototypes (acciones embebidas/Move, escala,
               caída y parcheo de .wlshm)
```

## Licencia

GPL-2.0-or-later, igual que wl_shimeji. Ver [LICENSE](LICENSE).
