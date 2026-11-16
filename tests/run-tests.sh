#!/bin/sh
# retro-de test runner: unit tests, then a full session on a throw-away X
# server driven by tests/drive (XTest input + ICCCM/EWMH assertions).
#
# Environment:
#   RETRO_TEST_DISPLAY  use this already running X server (e.g. :77)
#   RETRO_BIN           directory with the retro-* binaries   (default ./bin)
#   RETRO_TESTBIN       directory with unit/drive/testclient  (default ./tests)
#   RETRO_TEST_DIR      scratch directory                     (default tests/out)
#
# Without RETRO_TEST_DISPLAY the script starts Xvfb, or - if Xvfb is not
# installed but a Wayland session is running - a rootful Xwayland.

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${RETRO_BIN:-$ROOT/bin}
TB=${RETRO_TESTBIN:-$ROOT/tests}
WORK=${RETRO_TEST_DIR:-$ROOT/tests/out}
XPID=
FAILED=0

say() { printf '%s\n' "$*"; }
fail() { say "FAIL: $*"; FAILED=$((FAILED + 1)); }

cleanup() {
	[ -n "$XPID" ] && kill "$XPID" 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

rm -rf "$WORK"
mkdir -p "$WORK/config" "$WORK/markers" "$WORK/pathbin" "$WORK/shots" \
	"$WORK/logs" "$WORK/img" || exit 2

# ---- fixtures ---------------------------------------------------------------

# 2x2 binary PPM: red, green / blue, white
printf 'P6\n2 2\n255\n\377\000\000\000\377\000\000\000\377\377\377\377' \
	> "$WORK/img/test-p6.ppm"
printf 'P3\n# a comment\n2 1\n# another\n2\n2 1 0   0 0 2\n' \
	> "$WORK/img/test-p3.ppm"
printf 'P6\n64 64\n255\nshort' > "$WORK/img/broken.ppm"
cat > "$WORK/img/test.xpm" <<'EOF'
/* XPM */
static char *test[] = {
/* columns rows colors chars-per-pixel */
"4 2 4 1",
"  c None",
". c #FF0000",
"b c blue",
"y s accent c yellow m white",
" .by",
"y.b "
};
EOF
# 16x16 tiled wallpaper and a 32x32 icon, both XPM
{
	echo '/* XPM */'
	echo 'static char *wall[] = {'
	echo '"16 16 2 1",'
	echo '"a c #008080",'
	echo '"b c #007070",'
	i=0
	while [ $i -lt 16 ]; do
		if [ $((i % 2)) -eq 0 ]; then echo '"abababababababab",'
		else echo '"babababababababa",'; fi
		i=$((i + 1))
	done
	echo '};'
} > "$WORK/img/wall.xpm"
{
	echo '/* XPM */'
	echo 'static char *icon[] = {'
	echo '"32 32 3 1",'
	echo '"  c None",'
	echo '"o c #202020",'
	echo '"x c #f0c020",'
	i=0
	while [ $i -lt 32 ]; do
		if [ $i -lt 4 ] || [ $i -ge 28 ]; then
			echo '"                                ",'
		elif [ $i -eq 4 ] || [ $i -eq 27 ]; then
			echo '"    oooooooooooooooooooooooo    ",'
		else
			echo '"    oxxxxxxxxxxxxxxxxxxxxxxo    ",'
		fi
		i=$((i + 1))
	done
	echo '};'
} > "$WORK/img/icon.xpm"

cat > "$WORK/config/retro.conf" <<EOF
# test configuration: Classic look, no D-Bus, no XDG autostart, and power
# actions disabled so no test can ever shut the machine down
theme = Classic
style = classic
dbus = off
power_backend = none
workspaces = 4
wallpaper = $WORK/img/wall.xpm
wallpaper_mode = tile
clock_format = %H:%M
bind = Mod4+x exec touch $WORK/markers/bind-marker
bind = Mod4+y exec @app zzmarker
block = probe 2 touch $WORK/markers/block-marker; echo probe-ok
applets = cpu mem probe battery net dnd awake volume clock
EOF
# settings split by subject: these files are read next to retro.conf
cat > "$WORK/config/keys.conf" <<EOF
bind = Mod4+z exec touch $WORK/markers/keys-marker
EOF
cat > "$WORK/config/programs.conf" <<EOF
# how a program's windows start
rule = title=Ruled => workspace=3 geometry=260x170+40+50 above nofocus
rule = class=NoSuchProgram => fullscreen
EOF
cat > "$WORK/config/menu.conf" <<EOF
menu "Programs" {
    item "Terminal" @terminal
    menu "Nested" {
        item "Nothing" true
    }
}
item "Marker" touch $WORK/markers/menu-marker
separator
item "Run..." retro-run
item "Log Out" @quit
applications "Applications"
EOF
cat > "$WORK/config/desktop.conf" <<EOF
icon "Marker" terminal touch $WORK/markers/icon-marker
icon "Documents" folder true
icon "Readme" file true
icon "Custom XPM" $WORK/img/icon.xpm true
icon "Computer" computer true
icon "Network" network retro-net
app zzmarker
EOF
# freedesktop applications (isolated from the host's)
mkdir -p "$WORK/xdg/home/applications" "$WORK/xdg/sys/pixmaps"
cat > "$WORK/xdg/home/applications/menutest.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Menu Test
Exec=touch "$WORK/markers/menu-app-marker" %F
Categories=Utility;
EOF
cat > "$WORK/xdg/home/applications/zzmarker.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Zz Marker
Exec=touch "$WORK/markers/app-marker" %U
Icon=zzicon
Categories=Utility;
EOF
cp "$WORK/img/icon.xpm" "$WORK/xdg/sys/pixmaps/zzicon.xpm"
XDG_DATA_HOME=$WORK/xdg/home
XDG_DATA_DIRS=$WORK/xdg/sys
XDG_CONFIG_HOME=$WORK/xdg/cfg
XDG_CONFIG_DIRS=$WORK/xdg/none
export XDG_DATA_HOME XDG_DATA_DIRS XDG_CONFIG_HOME XDG_CONFIG_DIRS
# theme modules and wallpapers straight from the build tree
RETRO_DE_THEME_PATH=$ROOT/themes
RETRO_DE_DATA_DIR=$ROOT
export RETRO_DE_THEME_PATH RETRO_DE_DATA_DIR
printf '#!/bin/sh\ntouch "%s/markers/run-marker"\n' "$WORK" \
	> "$WORK/pathbin/zzretromarker"
chmod +x "$WORK/pathbin/zzretromarker"
printf 'touch "%s/markers/autostart-marker"\n' "$WORK" > "$WORK/config/autostart"

# second scenario: the default CDE look with D-Bus and XDG autostart on
mkdir -p "$WORK/config-cde" "$WORK/xdg/cfg/autostart"
cat > "$WORK/config-cde/retro.conf" <<EOF
panel_style = auto
power_backend = none
dbus = auto
xdg_autostart = on
locker = touch $WORK/markers/lock-marker
filemanager = touch $WORK/markers/files-marker
EOF
cp "$WORK/config/menu.conf" "$WORK/config-cde/menu.conf"
cat > "$WORK/config-cde/autostart" <<EOF
printf '%s\n' "\$DBUS_SESSION_BUS_ADDRESS" > "$WORK/markers/dbus-address"
dbus-send --bus="\$DBUS_SESSION_BUS_ADDRESS" --print-reply \
	--dest=org.freedesktop.DBus / org.freedesktop.DBus.ListNames \
	>/dev/null 2>&1 && touch "$WORK/markers/dbus-ok"
EOF
cat > "$WORK/xdg/cfg/autostart/zzauto.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Autostart test
Exec=touch "$WORK/markers/xdg-autostart-marker"
EOF
cat > "$WORK/xdg/cfg/autostart/disabled.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Disabled
Exec=touch "$WORK/markers/xdg-disabled-marker"
Hidden=true
EOF

for b in retro-wm retro-bar retro-desk retro-run retro-net retro-session \
    retro-settings retro-bt retro-display retro-calc retro-files \
    retro-programs retro-about; do
	[ -x "$BIN/$b" ] || { say "missing binary $BIN/$b (run make first)"; exit 2; }
done

# ---- X server ---------------------------------------------------------------

if [ -n "${RETRO_TEST_DISPLAY:-}" ]; then
	DISPLAY=$RETRO_TEST_DISPLAY
	say "using existing X server $DISPLAY"
else
	n=97
	while [ -e "/tmp/.X11-unix/X$n" ] && [ $n -lt 200 ]; do n=$((n + 1)); done
	DISPLAY=:$n
	if command -v Xvfb >/dev/null 2>&1; then
		Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp \
			> "$WORK/logs/xserver.log" 2>&1 &
		XPID=$!
		say "started Xvfb on $DISPLAY"
	elif command -v Xwayland >/dev/null 2>&1 && [ -n "${WAYLAND_DISPLAY:-}" ]; then
		Xwayland "$DISPLAY" -geometry 1280x800 -nolisten tcp -noreset \
			> "$WORK/logs/xserver.log" 2>&1 &
		XPID=$!
		say "Xvfb not found; started rootful Xwayland on $DISPLAY"
	else
		say "no Xvfb (or Xwayland) available and RETRO_TEST_DISPLAY not set"
		exit 2
	fi
	i=0
	while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $i -lt 10 ]; do
		sleep 1
		i=$((i + 1))
	done
fi
export DISPLAY

# ---- unit tests -------------------------------------------------------------

say "== unit tests =="
"$TB/unit" "$WORK/img" 2> "$WORK/logs/unit.err"
rc=$?
[ $rc -eq 0 ] || fail "unit tests: $rc check(s) failed"
if grep -E 'Sanitizer|runtime error' "$WORK/logs/unit.err" >/dev/null; then
	fail "sanitizer report in unit tests (see $WORK/logs/unit.err)"
fi

# ---- full sessions ----------------------------------------------------------

# run_session NAME CONFIG-DIR DRIVER-MODE: start retro-session, drive it,
# then check that it ended cleanly and logged no X protocol errors
run_session() {
	name=$1
	cfg=$2
	mode=$3
	LOG="$WORK/logs/session-$name.log"
	(
		[ "$mode" = cde ] && unset DBUS_SESSION_BUS_ADDRESS
		exec env RETRO_DE_CONFIG_DIR="$cfg" RETRO_DEBUG=1 \
			PATH="$WORK/pathbin:$BIN:$PATH" "$BIN/retro-session"
	) > "$LOG" 2>&1 &
	SPID=$!
	RETRO_DE_CONFIG_DIR="$cfg" PATH="$BIN:$PATH" \
		"$TB/drive" "$TB/testclient" "$WORK/markers" "$WORK/shots" "$mode"
	rc=$?
	[ $rc -eq 0 ] || fail "$name session: $rc check(s) failed"
	# the driver ends with the quit command: the session must end by itself
	i=0
	while kill -0 "$SPID" 2>/dev/null && [ $i -lt 10 ]; do
		sleep 1
		i=$((i + 1))
	done
	if kill -0 "$SPID" 2>/dev/null; then
		fail "$name: retro-session did not exit after the quit command"
		kill "$SPID" 2>/dev/null
	fi
	wait "$SPID"
	src=$?
	[ $src -eq 0 ] || fail "$name: retro-session exit status $src"
	for comp in retro-wm retro-bar retro-desk; do
		grep "^$comp: exiting, 0 X errors" "$LOG" >/dev/null ||
			fail "$name: $comp did not report a clean exit (see $LOG)"
	done
	nerr=$(grep -c 'X error:' "$LOG")
	[ "$nerr" -eq 0 ] || fail "$name: $nerr X protocol error(s) (see $LOG)"
	if grep -E 'Sanitizer|runtime error' "$LOG" >/dev/null; then
		fail "$name: sanitizer report (see $LOG)"
	fi
	if grep -E 'died|keeps dying' "$LOG" >/dev/null; then
		fail "$name: a component crashed (see $LOG)"
	fi
}

say "== session test: Classic style, no D-Bus, no XDG autostart =="
run_session classic "$WORK/config" classic
[ -e "$WORK/markers/autostart-marker" ] || fail "autostart script did not run"
[ ! -e "$WORK/markers/xdg-autostart-marker" ] ||
	fail "XDG autostart ran although it is switched off"
[ ! -e "$WORK/markers/dbus-address" ] || fail "unexpected D-Bus marker"

say "== session test: CDE style, D-Bus auto, XDG autostart on =="
run_session cde "$WORK/config-cde" cde
[ -e "$WORK/markers/xdg-autostart-marker" ] || fail "XDG autostart did not run"
[ ! -e "$WORK/markers/xdg-disabled-marker" ] ||
	fail "a Hidden=true autostart entry was started"
if command -v dbus-daemon >/dev/null 2>&1; then
	addr=$(cat "$WORK/markers/dbus-address" 2>/dev/null)
	case $addr in
	unix:*|tcp:*) say "  ok    session started its own D-Bus bus" ;;
	*) fail "no D-Bus session bus was started (got '$addr')" ;;
	esac
	[ -e "$WORK/markers/dbus-ok" ] || fail "the session bus did not answer"
	sleep 1
	if dbus-send --bus="$addr" --print-reply --dest=org.freedesktop.DBus / \
	    org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
		fail "the session bus is still running after logout"
	else
		say "  ok    the session bus was stopped at logout"
	fi
else
	say "  skip  D-Bus checks (dbus-daemon not installed)"
fi

say "== summary =="
if [ $FAILED -eq 0 ]; then
	say "all tests passed (logs and screenshots in $WORK)"
	exit 0
fi
say "$FAILED problem(s) - see $WORK/logs"
exit 1
