/*
 * libretroui settings: the shared part of retro.conf (theme, style, font,
 * default tools, D-Bus / XDG switches, power), retro.conf writing and the
 * reload request, and default-tool resolution.
 *
 * Precedence: retro.conf > theme > config.h.
 */
#include "retroui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

const char *const rui_tool_keys[RUI_TOOL_COUNT] = {
	"terminal", "browser", "filemanager", "editor", "locker", "clock"
};

static RuiSettings S;
static int loaded;

static int
tristate(const char *v)
{
	if (!strcmp(v, "auto"))
		return RUI_AUTO;
	return rui_conf_bool(v) ? RUI_ON : RUI_OFF;
}

static void
settings_conf(const char *key, const char *val, void *ud)
{
	static const char *const ckeys[RUI_C_THEMED] = {
		"color.face", "color.light", "color.shadow", "color.dark",
		"color.text", "color.disabled", "color.field", "color.select_bg",
		"color.select_fg", "color.title_active", "color.title_inactive",
		"color.title_fg", "color.title_fg_inactive", "color.desktop",
		"color.desktop_fg", "color.alert", "color.good",
	};
	int *style = ud, i;

	if (!strcmp(key, "theme"))
		rui_strlcpy(S.theme, val, sizeof S.theme);
	else if (!strcmp(key, "style"))
		*style = rui_style_parse(val);
	else if (!strcmp(key, "font"))
		rui_strlcpy(S.font, val, sizeof S.font);
	else if (!strcmp(key, "font.bold"))
		rui_strlcpy(S.font_bold, val, sizeof S.font_bold);
	else if (!strcmp(key, "desktop_pattern"))
		rui_strlcpy(S.pattern, val, sizeof S.pattern);
	else if (!strcmp(key, "dbus"))
		S.dbus = tristate(val);
	else if (!strcmp(key, "xdg_menus"))
		S.xdg_menus = rui_conf_bool(val);
	else if (!strcmp(key, "xdg_autostart"))
		S.xdg_autostart = rui_conf_bool(val);
	else if (!strcmp(key, "confirm_logout"))
		S.confirm = rui_conf_bool(val);
	else if (!strcmp(key, "power_backend"))
		rui_strlcpy(S.power, val, sizeof S.power);
	else if (!strcmp(key, "power_priv"))
		rui_strlcpy(S.power_priv, val, sizeof S.power_priv);
	else if (!strcmp(key, "workspace_names"))
		rui_strlcpy(S.workspace_names, val, sizeof S.workspace_names);
	else if (!strcmp(key, "programs_dir"))
		rui_strlcpy(S.programs_dir, val, sizeof S.programs_dir);
	else if (!strcmp(key, "screensaver"))
		rui_strlcpy(S.screensaver, val, sizeof S.screensaver);
	else if (!strcmp(key, "screensaver_timeout"))
		S.saver_minutes = atoi(val);       /* "never" is 0 */
	else if (!strcmp(key, "monitor_off"))
		S.dpms_minutes = atoi(val);
	else if (!strcmp(key, "bus"))
		S.bus = rui_conf_bool(val);
	else if (!strcmp(key, "notifications"))
		S.notify = rui_conf_bool(val);
	else if (!strcmp(key, "dbus_server"))
		rui_strlcpy(S.dbus_server, val, sizeof S.dbus_server);
	else if (!strcmp(key, "dbus_portal"))
		S.dbus_portal = rui_conf_bool(val);
	else if (!strcmp(key, "icon_theme"))
		rui_strlcpy(S.icon_theme, strcmp(val, "none") ? val : "",
		    sizeof S.icon_theme);
	else if (!strcmp(key, "notification_time"))
		S.notify_seconds = atoi(val);
	else {
		for (i = 0; i < RUI_TOOL_COUNT; i++)
			if (!strcmp(key, rui_tool_keys[i]))
				rui_strlcpy(S.tool[i], val, sizeof S.tool[i]);
		for (i = 0; i < RUI_C_THEMED; i++)
			if (!strcmp(key, ckeys[i]))
				rui_strlcpy(S.color[i], val, sizeof S.color[i]);
	}
}

const RuiSettings *
rui_settings(void)
{
	const RuiTheme *th;
	char *path;
	int style = -1, i;

	if (loaded)
		return &S;
	loaded = 1;
	memset(&S, 0, sizeof S);
	rui_strlcpy(S.theme, rui_config.theme, sizeof S.theme);
	if (rui_config.style && *rui_config.style)
		style = rui_style_parse(rui_config.style);
	for (i = 0; i < RUI_TOOL_COUNT; i++)
		rui_strlcpy(S.tool[i], rui_config.tools[i], sizeof S.tool[i]);
	S.dbus = tristate(rui_config.dbus);
	S.xdg_menus = 1;
	S.xdg_autostart = rui_config.xdg_autostart;
	S.confirm = 1;
	rui_strlcpy(S.screensaver, "auto", sizeof S.screensaver);
	S.saver_minutes = 10;
	S.dpms_minutes = 20;
	S.bus = 1;
	S.notify = 1;
	S.notify_seconds = 6;
	rui_strlcpy(S.dbus_server, "auto", sizeof S.dbus_server);
	S.dbus_portal = 1;
	rui_strlcpy(S.power, "auto", sizeof S.power);
	rui_strlcpy(S.power_priv, "auto", sizeof S.power_priv);
	rui_strlcpy(S.workspace_names, rui_config.workspace_names,
	    sizeof S.workspace_names);
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, settings_conf, &style);
	free(path);

	if (!(th = rui_theme_find(S.theme))) {
		rui_warn("unknown theme '%s', using %s", S.theme, rui_themes[0].name);
		th = rui_theme_find(rui_themes[0].name);
		rui_strlcpy(S.theme, rui_themes[0].name, sizeof S.theme);
	}
	S.style = style >= 0 ? style : th->style;
	if (!S.font[0])
		rui_strlcpy(S.font, th->font ? th->font : rui_config.font,
		    sizeof S.font);
	if (!S.pattern[0])
		rui_strlcpy(S.pattern, th->pattern ? th->pattern : "solid",
		    sizeof S.pattern);
	for (i = 0; i < RUI_C_THEMED; i++)
		if (!S.color[i][0] && th->colors[i])
			rui_strlcpy(S.color[i], th->colors[i], sizeof S.color[i]);
	return &S;
}

const char *
rui_saver_kind(void)
{
	const char *v = rui_settings()->screensaver;

	if (!strcmp(v, "off") || !strcmp(v, "none"))
		return "off";
	if (!strcmp(v, "blank"))
		return "blank";
	return rui_have_cmd("xscreensaver") ? "xscreensaver" : "blank";
}

int
rui_xdg_menus(void)
{
	return rui_config.desktop_entries && rui_settings()->xdg_menus;
}

/* ------------------------------------------------------- writing retro.conf */

int
rui_conf_set(const char *path, const char *key, const char *value)
{
	FILE *in = fopen(path, "r"), *out;
	char tmp[1100], line[2048], *slash, dir[1024];
	size_t klen = strlen(key);
	int done = 0;

	rui_strlcpy(dir, path, sizeof dir);
	if ((slash = strrchr(dir, '/'))) {
		*slash = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			if (in)
				fclose(in);
			return -1;
		}
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!(out = fopen(tmp, "w"))) {
		if (in)
			fclose(in);
		return -1;
	}
	while (in && fgets(line, sizeof line, in)) {
		const char *s = line + strspn(line, " \t");

		if (!done && !strncmp(s, key, klen) &&
		    (s[klen] == ' ' || s[klen] == '\t' || s[klen] == '=')) {
			const char *eq = s + klen + strspn(s + klen, " \t");

			if (*eq == '=') {
				fprintf(out, "%s = %s\n", key, value);
				done = 1;
				continue;
			}
		}
		fputs(line, out);
		if (!strchr(line, '\n'))
			fputc('\n', out);
	}
	if (!done)
		fprintf(out, "%s = %s\n", key, value);
	if (in)
		fclose(in);
	if (fclose(out) != 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	loaded = 0;                           /* re-read on the next access */
	return 0;
}

void
rui_request_reload(Display *dpy)
{
	Window root = DefaultRootWindow(dpy);
	Atom a = XInternAtom(dpy, "_RETRO_RELOAD", False), type;
	unsigned long serial = 0, nitems, after;
	unsigned char *data = NULL;
	int fmt;

	if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_CARDINAL, &type, &fmt,
	    &nitems, &after, &data) == Success && data) {
		if (fmt == 32 && nitems == 1)
			serial = *(unsigned long *)(void *)data;
		XFree(data);
	}
	serial++;
	XChangeProperty(dpy, root, a, XA_CARDINAL, 32, PropModeReplace,
	    (unsigned char *)&serial, 1);
	XFlush(dpy);
}

/* ------------------------------------------------------------ default tools */

static const char *const browsers[] = {
	"firefox", "chromium", "chromium-browser", "google-chrome-stable",
	"brave", "librewolf", "falkon", "epiphany", "midori", "netsurf-gtk",
	"dillo", "qutebrowser", "@terminal -e lynx", "@terminal -e w3m", NULL
};
static const char *const file_managers[] = {
	"retro-files", "pcmanfm", "thunar", "caja", "nemo", "nautilus", "dolphin",
	"spacefm", "pcmanfm-qt", "xfe", "rox", "@terminal -e mc",
	"@terminal -e ranger", "@terminal -e nnn", NULL
};
static const char *const editors[] = {
	"mousepad", "pluma", "gedit", "gnome-text-editor", "featherpad", "kate",
	"leafpad", "geany", "nedit", "xedit", "@terminal -e vi", NULL
};
static const char *const lockers[] = {
	"xsecurelock", "slock", "i3lock -c 5d6f8e", "xlock -mode blank",
	"xscreensaver-command -lock", "light-locker-command -l",
	"loginctl lock-session", NULL
};
/* xclock first: it is the clock of the default choice */
static const char *const clocks[] = {
	"xclock", "oclock", "dclock", "xdaliclock", "buici-clock", "alarm-clock",
	"gnome-clocks", "kclock", "@terminal -e tty-clock", NULL
};
static const char *const *const fallbacks[RUI_TOOL_COUNT] = {
	NULL, browsers, file_managers, editors, lockers, clocks
};
static const char *const mimes[RUI_TOOL_COUNT] = {
	NULL, "x-scheme-handler/http", "inode/directory", "text/plain", NULL, NULL
};

/* is the program a command line would run installed? */
static int
installed(const char *cmd)
{
	char prog[256];

	if (!strncmp(cmd, "@terminal -e ", 13))
		cmd += 13;
	else if (!strncmp(cmd, "@app ", 5))
		return 1;
	rui_strlcpy(prog, cmd, sizeof prog);
	prog[strcspn(prog, " \t")] = '\0';
	return rui_have_cmd(prog);
}

/* the XDG default application for a MIME type, as "@app ID" */
static char *
xdg_default(const char *mime)
{
	char cmd[256], out[256] = "";
	RuiDesktopEntry *e;
	FILE *p;

	if (!rui_xdg_menus() || !rui_have_cmd("xdg-mime"))
		return NULL;
	snprintf(cmd, sizeof cmd, "xdg-mime query default '%s' 2>/dev/null", mime);
	if (!(p = popen(cmd, "r")))
		return NULL;
	if (!fgets(out, sizeof out, p))
		out[0] = '\0';
	pclose(p);
	out[strcspn(out, "\r\n")] = '\0';
	if (!out[0] || !(e = rui_app_find(out)))
		return NULL;
	rui_desktop_free(e);
	return rui_strdupf("@app %s", out);
}

char *
rui_tool_command(int tool)
{
	const char *v;
	const char *const *c;
	char *x;

	if (tool < 0 || tool >= RUI_TOOL_COUNT)
		return NULL;
	v = rui_settings()->tool[tool];
	if (*v && strcmp(v, "auto"))
		return rui_strdup(v);
	if (tool == RUI_TOOL_TERMINAL)
		return rui_strdup(rui_terminal());
	/* a running xscreensaver is also the locker */
	if (tool == RUI_TOOL_LOCKER && !strcmp(rui_saver_kind(), "xscreensaver") &&
	    rui_have_cmd("xscreensaver-command"))
		return rui_strdup("xscreensaver-command -lock");
	if (mimes[tool] && (x = xdg_default(mimes[tool])))
		return x;
	for (c = fallbacks[tool]; *c; c++)
		if (installed(*c))
			return rui_strdup(*c);
	return NULL;
}
