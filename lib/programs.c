/*
 * libretroui programs folder: the user's own launcher.
 *
 * ~/.config/retro-de/programs/ (or `programs_dir` in retro.conf) holds one
 * sub-folder per category, nested as deep as you like, and one file per
 * program.  You arrange it with a file manager or mv/ln/mkdir - no menu
 * syntax involved:
 *
 *   programs/Games/Chess            a text file: its first line that is not
 *                                   a comment is the command ("xboard",
 *                                   "@terminal -e nethack", "@app gimp")
 *   programs/Games/doom.desktop     a .desktop file (or a link to one)
 *   programs/Tools/backup           an executable, or a link to one
 *   programs/Tools/10-First         "NN-" only sorts; it is not shown
 *   programs/Games/Chess.xpm        a picture is the icon of "Chess"
 *
 * `programs "Label"` in menu.conf turns the folder into a menu, and
 * retro-programs shows it as a window of icons.
 */
#include "retroui.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_DEPTH 6
#define MAX_ENTRIES 512

char *
rui_programs_dir(void)
{
	const char *d = rui_settings()->programs_dir;

	if (d[0] == '~' && d[1] == '/' && getenv("HOME"))
		return rui_strdupf("%s%s", getenv("HOME"), d + 1);
	if (d[0])
		return rui_strdup(d);
	return rui_config_path("programs");
}

/* ----------------------------------------------------------- the defaults */

static const struct {
	const char *path, *cmd;
} defaults[] = {
	{ "Accessories/Clock", "@clock" },
	{ "Accessories/Calculator", "retro-calc" },
	{ "Accessories/Files", "retro-files" },
	{ "Accessories/Text Editor", "@editor" },
	{ "Accessories/Magnifier", "xmag" },
	{ "Internet/Web Browser", "@browser" },
	{ "Internet/Network", "retro-net" },
	{ "Internet/Bluetooth", "retro-bt" },
	{ "System/Terminal", "@terminal" },
	{ "System/File Manager", "@filemanager" },
	{ "System/Process Monitor", "@terminal -e top" },
	{ "System/About retro-de", "retro-about" },
	{ "Settings/Style Manager", "retro-settings" },
	{ "Settings/Display", "retro-display" },
	{ "Settings/Screen Saver", "@screensaver" },
	{ "Settings/Bluetooth", "retro-bt" },
	{ "Settings/Network", "retro-net" },
};

/* mkdir -p for the directory part of path */
static int
make_parents(const char *path)
{
	char buf[1024], *s;

	if (rui_strlcpy(buf, path, sizeof buf) >= sizeof buf)
		return -1;
	for (s = buf + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		if (mkdir(buf, 0755) < 0 && errno != EEXIST)
			return -1;
		*s = '/';
	}
	return 0;
}

int
rui_programs_init(void)
{
	char *dir = rui_programs_dir(), *path;
	struct stat st;
	size_t i;
	FILE *f;

	if (stat(dir, &st) == 0) {
		free(dir);
		return 0;                          /* the user's: never touched */
	}
	for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
		path = rui_strdupf("%s/%s", dir, defaults[i].path);
		if (make_parents(path) == 0 && (f = fopen(path, "w"))) {
			fprintf(f, "%s\n", defaults[i].cmd);
			fclose(f);
		}
		free(path);
	}
	path = rui_strdupf("%s/.about", dir);
	if ((f = fopen(path, "w"))) {
		fputs("This folder is your Programs menu.  Folders are categories;\n"
		    "each file is a program: a text file with the command on its\n"
		    "first line, a .desktop file, or an executable (links work).\n"
		    "Rename, move, delete and add as you like; a leading \"NN-\" in\n"
		    "a name only sorts.  Files starting with a dot are ignored.\n", f);
		fclose(f);
	}
	free(path);
	i = stat(dir, &st) == 0;
	free(dir);
	return (int)i;
}

/* ------------------------------------------------------- reading a folder */

typedef struct Ent {
	char *file;                        /* name inside the directory */
	char *label;
	int is_dir;
} Ent;

static int
cmp_ent(const void *a, const void *b)
{
	const Ent *x = a, *y = b;
	int c;

	if (x->is_dir != y->is_dir)
		return y->is_dir - x->is_dir;      /* categories first */
	c = strcasecmp(x->file, y->file);      /* so "NN-" prefixes sort */
	return c ? c : strcmp(x->file, y->file);
}

static int
has_suffix(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);

	return a > b && !strcasecmp(s + a - b, suf);
}

/* pictures beside the programs are icons, not programs */
static int
is_image(const char *name)
{
	return has_suffix(name, ".xpm") || has_suffix(name, ".ppm") ||
	    has_suffix(name, ".pnm");
}

/* file name -> label: drops "NN-" and a launcher extension */
static char *
label_of(const char *file)
{
	static const char *const exts[] = { ".desktop", ".sh", ".cmd", NULL };
	const char *s = file;
	char *l;
	size_t len;
	int i;

	while (*s >= '0' && *s <= '9')
		s++;
	s = (s > file && *s == '-' && s[1]) ? s + 1 : file;
	l = rui_strdup(s);
	len = strlen(l);
	for (i = 0; exts[i]; i++) {
		size_t el = strlen(exts[i]);

		if (len > el && !strcasecmp(l + len - el, exts[i])) {
			l[len - el] = '\0';
			break;
		}
	}
	return l;
}

/* First line of a launcher file that is not blank or a comment; a
 * "# icon: SPEC" line among the first ones is returned in *icon. */
static char *
launcher_cmd(const char *path, char **icon)
{
	FILE *f = fopen(path, "r");
	char line[2048], *cmd = NULL;
	int n = 0;

	if (!f)
		return NULL;
	while (n++ < 32 && fgets(line, sizeof line, f)) {
		char *s;

		if (memchr(line, '\0', sizeof line) == NULL)
			break;
		s = rui_ltrim(rui_rtrim(line));
		if (*s == '#') {
			s = rui_ltrim(s + 1);
			if (icon && !*icon && !strncasecmp(s, "icon", 4) &&
			    (s[4] == ':' || s[4] == '=')) {
				s = rui_ltrim(s + 5);
				if (*s)
					*icon = rui_strdup(s);
			}
		} else if (*s && !cmd) {
			cmd = rui_strdup(s);
		}
	}
	fclose(f);
	return cmd;
}

/* an image file that exists, or NULL */
static char *
readable(char *path)
{
	struct stat st;

	if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    access(path, R_OK) == 0)
		return path;
	free(path);
	return NULL;
}

/* "NAME" in the user's and the installed icons/ folders */
static char *
named_image(const char *name)
{
	static const char *const exts[] = { ".xpm", ".ppm", ".pnm", "" };
	const char *bases[2];
	char *p;
	int b, e;

	bases[0] = rui_config_dir();
	bases[1] = rui_data_dir();
	for (b = 0; b < 2; b++)
		for (e = 0; e < 4; e++)
			if (bases[b] && (p = readable(rui_strdupf("%s/icons/%s%s",
			    bases[b], name, exts[e]))))
				return p;
	return NULL;
}

/* An icon SPEC -> an image path (returned) or a pictogram (*kind) */
static char *
resolve_icon(const char *spec, const char *dir, int *kind)
{
	int k = rui_icon_kind(spec);

	if (!spec || !*spec)
		return NULL;
	if (k >= 0) {
		*kind = k;
		return NULL;
	}
	if (spec[0] == '/')
		return readable(rui_strdup(spec));
	if (spec[0] == '~' && spec[1] == '/' && getenv("HOME"))
		return readable(rui_strdupf("%s%s", getenv("HOME"), spec + 1));
	if (strchr(spec, '/'))
		return readable(rui_strdupf("%s/%s", dir, spec));
	{
		char *p = readable(rui_strdupf("%s/%s", dir, spec));

		return p ? p : named_image(spec);
	}
}

/* "Chess.xpm" beside "Chess" (or beside "Chess.desktop") */
static char *
sidecar_icon(const char *dir, const char *file)
{
	static const char *const exts[] = { ".xpm", ".ppm", ".pnm" };
	char base[NAME_MAX + 1], *dot, *p;
	int i, round;

	rui_strlcpy(base, file, sizeof base);
	for (round = 0; round < 2; round++) {
		for (i = 0; i < 3; i++)
			if ((p = readable(rui_strdupf("%s/%s%s", dir, base, exts[i]))))
				return p;
		if (!(dot = strrchr(base, '.')) || dot == base)
			break;
		*dot = '\0';
	}
	return NULL;
}

/* fills in a program entry; 0 when the file is not a program */
static int
read_program(RuiProgram *p, const char *dir, const char *path, const Ent *e)
{
	char real[PATH_MAX], *cmd = NULL, *label = NULL, *icon = NULL;
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
		return 0;                          /* dangling link, device ... */
	if (!realpath(path, real))
		rui_strlcpy(real, path, sizeof real);
	if (has_suffix(e->file, ".desktop") || has_suffix(real, ".desktop")) {
		const char *base = strrchr(real, '/');
		RuiDesktopEntry *de = rui_desktop_load(real, base ? base + 1 : real);

		if (de && !de->hidden) {
			cmd = rui_desktop_command(de);
			/* a link keeps the name the user gave it */
			label = has_suffix(e->file, ".desktop") ? rui_strdup(de->name) :
			    rui_strdup(e->label);
			/* only a picture given by path: no icon theme lookups */
			if (de->icon && (de->icon[0] == '/' || rui_icon_kind(de->icon)
			    >= 0))
				icon = rui_strdup(de->icon);
		}
		rui_desktop_free(de);
	} else if (access(path, X_OK) == 0) {
		char first[4] = "";
		FILE *f = fopen(path, "r");
		size_t n = f ? fread(first, 1, 2, f) : 0;

		if (f)
			fclose(f);
		/* a script or a binary runs; plain text marked +x is a launcher */
		if (n == 2 && !memcmp(first, "#!", 2)) {
			free(launcher_cmd(path, &icon));
			cmd = rui_shell_quote(path);
		} else if (n == 2 && !memcmp(first, "\177E", 2)) {
			cmd = rui_shell_quote(path);
		} else {
			cmd = launcher_cmd(path, &icon);
		}
	} else {
		cmd = launcher_cmd(path, &icon);
	}
	if (!cmd || !*cmd) {
		free(cmd);
		free(label);
		free(icon);
		return 0;
	}
	memset(p, 0, sizeof *p);
	p->label = label ? label : rui_strdup(e->label);
	p->path = rui_strdup(path);
	p->cmd = cmd;
	p->icon = rui_icon_guess(cmd);
	if (icon)
		p->image = resolve_icon(icon, dir, &p->icon);
	if (!p->image && !(icon && rui_icon_kind(icon) >= 0))
		p->image = sidecar_icon(dir, e->file);
	free(icon);
	return 1;
}

int
rui_programs_read(const char *dir, RuiProgram **out)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	RuiProgram *v = NULL;
	Ent *e = NULL;
	int n = 0, cap = 0, i, np = 0;

	while (d && (de = readdir(d)) && n < MAX_ENTRIES) {
		char *path;
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		path = rui_strdupf("%s/%s", dir, de->d_name);
		if (stat(path, &st) == 0 && (S_ISDIR(st.st_mode) ||
		    (S_ISREG(st.st_mode) && !is_image(de->d_name)))) {
			if (n == cap) {
				cap = cap ? cap * 2 : 16;
				e = rui_realloc(e, (size_t)cap * sizeof *e);
			}
			e[n].file = rui_strdup(de->d_name);
			e[n].label = label_of(de->d_name);
			e[n].is_dir = S_ISDIR(st.st_mode);
			n++;
		}
		free(path);
	}
	if (d)
		closedir(d);
	if (n > 1)
		qsort(e, (size_t)n, sizeof *e, cmp_ent);
	if (n)
		v = rui_calloc((size_t)n, sizeof *v);
	for (i = 0; i < n; i++) {
		char *path = rui_strdupf("%s/%s", dir, e[i].file);

		if (e[i].is_dir) {
			RuiProgram *p = &v[np++];

			/* .icon.xpm inside the category, or Games.xpm beside it */
			p->label = rui_strdup(e[i].label);
			p->path = path;
			p->is_dir = 1;
			p->icon = RUI_ICON_FOLDER;
			p->image = sidecar_icon(path, ".icon");
			if (!p->image)
				p->image = sidecar_icon(dir, e[i].file);
			path = NULL;
		} else if (read_program(&v[np], dir, path, &e[i])) {
			np++;
		}
		free(path);
		free(e[i].file);
		free(e[i].label);
	}
	free(e);
	if (!np) {
		free(v);
		v = NULL;
	}
	*out = v;
	return np;
}

void
rui_programs_free(RuiProgram *v, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		free(v[i].label);
		free(v[i].path);
		free(v[i].cmd);
		free(v[i].image);
	}
	free(v);
}

/* ---------------------------------------------------------------- the menu */

static RuiMenu *
dir_menu(const char *dir, int depth)
{
	RuiMenu *m = rui_menu_new();
	RuiProgram *v;
	int n = rui_programs_read(dir, &v), i;

	for (i = 0; i < n; i++) {
		if (!v[i].is_dir) {
			rui_menu_add(m, v[i].label, v[i].cmd, 0);
		} else if (depth + 1 < MAX_DEPTH) {
			RuiMenu *sub = dir_menu(v[i].path, depth + 1);

			if (sub->n == 0)
				rui_menu_add(sub, "(empty)", NULL, 0)->disabled = 1;
			rui_menu_add_sub(m, v[i].label, sub);
		}
	}
	rui_programs_free(v, n);
	return m;
}

RuiMenu *
rui_programs_menu(void)
{
	char *dir = rui_programs_dir(), *q, *cmd;
	RuiMenu *m;

	char real[PATH_MAX];

	rui_programs_init();                   /* first use: the defaults */
	if (realpath(dir, real)) {             /* commands carry full paths */
		free(dir);
		dir = rui_strdup(real);
	}
	m = dir_menu(dir, 0);
	if (m->n)
		rui_menu_add_sep(m);
	rui_menu_add(m, "Show as Icons...", "retro-programs", 0);
	q = rui_shell_quote(dir);
	cmd = rui_strdupf("@filemanager %s", q);
	rui_menu_add(m, "Organize...", cmd, 0);
	free(cmd);
	free(q);
	free(dir);
	return m;
}

/* changes whenever something is added, removed or renamed in the folder */
static long long
dir_stamp(const char *dir, int depth)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	long long stamp;

	if (stat(dir, &st) < 0)
		return 0;
	stamp = (long long)st.st_mtime * 31 + (long long)st.st_ino;
	if (depth + 1 >= MAX_DEPTH || !(d = opendir(dir)))
		return stamp;
	while ((de = readdir(d))) {
		char *path;

		if (de->d_name[0] == '.')
			continue;
		path = rui_strdupf("%s/%s", dir, de->d_name);
		if (stat(path, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				stamp += dir_stamp(path, depth + 1) * 3;
			else
				stamp += (long long)st.st_mtime + (long long)st.st_size;
		}
		free(path);
	}
	closedir(d);
	return stamp;
}

long long
rui_programs_stamp(void)
{
	char *dir = rui_programs_dir();
	long long s = dir_stamp(dir, 0);

	free(dir);
	return s;
}
