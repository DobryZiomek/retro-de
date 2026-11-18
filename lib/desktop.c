/*
 * libretroui desktop entries: freedesktop.org .desktop files.
 *
 * Implements the parts of the Desktop Entry and Menu specifications a small
 * desktop needs: localized Name/GenericName/Comment, Exec quoting and field
 * codes, Terminal, Path, TryExec, NoDisplay, Hidden, OnlyShowIn/NotShowIn,
 * desktop-file IDs with $XDG_DATA_HOME overriding $XDG_DATA_DIRS, main
 * categories for the application menu, XPM/PPM icon lookup and (opt-in)
 * XDG autostart.  Plain text parsing; no libraries.
 */
#include "retroui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_DIRS 16
#define DIR_LEN 1024
#define MAX_DEPTH 3
#define MAX_FILE (256 * 1024)

/* ------------------------------------------------------------ small string */

typedef struct Str {
	char *s;
	size_t n, cap;
} Str;

static void
str_putc(Str *b, char c)
{
	if (b->n + 2 > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 64;
		b->s = rui_realloc(b->s, b->cap);
	}
	b->s[b->n++] = c;
	b->s[b->n] = '\0';
}

static void
str_puts(Str *b, const char *s)
{
	while (*s)
		str_putc(b, *s++);
}

static char *
str_take(Str *b)
{
	char *s = b->s ? b->s : rui_strdup("");

	b->s = NULL;
	b->n = b->cap = 0;
	return s;
}

/* ------------------------------------------------------------- directories */

/* snprintf for paths: 0 if the result did not fit (the caller skips it) */
static int
pathf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r >= 0 && (size_t)r < n;
}

static void
add_dir_list(char dirs[][DIR_LEN], int *n, const char *list)
{
	while (*list && *n < MAX_DIRS) {
		size_t len = strcspn(list, ":");

		while (len > 1 && list[len - 1] == '/')
			len--;                         /* "/usr/share/" -> "/usr/share" */
		if (len && len < DIR_LEN) {
			memcpy(dirs[*n], list, len);
			dirs[*n][len] = '\0';
			(*n)++;
		}
		list += strcspn(list, ":");
		if (*list == ':')
			list++;
	}
}

/* $XDG_DATA_HOME first, then $XDG_DATA_DIRS */
static int
data_dirs(char dirs[][DIR_LEN])
{
	const char *e = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	int n = 0;

	if (e && *e)
		add_dir_list(dirs, &n, e);
	else if (home && *home)
		snprintf(dirs[n++], DIR_LEN, "%s/.local/share", home);
	e = getenv("XDG_DATA_DIRS");
	/* the spec default plus pkgsrc's prefix used on NetBSD */
	add_dir_list(dirs, &n, e && *e ? e : "/usr/local/share:/usr/share:"
	    "/usr/pkg/share");
	return n;
}

/* $XDG_CONFIG_HOME first, then $XDG_CONFIG_DIRS */
static int
config_dirs(char dirs[][DIR_LEN])
{
	const char *e = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	int n = 0;

	if (e && *e)
		add_dir_list(dirs, &n, e);
	else if (home && *home)
		snprintf(dirs[n++], DIR_LEN, "%s/.config", home);
	e = getenv("XDG_CONFIG_DIRS");
	add_dir_list(dirs, &n, e && *e ? e : "/etc/xdg");
	return n;
}

static char *
read_small_file(const char *path)
{
	FILE *f = fopen(path, "r");
	size_t len = 0, r, cap = 4096;
	char *buf;

	if (!f)
		return NULL;
	buf = rui_malloc(cap);
	while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += r;
		if (len + 1 >= cap) {
			if (cap >= MAX_FILE) {
				fclose(f);
				free(buf);
				return NULL;
			}
			cap *= 2;
			buf = rui_realloc(buf, cap);
		}
	}
	fclose(f);
	buf[len] = '\0';
	return buf;
}

/* ------------------------------------------------------------------ locale */

typedef struct Locale {
	char lang[16], country[16], mod[32];
} Locale;

static void
locale_get(Locale *l)
{
	const char *v = getenv("LC_ALL");
	size_t i;

	memset(l, 0, sizeof *l);
	if (!v || !*v)
		v = getenv("LC_MESSAGES");
	if (!v || !*v)
		v = getenv("LANG");
	if (!v || !*v || !strcmp(v, "C") || !strcmp(v, "POSIX"))
		return;
	/* lang_COUNTRY.ENCODING@MODIFIER */
	for (i = 0; *v && !strchr("_.@", *v) && i + 1 < sizeof l->lang; v++)
		l->lang[i++] = *v;
	if (*v == '_')
		for (v++, i = 0; *v && !strchr(".@", *v) &&
		    i + 1 < sizeof l->country; v++)
			l->country[i++] = *v;
	v = strchr(v, '@');
	if (v)
		rui_strlcpy(l->mod, v + 1, sizeof l->mod);
}

/* how well "Key[tag]" matches the locale: 4 best .. 1, -1 no match */
static int
locale_rank(const Locale *l, const char *tag)
{
	char buf[80];

	if (!l->lang[0])
		return -1;
	if (l->country[0] && l->mod[0]) {
		snprintf(buf, sizeof buf, "%s_%s@%s", l->lang, l->country, l->mod);
		if (!strcmp(tag, buf))
			return 4;
	}
	if (l->country[0]) {
		snprintf(buf, sizeof buf, "%s_%s", l->lang, l->country);
		if (!strcmp(tag, buf))
			return 3;
	}
	if (l->mod[0]) {
		snprintf(buf, sizeof buf, "%s@%s", l->lang, l->mod);
		if (!strcmp(tag, buf))
			return 2;
	}
	return strcmp(tag, l->lang) ? -1 : 1;
}

/* ---------------------------------------------------------------- parsing */

/* general string escapes: \s \n \t \r \\ (others are kept for Exec) */
static char *
unescape(const char *v)
{
	Str b = { NULL, 0, 0 };

	for (; *v; v++) {
		if (*v == '\\' && v[1]) {
			switch (v[1]) {
			case 's': str_putc(&b, ' '); v++; continue;
			case 'n': str_putc(&b, '\n'); v++; continue;
			case 't': str_putc(&b, '\t'); v++; continue;
			case 'r': str_putc(&b, '\r'); v++; continue;
			case '\\': str_putc(&b, '\\'); v++; continue;
			default: break;
			}
		}
		str_putc(&b, *v);
	}
	return str_take(&b);
}

/* is any ';'-separated name in `list` our desktop? */
static int
desktop_match(const char *list)
{
	const char *cur = getenv("XDG_CURRENT_DESKTOP");

	while (*list) {
		size_t len = strcspn(list, ";");

		if (len) {
			const char *c = cur;

			if (strlen(rui_config.desktop_name) == len &&
			    !strncmp(list, rui_config.desktop_name, len))
				return 1;
			while (c && *c) {
				size_t cl = strcspn(c, ":");

				if (cl == len && !strncmp(c, list, len))
					return 1;
				c += cl;
				if (*c == ':')
					c++;
			}
		}
		list += len;
		if (*list == ';')
			list++;
	}
	return 0;
}

static int
shell_safe(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "0123456789_@%+=:,./-", *s))
			return 0;
	return 1;
}

static void
push_arg(Str *cmd, const char *arg)
{
	if (cmd->n)
		str_putc(cmd, ' ');
	if (shell_safe(arg)) {
		str_puts(cmd, arg);
	} else {
		char *q = rui_shell_quote(arg);

		str_puts(cmd, q);
		free(q);
	}
}

/* Splits Exec= into arguments following the spec's quoting rules, expands
 * or drops the field codes and joins it back as a /bin/sh command line. */
static char *
exec_to_shell(const char *raw, const char *name, const char *icon,
    const char *file)
{
	Str cmd = { NULL, 0, 0 }, arg = { NULL, 0, 0 };
	const char *p = raw;
	int nargs = 0;

	while (*p) {
		char *a;
		int quoted = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		while (*p && *p != ' ' && *p != '\t') {
			if (*p == '"') {
				p++;
				while (*p && *p != '"') {
					if (*p == '\\' && p[1] && strchr("\"`$\\", p[1]))
						p++;
					str_putc(&arg, *p++);
				}
				if (*p != '"')
					goto fail;             /* unterminated quote */
				p++;
				quoted = 1;
				continue;
			}
			str_putc(&arg, *p++);
		}
		a = str_take(&arg);
		if (!quoted && strlen(a) == 2 && a[0] == '%' &&
		    strchr("fFuUdDnNvm", a[1])) {
			free(a);                       /* file/URL lists: no files */
			continue;
		}
		if (!quoted && !strcmp(a, "%i")) {
			if (icon && *icon) {
				push_arg(&cmd, "--icon");
				push_arg(&cmd, icon);
				nargs += 2;
			}
			free(a);
			continue;
		}
		{
			Str x = { NULL, 0, 0 };
			const char *c;
			char *expanded;

			for (c = a; *c; c++) {
				if (*c != '%' || !c[1]) {
					str_putc(&x, *c);
					continue;
				}
				c++;
				if (*c == '%')
					str_putc(&x, '%');
				else if (*c == 'c')
					str_puts(&x, name ? name : "");
				else if (*c == 'k')
					str_puts(&x, file ? file : "");
				/* any other code expands to nothing */
			}
			expanded = str_take(&x);
			if (*expanded || quoted) {
				push_arg(&cmd, expanded);
				nargs++;
			}
			free(expanded);
		}
		free(a);
	}
	if (nargs == 0)
		goto fail;
	free(arg.s);
	return str_take(&cmd);
fail:
	free(arg.s);
	free(cmd.s);
	return NULL;
}

typedef struct LVal {
	char *v;
	int rank;
} LVal;

static void
lval_set(LVal *l, const char *val, int rank)
{
	if (rank < l->rank)
		return;
	free(l->v);
	l->v = unescape(val);
	l->rank = rank;
}

static void
set_str(char **dst, const char *val)
{
	free(*dst);
	*dst = unescape(val);
}

RuiDesktopEntry *
rui_desktop_parse(const char *text, const char *id, const char *file)
{
	LVal name = { NULL, -1 }, generic = { NULL, -1 }, comment = { NULL, -1 };
	char *type = NULL, *exec = NULL, *tryexec = NULL, *icon = NULL,
	    *cats = NULL, *path = NULL, *only = NULL, *notshow = NULL;
	int terminal = 0, nodisplay = 0, hidden = 0, auto_off = 0, in_group = 0;
	const char *p = text;
	char line[4096];
	Locale loc;
	RuiDesktopEntry *e = NULL;

	locale_get(&loc);
	while (rui_next_line(&p, line, sizeof line)) {
		char *s = rui_ltrim(rui_rtrim(line)), *eq, *key, *val, *br;
		int rank = 0;

		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			if (in_group)
				break;                     /* only [Desktop Entry] matters */
			in_group = !strcmp(s, "[Desktop Entry]");
			continue;
		}
		if (!in_group || !(eq = strchr(s, '=')))
			continue;
		*eq = '\0';
		key = rui_rtrim(s);
		val = rui_ltrim(eq + 1);
		if ((br = strchr(key, '['))) {
			char *close = strchr(br, ']');

			if (!close)
				continue;
			*close = '\0';
			*br = '\0';
			rank = locale_rank(&loc, br + 1);
			if (rank < 0)
				continue;
		}
		if (!strcmp(key, "Name"))
			lval_set(&name, val, rank);
		else if (!strcmp(key, "GenericName"))
			lval_set(&generic, val, rank);
		else if (!strcmp(key, "Comment"))
			lval_set(&comment, val, rank);
		else if (rank > 0)
			continue;                      /* other keys: unlocalized only */
		else if (!strcmp(key, "Type"))
			set_str(&type, val);
		else if (!strcmp(key, "Exec"))
			set_str(&exec, val);
		else if (!strcmp(key, "TryExec"))
			set_str(&tryexec, val);
		else if (!strcmp(key, "Icon"))
			set_str(&icon, val);
		else if (!strcmp(key, "Categories"))
			set_str(&cats, val);
		else if (!strcmp(key, "Path"))
			set_str(&path, val);
		else if (!strcmp(key, "OnlyShowIn"))
			set_str(&only, val);
		else if (!strcmp(key, "NotShowIn"))
			set_str(&notshow, val);
		else if (!strcmp(key, "Terminal"))
			terminal = !strcmp(val, "true");
		else if (!strcmp(key, "NoDisplay"))
			nodisplay = !strcmp(val, "true");
		else if (!strcmp(key, "Hidden"))
			hidden = !strcmp(val, "true");
		else if (!strcmp(key, "X-GNOME-Autostart-enabled"))
			auto_off = !strcmp(val, "false");
	}

	if (hidden || (type && !strcmp(type, "Application") && name.v && exec)) {
		e = rui_calloc(1, sizeof *e);
		e->id = rui_strdup(id ? id : "");
		e->hidden = hidden;
		e->nodisplay = nodisplay;
		e->terminal = terminal;
		e->autostart_off = auto_off;
		e->name = name.v ? name.v : rui_strdup(e->id);
		name.v = NULL;
		e->generic = generic.v;
		e->comment = comment.v;
		generic.v = comment.v = NULL;
		e->icon = icon;
		e->categories = cats;
		e->path = path;
		icon = cats = path = NULL;
		if (!hidden) {
			e->exec = exec_to_shell(exec, e->name, e->icon, file);
			if (!e->exec) {
				rui_warn("%s: malformed Exec line", file ? file : e->id);
				rui_desktop_free(e);
				e = NULL;
			}
		}
		if (e && !e->exec)
			e->exec = rui_strdup("");
		if (e && ((only && !desktop_match(only)) ||
		    (notshow && desktop_match(notshow)) ||
		    (tryexec && *tryexec && !rui_have_cmd(tryexec))))
			e->hidden = 1;
	}
	free(name.v);
	free(generic.v);
	free(comment.v);
	free(type);
	free(exec);
	free(tryexec);
	free(icon);
	free(cats);
	free(path);
	free(only);
	free(notshow);
	return e;
}

RuiDesktopEntry *
rui_desktop_load(const char *path, const char *id)
{
	char *text = read_small_file(path);
	RuiDesktopEntry *e;

	if (!text)
		return NULL;
	e = rui_desktop_parse(text, id, path);
	free(text);
	return e;
}

void
rui_desktop_free(RuiDesktopEntry *e)
{
	if (!e)
		return;
	free(e->id);
	free(e->name);
	free(e->generic);
	free(e->comment);
	free(e->exec);
	free(e->icon);
	free(e->categories);
	free(e->path);
	free(e);
}

char *
rui_desktop_command(const RuiDesktopEntry *e)
{
	char *cmd, *full;

	if (e->terminal)
		cmd = rui_strdupf("%s -e %s", rui_terminal(), e->exec);
	else
		cmd = rui_strdup(e->exec);
	if (!e->path || !*e->path)
		return cmd;
	{
		char *q = rui_shell_quote(e->path);

		full = rui_strdupf("cd %s && exec %s", q, cmd);
		free(q);
	}
	free(cmd);
	return full;
}

/* ------------------------------------------------------------ app lists */

static int
has_suffix(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);

	return a >= b && !strcmp(s + a - b, suf);
}

static void
list_push(RuiAppList *l, RuiDesktopEntry *e, int *cap)
{
	if (l->n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		l->v = rui_realloc(l->v, (size_t)*cap * sizeof *l->v);
	}
	l->v[l->n++] = e;
}

static int
list_has(const RuiAppList *l, const char *id)
{
	int i;

	for (i = 0; i < l->n; i++)
		if (!strcmp(l->v[i]->id, id))
			return 1;
	return 0;
}

/* desktop-file IDs: path below applications/ with '/' turned into '-' */
static void
scan_dir(RuiAppList *l, int *cap, const char *base, const char *rel, int depth)
{
	char dirpath[2048];
	DIR *d;
	struct dirent *de;

	if (!pathf(dirpath, sizeof dirpath, "%s%s%s", base, *rel ? "/" : "", rel) ||
	    !(d = opendir(dirpath)))
		return;
	while ((de = readdir(d))) {
		char full[2048], sub[1024], id[1024];
		struct stat st;
		size_t i;

		if (de->d_name[0] == '.')
			continue;
		if (!pathf(full, sizeof full, "%s/%s", dirpath, de->d_name) ||
		    stat(full, &st) < 0 ||
		    !pathf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", de->d_name))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < MAX_DEPTH)
				scan_dir(l, cap, base, sub, depth + 1);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !has_suffix(de->d_name, ".desktop"))
			continue;
		rui_strlcpy(id, sub, sizeof id);
		for (i = 0; id[i]; i++)
			if (id[i] == '/')
				id[i] = '-';
		if (!list_has(l, id)) {
			RuiDesktopEntry *e = rui_desktop_load(full, id);

			if (e)
				list_push(l, e, cap);  /* hidden ones mask later dirs */
		}
	}
	closedir(d);
}

static int
cmp_entry(const void *a, const void *b)
{
	const RuiDesktopEntry *x = *(RuiDesktopEntry *const *)a;
	const RuiDesktopEntry *y = *(RuiDesktopEntry *const *)b;
	int c = strcasecmp(x->name, y->name);

	return c ? c : strcmp(x->id, y->id);
}

RuiAppList *
rui_apps_scan(void)
{
	char dirs[MAX_DIRS][DIR_LEN];
	RuiAppList *l = rui_calloc(1, sizeof *l);
	int n = data_dirs(dirs), i, k, cap = 0;

	for (i = 0; i < n; i++) {
		char base[DIR_LEN + 16];

		if (pathf(base, sizeof base, "%s/applications", dirs[i]))
			scan_dir(l, &cap, base, "", 0);
	}
	for (i = k = 0; i < l->n; i++) {
		if (l->v[i]->hidden)
			rui_desktop_free(l->v[i]);
		else
			l->v[k++] = l->v[i];
	}
	l->n = k;
	if (l->n > 1)
		qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_entry);
	return l;
}

void
rui_apps_free(RuiAppList *l)
{
	int i;

	if (!l)
		return;
	for (i = 0; i < l->n; i++)
		rui_desktop_free(l->v[i]);
	free(l->v);
	free(l);
}

RuiDesktopEntry *
rui_app_find(const char *id_in)
{
	char dirs[MAX_DIRS][DIR_LEN], id[512];
	int n = data_dirs(dirs), i;

	if (!id_in || !*id_in || strchr(id_in, '/'))
		return NULL;
	if (has_suffix(id_in, ".desktop"))
		rui_strlcpy(id, id_in, sizeof id);
	else
		snprintf(id, sizeof id, "%s.desktop", id_in);
	for (i = 0; i < n; i++) {
		char path[2048], *dash;
		RuiDesktopEntry *e;
		struct stat st;

		if (!pathf(path, sizeof path, "%s/applications/%s", dirs[i], id) ||
		    stat(path, &st) < 0) {
			/* "kde4-foo.desktop" may live in applications/kde4/ */
			char sub[512];

			rui_strlcpy(sub, id, sizeof sub);
			if (!(dash = strchr(sub, '-')))
				continue;
			*dash = '/';
			if (!pathf(path, sizeof path, "%s/applications/%s", dirs[i], sub) ||
			    stat(path, &st) < 0)
				continue;
		}
		e = rui_desktop_load(path, id);
		if (!e)
			continue;
		if (e->hidden) {
			rui_desktop_free(e);
			return NULL;
		}
		return e;
	}
	return NULL;
}

/* ------------------------------------------------------------------ menus */

static const struct {
	const char *cat, *label;
} main_cats[] = {
	{ "AudioVideo", "Multimedia" }, { "Audio", "Multimedia" },
	{ "Video", "Multimedia" }, { "Development", "Development" },
	{ "Education", "Education" }, { "Game", "Games" },
	{ "Graphics", "Graphics" }, { "Network", "Internet" },
	{ "Office", "Office" }, { "Science", "Science" },
	{ "Settings", "Settings" }, { "System", "System" },
	{ "Utility", "Accessories" },
};

static const char *const cat_order[] = {
	"Accessories", "Development", "Education", "Games", "Graphics",
	"Internet", "Multimedia", "Office", "Science", "Settings", "System",
	"Other"
};

#define NCATS ((int)(sizeof cat_order / sizeof cat_order[0]))

/* label of the first main category listed in Categories= */
static const char *
category_of(const char *cats)
{
	while (cats && *cats) {
		size_t len = strcspn(cats, ";");
		size_t i;

		for (i = 0; i < sizeof main_cats / sizeof main_cats[0]; i++)
			if (strlen(main_cats[i].cat) == len &&
			    !strncmp(cats, main_cats[i].cat, len))
				return main_cats[i].label;
		cats += len;
		if (*cats == ';')
			cats++;
	}
	return "Other";
}

RuiMenu *
rui_apps_menu(const RuiAppList *l)
{
	RuiMenu *subs[NCATS], *m = rui_menu_new();
	int i, c;

	for (c = 0; c < NCATS; c++)
		subs[c] = NULL;
	for (i = 0; i < l->n; i++) {
		const RuiDesktopEntry *e = l->v[i];
		const char *label;
		char *cmd;

		if (e->nodisplay || e->hidden)
			continue;
		label = category_of(e->categories);
		for (c = 0; c < NCATS; c++)
			if (!strcmp(cat_order[c], label))
				break;
		if (!subs[c])
			subs[c] = rui_menu_new();
		cmd = rui_desktop_command(e);
		rui_menu_add(subs[c], e->name, cmd, 0);
		free(cmd);
	}
	for (c = 0; c < NCATS; c++)
		if (subs[c])
			rui_menu_add_sub(m, cat_order[c], subs[c]);
	return m;
}

long long
rui_apps_stamp(void)
{
	char dirs[MAX_DIRS][DIR_LEN];
	int n = data_dirs(dirs), i;
	long long stamp = 0;

	for (i = 0; i < n; i++) {
		char path[DIR_LEN + 16];
		struct stat st;

		if (pathf(path, sizeof path, "%s/applications", dirs[i]) &&
		    stat(path, &st) == 0)
			stamp += (long long)st.st_mtime * (i + 1) + (long long)st.st_ino;
	}
	return stamp;
}

/* ------------------------------------------------------------------ icons */

char *
rui_icon_lookup(const char *icon)
{
	/* an icon_theme first (retro.conf), then hicolor and pixmaps: plain
	 * file names in fixed places, no index.theme parsing */
	static const char *const sizes[] = { "48x48", "32x32", "64x64",
		"scalable", "128x128", "256x256", "24x24", "22x22", "16x16" };
	static const char *const cats[] = { "apps", "places", "devices",
		"mimetypes", "categories", "status", "actions" };
	static const char *const exts[] = { ".png", ".svg", ".xpm" };
	const char *themes[3];
	char dirs[MAX_DIRS + 2][DIR_LEN], path[2048], *p;
	int n, i, t, nt = 0;
	size_t z, c, e;

	if (!icon || !*icon)
		return NULL;
	if (icon[0] == '/') {
		if ((has_suffix(icon, ".xpm") || has_suffix(icon, ".ppm") ||
		    has_suffix(icon, ".pnm") || has_suffix(icon, ".png") ||
		    has_suffix(icon, ".svg")) && access(icon, R_OK) == 0)
			return rui_strdup(icon);
		return NULL;
	}
	if (strchr(icon, '/'))
		return NULL;
	if ((p = rui_icon_find(icon)))           /* ours, and learned ones */
		return p;
	/* our config dir first: icon themes from theme packages live in
	 * <config dir>/icons/THEME */
	rui_strlcpy(dirs[0], rui_config_dir(), DIR_LEN);
	n = 1 + data_dirs(dirs + 1);
	if (n < MAX_DIRS + 2) {
		const char *home = getenv("HOME");

		if (home && *home)               /* legacy ~/.icons */
			snprintf(dirs[n++], DIR_LEN, "%s/.icons", home);
	}
	if (rui_settings()->icon_theme[0])
		themes[nt++] = rui_settings()->icon_theme;
	themes[nt++] = "hicolor";
	for (t = 0; t < nt; t++)
		for (z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
			for (c = 0; c < sizeof cats / sizeof cats[0]; c++)
				for (i = 0; i < n; i++)
					for (e = 0; e < sizeof exts / sizeof exts[0]; e++) {
						if (!strcmp(sizes[z], "scalable") != !strcmp(exts[e],
						    ".svg"))
							continue;
						if (pathf(path, sizeof path, "%s/icons/%s/%s/%s/%s%s",
						    dirs[i], themes[t], sizes[z], cats[c], icon,
						    exts[e]) && access(path, R_OK) == 0)
							return rui_strdup(path);
					}
	for (i = 0; i < n; i++)
		for (e = 0; e < sizeof exts / sizeof exts[0]; e++)
			if (pathf(path, sizeof path, "%s/pixmaps/%s%s", dirs[i], icon,
			    exts[e]) && access(path, R_OK) == 0)
				return rui_strdup(path);
	return NULL;
}

/* -------------------------------------------------------------- autostart */

int
rui_autostart_xdg(void)
{
	char dirs[MAX_DIRS][DIR_LEN];
	RuiAppList seen = { NULL, 0 };
	int n = config_dirs(dirs), i, cap = 0, started = 0;

	for (i = 0; i < n; i++) {
		char base[DIR_LEN + 16];
		DIR *d;
		struct dirent *de;

		if (!pathf(base, sizeof base, "%s/autostart", dirs[i]) ||
		    !(d = opendir(base)))
			continue;
		while ((de = readdir(d))) {
			char path[2048];
			RuiDesktopEntry *e;

			if (de->d_name[0] == '.' || !has_suffix(de->d_name, ".desktop") ||
			    list_has(&seen, de->d_name))
				continue;
			if (!pathf(path, sizeof path, "%s/%s", base, de->d_name) ||
			    !(e = rui_desktop_load(path, de->d_name)))
				continue;
			if (!e->hidden && !e->autostart_off && e->exec[0]) {
				char *cmd = rui_desktop_command(e);

				rui_spawn(cmd);
				free(cmd);
				started++;
			}
			list_push(&seen, e, &cap);         /* first id wins */
		}
		closedir(d);
	}
	for (i = 0; i < seen.n; i++)
		rui_desktop_free(seen.v[i]);
	free(seen.v);
	return started;
}
