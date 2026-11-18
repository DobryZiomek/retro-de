# retro-de - portable POSIX Makefile (GNU make and BSD make).
#
#   make            build everything into ./bin (creates config.h first),
#                   the theme modules in themes/ and the wallpapers
#   make check      unit tests + full session test (needs libXtst)
#   make install    PREFIX from config.mk, DESTDIR supported
#   make install-dbus   the optional D-Bus stand-in (retro-dbus)
#   make clean
#
# Choose modules and defaults in config.h (copied from config.def.h), build
# settings in config.mk.

include config.mk

LIB     = lib/libretroui.a
LIBOBJS = lib/core.o lib/widgets.o lib/menu.o lib/sys.o lib/audio.o \
          lib/net.o lib/desktop.o lib/config.o lib/themes.o \
          lib/settings.o lib/power.o lib/programs.o lib/bt.o \
          lib/icons.o lib/bus.o lib/png.o lib/svg.o
MODOBJS = modules/audio_sndio.o modules/audio_pipewire.o \
          modules/audio_pulseaudio.o modules/audio_alsa.o modules/audio_oss.o \
          modules/audio_mixerctl.o modules/net_iwd.o modules/net_wpa.o \
          modules/net_ifconfig.o modules/battery_sysfs.o \
          modules/battery_acpi.o modules/battery_apm.o modules/power_systemd.o \
          modules/power_elogind.o modules/power_consolekit.o \
          modules/power_openbsd.o modules/power_freebsd.o \
          modules/power_netbsd.o modules/power_generic.o modules/bt_bluez.o
# Every program is linked into one executable, bin/retro; bin/retro-wm,
# bin/retro-bar ... are links to it (see main/retro.c).  Each program's
# main() is renamed while compiling (the rules below).
MAINOBJS = wm/retro-wm.o bar/retro-bar.o desk/retro-desk.o run/retro-run.o \
          net/retro-net.o session/retro-session.o settings/retro-settings.o \
          bt/retro-bt.o display/retro-display.o calc/retro-calc.o \
          files/retro-files.o programs/retro-programs.o about/retro-about.o \
          bus/retro-bus.o bus/retro-msg.o pkg/retro-pkg.o \
          wallpaper/retro-wallpaper.o
PROGOBJS = $(MAINOBJS) bar/frontpanel.o bar/notify.o desk/image.o \
          display/randr.o calc/expr.o main/retro.o
# the optional package: the D-Bus stand-in (make install-dbus)
DBUSOBJS = dbus/retro-dbus.o dbus/wire.o dbus/services.o
TESTOBJS = tests/unit.o tests/testclient.o tests/drive.o
NAMES   = retro-wm retro-bar retro-desk retro-run retro-net retro-session \
          retro-settings retro-bt retro-display retro-calc retro-files \
          retro-programs retro-about retro-bus retro-msg retro-pkg \
          retro-wallpaper
TESTBINS = tests/unit tests/testclient tests/drive
# Theme modules: one C file each, loaded at run time.  Add yours here, or
# build it anywhere with `retro-settings --install-theme mytheme.c`.
THEMES  = themes/Hotdog.so themes/Midnight.so themes/Redmond.so \
          themes/Terminal.so themes/Lilac.so
WALLSTAMP = wallpapers/.stamp
LINK    = $(CC) $(CFLAGS) $(LDFLAGS) $(EXPORT_FLAGS)
LIBS    = $(LIB) $(X_LIBS) $(DL_LIBS) -lm
DIRDEFS = -DRETRO_LIBDIR='"$(LIBDIR)"' -DRETRO_DATADIR='"$(DATADIR)"' \
          -DRETRO_INCDIR='"$(INCDIR)"'

all: bin/retro bin/retro-dbus $(THEMES) $(WALLSTAMP)

config.h:
	cp config.def.h config.h

.SUFFIXES: .c .o .so
.c.o:
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -c -o $@ $<
.c.so:
	$(CC) $(CFLAGS) $(SOFLAGS) -Ilib $(X_CFLAGS) -o $@ $<

$(LIBOBJS) $(MODOBJS) $(PROGOBJS) tests/unit.o: lib/retroui.h
$(MAINOBJS): config.mk
$(THEMES): lib/retroui.h
lib/config.o: config.h
desk/retro-desk.o desk/image.o tests/unit.o programs/retro-programs.o \
   wallpaper/retro-wallpaper.o: \
   desk/image.h
bar/retro-bar.o bar/frontpanel.o bar/notify.o: bar/bar.h desk/image.h
display/retro-display.o display/randr.o tests/unit.o: display/randr.h
calc/retro-calc.o calc/expr.o tests/unit.o: calc/expr.h

$(LIB): $(LIBOBJS) $(MODOBJS)
	rm -f $(LIB)
	$(AR) rcs $(LIB) $(LIBOBJS) $(MODOBJS)

wm/retro-wm.o: wm/retro-wm.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_wm_main -c -o $@ wm/retro-wm.c
bar/retro-bar.o: bar/retro-bar.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_bar_main -c -o $@ bar/retro-bar.c
desk/retro-desk.o: desk/retro-desk.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_desk_main -c -o $@ desk/retro-desk.c
run/retro-run.o: run/retro-run.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_run_main -c -o $@ run/retro-run.c
net/retro-net.o: net/retro-net.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_net_main -c -o $@ net/retro-net.c
session/retro-session.o: session/retro-session.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_session_main -c -o $@ session/retro-session.c
settings/retro-settings.o: settings/retro-settings.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_settings_main -c -o $@ settings/retro-settings.c
bt/retro-bt.o: bt/retro-bt.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_bt_main -c -o $@ bt/retro-bt.c
display/retro-display.o: display/retro-display.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_display_main -c -o $@ display/retro-display.c
calc/retro-calc.o: calc/retro-calc.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_calc_main -c -o $@ calc/retro-calc.c
files/retro-files.o: files/retro-files.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_files_main -c -o $@ files/retro-files.c
programs/retro-programs.o: programs/retro-programs.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_programs_main -c -o $@ programs/retro-programs.c
about/retro-about.o: about/retro-about.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_about_main -c -o $@ about/retro-about.c
bus/retro-bus.o: bus/retro-bus.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_bus_main -c -o $@ bus/retro-bus.c
bus/retro-msg.o: bus/retro-msg.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_msg_main -c -o $@ bus/retro-msg.c
pkg/retro-pkg.o: pkg/retro-pkg.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_pkg_main -c -o $@ pkg/retro-pkg.c
wallpaper/retro-wallpaper.o: wallpaper/retro-wallpaper.c
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -Dmain=retro_wallpaper_main -c -o $@ wallpaper/retro-wallpaper.c

bin/retro: $(PROGOBJS) $(LIB)
	mkdir -p bin
	$(LINK) -o $@ $(PROGOBJS) $(LIBS)
	for n in $(NAMES); do ln -sf retro bin/$$n; done
bin/retro-dbus: $(DBUSOBJS) $(LIB)
	mkdir -p bin
	$(LINK) -o $@ $(DBUSOBJS) $(LIBS)
$(DBUSOBJS): lib/retroui.h dbus/dbus.h

# the wallpapers are drawn by a small C program, not shipped as binaries
tools/mkwall: tools/mkwall.c
	$(CC) $(CFLAGS) -o $@ tools/mkwall.c -lm
$(WALLSTAMP): tools/mkwall
	mkdir -p wallpapers
	tools/mkwall wallpapers $(WALL_W) $(WALL_H)
	touch $(WALLSTAMP)

tests/unit: tests/unit.o desk/image.o display/randr.o calc/expr.o $(LIB)
	$(LINK) -o $@ tests/unit.o desk/image.o display/randr.o calc/expr.o \
	   $(LIBS) -lm
tests/testclient: tests/testclient.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/testclient.o $(X_LIBS)
tests/drive: tests/drive.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/drive.o $(XTST_LIBS) $(X_LIBS)

check: all $(TESTBINS)
	sh tests/run-tests.sh

install: all
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/wallpapers \
	   $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(LIBDIR)/themes \
	   $(DESTDIR)$(INCDIR)
	cp bin/retro $(DESTDIR)$(BINDIR)/
	cd $(DESTDIR)$(BINDIR) && for n in $(NAMES); do rm -f $$n; \
	   ln -s retro $$n; done
	cp examples/retro.conf examples/menu.conf examples/desktop.conf \
	   examples/autostart examples/xinitrc examples/example.theme \
	   examples/keys.conf examples/windows.conf examples/programs.conf \
	   $(DESTDIR)$(DATADIR)/
	cp $(THEMES) $(DESTDIR)$(LIBDIR)/themes/
	cp themes/*.c themes/README $(DESTDIR)$(DATADIR)/themes/
	cp lib/retroui.h $(DESTDIR)$(INCDIR)/
	cp wallpapers/*.png wallpapers/*.xpm $(DESTDIR)$(DATADIR)/wallpapers/
	mkdir -p $(DESTDIR)$(DATADIR)/packages
	cp -R examples/packages/Sunset $(DESTDIR)$(DATADIR)/packages/
	if [ -n "$(XSESSIONSDIR)" ]; then \
	   mkdir -p $(DESTDIR)$(XSESSIONSDIR) && \
	   sed 's|@BINDIR@|$(BINDIR)|' session/retro.desktop.in \
	      > $(DESTDIR)$(XSESSIONSDIR)/retro.desktop; fi

# the optional package: retro-dbus, a D-Bus session bus stand-in
install-dbus: bin/retro-dbus
	mkdir -p $(DESTDIR)$(BINDIR)
	cp bin/retro-dbus $(DESTDIR)$(BINDIR)/

uninstall-dbus:
	rm -f $(DESTDIR)$(BINDIR)/retro-dbus

uninstall: uninstall-dbus
	cd $(DESTDIR)$(BINDIR) && rm -f retro $(NAMES)
	rm -f $(DESTDIR)$(XSESSIONSDIR)/retro.desktop
	rm -rf $(DESTDIR)$(DATADIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)

clean:
	rm -f $(LIBOBJS) $(MODOBJS) $(PROGOBJS) $(DBUSOBJS) $(TESTOBJS) $(LIB) \
	   bin/retro bin/retro-dbus \
	   $(TESTBINS) $(THEMES) tools/mkwall $(WALLSTAMP)
	rm -f wallpapers/*.png wallpapers/*.ppm wallpapers/*.xpm
	for n in $(NAMES); do rm -f bin/$$n; done

.PHONY: all check install uninstall clean install-dbus uninstall-dbus
