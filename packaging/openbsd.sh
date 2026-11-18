#!/bin/sh
# openbsd.sh SRCDIR: builds SRCDIR/../retro-de-VERSION.tgz with pkg_create(1)
# on OpenBSD.  Install it with:  pkg_add -D unsigned ./retro-de-VERSION.tgz
set -eu
cd "$1"
V=$(sed -n 's/^VERSION *= *//p' config.mk)
S=$PWD/stage
make -j"$(sysctl -n hw.ncpuonline)" PREFIX=/usr/local
rm -rf "$S"
make install PREFIX=/usr/local DESTDIR="$S"
find "$S/usr/local/bin" -type f -exec strip {} +   # not the links
echo "Small CDE / Windows 95 style desktop environment for X11:" > desc
echo "window manager, front panel, Style Manager, file manager and tools," >> desc
echo "in plain C on Xlib and Xft.  https://github.com/DobryZiomek/retro-de" >> desc
(cd "$S/usr/local" &&
	# own only our directories, not bin/, share/, lib/ ...
	find . -type d -path '*retro-de*' | sort | sed 's|^\./||; s|$|/|'
	find . ! -type d | sort | sed 's|^\./||') > plist
pkg_create -A "$(uname -m)" \
    -D COMMENT="small CDE-style desktop environment for X11" \
    -D MAINTAINER="DobryZiomek <https://github.com/DobryZiomek/retro-de>" \
    -D FULLPKGPATH=x11/retro-de -D FULLPKGNAME="retro-de-$V" \
    -d desc -f plist -B "$S" -p /usr/local "../retro-de-$V.tgz"

# the optional package: retro-dbus
S2=$PWD/stage-dbus
rm -rf "$S2"
make install-dbus PREFIX=/usr/local DESTDIR="$S2"
find "$S2/usr/local/bin" -type f -exec strip {} +
echo "D-Bus session bus stand-in for retro-de: programs that need a" > desc2
echo "session bus work without dbus; notifications and portals go to" >> desc2
echo "retro-de.  https://github.com/DobryZiomek/retro-de" >> desc2
(cd "$S2/usr/local" && find . ! -type d | sort | sed 's|^\./||') > plist2
pkg_create -A "$(uname -m)" \
    -D COMMENT="D-Bus session bus stand-in for retro-de" \
    -D MAINTAINER="DobryZiomek <https://github.com/DobryZiomek/retro-de>" \
    -D FULLPKGPATH=x11/retro-de,-dbus -D FULLPKGNAME="retro-de-dbus-$V" \
    -P "x11/retro-de:retro-de-$V:retro-de-$V" \
    -d desc2 -f plist2 -B "$S2" -p /usr/local "../retro-de-dbus-$V.tgz"
