# retro-de - portable POSIX Makefile (GNU make and BSD make).
#
#   make            build everything into ./bin (creates config.h first),
#                   the theme modules in themes/ and the wallpapers
#   make check      unit tests + full session test (needs libXtst)
#   make install    PREFIX from config.mk, DESTDIR supported
#   make clean
#
# Choose modules and defaults in config.h (copied from config.def.h), build
# settings in config.mk.

include config.mk

LIB     = lib/libretroui.a
LIBOBJS = lib/core.o lib/widgets.o lib/menu.o lib/sys.o lib/audio.o \
          lib/net.o lib/desktop.o lib/config.o lib/themes.o \
          lib/settings.o lib/power.o lib/programs.o lib/bt.o \
          lib/icons.o
MODOBJS = modules/audio_sndio.o modules/audio_pipewire.o \
          modules/audio_pulseaudio.o modules/audio_alsa.o modules/audio_oss.o \
          modules/audio_mixerctl.o modules/net_iwd.o modules/net_wpa.o \
          modules/net_ifconfig.o modules/battery_sysfs.o \
          modules/battery_acpi.o modules/battery_apm.o modules/power_systemd.o \
          modules/power_elogind.o modules/power_consolekit.o \
          modules/power_openbsd.o modules/power_freebsd.o \
          modules/power_netbsd.o modules/power_generic.o modules/bt_bluez.o
PROGOBJS = wm/retro-wm.o bar/retro-bar.o bar/frontpanel.o desk/retro-desk.o \
          desk/image.o settings/retro-settings.o \
          run/retro-run.o net/retro-net.o session/retro-session.o \
          bt/retro-bt.o display/retro-display.o display/randr.o \
          calc/retro-calc.o calc/expr.o files/retro-files.o \
          programs/retro-programs.o about/retro-about.o
TESTOBJS = tests/unit.o tests/testclient.o tests/drive.o
BINS    = bin/retro-wm bin/retro-bar bin/retro-desk bin/retro-run \
          bin/retro-net bin/retro-session bin/retro-settings \
          bin/retro-bt bin/retro-display bin/retro-calc bin/retro-files \
          bin/retro-programs bin/retro-about
TESTBINS = tests/unit tests/testclient tests/drive
# Theme modules: one C file each, loaded at run time.  Add yours here, or
# build it anywhere with `retro-settings --install-theme mytheme.c`.
THEMES  = themes/Hotdog.so themes/Midnight.so themes/Redmond.so \
          themes/Terminal.so themes/Lilac.so
WALLSTAMP = wallpapers/.stamp
LINK    = $(CC) $(CFLAGS) $(LDFLAGS) $(EXPORT_FLAGS)
LIBS    = $(LIB) $(X_LIBS) $(DL_LIBS)
DIRDEFS = -DRETRO_LIBDIR='"$(LIBDIR)"' -DRETRO_DATADIR='"$(DATADIR)"' \
          -DRETRO_INCDIR='"$(INCDIR)"'

all: $(BINS) $(THEMES) $(WALLSTAMP)

config.h:
	cp config.def.h config.h

.SUFFIXES: .c .o .so
.c.o:
	$(CC) $(CFLAGS) $(DIRDEFS) $(X_CFLAGS) -c -o $@ $<
.c.so:
	$(CC) $(CFLAGS) $(SOFLAGS) -Ilib $(X_CFLAGS) -o $@ $<

$(LIBOBJS) $(MODOBJS) $(PROGOBJS) tests/unit.o: lib/retroui.h
$(THEMES): lib/retroui.h
lib/config.o: config.h
desk/retro-desk.o desk/image.o tests/unit.o programs/retro-programs.o: \
   desk/image.h
bar/retro-bar.o bar/frontpanel.o: bar/bar.h
display/retro-display.o display/randr.o tests/unit.o: display/randr.h
calc/retro-calc.o calc/expr.o tests/unit.o: calc/expr.h

$(LIB): $(LIBOBJS) $(MODOBJS)
	rm -f $(LIB)
	$(AR) rcs $(LIB) $(LIBOBJS) $(MODOBJS)

bin/retro-wm: wm/retro-wm.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ wm/retro-wm.o $(LIBS)
bin/retro-bar: bar/retro-bar.o bar/frontpanel.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ bar/retro-bar.o bar/frontpanel.o $(LIBS)
bin/retro-desk: desk/retro-desk.o desk/image.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ desk/retro-desk.o desk/image.o $(LIBS)
bin/retro-run: run/retro-run.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ run/retro-run.o $(LIBS)
bin/retro-net: net/retro-net.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ net/retro-net.o $(LIBS)
bin/retro-bt: bt/retro-bt.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ bt/retro-bt.o $(LIBS)
bin/retro-display: display/retro-display.o display/randr.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ display/retro-display.o display/randr.o $(LIBS)
bin/retro-calc: calc/retro-calc.o calc/expr.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ calc/retro-calc.o calc/expr.o $(LIBS) -lm
bin/retro-files: files/retro-files.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ files/retro-files.o $(LIBS)
bin/retro-programs: programs/retro-programs.o desk/image.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ programs/retro-programs.o desk/image.o $(LIBS)
bin/retro-about: about/retro-about.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ about/retro-about.o $(LIBS)
bin/retro-settings: settings/retro-settings.o desk/image.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ settings/retro-settings.o desk/image.o $(LIBS)
bin/retro-session: session/retro-session.o $(LIB)
	mkdir -p bin
	$(LINK) -o $@ session/retro-session.o $(LIBS)

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
	cp $(BINS) $(DESTDIR)$(BINDIR)/
	cp examples/retro.conf examples/menu.conf examples/desktop.conf \
	   examples/autostart examples/xinitrc examples/example.theme \
	   examples/keys.conf examples/windows.conf examples/programs.conf \
	   $(DESTDIR)$(DATADIR)/
	cp $(THEMES) $(DESTDIR)$(LIBDIR)/themes/
	cp themes/*.c themes/README $(DESTDIR)$(DATADIR)/themes/
	cp lib/retroui.h $(DESTDIR)$(INCDIR)/
	cp wallpapers/*.ppm wallpapers/*.xpm $(DESTDIR)$(DATADIR)/wallpapers/
	if [ -n "$(XSESSIONSDIR)" ]; then \
	   mkdir -p $(DESTDIR)$(XSESSIONSDIR) && \
	   sed 's|@BINDIR@|$(BINDIR)|' session/retro.desktop.in \
	      > $(DESTDIR)$(XSESSIONSDIR)/retro.desktop; fi

uninstall:
	cd $(DESTDIR)$(BINDIR) && rm -f retro-wm retro-bar retro-desk retro-run \
	   retro-net retro-session retro-settings retro-bt retro-display \
	   retro-calc retro-files retro-programs retro-about
	rm -f $(DESTDIR)$(XSESSIONSDIR)/retro.desktop
	rm -rf $(DESTDIR)$(DATADIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)

clean:
	rm -f $(LIBOBJS) $(MODOBJS) $(PROGOBJS) $(TESTOBJS) $(LIB) $(BINS) \
	   $(TESTBINS) $(THEMES) tools/mkwall $(WALLSTAMP)
	rm -f wallpapers/*.ppm wallpapers/*.xpm

.PHONY: all check install uninstall clean
