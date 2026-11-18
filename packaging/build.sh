#!/bin/sh
# Build release packages into dist/ with docker (Linux formats) and,
# optionally, an OpenBSD VM reachable over ssh.
#
#   sh packaging/build.sh                 source tarball + deb rpm arch apk
#   sh packaging/build.sh deb rpm         only these
#   OBSD_SSH="ssh -p 2399 root@127.0.0.1" sh packaging/build.sh openbsd
#
# Formats: src deb rpm arch apk openbsd.  Each package is built from the
# source tarball, so do `src` first (the default list does).
set -eu
cd "$(dirname "$0")/.."
V=$(sed -n 's/^VERSION *= *//p' config.mk)
OUT=$PWD/dist
mkdir -p "$OUT"

# image each Linux format is built in: the oldest still-working release,
# so the binaries run on that release and everything newer
img() {
	case $1 in
	deb)  echo ubuntu:20.04 ;;
	rpm)  echo almalinux:8 ;;
	arch) echo archlinux:latest ;;
	apk)  echo alpine:3.20 ;;
	esac
}

for f in ${*:-src deb rpm arch apk}; do
	echo "=== $f"
	case $f in
	src)
		git ls-files -co --exclude-standard | grep -v '^dist/' |
		    tar -cz --transform "s|^|retro-de-$V/|" -T - \
		    -f "$OUT/retro-de-$V.tar.gz" ;;
	deb|rpm|arch|apk)
		docker run --rm -v "$OUT:/out" -v "$PWD/packaging:/pkg:ro" \
		    "$(img "$f")" sh /pkg/in-container.sh "$f" "$V"
		docker run --rm -v "$OUT:/out" alpine:3.20 \
		    chown -R "$(id -u):$(id -g)" /out ;;
	openbsd)
		: "${OBSD_SSH:?set OBSD_SSH to the ssh command for the VM}"
		$OBSD_SSH "rm -rf /tmp/rpkg && mkdir /tmp/rpkg" </dev/null
		$OBSD_SSH "tar -xzf - -C /tmp/rpkg" < "$OUT/retro-de-$V.tar.gz"
		$OBSD_SSH "sh -s /tmp/rpkg/retro-de-$V" < packaging/openbsd.sh
		$OBSD_SSH "cat /tmp/rpkg/retro-de-$V.tgz" \
		    > "$OUT/retro-de-$V-openbsd-$($OBSD_SSH 'uname -r')-$($OBSD_SSH 'uname -m').tgz" ;;
	*)
		echo "unknown format $f" >&2; exit 2 ;;
	esac
done
cd "$OUT" && ls -l && sha256sum ./*.* | grep -v SHA256SUMS > SHA256SUMS
