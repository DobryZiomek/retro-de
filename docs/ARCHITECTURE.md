# Architecture

retro-de is a suite of small, separate X11 programs that share one toolkit.
There is no required runtime dependency beyond Xlib, Xft, Xinerama, Xrandr
and Xext. In place of D-Bus there is `retro-bus`, a small line-based
message bus on a Unix socket ([BUS.md](BUS.md)); everything works without
it, it only adds the ways programs can reach each other.

```
retro-session ──supervises──▶ retro-bus     message bus (Unix socket)
                              retro-dbus    D-Bus stand-in (optional package)
                              retro-wm      window manager (ICCCM/EWMH)
                              retro-bar     taskbar / CDE front panel
                              retro-desk    backdrop + desktop icons
                              (autostart)   anything in ~/.config autostart

retro-settings, retro-files, retro-calc, retro-run, retro-net, retro-bt,
retro-display, retro-programs, retro-about     ordinary X clients
retro-msg                                      shell client of the bus
retro-wallpaper                                wallpaper chooser (CRT preview)
retro-pkg                                      theme packages (.rtp)

            all of them link ──▶ libretroui.a   (lib/)
```

## Source layout

| Path | Contents |
|------|----------|
| `lib/` | `libretroui.a`: core event loop and drawing (`core.c`), widgets, menus, themes, icons, desktop-entry parsing, config files, plus thin front ends for audio, network, Bluetooth, power |
| `lib/retroui.h` | The single public header; must be the first include of every translation unit |
| `modules/` | One file per back end (`audio_*`, `net_*`, `bt_*`, `battery_*`, `power_*`). Each drives a CLI tool or kernel interface and registers a `Rui*Module` struct |
| `themes/` | Theme modules, built as `.so` and loaded with `dlopen(3)`. See `themes/README` |
| `wm/ bar/ desk/ ...` | One directory per program, each a single `retro-*.c` |
| `bus/` | `retro-bus` (the broker) and `retro-msg` (its command-line client); the client library is `lib/bus.c` |
| `dbus/` | `retro-dbus`, the optional D-Bus session bus: `wire.c` (marshalling), `retro-dbus.c` (the bus), `services.c` (notifications and portals) |
| `pkg/`, `wallpaper/` | `retro-pkg` (theme packages) and `retro-wallpaper` |
| `lib/png.c`, `lib/svg.c` | inflate, gzip, a PNG decoder and an SVG rasteriser: images without libpng, zlib or librsvg |
| `tools/mkwall.c` | Generates the bundled wallpapers at build time |
| `examples/` | Annotated sample configuration files |
| `tests/` | Unit tests and the XTest-driven session tests |
| `config.def.h` | Default build configuration (dwm style); `./configure` writes `config.h` |

## Design rules

- **Optional means optional.** D-Bus, XDG menus/autostart and audio servers
  are used when present and silently skipped when not. So is retro-bus:
  every `rui_bus_*` call returns quietly without it.
- **Icons without libraries.** Pictograms are drawn in code. Programs' own
  icons come from `_NET_WM_ICON` and are learned as XPM files
  (`lib/icons.c`). PNG and SVG are decoded by `lib/png.c` and
  `lib/svg.c`.
- **Modules are probed at run time** in the order listed in `config.h`, so
  one binary can serve several init systems or audio stacks.
- **Back ends shell out.** No module links a library; this keeps the build
  portable across Linux and the BSDs.
- **Plain text configuration**, split into small files (`retro.conf`,
  `menu.conf`, `programs.conf`, `keys.conf`, `windows.conf`, `desktop.conf`).
- **Portable C.** C11 / POSIX.1-2008, warning-free with
  `-Wall -Wextra -pedantic` under gcc and clang, GNU or BSD make.

## Adding a back end

1. Copy `modules/audio_alsa.c` (about 50 lines) as a template.
2. Implement the `Rui*Module` callbacks and add the module to `MODOBJS` in
   the `Makefile`.
3. Add it to the module list in `config.def.h` and to `./configure`.
4. Add a parsing test against captured tool output in `tests/unit.c`.

## Adding a theme

Copy a file from `themes/`, change the palette and metrics, then run
`make`. The Style Manager (`retro-settings`) lists and installs theme
modules.
