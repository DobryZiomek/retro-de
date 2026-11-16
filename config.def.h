/*
 * retro-de compile-time configuration.
 *
 * Works like dwm: `make` copies this file to config.h the first time (or run
 * ./configure for menus that write it for you); edit config.h and rebuild.
 * Modules left out of the lists below are not linked into the programs at
 * all.  Most values are only defaults that each user can still change in
 * ~/.config/retro-de/retro.conf or with the Style Manager.
 */

/* ===================================================================
 * Choices made by ./configure.  Keep each on one line: the installer
 * rewrites them in place.  Module lists end with a comma.
 * =================================================================== */

/* Audio (probed in order, the first that answers wins):
 *   &rui_audio_sndio       sndio, OpenBSD           (sndioctl)
 *   &rui_audio_pipewire    PipeWire                 (wpctl)
 *   &rui_audio_pulseaudio  PulseAudio/pipewire-pulse (pactl)
 *   &rui_audio_alsa        ALSA                     (amixer)
 *   &rui_audio_oss         FreeBSD OSS mixer        (mixer)
 *   &rui_audio_mixerctl    NetBSD audio(4)          (mixerctl)          */
#define AUDIO_MODULES &rui_audio_sndio, &rui_audio_pipewire, &rui_audio_pulseaudio, &rui_audio_alsa, &rui_audio_oss, &rui_audio_mixerctl,

/* Wi-Fi:  &rui_net_iwd (iwctl)  &rui_net_ifconfig (OpenBSD)
 *         &rui_net_wpa (wpa_cli, any system)                            */
#define NET_MODULES &rui_net_iwd, &rui_net_ifconfig, &rui_net_wpa,

/* Bluetooth:  &rui_bt_bluez (bluetoothctl)                              */
#define BT_MODULES &rui_bt_bluez,

/* Battery: &rui_battery_sysfs (Linux) &rui_battery_acpi (FreeBSD)
 *          &rui_battery_apm (OpenBSD, NetBSD)                           */
#define BATTERY_MODULES &rui_battery_sysfs, &rui_battery_acpi, &rui_battery_apm,

/* Shut down / reboot / suspend, whatever the init system:
 *   &rui_power_systemd     systemd is PID 1          (systemctl)
 *   &rui_power_elogind     elogind (OpenRC, runit..)  (loginctl)
 *   &rui_power_consolekit  ConsoleKit2 over D-Bus     (dbus-send)
 *   &rui_power_openbsd     shutdown(8), zzz(8)
 *   &rui_power_freebsd     shutdown(8), acpiconf(8)
 *   &rui_power_netbsd      shutdown(8), sysctl hw.acpi.sleep
 *   &rui_power_generic     poweroff/reboot/zzz: sysvinit, runit, s6,
 *                          OpenRC, dinit (needs doas/sudo rights)       */
#define POWER_MODULES &rui_power_systemd, &rui_power_elogind, &rui_power_consolekit, &rui_power_openbsd, &rui_power_freebsd, &rui_power_netbsd, &rui_power_generic,

/* D-Bus session bus: "auto" starts dbus-daemon when none is running and the
 * program exists, "on" warns if it cannot, "off" never touches D-Bus.     */
#define DBUS_MODE "auto"

/* freedesktop.org integration (both optional, nothing depends on them):
 * .desktop applications in menus/icons/@app, and ~/.config/autostart     */
#define DESKTOP_ENTRIES 1
#define XDG_AUTOSTART 0

/* Look: theme name (CDE, Classic, Broica, Solaris, Platinum, ...), the
 * style ("" = the theme's own, "cde" or "classic") and the font.         */
#define DEFAULT_THEME "CDE"
#define DEFAULT_STYLE ""
#define DEFAULT_FONT "sans-9"

/* Default tools: "auto" = XDG default application (if xdg-mime is
 * installed) or the first installed program from a built-in list.       */
#define DEFAULT_TERMINAL "auto"
#define DEFAULT_BROWSER "auto"
#define DEFAULT_FILEMANAGER "auto"
#define DEFAULT_EDITOR "auto"
#define DEFAULT_LOCKER "auto"
/* the clock opened from the panel clock and by @clock; auto: xclock */
#define DEFAULT_CLOCK "auto"

#define WORKSPACES 4
#define WORKSPACE_NAMES "One,Two,Three,Four,Five,Six,Seven,Eight,Nine"

/* =================================================================== */

static const RuiAudioModule *const audio_modules[] = { AUDIO_MODULES NULL };
static const RuiNetModule *const net_modules[] = { NET_MODULES NULL };
static const RuiBatteryModule *const battery_modules[] = { BATTERY_MODULES NULL };
static const RuiPowerModule *const power_modules[] = { POWER_MODULES NULL };
static const RuiBtModule *const bt_modules[] = { BT_MODULES NULL };

/* ----------------------------------------------------------------- panel */

/* Taskbar applets, left to right.  Built in: cpu mem battery net bluetooth
 * volume clock; any block name from below may be used as well.  The CDE
 * front panel shows the same applets in its status area.  dnd and awake
 * are the "do not disturb" and "keep the screen on" switches. */
static const char bar_applets[] = "cpu mem battery net bluetooth dnd awake volume clock";

/* CDE front panel controls left and right of the workspace switch:
 * clock calendar files editor terminal browser style apps network        */
static const char fp_left[]  = "clock calendar files editor";
static const char fp_right[] = "terminal browser style apps";

/* Blocks: dwmblocks-style status items fed by a shell command. */
static const RuiBlock blocks[] = {
	/* name      seconds  command                          on click      */
	{ "load",    10,      "uptime | sed 's/.*average[s]*: //'", NULL },
	{ "disk",    60,      "df -h / | awk 'NR==2 {print \"/ \" $4}'", NULL },
	{ NULL,      0,       NULL,                            NULL },
};

/* -------------------------------------------------------- window manager */

/* The first layout is the default of every workspace.  Algorithms:
 * float (no tiling), tile (master + stack), monocle (one at a time). */
static const RuiLayoutDef layouts[] = {
	/* name       symbol */
	{ "float",    "><>" },
	{ "tile",     "[]=" },
	{ "monocle",  "[M]" },
};

/* "<Modifiers+Key> <action> [argument]" - see README for the actions. */
static const char *const keys[] = {
	"Mod1+Tab cycle",
	"Mod1+Shift+Tab cycle_back",
	"Mod1+F4 close",
	"Mod1+F2 exec retro-run",
	"Mod1+F9 minimize",
	"Mod1+F10 maximize",
	"Mod1+F11 fullscreen",
	"Mod1+F12 shade",
	"Mod1+space window_menu",
	"Mod4+s sticky",
	"Mod4+a above",
	"Control+Mod1+Right next_workspace",
	"Control+Mod1+Left prev_workspace",
	"Mod4+1 workspace 1", "Mod4+2 workspace 2", "Mod4+3 workspace 3",
	"Mod4+4 workspace 4", "Mod4+5 workspace 5", "Mod4+6 workspace 6",
	"Mod4+7 workspace 7", "Mod4+8 workspace 8", "Mod4+9 workspace 9",
	"Mod4+Shift+1 sendto 1", "Mod4+Shift+2 sendto 2", "Mod4+Shift+3 sendto 3",
	"Mod4+Shift+4 sendto 4", "Mod4+Shift+5 sendto 5", "Mod4+Shift+6 sendto 6",
	"Mod4+Shift+7 sendto 7", "Mod4+Shift+8 sendto 8", "Mod4+Shift+9 sendto 9",
	/* moving windows: halves, monitors, pixel steps (see windows.conf) */
	"Mod4+Left snap left", "Mod4+Right snap right",
	"Mod4+Up maximize", "Mod4+Down center",
	"Mod4+Shift+Left to_monitor prev", "Mod4+Shift+Right to_monitor next",
	"Mod4+Mod1+Left move left", "Mod4+Mod1+Right move right",
	"Mod4+Mod1+Up move up", "Mod4+Mod1+Down move down",
	"Mod4+Control+Left resize -32 0", "Mod4+Control+Right resize 32 0",
	"Mod4+Control+Up resize 0 -32", "Mod4+Control+Down resize 0 32",
	"Mod4+n dnd", "Mod4+k awake",
	"Mod4+t layout tile",
	"Mod4+m layout monocle",
	"Mod4+space next_layout",
	"Mod4+f toggle_float",
	"Mod4+Return swap_master",
	"Mod4+h master_shrink",
	"Mod4+l master_grow",
	"Mod4+e exec @filemanager",
	"Mod4+w exec @browser",
	"Mod4+Escape exec @lock",
	"Control+Mod1+t exec @terminal",
	"Control+Mod1+Delete exec @logout",
	"Control+Mod1+BackSpace quit",
	NULL
};

/* ---------------------------------------------------------- applications */

/* tried in order by @terminal when the terminal is "auto" (after
 * $TERMINAL) */
static const char *const terminals[] = {
	"xterm", "uxterm", "urxvt", "rxvt", "st", "alacritty", "kitty", "foot",
	"xfce4-terminal", "lxterminal", "mate-terminal", "konsole",
	"gnome-terminal", "dtterm", NULL
};

/* matched against OnlyShowIn= / NotShowIn=, exported as XDG_CURRENT_DESKTOP */
static const char desktop_name[] = "Retro";
