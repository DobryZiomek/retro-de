#!/bin/sh
# deb.sh TARBALL OUTDIR: builds retro-de_VERSION-1_ARCH.deb with dpkg-deb
# (no debhelper needed; dependencies come from dpkg-shlibdeps).
set -eu
TB=$1 OUT=$2
W=$(mktemp -d)
tar -xzf "$TB" -C "$W"
cd "$W"/retro-de-*
V=$(sed -n 's/^VERSION *= *//p' config.mk)
R=$W/root
make -j"$(getconf _NPROCESSORS_ONLN)" PREFIX=/usr
make install PREFIX=/usr DESTDIR="$R"
D=$W/root-dbus
make install-dbus PREFIX=/usr DESTDIR="$D"
strip "$R"/usr/bin/* "$D"/usr/bin/*
strip --strip-unneeded "$R"/usr/lib/retro-de/themes/*.so
mkdir -p "$R/usr/share/doc/retro-de"
cp LICENSE "$R/usr/share/doc/retro-de/copyright"
cp README.md "$R/usr/share/doc/retro-de/"

mkdir debian "$R/DEBIAN"
echo "Source: retro-de" > debian/control
DEPS=$(dpkg-shlibdeps -O "$R"/usr/bin/* | sed 's/^shlibs:Depends=//')
cat > "$R/DEBIAN/control" <<CTL
Package: retro-de
Version: $V-1
Architecture: $(dpkg --print-architecture)
Maintainer: DobryZiomek <https://github.com/DobryZiomek/retro-de>
Installed-Size: $(du -sk "$R/usr" | cut -f1)
Depends: $DEPS
Recommends: fonts-dejavu-core, xinit
Suggests: gcc, xscreensaver, xdg-utils, retro-de-dbus
Section: x11
Priority: optional
Homepage: https://github.com/DobryZiomek/retro-de
Description: small CDE / Windows 95 style desktop environment for X11
 retro-de is a window manager, front panel, desktop icons, Style Manager,
 file manager, program groups, calculator and network / Bluetooth /
 display tools, written in plain C on Xlib and Xft.  It needs no D-Bus,
 no toolkit and no particular init system.
CTL
dpkg-deb --root-owner-group -Zxz --build "$R" \
    "$OUT/retro-de_${V}-1_$(dpkg --print-architecture).deb"

# the optional package: retro-dbus
mkdir -p "$D/DEBIAN" "$D/usr/share/doc/retro-de-dbus"
cp LICENSE "$D/usr/share/doc/retro-de-dbus/copyright"
DDEPS=$(dpkg-shlibdeps -O "$D"/usr/bin/* | sed 's/^shlibs:Depends=//')
cat > "$D/DEBIAN/control" <<CTL
Package: retro-de-dbus
Version: $V-1
Architecture: $(dpkg --print-architecture)
Maintainer: DobryZiomek <https://github.com/DobryZiomek/retro-de>
Installed-Size: $(du -sk "$D/usr" | cut -f1)
Depends: retro-de (= $V-1), $DDEPS
Section: x11
Priority: optional
Homepage: https://github.com/DobryZiomek/retro-de
Description: D-Bus session bus stand-in for retro-de
 retro-dbus speaks the D-Bus protocol, so programs that need a session
 bus work without dbus installed.  Desktop notifications and the file
 chooser / open-URI portals are answered by retro-de itself.
CTL
dpkg-deb --root-owner-group -Zxz --build "$D" \
    "$OUT/retro-de-dbus_${V}-1_$(dpkg --print-architecture).deb"
