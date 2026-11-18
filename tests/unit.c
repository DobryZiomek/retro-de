/*
 * unit - parser / backend unit tests that need no window manager.
 *
 *   unit <dir-with-test-images>
 *
 * The XPM colour test needs an X display for XParseColor and is skipped
 * when $DISPLAY cannot be opened.
 */
#include "../lib/retroui.h"
#include "../desk/image.h"
#include "../display/randr.h"
#include "../calc/expr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (cond) { printf("  ok    "); } else { printf("  FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); \
} while (0)

static int conf_seen;

static void
conf_cb(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "font") && !strcmp(val, "sans-9"))
		conf_seen |= 1;
	if (!strcmp(key, "bind") && !strcmp(val, "Mod1+F4 close"))
		conf_seen |= 2;
	if (!strcmp(key, "color.face") && !strcmp(val, "#c0c0c0"))
		conf_seen |= 4;
	if (!strcmp(key, "focus") && !strcmp(val, "click"))
		conf_seen |= 8;
}

static void
test_util(const char *dir)
{
	char buf[64], path[1024], *q;
	FILE *f;

	printf("[util]\n");
	q = rui_shell_quote("it's a test");
	CHECK(!strcmp(q, "'it'\\''s a test'"), "shell quoting: %s", q);
	free(q);
	rui_strlcpy(buf, "\033[1;90mgrey\033[0m plain", sizeof buf);
	rui_strip_ansi(buf);
	CHECK(!strcmp(buf, "grey plain"), "ANSI escapes are stripped");
	{
		char g1[64], g2[64], g3[64], g4[64], g5[8];

		rui_gecos_name("Jan Kowalski,,,", "jan", g1, sizeof g1);
		rui_gecos_name("Charlie &", "root", g2, sizeof g2);
		rui_gecos_name(",,,", "x", g3, sizeof g3);
		rui_gecos_name(NULL, "x", g4, sizeof g4);
		rui_gecos_name("& the &s", "abcdefgh", g5, sizeof g5);
		CHECK(!strcmp(g1, "Jan Kowalski") && !strcmp(g2, "Charlie Root") &&
		    !g3[0] && !g4[0] && !strcmp(g5, "Abcdefg"),
		    "GECOS full name: comma fields dropped, '&' is the login");
	}
	CHECK(rui_have_cmd("sh") && !rui_have_cmd("no-such-command-xyz"),
	    "PATH lookup");
	CHECK(rui_strlcpy(buf, "0123456789", 5) == 10 && !strcmp(buf, "0123"),
	    "strlcpy truncates safely");
	snprintf(path, sizeof path, "%s/unit.conf", dir);
	f = fopen(path, "w");
	if (f) {
		fputs("# comment\nfont = sans-9\n  bind=Mod1+F4 close  \n"
		    "color.face   =   #c0c0c0\nnot a setting\n"
		    "focus = click   # click | sloppy\n", f);
		fclose(f);
	}
	CHECK(rui_conf_parse(path, conf_cb, NULL) == 0 && conf_seen == 15,
	    "key = value config parser, trailing comments");
	CHECK(rui_conf_parse("/nonexistent/x.conf", conf_cb, NULL) == -1,
	    "missing config file is reported, not fatal");
}

static void
test_menu(void)
{
	static const char text[] =
		"# test\n"
		"menu \"Top \\\"quoted\\\"\" {\n"
		"  item \"One\" echo one\n"
		"  menu Sub {\n"
		"    item Two   echo two words\n"
		"    separator\n"
		"  }\n"
		"}\n"
		"bogus line\n"
		"item \"No command\"\n"
		"item \"Three\" @quit\n"
		"}\n";
	RuiMenu *m;

	printf("[menu.conf]\n");
	m = rui_menu_parse(rui_default_menu, "default");
	CHECK(m->n == 10 && m->items[0].sub && rui_menu_count(m) == 37,
	    "embedded fallback menu: %d top-level entries, %d commands", m->n,
	    rui_menu_count(m));
	CHECK(!strcmp(m->items[0].label, "Programs") &&
	    m->items[0].sub->items[0].sub &&
	    !strcmp(m->items[0].sub->items[0].label, "Accessories") &&
	    !strcmp(m->items[1].label, "Applications") && m->items[1].sub &&
	    m->items[1].sub->n == 3 && !strcmp(m->items[2].label, "Settings") &&
	    m->items[3].separator,
	    "fallback menu: programs folder, .desktop applications, settings");
	rui_menu_free(m);
	fprintf(stderr, "    (three parser warnings expected below)\n");
	m = rui_menu_parse(text, "test");
	CHECK(m->n == 2 && !strcmp(m->items[0].label, "Top \"quoted\"") &&
	    m->items[0].sub && m->items[0].sub->n == 2, "nested menus and quoting");
	CHECK(m->items[0].sub->items[1].sub &&
	    !strcmp(m->items[0].sub->items[1].sub->items[0].cmd, "echo two words") &&
	    m->items[0].sub->items[1].sub->items[1].separator,
	    "bare-word labels, multi-word commands, separators");
	CHECK(!strcmp(m->items[1].label, "Three") && rui_menu_count(m) == 3,
	    "malformed lines are skipped with a warning");
	rui_menu_free(m);
	m = rui_menu_load("/nonexistent/menu.conf");
	CHECK(m && rui_menu_count(m) == 37, "missing menu.conf falls back");
	rui_menu_free(m);
}

static int
apar(const RuiAudioModule *m, const char *out, int *vol, int *muted)
{
	*vol = -1;
	*muted = 0;
	return out[0] && m->parse(out, vol, muted);
}

static void
test_audio(void)
{
	int vol = -1, muted = -1;

	printf("[audio backends]\n");
	CHECK(apar(&rui_audio_sndio, "0.500\n1\n", &vol, &muted) &&
	    vol == 50 && muted == 1, "sndioctl: level 0.500, muted");
	CHECK(apar(&rui_audio_pipewire, "Volume: 0.40 [MUTED]\n", &vol, &muted) &&
	    vol == 40 && muted == 1, "wpctl: 0.40 [MUTED]");
	CHECK(apar(&rui_audio_pipewire, "Volume: 1.00\n", &vol, &muted) &&
	    vol == 100 && muted == 0, "wpctl: 1.00");
	CHECK(apar(&rui_audio_pulseaudio, "Volume: front-left: 45875 /  70% / -9.29 dB,"
	    "   front-right: 45875 /  70% / -9.29 dB\n        balance 0.00\n"
	    "Mute: no\n", &vol, &muted) && vol == 70 && muted == 0, "pactl: 70%%");
	CHECK(apar(&rui_audio_alsa, "Simple mixer control 'Master',0\n"
	    "  Limits: Playback 0 - 87\n  Mono: Playback 60 [69%] [-20.25dB] [off]\n",
	    &vol, &muted) && vol == 69 && muted == 1, "amixer: [69%%] [off]");
	CHECK(apar(&rui_audio_oss, "vol.volume=0.75:0.75\nvol.mute=off\n", &vol,
	    &muted) && vol == 75 && muted == 0, "FreeBSD mixer (new syntax)");
	CHECK(apar(&rui_audio_oss, "vol 85:85\n", &vol, &muted) && vol == 85,
	    "FreeBSD mixer (old syntax)");
	CHECK(apar(&rui_audio_mixerctl, "128,128\non\n", &vol, &muted) &&
	    vol == 50 && muted == 1, "NetBSD mixerctl: 128/255, mute on");
	CHECK(!apar(&rui_audio_pulseaudio, "Connection failure\n", &vol, &muted) &&
	    !apar(&rui_audio_pipewire, "", &vol, &muted),
	    "garbage output is rejected so the next backend is probed");
}

static void
test_net(void)
{
	static const char iw_list[] =
		"                            Devices in Station Mode\n"
		"----------------------------------------------------------------\n"
		"\033[1;90m      Name                  State            Scanning\033[0m\n"
		"----------------------------------------------------------------\n"
		"\033[0m      wlan0                 connected        \n";
	static const char iw_show[] =
		"                                 Station: wlan0\n"
		"----------------------------------------------------------------\n"
		"      Settable  Property              Value\n"
		"----------------------------------------------------------------\n"
		"            Scanning              no\n"
		"            State                 connected\n"
		"            Connected network     Home Net 5G\n";
	static const char iw_nets[] =
		"                               Available networks\n"
		"----------------------------------------------------------------\n"
		"      Network name                      Security            Signal\n"
		"----------------------------------------------------------------\n"
		"\033[0m  \033[1;90m> \033[0m  Home Net 5G                       psk"
		"                 ****\033[1;90m\033[0m    \n"
		"      Cafe                              open                "
		"**\033[1;90m**\033[0m    \n";
	static const char wpa_scan[] =
		"Selected interface 'wlan0'\n"
		"bssid / frequency / signal level / flags / ssid\n"
		"00:11:22:33:44:55\t2437\t-55\t[WPA2-PSK-CCMP][ESS]\tOffice\n"
		"00:11:22:33:44:66\t2412\t-80\t[ESS]\tGuest Net\n"
		"00:11:22:33:44:77\t5180\t-40\t[WPA2-PSK-CCMP][ESS]\tOffice\n";
	static const char wpa_status[] =
		"bssid=00:11:22:33:44:55\nssid=Office\nwpa_state=COMPLETED\n"
		"ip_address=10.0.0.2\n";
	static const char ifc[] =
		"iwm0: flags=808843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> mtu 1500\n"
		"\tlladdr 00:11:22:33:44:55\n"
		"\tgroups: wlan egress\n"
		"\tmedia: IEEE802.11 autoselect (HT-MCS15 mode 11n)\n"
		"\tstatus: active\n"
		"\tieee80211: join \"My Home\" chan 6 bssid 00:aa:bb:cc:dd:ee 61% "
		"wpakey wpaprotos wpa2\n";
	static const char ifc_scan[] =
		"iwm0: flags=808843<UP,BROADCAST,RUNNING> mtu 1500\n"
		"\tieee80211: nwid \"\"\n"
		"\t\tnwid \"My Home\" chan 6 bssid 00:aa:bb:cc:dd:ee 61% HT-MCS15 "
		"privacy,short_preamble,short_slottime,wpa2\n"
		"\t\tnwid openbsd chan 11 bssid 00:aa:bb:cc:dd:ff 30% 54M ess\n";
	RuiNetwork nets[8];
	RuiNetStatus st;
	char iface[32], *cmd;
	int n;

	printf("[network backends]\n");
	CHECK(rui_net_parse_iface(&rui_net_iwd, iw_list, iface, sizeof iface) &&
	    !strcmp(iface, "wlan0"), "iwctl: station device is %s", iface);
	CHECK(rui_net_parse_status(&rui_net_iwd, iw_show, &st) && st.connected &&
	    !strcmp(st.ssid, "Home Net 5G"), "iwctl: connected to '%s'", st.ssid);
	n = rui_net_parse_scan(&rui_net_iwd, iw_nets, nets, 8);
	CHECK(n == 2 && !strcmp(nets[0].ssid, "Home Net 5G") && nets[0].connected &&
	    nets[0].secure && nets[0].signal == 100,
	    "iwctl: network list, connected entry first");
	CHECK(n == 2 && !strcmp(nets[1].ssid, "Cafe") && !nets[1].secure &&
	    nets[1].signal == 50, "iwctl: open network, dimmed signal bars");
	n = rui_net_parse_scan(&rui_net_wpa, wpa_scan, nets, 8);
	CHECK(n == 2 && !strcmp(nets[0].ssid, "Office") && nets[0].secure &&
	    nets[0].signal == 100 && !strcmp(nets[1].ssid, "Guest Net") &&
	    !nets[1].secure && nets[1].signal == 40,
	    "wpa_cli: scan results merged per SSID and sorted");
	n = rui_net_parse_scan(&rui_net_wpa,
	    "aa:bb:cc:dd:ee:01\t2412\t-99999999999\t[WPA2]\tLow\n"
	    "aa:bb:cc:dd:ee:02\t2412\t99999999999\t[ESS]\tHigh\n", nets, 8);
	CHECK(n == 2 && !strcmp(nets[0].ssid, "High") && nets[0].signal == 100 &&
	    !strcmp(nets[1].ssid, "Low") && nets[1].signal == 0,
	    "wpa_cli: absurd signal values are clamped, not overflowed");
	CHECK(rui_net_parse_status(&rui_net_wpa, wpa_status, &st) &&
	    st.connected && !strcmp(st.ssid, "Office"), "wpa_cli: status");
	CHECK(rui_net_parse_iface(&rui_net_ifconfig, ifc, iface, sizeof iface) &&
	    !strcmp(iface, "iwm0"), "ifconfig: wlan group device is %s", iface);
	CHECK(rui_net_parse_status(&rui_net_ifconfig, ifc, &st) && st.connected &&
	    !strcmp(st.ssid, "My Home"), "ifconfig: joined '%s'", st.ssid);
	n = rui_net_parse_scan(&rui_net_ifconfig, ifc_scan, nets, 8);
	CHECK(n == 2 && !strcmp(nets[0].ssid, "My Home") && nets[0].secure &&
	    nets[0].signal == 61 && !strcmp(nets[1].ssid, "openbsd") &&
	    !nets[1].secure, "ifconfig: scan list");
	cmd = rui_net_connect_cmd(&rui_net_iwd, "wlan0", "it's", "p w", NULL);
	CHECK(cmd && !strcmp(cmd, "iwctl --passphrase 'p w' station 'wlan0' "
	    "connect 'it'\\''s' 2>&1"), "connect command is shell-safe");
	free(cmd);
	cmd = rui_net_connect_cmd(&rui_net_ifconfig, "iwm0", "net", "", "doas");
	CHECK(cmd && !strcmp(cmd, "doas ifconfig 'iwm0' join 'net' -wpakey 2>&1"),
	    "ifconfig connect uses the configured privilege helper");
	free(cmd);
}

static void
test_sys(void)
{
	double busy = -1, used = -1;
	unsigned long total = 0;
	int pct = -1, chg = -1;

	printf("[system probes]\n");
	CHECK(rui_battery_apm.parse("85\n1\n", &pct, &chg) && pct == 85 && chg == 1,
	    "apm output: 85%% on AC");
	CHECK(!rui_battery_apm.parse("255\n255\n", &pct, &chg),
	    "apm 'no battery' value is rejected");
	CHECK(!rui_battery_apm.parse("0\n255\n4\n", &pct, &chg) &&
	    rui_battery_apm.parse("40\n0\n1\n", &pct, &chg) && pct == 40 &&
	    chg == 0, "apm: OpenBSD's 0%% with state 'absent' is no battery");
#if defined(__linux__) || defined(__OpenBSD__) || defined(__FreeBSD__) || \
    defined(__NetBSD__)
	CHECK(rui_sys_cpu(&busy) && busy >= 0.0 && busy <= 1.0, "CPU probe works");
	CHECK(rui_sys_mem(&used, &total) && used > 0.0 && used < 1.0 && total > 0,
	    "memory probe: %.0f%% of %lu MB used", used * 100, total);
#else
	(void)busy;
	(void)used;
	(void)total;
#endif
}

static void
test_images(const char *dir)
{
	char path[1024];
	Image *img;
	Display *dpy;

	printf("[image loaders]\n");
	snprintf(path, sizeof path, "%s/test-p6.ppm", dir);
	img = image_load(NULL, path);
	CHECK(img && img->w == 2 && img->h == 2 && img->rgb[0] == 255 &&
	    img->rgb[1] == 0 && img->rgb[3] == 0 && img->rgb[4] == 255 &&
	    img->rgb[11] == 255, "binary PPM (P6)");
	image_free(img);
	snprintf(path, sizeof path, "%s/test-p3.ppm", dir);
	img = image_load(NULL, path);
	CHECK(img && img->w == 2 && img->h == 1 && img->rgb[0] == 255 &&
	    img->rgb[1] == 127 && img->rgb[5] == 255, "ASCII PPM (P3) with comments");
	image_free(img);
	fprintf(stderr, "    (two image warnings expected below)\n");
	snprintf(path, sizeof path, "%s/broken.ppm", dir);
	CHECK(image_load(NULL, path) == NULL, "truncated PPM is rejected");
	CHECK(image_load(NULL, "/nonexistent.ppm") == NULL, "missing file is rejected");
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		printf("  skip  XPM colours (no display)\n");
		return;
	}
	{
		RuiApp app;

		memset(&app, 0, sizeof app);
		app.dpy = dpy;
		app.cmap = DefaultColormap(dpy, DefaultScreen(dpy));
		snprintf(path, sizeof path, "%s/test.xpm", dir);
		img = image_load(&app, path);
		CHECK(img && img->w == 4 && img->h == 2 && img->alpha &&
		    img->alpha[0] == 0 && img->alpha[1] == 1 && img->rgb[3] == 255 &&
		    img->rgb[4] == 0 && img->rgb[2 * 3 + 2] == 255 &&
		    img->rgb[3 * 3] == 255 && img->rgb[3 * 3 + 1] == 255,
		    "XPM3 with None, #rrggbb and named colours");
		image_free(img);
	}
	XCloseDisplay(dpy);
}

/* ------------------------------------------------------- .desktop files */

static void
write_file(const char *dir, const char *name, const char *text)
{
	char path[1024], cmd[1200];
	FILE *f;

	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
	if (system(cmd) != 0)
		return;
	snprintf(path, sizeof path, "%s/%s", dir, name);
	if ((f = fopen(path, "w"))) {
		fputs(text, f);
		fclose(f);
	}
}

#define APP(name, extra) "[Desktop Entry]\nType=Application\nName=" name \
	"\nExec=true\n" extra

static void
desktop_fixtures(const char *dir)
{
	char d[1024], buf[2048], cmd[1100];

	snprintf(cmd, sizeof cmd, "rm -rf '%s/xdg'", dir);
	if (system(cmd) != 0)
		return;
	snprintf(d, sizeof d, "%s/xdg/home/applications", dir);
	write_file(d, "alpha.desktop", APP("Alpha", "Icon=alpha\n"
	    "Categories=Utility;TextEditor;\n"));
	write_file(d, "shadow.desktop", "[Desktop Entry]\nHidden=true\n");
	write_file(d, "nodisp.desktop", APP("Hidden Tool", "NoDisplay=true\n"));
	write_file(d, "term.desktop", APP("Term App", "Terminal=true\n"
	    "Categories=System;\n"));
	snprintf(d, sizeof d, "%s/xdg/home/applications/kde4", dir);
	write_file(d, "sub.desktop", APP("Sub App", "Categories=Game;\n"));
	snprintf(d, sizeof d, "%s/xdg/sys/applications", dir);
	write_file(d, "shadow.desktop", APP("Shadow", ""));
	write_file(d, "gnome.desktop", APP("Gnome Only", "OnlyShowIn=GNOME;\n"));
	write_file(d, "tryexec.desktop", APP("Missing", "TryExec=no-such-xyz\n"));
	write_file(d, "link.desktop", "[Desktop Entry]\nType=Link\nName=L\n"
	    "URL=http://example.org/\n");
	snprintf(d, sizeof d, "%s/xdg/sys/pixmaps", dir);
	write_file(d, "alpha.xpm", "/* XPM */\nstatic char *a[] = {\n"
	    "\"1 1 1 1\",\n\". c #ff0000\",\n\".\"\n};\n");
	snprintf(d, sizeof d, "%s/xdg/cfg/autostart", dir);
	snprintf(buf, sizeof buf, "[Desktop Entry]\nType=Application\nName=A1\n"
	    "Exec=touch \"%s/xdg/auto1\"\n", dir);
	write_file(d, "a1.desktop", buf);
	write_file(d, "a2.desktop", "[Desktop Entry]\nType=Application\nName=A2\n"
	    "Exec=true\nHidden=true\n");
	snprintf(buf, sizeof buf, "[Desktop Entry]\nType=Application\nName=A3\n"
	    "Exec=touch \"%s/xdg/auto3\"\nX-GNOME-Autostart-enabled=false\n", dir);
	write_file(d, "a3.desktop", buf);

	snprintf(d, sizeof d, "%s/xdg/home", dir);
	setenv("XDG_DATA_HOME", d, 1);
	snprintf(d, sizeof d, "%s/xdg/sys", dir);
	setenv("XDG_DATA_DIRS", d, 1);
	snprintf(d, sizeof d, "%s/xdg/cfg", dir);
	setenv("XDG_CONFIG_HOME", d, 1);
	snprintf(d, sizeof d, "%s/xdg/none", dir);
	setenv("XDG_CONFIG_DIRS", d, 1);
	unsetenv("XDG_CURRENT_DESKTOP");
	unsetenv("LC_ALL");
	unsetenv("LC_MESSAGES");
	setenv("LANG", "pl_PL.UTF-8", 1);
	setenv("TERMINAL", "sh", 1);
}

static RuiDesktopEntry *
parse(const char *text)
{
	return rui_desktop_parse(text, "t.desktop", "/x/t.desktop");
}

static void
test_desktop(const char *dir)
{
	static const char ed[] =
		"# comment\n"
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Text Editor\n"
		"Name[pl]=Edytor\n"
		"Name[pl_PL]=Edytor Tekstu\n"
		"Name[de]=Texteditor\n"
		"Comment=Edit\\stext\n"
		"Exec=/usr/bin/ed --opt \"a b\" %U %%x %c \"--q=\\\\$HOME\" %i %k\n"
		"Icon=ed\n"
		"[Desktop Action new]\n"
		"Name=Ignored\n";
	RuiDesktopEntry *e;
	RuiAppList *l;
	RuiMenu *m;
	char *cmd, path[1100];
	int i, ok;

	printf("[.desktop entries]\n");
	e = parse(ed);
	CHECK(e && !strcmp(e->name, "Edytor Tekstu") &&
	    !strcmp(e->comment, "Edit text"),
	    "localized Name[pl_PL] wins, string escapes: '%s'", e ? e->name : "");
	CHECK(e && !strcmp(e->exec, "/usr/bin/ed --opt 'a b' %x 'Edytor Tekstu' "
	    "'--q=$HOME' --icon ed /x/t.desktop"),
	    "Exec quoting and field codes: %s", e ? e->exec : "(null)");
	rui_desktop_free(e);
	e = parse(APP("Top", "Terminal=true\nPath=/tmp\n"));
	cmd = e ? rui_desktop_command(e) : NULL;
	CHECK(cmd && !strcmp(cmd, "cd '/tmp' && exec sh -e true"),
	    "Terminal=true and Path=: %s", cmd ? cmd : "(null)");
	free(cmd);
	rui_desktop_free(e);
	fprintf(stderr, "    (one Exec warning expected below)\n");
	CHECK(!parse("[Desktop Entry]\nType=Link\nName=x\nURL=y\n") &&
	    !parse("[Desktop Entry]\nType=Application\nName=x\n") &&
	    !parse("[Desktop Entry]\nType=Application\nName=x\nExec=a \"b\n") &&
	    !parse("[Other]\nType=Application\nName=x\nExec=y\n"),
	    "links, missing Exec, bad quoting and foreign groups are rejected");
	e = parse(APP("G", "OnlyShowIn=GNOME;KDE;\n"));
	ok = e && e->hidden;
	rui_desktop_free(e);
	e = parse(APP("R", "OnlyShowIn=XFCE;Retro;\n"));
	ok = ok && e && !e->hidden;
	rui_desktop_free(e);
	e = parse(APP("N", "NotShowIn=Retro;\n"));
	ok = ok && e && e->hidden;
	rui_desktop_free(e);
	CHECK(ok, "OnlyShowIn / NotShowIn against desktop name 'Retro'");
	e = parse(APP("T", "TryExec=no-such-xyz\n"));
	ok = e && e->hidden;
	rui_desktop_free(e);
	e = parse(APP("T", "TryExec=sh\nNoDisplay=true\n"));
	CHECK(ok && e && !e->hidden && e->nodisplay, "TryExec and NoDisplay");
	rui_desktop_free(e);

	l = rui_apps_scan();
	CHECK(l->n == 4 && !strcmp(l->v[0]->name, "Alpha") &&
	    !strcmp(l->v[1]->name, "Hidden Tool") &&
	    !strcmp(l->v[2]->id, "kde4-sub.desktop") &&
	    !strcmp(l->v[3]->name, "Term App"),
	    "scan: user dir masks system ids, subdir ids, filters, sorted (%d)",
	    l->n);
	m = rui_apps_menu(l);
	CHECK(m->n == 3 && !strcmp(m->items[0].label, "Accessories") &&
	    !strcmp(m->items[1].label, "Games") &&
	    !strcmp(m->items[2].label, "System") && rui_menu_count(m) == 3,
	    "application menu grouped by main category, NoDisplay left out");
	rui_menu_free(m);
	rui_apps_free(l);
	e = rui_app_find("alpha");
	ok = e && !strcmp(e->id, "alpha.desktop");
	rui_desktop_free(e);
	e = rui_app_find("kde4-sub.desktop");
	ok = ok && e;
	rui_desktop_free(e);
	CHECK(ok && !rui_app_find("shadow") && !rui_app_find("nope") &&
	    !rui_app_find("../etc/passwd"), "lookup by desktop-file id");
	cmd = rui_icon_lookup("alpha");
	snprintf(path, sizeof path, "%s/xdg/sys/pixmaps/alpha.xpm", dir);
	CHECK(cmd && !strcmp(cmd, path) && !rui_icon_lookup("/x/y.png") &&
	    !rui_icon_lookup("missing"), "icon lookup finds XPM pixmaps");
	free(cmd);
	CHECK(rui_autostart_xdg() == 1, "XDG autostart starts only enabled entries");
	snprintf(path, sizeof path, "%s/xdg/auto1", dir);
	for (i = 0; i < 100 && access(path, F_OK) != 0; i++)
		usleep(20000);
	CHECK(access(path, F_OK) == 0, "autostart command ran");
}

/* ------------------------------------------------- themes and settings */

static int
file_has(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	char buf[8192];
	size_t n;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = '\0';
	fclose(f);
	return strstr(buf, needle) != NULL;
}

static void
settings_fixtures(const char *dir)
{
	char d[1024];

	snprintf(d, sizeof d, "%s/cfg", dir);
	setenv("RETRO_DE_CONFIG_DIR", d, 1);
	write_file(d, "retro.conf", "# kept comment\n"
	    "theme = Broica\n"
	    "font = serif-11\n"
	    "browser = mybrowser --flag\n"
	    "color.face = #123456\n"
	    "dbus = off\n"
	    "xdg_autostart = yes\n"
	    "clock = myclock -digital\n"
	    "power_backend = none   # never touch the real machine\n");
	snprintf(d, sizeof d, "%s/cfg/themes", dir);
	write_file(d, "Mine.theme", "style = classic\ncolor.face = #010203\n");
}

static void
test_settings(const char *dir)
{
	const RuiSettings *st = rui_settings();
	const RuiTheme *t;
	char **list, path[1100], *cmd;
	int i, cde = 0, classic = 0, mine = 0, dup = 0;

	printf("[themes and settings]\n");
	CHECK(!strcmp(st->theme, "Broica") && st->style == RUI_STYLE_CDE &&
	    !strcmp(st->font, "serif-11"),
	    "retro.conf picks theme and font; style comes from the theme");
	CHECK(!strcmp(st->color[RUI_C_FACE], "#123456") &&
	    !strcmp(st->color[RUI_C_TITLE_ACTIVE], "#8a5a33") &&
	    !strcmp(st->pattern, "bricks"),
	    "colour overrides beat the theme; theme supplies the rest");
	CHECK(st->dbus == RUI_OFF && st->xdg_autostart == 1 &&
	    !strcmp(st->power, "none") && st->confirm == 1,
	    "D-Bus, XDG and power switches");
	cmd = rui_tool_command(RUI_TOOL_BROWSER);
	CHECK(cmd && !strcmp(cmd, "mybrowser --flag"),
	    "configured default tool is used as given");
	free(cmd);
	cmd = rui_tool_command(RUI_TOOL_CLOCK);
	CHECK(cmd && !strcmp(cmd, "myclock -digital") &&
	    !strcmp(rui_tool_keys[RUI_TOOL_CLOCK], "clock") &&
	    !strcmp(rui_config.tools[RUI_TOOL_CLOCK], "auto"),
	    "the clock is a default application (auto: xclock first)");
	free(cmd);
	t = rui_theme_find("cde");
	CHECK(t && !strcmp(t->name, "CDE") && t->style == RUI_STYLE_CDE &&
	    !rui_theme_find("nope") && !rui_theme_find("../x"),
	    "built-in themes by name, case-insensitive");
	t = rui_theme_find("Mine");
	CHECK(t && t->style == RUI_STYLE_CLASSIC &&
	    !strcmp(t->colors[RUI_C_FACE], "#010203") &&
	    !strcmp(t->colors[RUI_C_DESKTOP], "#008080"),
	    "user theme file, missing colours from Classic");
	list = rui_theme_list();
	for (i = 0; list[i]; i++) {
		cde += !strcmp(list[i], "CDE");
		classic += !strcmp(list[i], "Classic");
		mine += !strcmp(list[i], "Mine");
		dup += i > 0 && !strcmp(list[i], list[i - 1]);
	}
	CHECK(cde == 1 && classic == 1 && mine == 1 && !dup && i >= 9,
	    "theme list: %d built-in and user themes", i);
	rui_strv_free(list);
	CHECK(rui_style_parse("Motif") == RUI_STYLE_CDE &&
	    rui_style_parse("classic") == RUI_STYLE_CLASSIC &&
	    rui_style_parse("x") < 0, "style names");

	snprintf(path, sizeof path, "%s/cfg/retro.conf", dir);
	CHECK(rui_conf_set(path, "theme", "Mine") == 0 &&
	    rui_conf_set(path, "style", "classic") == 0 &&
	    file_has(path, "# kept comment\ntheme = Mine\nfont = serif-11\n") &&
	    file_has(path, "\nstyle = classic\n") && !file_has(path, "Broica"),
	    "retro.conf writer replaces in place, appends, keeps comments");
	st = rui_settings();
	CHECK(!strcmp(st->theme, "Mine") && st->style == RUI_STYLE_CLASSIC,
	    "settings are re-read after writing");
}

static void
test_power(void)
{
	static const RuiPowerModule fake = {
		"fake", "sh", NULL,
		{ "sh -c true", NULL, "no-such-program-xyz", "sh -c :" }, 1
	};
	char *a, *b, *c, *d;

	printf("[power modules]\n");
	a = rui_power_command(&fake, RUI_POWEROFF, "doas -n");
	b = rui_power_command(&fake, RUI_REBOOT, "doas -n");
	c = rui_power_command(&fake, RUI_SUSPEND, "doas -n");
	d = rui_power_command(&fake, RUI_HIBERNATE, "");
	CHECK(a && !strcmp(a, "doas -n sh -c true") && !b && !c && d &&
	    !strcmp(d, "sh -c :"),
	    "privilege prefix, unsupported and uninstalled actions");
	free(a);
	free(d);
	CHECK(!strcmp(rui_power_systemd.cmd[RUI_REBOOT], "systemctl reboot") &&
	    !rui_power_systemd.needs_priv &&
	    !strcmp(rui_power_elogind.cmd[RUI_SUSPEND], "loginctl suspend") &&
	    !strcmp(rui_power_openbsd.cmd[RUI_POWEROFF], "shutdown -p now") &&
	    rui_power_openbsd.needs_priv && rui_power_netbsd.cmd[RUI_HIBERNATE] ==
	    NULL && !strcmp(rui_power_generic.cmd[RUI_SUSPEND], "zzz") &&
	    strstr(rui_power_consolekit.cmd[RUI_POWEROFF], "ConsoleKit"),
	    "module command tables (systemd, elogind, ConsoleKit, BSDs, generic)");
	CHECK(rui_power_detect("none") == NULL &&
	    rui_power_detect("no-such-module") == NULL,
	    "power_backend = none disables power actions");
#if defined(__linux__)
	CHECK(!rui_power_openbsd.usable() && !rui_power_freebsd.usable() &&
	    !rui_power_netbsd.usable() && rui_power_generic.usable() !=
	    rui_power_systemd.usable(),
	    "init detection: exactly one of systemd/generic applies here");
#endif
}


/* ------------------------------------------------------ programs folder */

static RuiMenuItem *
menu_item(RuiMenu *m, const char *label)
{
	int i;

	for (i = 0; m && i < m->n; i++)
		if (m->items[i].label && !strcmp(m->items[i].label, label))
			return &m->items[i];
	return NULL;
}

static void
test_programs(const char *dir)
{
	char d[1100], cmd[6000], *pd = rui_programs_dir();
	RuiMenu *m;
	RuiMenuItem *it, *games;
	long long s1, s2;

	printf("[programs folder]\n");
	snprintf(d, sizeof d, "%s/cfg/programs", dir);
	CHECK(!strcmp(pd, d), "the folder is <config dir>/programs");
	m = rui_programs_menu();               /* the first use fills it in */
	it = menu_item(m, "Accessories");
	CHECK(it && it->sub && menu_item(it->sub, "Clock") &&
	    !strcmp(menu_item(it->sub, "Clock")->cmd, "@clock") &&
	    menu_item(m, "Internet") && menu_item(m, "System") &&
	    menu_item(m, "Settings") && rui_programs_init() == 0,
	    "default categories are created once, with xclock's @clock in them");
	it = menu_item(m, "Settings");
	CHECK(it && it->sub && menu_item(it->sub, "Display") &&
	    !strcmp(menu_item(it->sub, "Display")->cmd, "retro-display") &&
	    menu_item(it->sub, "Bluetooth") &&
	    !strcmp(m->items[m->n - 1].label, "Organize...") &&
	    !strncmp(m->items[m->n - 1].cmd, "@filemanager '", 14),
	    "Display and Bluetooth are in Settings; Organize opens the folder");
	rui_menu_free(m);
	s1 = rui_programs_stamp();

	/* the user rearranges things with plain file operations */
	snprintf(d, sizeof d, "%s/cfg/programs/05-Games/Board", dir);
	write_file(d, "Chess", "# my favourite\n\n  xboard -size small  \n");
	snprintf(d, sizeof d, "%s/cfg/programs/05-Games", dir);
	write_file(d, "20-Rogue.sh", "#!/bin/sh\nexec nethack\n");
	write_file(d, "10-doom.desktop", "[Desktop Entry]\nType=Application\n"
	    "Name=Doom Engine\nExec=doom -fullscreen %f\n");
	write_file(d, ".hidden", "should-not-show\n");
	write_file(d, "Empty note", "# nothing to run here\n");
	snprintf(cmd, sizeof cmd, "chmod +x '%s/20-Rogue.sh' && mkdir -p "
	    "'%s/Nothing here' && rm -rf '%s/cfg/programs/Internet' && "
	    "ln -sf 10-doom.desktop '%s/30-Linked'", d, d, dir, d);
	CHECK(system(cmd) == 0, "fixture: user edits to the folder");
	s2 = rui_programs_stamp();
	CHECK(s1 != s2 && s1 != 0, "the menu notices changes to the folder");
	m = rui_programs_menu();
	games = menu_item(m, "Games");
	CHECK(games && games->sub && m->items[0].sub == games->sub &&
	    !menu_item(m, "Internet") && !menu_item(m, "05-Games"),
	    "a category is a folder: \"NN-\" sorts it first, deleting removes it");
	it = games ? menu_item(games->sub, "Board") : NULL;
	CHECK(it && it->sub && menu_item(it->sub, "Chess") &&
	    !strcmp(menu_item(it->sub, "Chess")->cmd, "xboard -size small"),
	    "nested categories; a text file's first real line is the command");
	it = games ? menu_item(games->sub, "Doom Engine") : NULL;
	CHECK(it && !strcmp(it->cmd, "doom -fullscreen") &&
	    menu_item(games->sub, "Linked") &&
	    !strcmp(menu_item(games->sub, "Linked")->cmd, "doom -fullscreen"),
	    ".desktop files, and links to them under the link's own name");
	it = games ? menu_item(games->sub, "Rogue") : NULL;
	CHECK(it && !strncmp(it->cmd, "'/", 2) &&
	    strstr(it->cmd, "/cfg/programs/05-Games/20-Rogue.sh'"),
	    "an executable runs by its full path");
	it = games ? menu_item(games->sub, "Nothing here") : NULL;
	CHECK(games && !menu_item(games->sub, ".hidden") &&
	    !menu_item(games->sub, "hidden") &&
	    !menu_item(games->sub, "Empty note") && it && it->sub &&
	    it->sub->n == 1 && it->sub->items[0].disabled &&
	    games->sub->items[0].sub && games->sub->items[1].sub &&
	    !strcmp(games->sub->items[2].label, "Doom Engine"),
	    "dot files and empty launchers are skipped; folders come first");
	CHECK(!strcmp(m->items[m->n - 2].label, "Show as Icons...") &&
	    !strcmp(m->items[m->n - 2].cmd, "retro-programs"),
	    "the menu offers the Programs window");
	rui_menu_free(m);

	/* icons for the Programs window: no icon themes, just files */
	{
		RuiProgram *v;
		int n, i, chess = -1, rogue = -1, doom = -1, board = -1, nh = -1;

		snprintf(d, sizeof d, "%s/cfg/programs/05-Games", dir);
		write_file(d, "Board.xpm", "/* XPM */\n");
		write_file(d, "Hack", "# icon: game\nnethack\n");
		snprintf(cmd, sizeof cmd, "cp '%s/icon.xpm' '%s/Board/Chess.xpm'",
		    dir, d);
		CHECK(system(cmd) == 0, "fixture: pictures beside programs");
		n = rui_programs_read(d, &v);
		for (i = 0; i < n; i++) {
			if (!strcmp(v[i].label, "Board"))
				board = i;
			else if (!strcmp(v[i].label, "Rogue"))
				rogue = i;
			else if (!strcmp(v[i].label, "Doom Engine"))
				doom = i;
			else if (!strcmp(v[i].label, "Hack"))
				nh = i;
			CHECK(!strstr(v[i].label, ".xpm"), "pictures are not programs");
		}
		CHECK(board >= 0 && v[board].is_dir && v[board].image &&
		    strstr(v[board].image, "/Board.xpm"),
		    "a picture beside a category is its icon");
		CHECK(nh >= 0 && !v[nh].image && v[nh].icon == RUI_ICON_GAME &&
		    !strcmp(v[nh].cmd, "nethack"),
		    "\"# icon: game\" picks a pictogram and is not the command");
		CHECK(doom >= 0 && rogue >= 0 && v[doom].icon == RUI_ICON_GAME,
		    "a pictogram is guessed from the command");
		rui_programs_free(v, n);
		n = rui_programs_read(strcat(d, "/Board"), &v);
		for (i = 0; i < n; i++)
			if (!strcmp(v[i].label, "Chess"))
				chess = i;
		CHECK(n == 1 && chess == 0 && v[0].image &&
		    strstr(v[0].image, "/Chess.xpm"), "Chess.xpm is Chess's icon");
		rui_programs_free(v, n);
	}
	CHECK(rui_icon_guess("@terminal -e top") == RUI_ICON_COMPUTER &&
	    rui_icon_guess("/usr/bin/firefox %u") == RUI_ICON_BROWSER &&
	    rui_icon_guess("retro-calc") == RUI_ICON_CALC &&
	    rui_icon_guess("retro-about") == RUI_ICON_INFO &&
	    rui_icon_guess("whatever") == RUI_ICON_APP &&
	    rui_icon_kind("programs") == RUI_ICON_PROGRAMS &&
	    rui_icon_kind("nope") == -1,
	    "pictograms by name and by command");
	free(pd);
}

/* ------------------------------------------------------------ bluetooth */

static void
test_bluetooth(void)
{
	static const char show[] =
		"Controller 00:1A:7D:DA:71:13 (public)\n"
		"\tName: box\n\tAlias: box\n\tClass: 0x006c010c\n"
		"\tPowered: yes\n\tDiscoverable: no\n\tPairable: yes\n"
		"Device 11:22:33:44:55:66 WH-1000XM4\n"
		"Device AA:BB:CC:DD:EE:FF\n";
	static const char off[] =
		"Controller 00:1A:7D:DA:71:13 (public)\n\tPowered: no\n";
	static const char info[] =
		"Device AA:BB:CC:DD:EE:FF (random)\n"
		"\tName: MX Master\n\tAlias: Mouse\n\tPaired: yes\n\tBonded: yes\n"
		"\tTrusted: yes\n\tBlocked: no\n\tConnected: no\n"
		"Device 11:22:33:44:55:66 (public)\n"
		"\tName: WH-1000XM4\n\tAlias: WH-1000XM4\n\tPaired: yes\n"
		"\tTrusted: yes\n\tConnected: yes\n\tUUID: Audio Sink\n"
		"Device 77:88:99:00:11:22 (public)\n"
		"\tPaired: no\n\tConnected: no\n"
		"\033[0;94m[CHG]\033[0m Device 33:33:33:33:33:33 RSSI: -70\n";
	RuiBtStatus st;
	RuiBtDevice d[8];
	char *cmd;
	int n;

	printf("[bluetooth module]\n");
	CHECK(rui_bt_parse_status(&rui_bt_bluez, show, &st) && st.present &&
	    st.powered && st.nconnected == 2 && !strcmp(st.device, "WH-1000XM4"),
	    "bluez: adapter on, connected devices counted and named");
	CHECK(rui_bt_parse_status(&rui_bt_bluez, off, &st) && st.present &&
	    !st.powered && st.nconnected == 0, "bluez: adapter switched off");
	CHECK(!rui_bt_parse_status(&rui_bt_bluez,
	    "No default controller available\n", &st) && !st.present &&
	    !rui_bt_parse_status(NULL, show, &st), "bluez: no adapter");
	n = rui_bt_parse_devices(&rui_bt_bluez, info, d, 8);
	CHECK(n == 3 && !strcmp(d[0].name, "WH-1000XM4") && d[0].connected &&
	    d[0].paired && !strcmp(d[0].addr, "11:22:33:44:55:66"),
	    "bluez: %d devices, the connected one first", n);
	CHECK(n == 3 && !strcmp(d[1].name, "Mouse") && d[1].paired &&
	    d[1].trusted && !d[1].connected &&
	    !strcmp(d[2].name, "77:88:99:00:11:22") && !d[2].paired,
	    "bluez: alias beats name; a nameless device shows its address");
	cmd = rui_bt_bluez.connect_cmd("AA:BB:CC:DD:EE:FF", 0);
	CHECK(cmd && strstr(cmd, "pair 'AA:BB:CC:DD:EE:FF'") &&
	    strstr(cmd, "trust 'AA:BB:CC:DD:EE:FF'") &&
	    strstr(cmd, "connect 'AA:BB:CC:DD:EE:FF'"),
	    "bluez: a new device is paired and trusted before connecting");
	free(cmd);
	cmd = rui_bt_bluez.connect_cmd("AA:BB:CC:DD:EE:FF", 1);
	CHECK(cmd && !strstr(cmd, "pair") && strstr(cmd, "connect"),
	    "bluez: a paired device is only connected");
	free(cmd);
	CHECK(rui_bt_detect("none") == NULL &&
	    rui_bt_detect("no-such-module") == NULL &&
	    strstr(rui_bt_bluez.power_cmd[1], "power on") &&
	    strstr(rui_bt_bluez.power_cmd[0], "power off"),
	    "bluetooth_backend = none; power commands");
}

/* -------------------------------------------- theme modules, wallpapers */

static void
test_theme_modules(const char *dir)
{
	const RuiThemeModule *m;
	const RuiTheme *t;
	char d[1100], **list, *w;
	int i, term = 0, broken = 0, builtin_first;

	printf("[theme modules and wallpapers]\n");
	if (!getenv("RETRO_DE_THEME_PATH") || !getenv("RETRO_DE_DATA_DIR")) {
		printf("  skip  RETRO_DE_THEME_PATH / RETRO_DE_DATA_DIR not set\n");
		return;
	}
	m = rui_theme_module("Terminal");
	t = rui_theme_find("terminal");
	CHECK(m && m->abi == RUI_THEME_ABI && m->bevel && t == &m->theme &&
	    t->style == RUI_STYLE_CLASSIC &&
	    !strcmp(t->colors[RUI_C_TEXT], "#33ff66") &&
	    !strcmp(rui_theme_kind("Terminal"), "module"),
	    "a compiled C theme is loaded, with its own bevel drawing");
	m = rui_theme_module("Redmond");
	CHECK(m && !m->bevel && m->wallpaper && !strcmp(m->wallpaper, "Clouds") &&
	    rui_theme_module("Redmond") == m,
	    "a module can name a wallpaper; modules are loaded once");
	snprintf(d, sizeof d, "%s/cfg/themes", dir);
	write_file(d, "Broken.so", "this is not a shared object\n");
	fprintf(stderr, "    (one theme module warning expected below)\n");
	CHECK(!rui_theme_module("Broken") && !rui_theme_find("Broken") &&
	    !rui_theme_module("CDE") && !rui_theme_module("../x") &&
	    !strcmp(rui_theme_kind("CDE"), "built-in") &&
	    !strcmp(rui_theme_kind("Mine"), "file") && !rui_theme_kind("nope"),
	    "a broken module is refused; built-in and file themes are not modules");
	list = rui_theme_list();
	builtin_first = list[0] && !strcmp(list[0], "CDE");
	for (i = 0; list[i]; i++) {
		term += !strcmp(list[i], "Terminal");
		broken += !strcmp(list[i], "Broken");
	}
	CHECK(term == 1 && broken == 1 && builtin_first && i >= 14,
	    "theme list: built-ins first, then %d modules and files", i - 9);
	rui_strv_free(list);

	w = rui_wallpaper_find("Clouds");
	CHECK(w && strstr(w, "/wallpapers/Clouds.ppm") && access(w, R_OK) == 0,
	    "an installed wallpaper is found by name");
	free(w);
	snprintf(d, sizeof d, "%s/cfg/wallpapers", dir);
	write_file(d, "Clouds.xpm", "/* XPM */\n");
	write_file(d, "Mine.ppm", "P6\n1 1\n255\nabc");
	w = rui_wallpaper_find("Clouds");
	CHECK(w && strstr(w, "/cfg/wallpapers/Clouds.xpm"),
	    "the user's wallpapers/ folder comes first");
	free(w);
	w = rui_wallpaper_find("/some/where/x.ppm");
	CHECK(w && !strcmp(w, "/some/where/x.ppm") && !rui_wallpaper_find("") &&
	    !rui_wallpaper_find("none") && !rui_wallpaper_find("no-such-wall"),
	    "paths pass through; unknown names and \"none\" give nothing");
	free(w);
	list = rui_wallpaper_list();
	for (i = 0, term = broken = 0; list[i]; i++) {
		term += strstr(list[i], "/Night Sky.ppm") != NULL;
		broken += strstr(list[i], "/cfg/wallpapers/Mine.ppm") != NULL;
	}
	CHECK(term == 1 && broken == 1 && i >= 14,
	    "wallpaper list: %d pictures, built and the user's", i);
	rui_strv_free(list);
	CHECK(image_mode("fill") == IMG_FILL && image_mode("stretch") ==
	    IMG_STRETCH && image_mode("center") == IMG_CENTER &&
	    image_mode("tile") == IMG_TILE && image_mode("?") == IMG_TILE,
	    "wallpaper placement names");
}

/* ------------------------------------------------------ monitor layouts */

static void
add_mode(DspOutput *o, int w, int h, double rate)
{
	DspMode *m = &o->modes[o->nmodes];

	m->id = 100 + (unsigned long)o->nmodes;
	m->w = w;
	m->h = h;
	m->rate = rate;
	m->interlaced = 0;
	o->nmodes++;
}

static void
test_display(const char *dir)
{
	DspLayout l, l2;
	DspOutput *a, *b, *c;
	char path[1100], *text;
	int w, h;

	printf("[monitor layouts]\n");
	memset(&l, 0, sizeof l);
	l.n = 3;
	a = &l.out[0];
	b = &l.out[1];
	c = &l.out[2];
	rui_strlcpy(a->name, "DP-1", sizeof a->name);
	add_mode(a, 2560, 1440, 143.97);
	add_mode(a, 2560, 1440, 59.95);
	add_mode(a, 1920, 1080, 120.0);
	add_mode(a, 1920, 1080, 60.0);
	rui_strlcpy(b->name, "HDMI-1", sizeof b->name);
	add_mode(b, 1920, 1080, 60.0);
	add_mode(b, 1920, 1080, 50.0);
	add_mode(b, 1280, 720, 60.0);
	rui_strlcpy(c->name, "eDP-1", sizeof c->name);
	add_mode(c, 1366, 768, 60.02);
	a->enabled = b->enabled = 1;
	a->mode = 0;
	a->primary = 1;
	b->mode = 0;
	b->x = 2560;
	c->mode = -1;

	CHECK(dsp_find_mode(a, 2560, 1440, 144.0) == 0 &&
	    dsp_find_mode(a, 2560, 1440, 60.0) == 1 &&
	    dsp_find_mode(a, 1920, 1080, 100.0) == 2 &&
	    dsp_find_mode(a, 800, 600, 60.0) == -1,
	    "a mode is its size plus the nearest refresh rate");
	dsp_bounds(&l, &w, &h);
	CHECK(w == 4480 && h == 1440, "screen size spans the monitors: %dx%d", w,
	    h);

	/* 144 Hz and 60 Hz side by side survive a save and a load */
	text = dsp_format(&l);
	CHECK(strstr(text, "output DP-1 mode 2560x1440 rate 143.970 pos 0x0 "
	    "rotate 0 primary\n") && strstr(text, "output HDMI-1 mode 1920x1080 "
	    "rate 60.000 pos 2560x0 rotate 0\n") && strstr(text,
	    "output eDP-1 off\n"), "monitors.conf keeps a rate per monitor");
	l2 = l;
	l2.out[0].mode = 3;
	l2.out[1].enabled = 0;
	l2.out[2].enabled = 1;
	l2.out[2].mode = 0;
	CHECK(dsp_parse(&l2, text) == 3 && dsp_equal(&l, &l2) &&
	    l2.out[0].mode == 0 && l2.out[1].enabled && !l2.out[2].enabled,
	    "a saved layout is applied back exactly");
	free(text);
	snprintf(path, sizeof path, "%s/cfg/monitors.conf", dir);
	l2 = l;
	l2.out[1].mode = 2;
	CHECK(dsp_save(&l, path) == 0 && dsp_load(&l2, path) == 3 &&
	    dsp_equal(&l, &l2) && dsp_load(&l2, "/nonexistent/m.conf") == -1,
	    "monitors.conf round trip through the file");

	/* another monitor on the same port: unknown mode -> its native one;
	 * an unplugged output in the file is ignored */
	l2 = l;
	l2.out[1].preferred = 2;
	CHECK(dsp_parse(&l2, "output HDMI-1 mode 3840x2160 rate 30 pos 2560x0\n"
	    "output VGA-9 mode 1024x768 rate 60\n# note\n") == 2 &&
	    l2.out[1].mode == 2 && l2.out[1].enabled,
	    "a mode the monitor no longer has falls back to the native one");

	/* rotation swaps the size; layouts start at 0,0 with one primary */
	l2 = l;
	l2.out[1].rot = 90;
	dsp_size(&l2.out[1], &w, &h);
	CHECK(w == 1080 && h == 1920, "a rotated monitor is %dx%d", w, h);
	l2 = l;
	l2.out[0].x = 500;
	l2.out[0].y = 300;
	l2.out[1].x = 3060;
	l2.out[1].y = 300;
	l2.out[1].primary = 1;
	dsp_normalize(&l2);
	CHECK(l2.out[0].x == 0 && l2.out[0].y == 0 && l2.out[1].x == 2560 &&
	    l2.out[0].primary + l2.out[1].primary == 1,
	    "normalizing moves the layout to the origin, keeps one primary");
	l2 = l;
	l2.out[0].enabled = l2.out[1].enabled = 0;
	dsp_normalize(&l2);
	CHECK(l2.out[0].enabled && l2.out[0].primary && !l2.out[1].enabled,
	    "the last monitor is never switched off");

	/* dropping a dragged monitor snaps it to the nearest free edge */
	l2 = l;
	l2.out[1].x = 2300;                    /* overlapping, a bit low */
	l2.out[1].y = 60;
	dsp_snap(&l2, 1);
	CHECK(l2.out[1].x == 2560 && l2.out[1].y == 0 && l2.out[0].x == 0,
	    "snap: right of the other, tops aligned (%d,%d)", l2.out[1].x,
	    l2.out[1].y);
	l2 = l;
	l2.out[1].x = -1700;
	l2.out[1].y = 600;
	dsp_snap(&l2, 1);
	CHECK(l2.out[1].x == 0 && l2.out[0].x == 1920 && l2.out[1].y == 600,
	    "snap: left of the other; the layout is renormalized (%d,%d)",
	    l2.out[1].x, l2.out[1].y);
	l2 = l;
	l2.out[1].x = 200;
	l2.out[1].y = 1300;
	dsp_snap(&l2, 1);
	CHECK(l2.out[1].y == 1440 && l2.out[1].x == 200,
	    "snap: below, keeping its offset (%d,%d)", l2.out[1].x, l2.out[1].y);
}

/* -------------------------------------------- split config files, globs */

static char split_seen[512];

static void
split_cb(const char *key, const char *val, void *ud)
{
	size_t l = strlen(split_seen);

	(void)ud;
	snprintf(split_seen + l, sizeof split_seen - l, "%s=%s;", key, val);
}

static void
test_split_config(const char *dir)
{
	char d[1100], path[1200];

	printf("[split configuration files]\n");
	snprintf(d, sizeof d, "%s/split", dir);
	write_file(d, "retro.conf", "theme = CDE\ninclude = extra/more.conf\n"
	    "focus = click\n");
	write_file(d, "keys.conf", "bind = Mod4+z exec true\n");
	write_file(d, "windows.conf", "focus = sloppy\nsnap_distance = 12\n");
	write_file(d, "programs.conf", "rule = class=XTerm => float\n");
	write_file(d, "unrelated.conf", "nope = 1\n");
	snprintf(path, sizeof path, "%s/extra", d);
	write_file(path, "more.conf", "included = yes\ninclude = ../deep.conf\n");
	write_file(d, "deep.conf", "deep = 1\n");
	snprintf(path, sizeof path, "%s/retro.conf", d);
	split_seen[0] = '\0';
	CHECK(rui_conf_parse(path, split_cb, NULL) == 0 && !strcmp(split_seen,
	    "theme=CDE;included=yes;deep=1;focus=click;bind=Mod4+z exec true;"
	    "focus=sloppy;snap_distance=12;rule=class=XTerm => float;"),
	    "retro.conf, its includes, then keys/windows/programs.conf in order");
	snprintf(path, sizeof path, "%s/keys.conf", d);
	split_seen[0] = '\0';
	CHECK(rui_conf_parse(path, split_cb, NULL) == 0 &&
	    !strcmp(split_seen, "bind=Mod4+z exec true;"),
	    "any other file is read alone");
	write_file(d, "loop.conf", "include = loop.conf\n");
	snprintf(path, sizeof path, "%s/loop.conf", d);
	fprintf(stderr, "    (include-depth warnings expected below)\n");
	CHECK(rui_conf_parse(path, split_cb, NULL) == 0,
	    "an include loop ends with a warning, not a crash");
	CHECK(rui_glob("*fire*", "Mozilla Firefox") && rui_glob("xterm", "XTerm") &&
	    rui_glob("x?erm*", "xterm") && rui_glob("*", "") &&
	    !rui_glob("xterm", "xterm2") && !rui_glob("a*b", "abc") &&
	    rui_glob("a**c", "abXc") && !rui_glob("?", ""),
	    "rule patterns: * and ?, ignoring case");
	CHECK((!strcmp(rui_saver_kind(), "xscreensaver")) ==
	    rui_have_cmd("xscreensaver") &&
	    rui_settings()->saver_minutes == 10 &&
	    rui_settings()->dpms_minutes == 20,
	    "screen saver: xscreensaver when installed, else blanking (%s)",
	    rui_saver_kind());
}

/* ------------------------------------------------------------ calculator */

static int
calc_is(CalcState *st, const char *expr, const char *want)
{
	char err[96], out[128];
	double v;

	if (calc_eval(st, expr, &v, err, sizeof err) < 0)
		snprintf(out, sizeof out, "error: %s", err);
	else
		calc_format(v, 10, out, sizeof out);
	if (strcmp(out, want))
		printf("        %s gave \"%s\", expected \"%s\"\n", expr, out, want);
	return !strcmp(out, want);
}

static void
test_calc(void)
{
	CalcState st;
	char buf[80];
	int bad = 0;

	printf("[calculator]\n");
	memset(&st, 0, sizeof st);
	CHECK(calc_is(&st, "1 + 2 * 3", "7") && calc_is(&st, "(1 + 2) * 3", "9") &&
	    calc_is(&st, "-2^2", "-4") && calc_is(&st, "2^3^2", "512") &&
	    calc_is(&st, "2**10", "1024") && calc_is(&st, "7 % 4", "3") &&
	    calc_is(&st, "10 mod 4", "2") && calc_is(&st, "0.1 + 0.2", "0.3") &&
	    calc_is(&st, "1e3 / 4", "250") && calc_is(&st, "--3", "3"),
	    "arithmetic: precedence, right-to-left powers, rounding");
	CHECK(calc_is(&st, "2pi", "6.28318530718") &&
	    calc_is(&st, "3(4 + 1)", "15") && calc_is(&st, "2 sqrt(16)", "8") &&
	    calc_is(&st, "(1 + 1)(2 + 1)", "6"),
	    "multiplication without a sign: 2pi, 3(4+1)");
	CHECK(calc_is(&st, "sin(pi/2)", "1") && calc_is(&st, "cos(0)", "1") &&
	    calc_is(&st, "atan2(1, 1)*4", "3.14159265359") &&
	    calc_is(&st, "ln(e^2)", "2") && calc_is(&st, "log(1000)", "3") &&
	    calc_is(&st, "log2(1024)", "10") && calc_is(&st, "logb(81, 3)", "4") &&
	    calc_is(&st, "sqrt(2)^2", "2") && calc_is(&st, "cbrt(27)", "3") &&
	    calc_is(&st, "root(-27, 3)", "-3") && calc_is(&st, "hypot(3, 4)", "5") &&
	    calc_is(&st, "exp(0)", "1") && calc_is(&st, "abs(-2.5)", "2.5") &&
	    calc_is(&st, "floor(-1.5) + ceil(1.2) + round(2.5)", "3"),
	    "scientific functions in radians");
	st.degrees = 1;
	CHECK(calc_is(&st, "sin(30)", "0.5") && calc_is(&st, "cos(60)", "0.5") &&
	    calc_is(&st, "tan(45)", "1") && calc_is(&st, "asin(1)", "90") &&
	    calc_is(&st, "atan(1)", "45") && calc_is(&st, "rad(180)", "3.14159265359"),
	    "degree mode");
	st.degrees = 0;
	CHECK(calc_is(&st, "5!", "120") && calc_is(&st, "3!!", "720") &&
	    calc_is(&st, "0!", "1") && calc_is(&st, "ncr(52, 5)", "2598960") &&
	    calc_is(&st, "npr(5, 2)", "20") && calc_is(&st, "gcd(12, 18)", "6") &&
	    calc_is(&st, "lcm(4, 6)", "12") && calc_is(&st, "max(2, min(9, 5))", "5"),
	    "factorials and combinatorics");
	CHECK(calc_is(&st, "ncr(1000, 500)", "2.70288240945e+299") &&
	    calc_is(&st, "ncr(1100, 550)", "error: too large (overflow)") &&
	    calc_is(&st, "ncr(1e15, 5e14)", "error: too large (overflow)") &&
	    calc_is(&st, "ncr(1e300, 1e299)", "error: too large (overflow)") &&
	    calc_is(&st, "ncr(1e15, 2)", "5e+29") &&
	    calc_is(&st, "npr(57!, 55!)", "error: too large") &&
	    calc_is(&st, "ncr(2e499, 5e1405)", "error: too large") &&
	    calc_is(&st, "ncr(1e400, 3)", "error: too large"),
	    "huge ncr/npr give up quickly instead of hanging");
	CHECK(calc_is(&st, "0xff", "255") && calc_is(&st, "0b1010", "10") &&
	    calc_is(&st, "0o17", "15") && calc_is(&st, "0xff & 0b1010", "10") &&
	    calc_is(&st, "1 << 10", "1024") && calc_is(&st, "1024 >> 3", "128") &&
	    calc_is(&st, "5 | 2", "7") && calc_is(&st, "6 xor 3", "5") &&
	    calc_is(&st, "~5", "-6") && calc_is(&st, "1 + 2 << 2", "12"),
	    "other bases and bit operations");
	CHECK(calc_is(&st, "r = 2.5", "2.5") && calc_is(&st, "2 r", "5") &&
	    calc_is(&st, "ans + 1", "6") && calc_is(&st, "r = r * 2", "5") &&
	    calc_is(&st, "area_1 = pi r^2", "78.5398163397") &&
	    calc_is(&st, "area_1 / pi", "25") && st.nvar == 2,
	    "variables and ans");
	CHECK(calc_is(&st, "1/0", "error: division by zero") &&
	    calc_is(&st, "sqrt(-1)", "error: not a real number") &&
	    calc_is(&st, "2 +", "error: the expression is incomplete") &&
	    calc_is(&st, "(1 + 2", "error: ')' expected") &&
	    calc_is(&st, "1 + 2)", "error: ')' without '('") &&
	    calc_is(&st, "foo + 1", "error: unknown name 'foo'") &&
	    calc_is(&st, "sin", "error: '(' expected after a function") &&
	    calc_is(&st, "pow(2)", "error: this function takes two arguments") &&
	    calc_is(&st, "pi = 3", "error: that name is taken") &&
	    calc_is(&st, "1.5 & 1", "error: bit operations need whole numbers") &&
	    calc_is(&st, "10^400", "error: too large (overflow)") &&
	    calc_is(&st, "2 $ 3", "error: unexpected character") &&
	    calc_is(&st, "ans", "25"),
	    "errors are reported and leave the last result alone");
	calc_format(255, 16, buf, sizeof buf);
	bad += strcmp(buf, "0xFF") != 0;
	calc_format(10, 2, buf, sizeof buf);
	bad += strcmp(buf, "0b1010") != 0;
	calc_format(-8, 8, buf, sizeof buf);
	bad += strcmp(buf, "-0o10") != 0;
	calc_format(2.5, 16, buf, sizeof buf);
	bad += strcmp(buf, "2.5") != 0;
	calc_format(1e21, 10, buf, sizeof buf);
	bad += strcmp(buf, "1e+21") != 0;
	CHECK(bad == 0, "results in hex, binary and octal; fractions stay decimal");
}

/* a zlib stream of stored blocks around data (what PNG wants) */
static unsigned char *
zlib_store(const unsigned char *d, size_t n, size_t *out)
{
	size_t gl, i;
	unsigned char *gz = rui_gzip_store(d, n, &gl), *z;
	unsigned long a = 1, b = 0;

	/* the deflate part of the gzip member sits between 10-byte header and
	 * 8-byte trailer */
	z = rui_malloc(gl - 18 + 6);
	z[0] = 0x78;
	z[1] = 0x01;
	memcpy(z + 2, gz + 10, gl - 18);
	for (i = 0; i < n; i++) {
		a = (a + d[i]) % 65521;
		b = (b + a) % 65521;
	}
	z[gl - 16] = (unsigned char)(b >> 8);
	z[gl - 15] = (unsigned char)b;
	z[gl - 14] = (unsigned char)(a >> 8);
	z[gl - 13] = (unsigned char)a;
	*out = gl - 18 + 6;
	free(gz);
	return z;
}

static void
put32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static size_t
png_chunk(unsigned char *o, const char *type, const unsigned char *d,
    size_t n)
{
	put32(o, (unsigned long)n);
	memcpy(o + 4, type, 4);
	memcpy(o + 8, d, n);
	put32(o + 8 + n, rui_crc32(0, o + 4, n + 4));
	return n + 12;
}

static int
px_is(const unsigned char *rgba, int w, int x, int y, int r, int g, int b,
    int amin)
{
	const unsigned char *p = rgba + ((size_t)y * w + x) * 4;

	return abs(p[0] - r) < 24 && abs(p[1] - g) < 24 && abs(p[2] - b) < 24 &&
	    p[3] >= amin;
}

static void
test_media(void)
{
	static const unsigned char raw[] = {
		0, 255, 0, 0, 255, 0, 255, 0, 128,      /* row 0: red, green 50% */
		0, 0, 0, 255, 255, 0, 0, 0, 0           /* row 1: blue, clear */
	};
	unsigned char big[5000], *gz, *back, png[512], ihdr[13], *z, *rgba;
	size_t gl, bl, zl, pl = 8;
	char svg[600];
	int i, w, h, ok;

	printf("[PNG, SVG, gzip]\n");
	CHECK(rui_crc32(0, (const unsigned char *)"123456789", 9) == 0xcbf43926UL,
	    "crc32 check value");
	for (i = 0; i < (int)sizeof big; i++)
		big[i] = (unsigned char)(i * 7 % 251);
	gz = rui_gzip_store(big, sizeof big, &gl);
	back = rui_gunzip(gz, gl, &bl);
	CHECK(back && bl == sizeof big && !memcmp(back, big, bl),
	    "gzip written and read back");
	free(back);
	CHECK(rui_gunzip(gz, gl / 2, &bl) == NULL, "a truncated gzip is refused");
	free(gz);

	/* a 2x2 RGBA PNG made by hand */
	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	put32(ihdr, 2);
	put32(ihdr + 4, 2);
	ihdr[8] = 8;
	ihdr[9] = 6;
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	pl += png_chunk(png + pl, "IHDR", ihdr, 13);
	z = zlib_store(raw, sizeof raw, &zl);
	pl += png_chunk(png + pl, "IDAT", z, zl);
	free(z);
	pl += png_chunk(png + pl, "IEND", NULL, 0);
	rgba = rui_png_decode(png, pl, &w, &h);
	CHECK(rgba && w == 2 && h == 2, "PNG decoded (2x2)");
	if (rgba) {
		CHECK(px_is(rgba, 2, 0, 0, 255, 0, 0, 255) &&
		    px_is(rgba, 2, 1, 0, 0, 255, 0, 128) && rgba[7] == 128 &&
		    px_is(rgba, 2, 0, 1, 0, 0, 255, 255) && rgba[15] == 0,
		    "PNG pixels and alpha");
		free(rgba);
	}
	png[20] ^= 0xff;                       /* corrupt the width */
	rgba = rui_png_decode(png, pl, &w, &h);
	CHECK(rgba == NULL, "a broken PNG is refused");
	free(rgba);

	/* SVG: shapes, a path without separators, a gradient */
	snprintf(svg, sizeof svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
	    "viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"5\" "
	    "fill=\"#ff0000\"/><circle cx=\"5\" cy=\"7.5\" r=\"2\" "
	    "style=\"fill:blue\"/></svg>");
	w = h = 20;
	rgba = rui_svg_render(svg, strlen(svg), &w, &h);
	ok = rgba && px_is(rgba, 20, 10, 4, 255, 0, 0, 250) &&
	    px_is(rgba, 20, 10, 15, 0, 0, 255, 250) && rgba[(19 * 20) * 4 + 3] == 0;
	CHECK(ok, "SVG rect, circle and transparency");
	free(rgba);
	snprintf(svg, sizeof svg, "<svg width=\"8\" height=\"8\"><path "
	    "d=\"M0 0L8 0L8 8L0 8z\" fill=\"#00ff00\"/></svg>");
	w = h = 0;
	rgba = rui_svg_render(svg, strlen(svg), &w, &h);
	CHECK(rgba && w == 8 && h == 8 && px_is(rgba, 8, 0, 0, 0, 255, 0, 250) &&
	    px_is(rgba, 8, 7, 7, 0, 255, 0, 250), "SVG path commands right after "
	    "numbers (\"0L8\")");
	free(rgba);
	snprintf(svg, sizeof svg, "<svg viewBox=\"0 0 20 4\"><defs>"
	    "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#000\"/>"
	    "<stop offset=\"1\" stop-color=\"#fff\"/></linearGradient></defs>"
	    "<rect width=\"20\" height=\"4\" fill=\"url(#g)\"/></svg>");
	w = 20;
	h = 4;
	rgba = rui_svg_render(svg, strlen(svg), &w, &h);
	CHECK(rgba && rgba[(1 * 20 + 1) * 4] < 40 && rgba[(1 * 20 + 18) * 4] > 215,
	    "SVG linear gradient");
	free(rgba);
}

static void
test_bus(const char *dir)
{
	char *e, buf[128], *f[4], *path;
	int n;
	unsigned char rgba[4 * 4 * 4];
	FILE *fp;

	printf("[bus and learned icons]\n");
	e = rui_bus_escape("a\\b\tc\nd\re");
	CHECK(!strcmp(e, "a\\\\b\\tc\\nd\\re"), "escape: %s", e);
	rui_bus_unescape(e);
	CHECK(!strcmp(e, "a\\b\tc\nd\re"), "unescape restores the text");
	free(e);
	rui_strlcpy(buf, "one\ttwo\t\tfour", sizeof buf);
	n = rui_bus_fields(buf, f, 4);
	CHECK(n == 4 && !strcmp(f[0], "one") && !strcmp(f[1], "two") &&
	    !*f[2] && !strcmp(f[3], "four"), "fields split at tabs (%d)", n);
	CHECK(rui_bus_word_ok("notify.show", 0) && !rui_bus_word_ok("a b", 0) &&
	    !rui_bus_word_ok("", 0) && !rui_bus_word_ok("wm.*", 0) &&
	    rui_bus_word_ok("wm.*", 1), "names, methods and patterns");
	unsetenv("RETRO_BUS");
	setenv("DISPLAY", ":3.0", 1);
	path = rui_bus_path();
	CHECK(strstr(path, "/bus-3") && !strstr(path, "3.0"),
	    "bus path per display: %s", path);
	free(path);
	setenv("RETRO_BUS", "/x/y", 1);
	path = rui_bus_path();
	CHECK(!strcmp(path, "/x/y"), "RETRO_BUS wins");
	free(path);
	unsetenv("RETRO_BUS");
	CHECK(rui_bus_connect() < 0 && rui_bus_emit("x.y", "") < 0,
	    "no bus: calls fail quietly");

	/* a 4x4 icon: transparent top row, then red, green, blue rows */
	for (n = 0; n < 16; n++) {
		rgba[n * 4] = n >= 4 && n < 8 ? 255 : 0;
		rgba[n * 4 + 1] = n >= 8 && n < 12 ? 255 : 0;
		rgba[n * 4 + 2] = n >= 12 ? 255 : 0;
		rgba[n * 4 + 3] = n < 4 ? 0 : 255;
	}
	(void)dir;                            /* the config dir is fixed by now */
	path = rui_icon_learned_path("Foo App!");
	CHECK(path && strstr(path, "/icons/learned/fooapp.xpm"),
	    "learned icon name: %s", path ? path : "(null)");
	free(path);
	CHECK(rui_icon_learned_path("...") == NULL, "unusable names refused");
	path = rui_strdupf("%s/icons", rui_config_dir());
	mkdir(path, 0755);
	free(path);
	path = rui_strdupf("%s/icons/learned", rui_config_dir());
	mkdir(path, 0755);
	free(path);
	path = rui_icon_learned_path("unittest");
	CHECK(rui_icon_save_xpm(path, 4, 4, rgba) == 0, "XPM written");
	if ((fp = fopen(path, "r"))) {
		char all[1024];
		size_t len = fread(all, 1, sizeof all - 1, fp);

		all[len] = '\0';
		fclose(fp);
		CHECK(strstr(all, "\"4 4 4 2\"") && strstr(all, "c None") &&
		    strstr(all, "#ff0000") && strstr(all, "#0000ff") &&
		    strstr(all, "\"        \""), "XPM: size, colours, transparency");
	}
	free(path);
	path = rui_icon_find("unittest");
	CHECK(path != NULL, "found by name among the learned icons");
	free(path);
	path = rui_icon_for_cmd("/usr/bin/unittest --flag");
	CHECK(path != NULL, "found by a command's program name");
	free(path);
	CHECK(rui_icon_for_cmd("@terminal") == NULL, "@-commands keep pictograms");
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";

	desktop_fixtures(dir);
	settings_fixtures(dir);
	test_util(dir);
	test_menu();
	test_audio();
	test_net();
	test_sys();
	test_images(dir);
	test_desktop(dir);
	test_settings(dir);
	test_power();
	test_programs(dir);
	test_bluetooth();
	test_theme_modules(dir);
	test_display(dir);
	test_split_config(dir);
	test_calc();
	test_bus(dir);
	test_media();
	printf("%d checks, %d failed\n", checks, failures);
	return failures;
}
