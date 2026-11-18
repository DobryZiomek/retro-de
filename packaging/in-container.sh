#!/bin/sh
# Runs inside the build container: in-container.sh FORMAT VERSION
# /out holds retro-de-VERSION.tar.gz and receives the package,
# /pkg is this directory.
set -eu
fmt=$1 V=$2 TB=/out/retro-de-$V.tar.gz
J=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

case $fmt in
deb)
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends build-essential \
	    pkg-config libx11-dev libxft-dev libxinerama-dev libxrandr-dev \
	    libxext-dev >/dev/null
	sh /pkg/deb.sh "$TB" /out
	;;
rpm)
	dnf -y -q install rpm-build gcc make pkgconf-pkg-config libX11-devel \
	    libXft-devel libXinerama-devel libXrandr-devel libXext-devel
	mkdir -p ~/rpmbuild/SOURCES
	cp "$TB" ~/rpmbuild/SOURCES/
	sed "s/^Version:.*/Version:        $V/" /pkg/retro-de.spec > /tmp/r.spec
	rpmbuild -bb --define "_smp_mflags -j$J" /tmp/r.spec
	cp ~/rpmbuild/RPMS/*/retro-de-*.rpm /out/
	;;
arch)
	pacman -Syu --noconfirm --needed base-devel libx11 libxft \
	    libxinerama libxrandr libxext >/dev/null
	useradd -m b
	mkdir /home/b/p
	sed "s/^pkgver=.*/pkgver=$V/" /pkg/PKGBUILD > /home/b/p/PKGBUILD
	cp "$TB" /home/b/p/
	chown -R b /home/b/p
	su b -c "cd /home/b/p && MAKEFLAGS=-j$J PKGEXT=.pkg.tar.zst \
	    makepkg --noconfirm --skipchecksums"
	cp /home/b/p/retro-de-$V-*.pkg.tar.zst /out/
	;;
apk)
	apk add -q alpine-sdk libx11-dev libxft-dev libxinerama-dev \
	    libxrandr-dev libxext-dev
	adduser -D b
	addgroup b abuild
	mkdir /home/b/p
	sed "s/^pkgver=.*/pkgver=$V/" /pkg/APKBUILD > /home/b/p/APKBUILD
	cp "$TB" /home/b/p/
	chown -R b /home/b/p
	# a throw-away signing key: install with --allow-untrusted
	su b -c "abuild-keygen -a -n -q" >/dev/null 2>&1
	cp /home/b/.abuild/*.rsa.pub /etc/apk/keys/
	su b -c "cd /home/b/p && abuild checksum && MAKEFLAGS=-j$J abuild -d"
	for a in /home/b/packages/*/*/retro-de-*.apk; do
		cp "$a" "/out/$(basename "$a" .apk)-$(apk --print-arch).apk"
	done
	;;
esac
