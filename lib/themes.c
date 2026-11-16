/*
 * libretroui themes: built-in palettes, user theme files in
 * ~/.config/retro-de/themes/ (one NAME.theme file per theme) and theme
 * modules - compiled C code in NAME.so files (see RuiThemeModule).
 *
 * A theme file uses the retro.conf syntax:
 *   style = cde            (or classic)
 *   pattern = weave        (default backdrop pattern, optional)
 *   font = sans-9          (optional)
 *   color.face = #aeb2c3   (any of the color.* keys)
 */
#include "retroui.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef RETRO_LIBDIR
# define RETRO_LIBDIR "/usr/local/lib/retro-de"
#endif
#ifndef RETRO_DATADIR
# define RETRO_DATADIR "/usr/local/share/retro-de"
#endif

/* face light shadow dark text disabled field sel_bg sel_fg title_active
 * title_inactive title_fg title_fg_inactive desktop desktop_fg alert good */
const RuiTheme rui_themes[] = {
	{ "CDE", RUI_STYLE_CDE, {
	  "#aeb2c3", "#e1e3eb", "#5f6680", "#2d3040", "#000000", "#7c8296",
	  "#c4c7d4", "#8a90a9", "#000000", "#b24d7a", "#aeb2c3", "#ffffff",
	  "#000000", "#5d6f8e", "#ffffff", "#b02020", "#2e8b57" },
	  "weave", NULL },
	{ "Classic", RUI_STYLE_CLASSIC, {
	  "#c0c0c0", "#ffffff", "#808080", "#000000", "#000000", "#808080",
	  "#ffffff", "#000080", "#ffffff", "#000080", "#808080", "#ffffff",
	  "#c0c0c0", "#008080", "#ffffff", "#c00000", "#008000" },
	  "solid", NULL },
	{ "Broica", RUI_STYLE_CDE, {
	  "#b8aa8c", "#e4dccb", "#6e624b", "#2f271a", "#000000", "#7d725d",
	  "#d2c7b0", "#9c8d70", "#000000", "#8a5a33", "#b8aa8c", "#ffffff",
	  "#000000", "#7a684b", "#ffffff", "#a02818", "#4f7a32" },
	  "bricks", NULL },
	{ "Solaris", RUI_STYLE_CDE, {
	  "#9fa9c6", "#d9deeb", "#535d7c", "#262b3b", "#000000", "#6b7393",
	  "#bcc3d9", "#7f89ab", "#000000", "#4c5f99", "#9fa9c6", "#ffffff",
	  "#000000", "#45507a", "#ffffff", "#b02020", "#2e8b57" },
	  "dots", NULL },
	{ "Olive", RUI_STYLE_CDE, {
	  "#a9ad8c", "#dde0c9", "#5d6146", "#2a2c1d", "#000000", "#777b5f",
	  "#c4c7ad", "#8c906f", "#000000", "#6b7a3a", "#a9ad8c", "#ffffff",
	  "#000000", "#58603f", "#ffffff", "#a83020", "#3b6e2c" },
	  "diagonal", NULL },
	{ "Crimson", RUI_STYLE_CDE, {
	  "#c2a7a7", "#ecdede", "#6f5050", "#331f1f", "#000000", "#8a6f6f",
	  "#dccaca", "#a68787", "#000000", "#8b1e2e", "#c2a7a7", "#ffffff",
	  "#000000", "#6b3a40", "#ffffff", "#a01010", "#3f7040" },
	  "weave", NULL },
	{ "Motif", RUI_STYLE_CDE, {
	  "#bebebe", "#efefef", "#717171", "#2b2b2b", "#000000", "#7f7f7f",
	  "#d6d6d6", "#9e9e9e", "#000000", "#7186a8", "#bebebe", "#ffffff",
	  "#000000", "#5a6a7a", "#ffffff", "#b02020", "#2e8b57" },
	  "solid", NULL },
	{ "Platinum", RUI_STYLE_CLASSIC, {
	  "#dddddd", "#ffffff", "#888888", "#262626", "#000000", "#999999",
	  "#ffffff", "#3d3d99", "#ffffff", "#cccccc", "#dddddd", "#000000",
	  "#777777", "#6b6bb3", "#ffffff", "#b02020", "#2e8b57" },
	  "checker", NULL },
	{ "HighContrast", RUI_STYLE_CDE, {
	  "#ffffff", "#ffffff", "#000000", "#000000", "#000000", "#606060",
	  "#ffffff", "#000000", "#ffffff", "#000000", "#ffffff", "#ffffff",
	  "#000000", "#000000", "#ffffff", "#d00000", "#008000" },
	  "solid", "sans-11" },
	{ NULL, 0, { NULL }, NULL, NULL }
};

static const char *const color_keys[RUI_C_THEMED] = {
	"color.face", "color.light", "color.shadow", "color.dark", "color.text",
	"color.disabled", "color.field", "color.select_bg", "color.select_fg",
	"color.title_active", "color.title_inactive", "color.title_fg",
	"color.title_fg_inactive", "color.desktop", "color.desktop_fg",
	"color.alert", "color.good",
};

int
rui_style_parse(const char *s)
{
	if (!strcasecmp(s, "classic") || !strcasecmp(s, "win95"))
		return RUI_STYLE_CLASSIC;
	if (!strcasecmp(s, "cde") || !strcasecmp(s, "motif"))
		return RUI_STYLE_CDE;
	return -1;
}

/* one user theme is loaded at a time; its strings live in this buffer */
static struct {
	RuiTheme t;
	char name[64], colors[RUI_C_THEMED][64], pattern[32], font[128];
} user;

static void
theme_conf(const char *key, const char *val, void *ud)
{
	int i;

	(void)ud;
	if (!strcmp(key, "style")) {
		if (rui_style_parse(val) >= 0)
			user.t.style = rui_style_parse(val);
	} else if (!strcmp(key, "pattern")) {
		rui_strlcpy(user.pattern, val, sizeof user.pattern);
		user.t.pattern = user.pattern;
	} else if (!strcmp(key, "font")) {
		rui_strlcpy(user.font, val, sizeof user.font);
		user.t.font = user.font;
	} else {
		for (i = 0; i < RUI_C_THEMED; i++)
			if (!strcmp(key, color_keys[i])) {
				rui_strlcpy(user.colors[i], val, sizeof user.colors[i]);
				user.t.colors[i] = user.colors[i];
			}
	}
}

/* ---------------------------------------------------------- theme modules */

#define MAX_THEME_DIRS 8
#define MAX_MODULES 64

/* directories searched for NAME.so, most specific first */
static int
theme_dirs(char dirs[MAX_THEME_DIRS][1024])
{
	const char *e = getenv("RETRO_DE_THEME_PATH");
	char *user;
	int n = 0;

	while (e && *e && n < MAX_THEME_DIRS - 2) {
		size_t len = strcspn(e, ":");

		if (len && len < sizeof dirs[0]) {
			memcpy(dirs[n], e, len);
			dirs[n++][len] = '\0';
		}
		e += len;
		if (*e == ':')
			e++;
	}
	user = rui_config_path("themes");
	rui_strlcpy(dirs[n++], user, sizeof dirs[0]);
	free(user);
	rui_strlcpy(dirs[n++], RETRO_LIBDIR "/themes", sizeof dirs[0]);
	return n;
}

/* modules stay loaded: their strings are referenced for the process's life */
static struct {
	char name[64];
	const RuiThemeModule *mod;
} loaded[MAX_MODULES];
static int nloaded;

const RuiThemeModule *
rui_theme_module(const char *name)
{
	char dirs[MAX_THEME_DIRS][1024];
	int nd, i;

	if (!name || !*name || strchr(name, '/') || strlen(name) >= 64)
		return NULL;
	for (i = 0; i < nloaded; i++)
		if (!strcasecmp(loaded[i].name, name))
			return loaded[i].mod;
	nd = theme_dirs(dirs);
	for (i = 0; i < nd; i++) {
		char *path = rui_strdupf("%s/%s.so", dirs[i], name);
		const RuiThemeModule *m = NULL;
		void *h = NULL;

		if (access(path, R_OK) == 0 &&
		    !(h = dlopen(path, RTLD_NOW | RTLD_LOCAL)))
			rui_warn("theme module: %s", dlerror());
		if (h) {
			m = dlsym(h, "retro_theme");
			if (!m || m->abi != RUI_THEME_ABI) {
				rui_warn("theme module %s: %s", path, m ? "built for "
				    "another version of retro-de" : "no `retro_theme` "
				    "symbol");
				dlclose(h);
				m = NULL;
			}
		}
		free(path);
		if (!m)
			continue;
		if (nloaded < MAX_MODULES) {
			rui_strlcpy(loaded[nloaded].name, name,
			    sizeof loaded[nloaded].name);
			loaded[nloaded++].mod = m;
		}
		return m;
	}
	return NULL;
}

const char *
rui_theme_kind(const char *name)
{
	char file[160], *path;
	int i, r;

	if (rui_theme_module(name))
		return "module";
	if (!name || !*name || strchr(name, '/') || strlen(name) >= 64)
		return NULL;
	snprintf(file, sizeof file, "themes/%s.theme", name);
	path = rui_config_path(file);
	r = access(path, R_OK);
	free(path);
	if (r == 0)
		return "file";
	for (i = 0; rui_themes[i].name; i++)
		if (!strcasecmp(rui_themes[i].name, name))
			return "built-in";
	return NULL;
}

const RuiTheme *
rui_theme_find(const char *name)
{
	const RuiThemeModule *mod;
	char file[128], *path;
	int i;

	if (!name || !*name || strchr(name, '/'))
		return NULL;
	if ((mod = rui_theme_module(name)))
		return &mod->theme;
	snprintf(file, sizeof file, "themes/%s.theme", name);
	path = rui_config_path(file);
	memset(&user, 0, sizeof user);
	/* start from the Classic palette so partial files still work */
	user.t = rui_themes[1];
	rui_strlcpy(user.name, name, sizeof user.name);
	user.t.name = user.name;
	user.t.style = RUI_STYLE_CDE;
	user.t.font = NULL;
	if (rui_conf_parse(path, theme_conf, NULL) == 0) {
		free(path);
		return &user.t;
	}
	free(path);
	for (i = 0; rui_themes[i].name; i++)
		if (!strcasecmp(rui_themes[i].name, name))
			return &rui_themes[i];
	return NULL;
}

void
rui_strv_free(char **v)
{
	int i;

	if (!v)
		return;
	for (i = 0; v[i]; i++)
		free(v[i]);
	free(v);
}

static int
strv_has(char **v, int n, const char *s)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcasecmp(v[i], s))
			return 1;
	return 0;
}

static int
cmp_strp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* appends the NAME of every NAME<ext> file in dir (no duplicates) */
static void
list_dir(char ***v, int *n, int *cap, const char *dir, const char *ext,
    int full_path)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	size_t el = strlen(ext);

	while (d && (de = readdir(d))) {
		size_t len = strlen(de->d_name);
		char name[256];

		if (de->d_name[0] == '.' || len <= el ||
		    strcasecmp(de->d_name + len - el, ext) || len >= sizeof name)
			continue;
		if (full_path) {
			rui_strlcpy(name, de->d_name, sizeof name);
		} else {
			if (len - el >= 64)
				continue;
			memcpy(name, de->d_name, len - el);
			name[len - el] = '\0';
			if (strv_has(*v, *n, name))
				continue;
		}
		if (*n + 2 > *cap) {
			*cap *= 2;
			*v = rui_realloc(*v, (size_t)*cap * sizeof **v);
		}
		(*v)[(*n)++] = full_path ? rui_strdupf("%s/%s", dir, name) :
		    rui_strdup(name);
	}
	if (d)
		closedir(d);
}

char **
rui_theme_list(void)
{
	char dirs[MAX_THEME_DIRS][1024], *dir = rui_config_path("themes");
	char **v;
	int n = 0, cap = 32, i, nb, nd = theme_dirs(dirs);

	v = rui_malloc((size_t)cap * sizeof *v);
	for (i = 0; rui_themes[i].name; i++)
		v[n++] = rui_strdup(rui_themes[i].name);
	nb = n;
	list_dir(&v, &n, &cap, dir, ".theme", 0);
	for (i = 0; i < nd; i++)
		list_dir(&v, &n, &cap, dirs[i], ".so", 0);
	free(dir);
	if (n - nb > 1)                        /* built-ins first, then sorted */
		qsort(v + nb, (size_t)(n - nb), sizeof *v, cmp_strp);
	v[n] = NULL;
	return v;
}

/* -------------------------------------------------------------- wallpapers */

const char *
rui_data_dir(void)
{
	const char *e = getenv("RETRO_DE_DATA_DIR");

	return e && *e ? e : RETRO_DATADIR;
}

static const char *const wp_exts[] = { "", ".ppm", ".xpm", NULL };

char *
rui_wallpaper_find(const char *name)
{
	char *dirs[2], *found = NULL;
	struct stat st;
	int d, e;

	if (!name || !*name || !strcmp(name, "none"))
		return NULL;
	if (name[0] == '~' && name[1] == '/' && getenv("HOME"))
		return rui_strdupf("%s%s", getenv("HOME"), name + 1);
	if (strchr(name, '/'))
		return rui_strdup(name);
	dirs[0] = rui_config_path("wallpapers");
	dirs[1] = rui_strdupf("%s/wallpapers", rui_data_dir());
	for (d = 0; d < 2 && !found; d++) {
		for (e = 0; wp_exts[e] && !found; e++) {
			char *p = rui_strdupf("%s/%s%s", dirs[d], name, wp_exts[e]);

			if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
				found = p;
			else
				free(p);
		}
	}
	free(dirs[0]);
	free(dirs[1]);
	return found;
}

static int
cmp_base(const void *a, const void *b)
{
	const char *x = strrchr(*(char *const *)a, '/');
	const char *y = strrchr(*(char *const *)b, '/');

	return strcasecmp(x ? x : "", y ? y : "");
}

char **
rui_wallpaper_list(void)
{
	char *user = rui_config_path("wallpapers");
	char *sys = rui_strdupf("%s/wallpapers", rui_data_dir());
	char **v;
	int n = 0, cap = 32, e;

	v = rui_malloc((size_t)cap * sizeof *v);
	for (e = 1; wp_exts[e]; e++) {
		list_dir(&v, &n, &cap, user, wp_exts[e], 1);
		list_dir(&v, &n, &cap, sys, wp_exts[e], 1);
	}
	free(user);
	free(sys);
	if (n > 1)
		qsort(v, (size_t)n, sizeof *v, cmp_base);
	v[n] = NULL;
	return v;
}
