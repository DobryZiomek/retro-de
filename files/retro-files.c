/*
 * retro-files - a light file explorer.
 *
 *   retro-files [DIRECTORY]
 *
 * One window: places on the left, the folder as a list (name, size, date)
 * on the right, the path on top.  Files open through @open (xdg-open, or
 * the default editor / file tools).  Copying, moving and deleting are done
 * by cp, mv and rm in the background, so the window never hangs on a big
 * folder.
 *
 * Mouse: double-click opens, right-click for the menu, Ctrl/Shift-click
 * select several, click a column title to sort.
 * Keys:  Enter open, Backspace up, Alt+Left back, Alt+Home home,
 *        F2 rename, Delete delete, F5 reload, Ctrl+N new folder,
 *        Ctrl+C / Ctrl+X / Ctrl+V copy / cut / paste, Ctrl+A select all,
 *        Ctrl+H hidden files, Ctrl+L or / edit the path, Ctrl+T terminal
 *        here, letters jump to a name, Ctrl+Q quit.
 */
#include "../lib/retroui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define SIDE_W 132
#define MAX_PLACES 24
#define MAX_BACK 32

enum { SORT_NAME, SORT_SIZE, SORT_DATE };
enum { FOCUS_LIST, FOCUS_PATH, FOCUS_PROMPT };
enum { PR_NONE, PR_RENAME, PR_NEWDIR, PR_NEWFILE, PR_OPENWITH };
enum { P_FOLDER, P_FOLDER_DK, P_PAPER, P_EXEC, P_LINK, P_COUNT };

typedef struct Ent {
	char *name;
	int dir, link, exec, sel;
	off_t size;
	time_t mtime;
	mode_t mode;
} Ent;

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int w, h, running, dirty;
	char cwd[PATH_MAX];
	Ent *ents;
	int n, cur, anchor, top, rows;
	int sort, desc, hidden;
	char *back[MAX_BACK];
	int nback;
	RuiInput path, prompt;
	int focus, prompt_kind;
	RuiButton b_back, b_up, b_home, b_reload;
	RuiScrollbar sb;
	RuiRect r_side, r_list, r_head, r_status;
	struct {
		char label[32];
		char path[PATH_MAX];
	} place[MAX_PLACES];
	int nplaces;
	char **clip;                       /* copied or cut paths            */
	int nclip, cut;
	int busy;
	char msg[256];
	time_t dir_mtime;
	long long last_click, type_time;
	int last_idx;
	char typed[32];
	unsigned long pal[P_COUNT];
} fm;

static void reload(const char *keep);

/* ----------------------------------------------------------------- helpers */

static void
set_msg(const char *m)
{
	rui_strlcpy(fm.msg, m, sizeof fm.msg);
	fm.dirty = 1;
}

static char *
join(const char *dir, const char *name)
{
	return !strcmp(dir, "/") ? rui_strdupf("/%s", name) :
	    rui_strdupf("%s/%s", dir, name);
}

static void
fmt_size(off_t sz, char *buf, size_t n)
{
	static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)sz;
	int u = 0;

	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(buf, n, "%lld B", (long long)sz);
	else
		snprintf(buf, n, v < 10 ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

static int
nselected(void)
{
	int i, n = 0;

	for (i = 0; i < fm.n; i++)
		n += fm.ents[i].sel;
	return n;
}

static void
select_only(int idx)
{
	int i;

	for (i = 0; i < fm.n; i++)
		fm.ents[i].sel = i == idx;
	fm.cur = fm.anchor = idx;
	fm.dirty = 1;
}

static void
select_range(int a, int b)
{
	int i, lo = a < b ? a : b, hi = a < b ? b : a;

	for (i = 0; i < fm.n; i++)
		fm.ents[i].sel = i >= lo && i <= hi;
	fm.dirty = 1;
}

static void
ensure_visible(void)
{
	if (fm.cur < 0)
		return;
	if (fm.cur < fm.top)
		fm.top = fm.cur;
	if (fm.rows > 0 && fm.cur >= fm.top + fm.rows)
		fm.top = fm.cur - fm.rows + 1;
}

/* ------------------------------------------------------------- the folder */

static int
cmp_ent(const void *a, const void *b)
{
	const Ent *x = a, *y = b;
	int c = 0;

	if (x->dir != y->dir)
		return y->dir - x->dir;            /* folders first, always */
	if (fm.sort == SORT_SIZE && x->size != y->size)
		c = x->size < y->size ? -1 : 1;
	else if (fm.sort == SORT_DATE && x->mtime != y->mtime)
		c = x->mtime < y->mtime ? -1 : 1;
	if (!c)
		c = strcasecmp(x->name, y->name);
	if (!c)
		c = strcmp(x->name, y->name);
	return fm.desc ? -c : c;
}

static void
free_ents(void)
{
	int i;

	for (i = 0; i < fm.n; i++)
		free(fm.ents[i].name);
	free(fm.ents);
	fm.ents = NULL;
	fm.n = 0;
}

/* re-reads the folder; `keep` (a name) stays under the cursor */
static void
reload(const char *keep)
{
	DIR *d = opendir(fm.cwd);
	struct dirent *de;
	struct stat st;
	char *kept = keep ? rui_strdup(keep) : fm.cur >= 0 && fm.cur < fm.n ?
	    rui_strdup(fm.ents[fm.cur].name) : NULL;
	int cap = 0, i;

	free_ents();
	fm.cur = fm.anchor = -1;
	if (!d) {
		char buf[256];

		snprintf(buf, sizeof buf, "Cannot read this folder: %s",
		    strerror(errno));
		set_msg(buf);
	}
	while (d && (de = readdir(d))) {
		char *full;
		Ent *e;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!fm.hidden && de->d_name[0] == '.')
			continue;
		full = join(fm.cwd, de->d_name);
		if (lstat(full, &st) < 0) {
			free(full);
			continue;
		}
		if (fm.n == cap) {
			cap = cap ? cap * 2 : 64;
			fm.ents = rui_realloc(fm.ents, (size_t)cap * sizeof *fm.ents);
		}
		e = &fm.ents[fm.n++];
		memset(e, 0, sizeof *e);
		e->name = rui_strdup(de->d_name);
		e->link = S_ISLNK(st.st_mode);
		if (e->link && stat(full, &st) < 0)
			lstat(full, &st);              /* a dangling link */
		e->dir = S_ISDIR(st.st_mode);
		e->exec = !e->dir && (st.st_mode & 0111);
		e->size = st.st_size;
		e->mtime = st.st_mtime;
		e->mode = st.st_mode;
		free(full);
	}
	if (d)
		closedir(d);
	if (fm.n > 1)
		qsort(fm.ents, (size_t)fm.n, sizeof *fm.ents, cmp_ent);
	for (i = 0; kept && i < fm.n; i++)
		if (!strcmp(fm.ents[i].name, kept))
			select_only(i);
	free(kept);
	if (stat(fm.cwd, &st) == 0)
		fm.dir_mtime = st.st_mtime;
	if (fm.top > fm.n - 1)
		fm.top = fm.n > 0 ? fm.n - 1 : 0;
	ensure_visible();
	rui_input_set(&fm.path, fm.cwd);
	fm.dirty = 1;
}

/* goes to a folder; `remember` puts the old one on the Back stack */
static void
go(const char *dir, int remember)
{
	char real[PATH_MAX], buf[PATH_MAX + 64];
	struct stat st;
	const char *from = NULL;

	if (dir[0] == '~' && (dir[1] == '/' || !dir[1]) && getenv("HOME")) {
		snprintf(buf, sizeof buf, "%s%s", getenv("HOME"), dir + 1);
		dir = buf;
	}
	if (!realpath(dir, real) || stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) {
		snprintf(buf, sizeof buf, "No such folder: %.200s", dir);
		set_msg(buf);
		rui_input_set(&fm.path, fm.cwd);
		return;
	}
	if (!strcmp(real, fm.cwd))
		return;
	if (remember && fm.cwd[0]) {
		if (fm.nback == MAX_BACK) {
			free(fm.back[0]);
			memmove(&fm.back[0], &fm.back[1], (MAX_BACK - 1) *
			    sizeof fm.back[0]);
			fm.nback--;
		}
		fm.back[fm.nback++] = rui_strdup(fm.cwd);
	}
	/* going up: the folder we came out of stays selected */
	if (!strncmp(fm.cwd, real, strlen(real)) && strlen(fm.cwd) > strlen(real) &&
	    (fm.cwd[strlen(real)] == '/' || !strcmp(real, "/"))) {
		from = fm.cwd + strlen(real);
		while (*from == '/')
			from++;
		snprintf(buf, sizeof buf, "%s", from);
		buf[strcspn(buf, "/")] = '\0';
		from = buf;
	}
	fm.top = 0;
	fm.cur = -1;
	free_ents();
	if (from) {
		char *keep = rui_strdup(from);

		rui_strlcpy(fm.cwd, real, sizeof fm.cwd);
		reload(keep);
		free(keep);
	} else {
		rui_strlcpy(fm.cwd, real, sizeof fm.cwd);
		reload(NULL);
	}
	rui_set_title(fm.app, fm.win, fm.cwd, "retro-files");
	set_msg("");
}

static void
go_up(void)
{
	char *p = rui_strdup(fm.cwd), *slash = strrchr(p, '/');

	if (slash && slash != p)
		*slash = '\0';
	else if (slash)
		slash[1] = '\0';
	go(p, 1);
	free(p);
}

static void
go_back(void)
{
	char *p;

	if (!fm.nback)
		return;
	p = fm.back[--fm.nback];
	go(p, 0);
	free(p);
}

static void
add_place(const char *label, const char *path)
{
	struct stat st;
	int i;

	if (fm.nplaces == MAX_PLACES || stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
		return;
	for (i = 0; i < fm.nplaces; i++)
		if (!strcmp(fm.place[i].path, path))
			return;
	rui_strlcpy(fm.place[fm.nplaces].label, label,
	    sizeof fm.place[0].label);
	rui_strlcpy(fm.place[fm.nplaces].path, path, sizeof fm.place[0].path);
	fm.nplaces++;
}

/* removable media: the folders under dir */
static void
add_mounts(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d))) {
		char *p;

		if (de->d_name[0] == '.')
			continue;
		p = join(dir, de->d_name);
		add_place(de->d_name, p);
		free(p);
	}
	if (d)
		closedir(d);
}

static void
init_places(void)
{
	static const char *const sub[] = {
		"Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos",
		NULL
	};
	const char *home = getenv("HOME"), *user = getenv("USER");
	char *p;
	int i;

	if (home && *home) {
		add_place("Home", home);
		for (i = 0; sub[i]; i++) {
			p = join(home, sub[i]);
			add_place(sub[i], p);
			free(p);
		}
	}
	p = rui_programs_dir();
	add_place("Programs", p);              /* the Programs menu */
	free(p);
	p = rui_config_path("wallpapers");
	add_place("Wallpapers", p);
	free(p);
	add_place("Settings", rui_config_dir());
	add_place("Computer", "/");
	if (user && *user) {
		p = rui_strdupf("/run/media/%s", user);
		add_mounts(p);
		free(p);
		p = rui_strdupf("/media/%s", user);
		add_mounts(p);
		free(p);
	}
	add_mounts("/mnt");
}

/* ------------------------------------------------------------ file actions */

/* the selected entries, or the one under the cursor */
static int
targets(int **out)
{
	int *v = rui_malloc((size_t)(fm.n + 1) * sizeof *v), i, n = 0;

	for (i = 0; i < fm.n; i++)
		if (fm.ents[i].sel)
			v[n++] = i;
	if (!n && fm.cur >= 0 && fm.cur < fm.n)
		v[n++] = fm.cur;
	*out = v;
	return n;
}

static void
on_job(const char *out, int status, void *ud)
{
	char last[200] = "", line[200], buf[256];
	const char *p = out ? out : "";

	(void)ud;
	fm.busy = 0;
	while (rui_next_line(&p, line, sizeof line))
		if (*rui_ltrim(line))
			rui_strlcpy(last, rui_ltrim(line), sizeof last);
	if (status == 0)
		snprintf(buf, sizeof buf, "Done.");
	else
		snprintf(buf, sizeof buf, "Failed: %s", last[0] ? last :
		    "the command returned an error");
	reload(NULL);
	set_msg(buf);
}

static void
run_job(char *cmd, const char *what)
{
	if (fm.busy) {
		set_msg("Still busy with the previous operation.");
	} else if (rui_job_start(cmd, on_job, NULL) == 0) {
		fm.busy = 1;
		set_msg(what);
	}
	free(cmd);
}

static char *
append_quoted(char *cmd, const char *path)
{
	char *q = rui_shell_quote(path), *n = rui_strdupf("%s %s", cmd, q);

	free(q);
	free(cmd);
	return n;
}

static void
open_entry(int idx)
{
	char *full, *q, *cmd;

	if (idx < 0 || idx >= fm.n)
		return;
	full = join(fm.cwd, fm.ents[idx].name);
	if (fm.ents[idx].dir) {
		go(full, 1);
	} else {
		char buf[256];

		q = rui_shell_quote(full);
		cmd = rui_strdupf("@open %s", q);
		rui_run_command(fm.app, cmd);
		snprintf(buf, sizeof buf, "Opening %.200s", fm.ents[idx].name);
		set_msg(buf);
		free(cmd);
		free(q);
	}
	free(full);
}

static void
clip_free(void)
{
	int i;

	for (i = 0; i < fm.nclip; i++)
		free(fm.clip[i]);
	free(fm.clip);
	fm.clip = NULL;
	fm.nclip = 0;
}

static void
do_copy(int cut)
{
	int *v, n = targets(&v), i;
	char buf[128];

	if (n) {
		clip_free();
		fm.clip = rui_malloc((size_t)n * sizeof *fm.clip);
		for (i = 0; i < n; i++)
			fm.clip[i] = join(fm.cwd, fm.ents[v[i]].name);
		fm.nclip = n;
		fm.cut = cut;
		snprintf(buf, sizeof buf, "%d item%s to %s: go to the target folder "
		    "and paste (Ctrl+V).", n, n == 1 ? "" : "s", cut ? "move" : "copy");
		set_msg(buf);
	}
	free(v);
}

static void
do_paste(void)
{
	char *cmd = rui_strdup(":"), *dest;
	struct stat st;
	int i;

	if (!fm.nclip) {
		set_msg("Nothing was copied or cut.");
		free(cmd);
		return;
	}
	for (i = 0; i < fm.nclip; i++) {
		const char *base = strrchr(fm.clip[i], '/');
		char *one;

		base = base ? base + 1 : fm.clip[i];
		dest = join(fm.cwd, base);
		if (!strcmp(dest, fm.clip[i])) {
			if (fm.cut) {                  /* moving onto itself */
				free(dest);
				continue;
			}
			/* a copy next to the original gets a new name */
			free(dest);
			one = rui_strdupf("%s (copy)", base);
			dest = join(fm.cwd, one);
			free(one);
		}
		if (lstat(dest, &st) == 0) {
			char buf[256];

			snprintf(buf, sizeof buf, "\"%.150s\" exists here already; "
			    "nothing was pasted.", strrchr(dest, '/') + 1);
			set_msg(buf);
			free(dest);
			free(cmd);
			return;
		}
		one = rui_strdupf("%s && %s", cmd, fm.cut ? "mv --" : "cp -R --");
		free(cmd);
		cmd = append_quoted(append_quoted(one, fm.clip[i]), dest);
		free(dest);
	}
	dest = rui_strdupf("{ %s; } 2>&1", cmd);
	free(cmd);
	run_job(dest, fm.cut ? "Moving..." : "Copying...");
	if (fm.cut)
		clip_free();
}

static void
do_delete(void)
{
	int *v, n = targets(&v), i;
	char text[200], *cmd;

	if (!n) {
		free(v);
		return;
	}
	if (n == 1)
		snprintf(text, sizeof text, "Delete \"%.120s\" for good?",
		    fm.ents[v[0]].name);
	else
		snprintf(text, sizeof text, "Delete these %d items for good?", n);
	if (rui_confirm(fm.app, "Delete", text, "Delete")) {
		cmd = rui_strdup("rm -rf --");
		for (i = 0; i < n; i++) {
			char *full = join(fm.cwd, fm.ents[v[i]].name);

			cmd = append_quoted(cmd, full);
			free(full);
		}
		{
			char *full = rui_strdupf("%s 2>&1", cmd);

			free(cmd);
			run_job(full, "Deleting...");
		}
	}
	free(v);
}

static void
start_prompt(int kind, const char *initial)
{
	fm.prompt_kind = kind;
	fm.focus = FOCUS_PROMPT;
	rui_input_set(&fm.prompt, initial);
	fm.dirty = 1;
}

static void
finish_prompt(void)
{
	const char *t = fm.prompt.text;
	char *full, buf[300];
	int kind = fm.prompt_kind, fd;

	fm.prompt_kind = PR_NONE;
	fm.focus = FOCUS_LIST;
	fm.dirty = 1;
	if (!*t)
		return;
	if (kind == PR_OPENWITH) {
		if (fm.cur >= 0) {
			char *path = join(fm.cwd, fm.ents[fm.cur].name);
			char *q = rui_shell_quote(path);
			char *cmd = rui_strdupf("%s %s", t, q);

			rui_run_command(fm.app, cmd);
			free(cmd);
			free(q);
			free(path);
		}
		return;
	}
	if (strchr(t, '/') || !strcmp(t, ".") || !strcmp(t, "..")) {
		set_msg("A name cannot contain a slash.");
		return;
	}
	full = join(fm.cwd, t);
	if (kind == PR_NEWDIR) {
		if (mkdir(full, 0777) < 0)
			snprintf(buf, sizeof buf, "Cannot create the folder: %s",
			    strerror(errno));
		else
			buf[0] = '\0';
	} else if (kind == PR_NEWFILE) {
		if ((fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) {
			snprintf(buf, sizeof buf, "Cannot create the file: %s",
			    strerror(errno));
		} else {
			close(fd);
			buf[0] = '\0';
		}
	} else {
		char *old = fm.cur >= 0 ? join(fm.cwd, fm.ents[fm.cur].name) : NULL;
		struct stat st;

		buf[0] = '\0';
		if (old && strcmp(old, full)) {
			if (lstat(full, &st) == 0)
				snprintf(buf, sizeof buf, "\"%.150s\" exists already.", t);
			else if (rename(old, full) < 0)
				snprintf(buf, sizeof buf, "Cannot rename: %s",
				    strerror(errno));
		}
		free(old);
	}
	free(full);
	reload(t);
	set_msg(buf);
}

static void
terminal_here(void)
{
	char *q = rui_shell_quote(fm.cwd);
	char *cmd = rui_strdupf("cd %s && exec %s", q, rui_terminal());

	rui_spawn(cmd);
	free(cmd);
	free(q);
}

/* -------------------------------------------------------------------- draw */

static void
layout(void)
{
	RuiApp *a = fm.app;
	int fh = a->font->height, bh = fh + 10, y = 6, x = 6, bw = 52;
	int rh = fh + 6 < 18 ? 18 : fh + 6, sh = fh + 10;

	fm.b_back.r.x = x;
	fm.b_up.r.x = x + bw + 3;
	fm.b_home.r.x = x + 2 * (bw + 3);
	fm.b_reload.r.x = x + 3 * (bw + 3);
	fm.b_back.r.y = fm.b_up.r.y = fm.b_home.r.y = fm.b_reload.r.y = y;
	fm.b_back.r.w = fm.b_up.r.w = fm.b_home.r.w = fm.b_reload.r.w = bw;
	fm.b_back.r.h = fm.b_up.r.h = fm.b_home.r.h = fm.b_reload.r.h = bh;
	fm.path.r.x = x + 4 * (bw + 3) + 4;
	fm.path.r.y = y;
	fm.path.r.w = fm.w - fm.path.r.x - 6;
	fm.path.r.h = bh;
	y += bh + 6;
	fm.r_status.x = 6;
	fm.r_status.w = fm.w - 12;
	fm.r_status.h = sh;
	fm.r_status.y = fm.h - sh - 5;
	fm.r_side.x = 6;
	fm.r_side.y = y;
	fm.r_side.w = SIDE_W;
	fm.r_side.h = fm.r_status.y - y - 5;
	fm.r_head.x = 6 + SIDE_W + 6;
	fm.r_head.y = y;
	fm.r_head.w = fm.w - fm.r_head.x - 6;
	fm.r_head.h = fh + 8;
	fm.r_list.x = fm.r_head.x;
	fm.r_list.y = y + fm.r_head.h;
	fm.r_list.w = fm.r_head.w;
	fm.r_list.h = fm.r_side.y + fm.r_side.h - fm.r_list.y;
	fm.rows = (fm.r_list.h - 4) / rh;
	if (fm.rows < 1)
		fm.rows = 1;
	fm.sb.r.x = fm.r_list.x + fm.r_list.w - 18;
	fm.sb.r.y = fm.r_list.y + 2;
	fm.sb.r.w = 16;
	fm.sb.r.h = fm.r_list.h - 4;
	fm.sb.total = fm.n;
	fm.sb.page = fm.rows;
	if (fm.top > fm.n - fm.rows)
		fm.top = fm.n > fm.rows ? fm.n - fm.rows : 0;
	if (fm.top < 0)
		fm.top = 0;
	fm.sb.pos = fm.top;
}

static int
row_h(void)
{
	int fh = fm.app->font->height;

	return fh + 6 < 18 ? 18 : fh + 6;
}

/* column positions inside the list */
static void
columns(int *size_x, int *date_x, int *end_x)
{
	*end_x = fm.r_list.x + fm.r_list.w - 20;
	*date_x = *end_x - 124;
	*size_x = *date_x - 76;
}

/* 16x16 pictograms */
static void
draw_icon(RuiSurf *s, const Ent *e, int x, int y)
{
	if (e->dir) {
		rui_fill_pixel(s, x + 1, y + 2, 6, 3, fm.pal[P_FOLDER_DK]);
		rui_fill_pixel(s, x, y + 4, 16, 10, fm.pal[P_FOLDER]);
		rui_fill(s, x, y + 4, 16, 1, RUI_C_LIGHT);
		rui_fill(s, x, y + 13, 16, 1, RUI_C_DARK);
		rui_fill(s, x + 15, y + 4, 1, 10, RUI_C_DARK);
	} else {
		int i;

		rui_fill_pixel(s, x + 2, y, 12, 16, e->exec ? fm.pal[P_EXEC] :
		    fm.pal[P_PAPER]);
		rui_fill(s, x + 2, y, 12, 1, RUI_C_DARK);
		rui_fill(s, x + 2, y, 1, 16, RUI_C_DARK);
		rui_fill(s, x + 13, y, 1, 16, RUI_C_DARK);
		rui_fill(s, x + 2, y + 15, 12, 1, RUI_C_DARK);
		for (i = 0; i < 4 && !e->exec; i++)
			rui_fill(s, x + 4, y + 3 + i * 3, 8, 1, RUI_C_SHADOW);
		if (e->exec)
			rui_arrow(s, x + 8, y + 8, 3, 3, RUI_C_DARK);
	}
	if (e->link) {                         /* a link: small corner mark */
		rui_fill_pixel(s, x, y + 9, 7, 7, fm.pal[P_LINK]);
		rui_fill(s, x + 1, y + 12, 5, 1, RUI_C_LIGHT);
		rui_fill(s, x + 3, y + 10, 1, 5, RUI_C_LIGHT);
	}
}

static void
draw_status(void)
{
	static const char *const labels[] = {
		"", "Rename to:", "New folder:", "New file:", "Open with:"
	};
	RuiSurf *s = fm.s;
	RuiApp *a = fm.app;
	RuiRect r = fm.r_status;
	char buf[400];

	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	r.x += 6;
	r.w -= 12;
	if (fm.focus == FOCUS_PROMPT) {
		RuiRect l = r;

		l.w = 96;
		rui_text_fit(s, a->bold, l, labels[fm.prompt_kind], RUI_C_TEXT,
		    RUI_LEFT);
		fm.prompt.r.x = r.x + 100;
		fm.prompt.r.y = r.y + 1;
		fm.prompt.r.w = r.w - 100;
		fm.prompt.r.h = r.h - 2;
		fm.prompt.focused = 1;
		rui_input_draw(s, &fm.prompt);
		return;
	}
	if (fm.msg[0]) {
		rui_text_fit(s, a->font, r, fm.msg, RUI_C_TEXT, RUI_LEFT);
		return;
	}
	if (nselected() == 1 && fm.cur >= 0 && fm.ents[fm.cur].sel) {
		const Ent *e = &fm.ents[fm.cur];
		char sz[32], when[40], mode[11] = "----------";
		struct tm tm;
		int i;

		for (i = 0; i < 9; i++)
			if (e->mode & (0400 >> i))
				mode[i + 1] = "rwx"[i % 3];
		mode[0] = e->dir ? 'd' : e->link ? 'l' : '-';
		fmt_size(e->size, sz, sizeof sz);
		localtime_r(&e->mtime, &tm);
		strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
		snprintf(buf, sizeof buf, "%.180s   %s   %s   %s%s", e->name,
		    e->dir ? "folder" : sz, mode, when, e->link ? "   (link)" : "");
	} else {
		struct statvfs vfs;
		char fr[32] = "?";
		int ns = nselected();

		if (statvfs(fm.cwd, &vfs) == 0)
			fmt_size((off_t)vfs.f_bavail * (off_t)vfs.f_frsize, fr, sizeof fr);
		if (ns > 1)
			snprintf(buf, sizeof buf, "%d of %d items selected", ns, fm.n);
		else
			snprintf(buf, sizeof buf, "%d item%s%s   %s free", fm.n,
			    fm.n == 1 ? "" : "s", fm.hidden ? " (with hidden)" : "", fr);
	}
	rui_text_fit(s, a->font, r, buf, RUI_C_TEXT, RUI_LEFT);
}

static void
draw(void)
{
	static const char *const heads[] = { "Name", "Size", "Modified" };
	RuiSurf *s = fm.s;
	RuiApp *a = fm.app;
	RuiRect r;
	int i, rh = row_h(), sx, dx, ex, hx[4];

	layout();
	rui_fill(s, 0, 0, fm.w, fm.h, RUI_C_FACE);
	fm.b_back.disabled = fm.nback == 0;
	fm.b_up.disabled = !strcmp(fm.cwd, "/");
	rui_button_draw(s, &fm.b_back);
	rui_button_draw(s, &fm.b_up);
	rui_button_draw(s, &fm.b_home);
	rui_button_draw(s, &fm.b_reload);
	fm.path.focused = fm.focus == FOCUS_PATH;
	rui_input_draw(s, &fm.path);

	/* places */
	rui_fill(s, fm.r_side.x, fm.r_side.y, fm.r_side.w, fm.r_side.h,
	    RUI_C_FIELD);
	rui_bevel(s, fm.r_side.x, fm.r_side.y, fm.r_side.w, fm.r_side.h,
	    RUI_SUNKEN);
	for (i = 0; i < fm.nplaces; i++) {
		Ent folder;
		int here = !strcmp(fm.place[i].path, fm.cwd);

		memset(&folder, 0, sizeof folder);
		folder.dir = 1;
		r.x = fm.r_side.x + 2;
		r.y = fm.r_side.y + 3 + i * rh;
		r.w = fm.r_side.w - 4;
		r.h = rh;
		if (r.y + rh > fm.r_side.y + fm.r_side.h - 2)
			break;
		if (here)
			rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_SEL_BG);
		draw_icon(s, &folder, r.x + 4, r.y + (rh - 16) / 2);
		r.x += 24;
		r.w -= 26;
		rui_text_fit(s, a->font, r, fm.place[i].label, here ? RUI_C_SEL_FG :
		    RUI_C_TEXT, RUI_LEFT);
	}

	/* column titles: click to sort */
	columns(&sx, &dx, &ex);
	hx[0] = fm.r_head.x;
	hx[1] = sx;
	hx[2] = dx;
	hx[3] = fm.r_head.x + fm.r_head.w;
	for (i = 0; i < 3; i++) {
		r.x = hx[i];
		r.y = fm.r_head.y;
		r.w = hx[i + 1] - hx[i];
		r.h = fm.r_head.h;
		rui_bevel(s, r.x, r.y, r.w, r.h, RUI_RAISED_THIN);
		r.x += 6;
		r.w -= 20;
		rui_text_fit(s, a->bold, r, heads[i], RUI_C_TEXT, RUI_LEFT);
		if (fm.sort == i)
			rui_arrow(s, hx[i + 1] - 10, r.y + r.h / 2, 3, fm.desc ? 0 : 1,
			    RUI_C_TEXT);
	}

	/* the folder */
	rui_fill(s, fm.r_list.x, fm.r_list.y, fm.r_list.w, fm.r_list.h,
	    RUI_C_FIELD);
	rui_bevel(s, fm.r_list.x, fm.r_list.y, fm.r_list.w, fm.r_list.h,
	    RUI_SUNKEN);
	for (i = fm.top; i < fm.n && i < fm.top + fm.rows; i++) {
		const Ent *e = &fm.ents[i];
		int y = fm.r_list.y + 2 + (i - fm.top) * rh;
		int fg = e->sel ? RUI_C_SEL_FG : RUI_C_TEXT, cutting = 0, k;
		char buf[64];
		struct tm tm;

		if (e->sel)
			rui_fill(s, fm.r_list.x + 2, y, ex - fm.r_list.x - 2, rh,
			    RUI_C_SEL_BG);
		for (k = 0; fm.cut && k < fm.nclip && !cutting; k++) {
			char *full = join(fm.cwd, e->name);

			cutting = !strcmp(full, fm.clip[k]);
			free(full);
		}
		if (cutting && !e->sel)
			fg = RUI_C_DISABLED;           /* about to be moved away */
		draw_icon(s, e, fm.r_list.x + 6, y + (rh - 16) / 2);
		r.x = fm.r_list.x + 28;
		r.y = y;
		r.w = sx - r.x - 6;
		r.h = rh;
		rui_text_fit(s, e->dir ? a->bold : a->font, r, e->name, fg, RUI_LEFT);
		if (i == fm.cur && fm.focus == FOCUS_LIST)
			rui_bevel_c(s, fm.r_list.x + 2, y, ex - fm.r_list.x - 2, rh, 1,
			    RUI_C_SHADOW, RUI_C_SHADOW, 1);
		r.x = sx;
		r.w = dx - sx - 8;
		if (!e->dir) {
			fmt_size(e->size, buf, sizeof buf);
			rui_text_fit(s, a->font, r, buf, fg, RUI_RIGHT);
		}
		localtime_r(&e->mtime, &tm);
		strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
		r.x = dx + 4;
		r.w = ex - dx - 6;
		rui_text_fit(s, a->font, r, buf, fg, RUI_LEFT);
	}
	if (!fm.n) {
		r = fm.r_list;
		r.y += 10;
		r.h = a->font->height + 4;
		rui_text_fit(s, a->font, r, "This folder is empty.", RUI_C_DISABLED,
		    RUI_CENTER);
	}
	if (fm.n > fm.rows)
		rui_scrollbar_draw(s, &fm.sb);
	draw_status();
	rui_surf_flush(s);
	fm.dirty = 0;
}

/* ------------------------------------------------------------------ events */

static void
context_menu(int xr, int yr, int on_item)
{
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	int id, many = nselected() > 1;

	if (on_item) {
		rui_menu_add(m, "Open", NULL, 1)->disabled = many;
		rui_menu_add(m, "Open With...", NULL, 2)->disabled = many;
		rui_menu_add_sep(m);
		rui_menu_add(m, "Cut", NULL, 3);
		rui_menu_add(m, "Copy", NULL, 4);
	}
	rui_menu_add(m, "Paste", NULL, 5)->disabled = fm.nclip == 0;
	if (on_item) {
		rui_menu_add_sep(m);
		rui_menu_add(m, "Rename...", NULL, 6)->disabled = many;
		rui_menu_add(m, "Delete", NULL, 7);
	}
	rui_menu_add_sep(m);
	rui_menu_add(m, "New Folder...", NULL, 8);
	rui_menu_add(m, "New File...", NULL, 9);
	rui_menu_add(m, "Terminal Here", NULL, 10);
	rui_menu_add_sep(m);
	rui_menu_add(m, "Show Hidden Files", NULL, 11)->checked = fm.hidden;
	rui_menu_add(m, "Reload", NULL, 12);
	it = rui_menu_popup(fm.app, m, xr, yr, 0);
	id = it ? it->id : 0;
	rui_menu_free(m);
	switch (id) {
	case 1:
		open_entry(fm.cur);
		break;
	case 2:
		start_prompt(PR_OPENWITH, "");
		break;
	case 3:
	case 4:
		do_copy(id == 3);
		break;
	case 5:
		do_paste();
		break;
	case 6:
		if (fm.cur >= 0)
			start_prompt(PR_RENAME, fm.ents[fm.cur].name);
		break;
	case 7:
		do_delete();
		break;
	case 8:
		start_prompt(PR_NEWDIR, "New Folder");
		break;
	case 9:
		start_prompt(PR_NEWFILE, "new file.txt");
		break;
	case 10:
		terminal_here();
		break;
	case 11:
		fm.hidden = !fm.hidden;
		reload(NULL);
		break;
	case 12:
		reload(NULL);
		set_msg("");
		break;
	default:
		break;
	}
	fm.dirty = 1;
}

static void
on_button(XButtonEvent *e)
{
	int rh = row_h(), sx, dx, ex, idx = -1;

	if (fm.focus == FOCUS_PROMPT) {
		if (rui_in_rect(fm.prompt.r, e->x, e->y)) {
			XEvent ev;

			ev.xbutton = *e;
			rui_input_event(fm.app, &fm.prompt, &ev);
		} else {
			fm.prompt_kind = PR_NONE;      /* clicked away: cancelled */
			fm.focus = FOCUS_LIST;
		}
		fm.dirty = 1;
		return;
	}
	if (rui_in_rect(fm.path.r, e->x, e->y)) {
		XEvent ev;

		fm.focus = FOCUS_PATH;
		ev.xbutton = *e;
		rui_input_event(fm.app, &fm.path, &ev);
		fm.dirty = 1;
		return;
	}
	fm.focus = FOCUS_LIST;
	fm.dirty = 1;
	if (rui_in_rect(fm.r_side, e->x, e->y) && e->button == Button1) {
		int i = (e->y - fm.r_side.y - 3) / rh;

		if (i >= 0 && i < fm.nplaces)
			go(fm.place[i].path, 1);
		return;
	}
	columns(&sx, &dx, &ex);
	if (rui_in_rect(fm.r_head, e->x, e->y) && e->button == Button1) {
		int col = e->x >= dx ? SORT_DATE : e->x >= sx ? SORT_SIZE : SORT_NAME;

		if (col == fm.sort) {
			fm.desc = !fm.desc;
		} else {
			fm.sort = col;
			fm.desc = col != SORT_NAME;    /* largest / newest first */
		}
		reload(NULL);
		return;
	}
	if (!rui_in_rect(fm.r_list, e->x, e->y))
		return;
	if (e->button == Button4 || e->button == Button5) {
		fm.top += e->button == Button4 ? -3 : 3;
		return;
	}
	if (e->x < ex) {
		idx = fm.top + (e->y - fm.r_list.y - 2) / rh;
		if (idx < 0 || idx >= fm.n || idx >= fm.top + fm.rows)
			idx = -1;
	} else {
		return;                            /* the scrollbar */
	}
	set_msg("");
	if (e->button == Button1) {
		long long now = rui_now_ms();

		if (idx < 0) {
			select_only(-1);
		} else if (e->state & ControlMask) {
			fm.ents[idx].sel = !fm.ents[idx].sel;
			fm.cur = fm.anchor = idx;
		} else if ((e->state & ShiftMask) && fm.anchor >= 0) {
			select_range(fm.anchor, idx);
			fm.cur = idx;
		} else if (idx == fm.last_idx && now - fm.last_click < 400) {
			open_entry(idx);
			fm.last_idx = -1;
			return;
		} else {
			select_only(idx);
		}
		fm.last_idx = idx;
		fm.last_click = now;
	} else if (e->button == Button3) {
		if (idx >= 0 && !fm.ents[idx].sel)
			select_only(idx);
		else if (idx >= 0)
			fm.cur = idx;
		else
			select_only(-1);
		draw();
		context_menu(e->x_root, e->y_root, idx >= 0);
	} else if (e->button == Button2 && idx >= 0) {
		select_only(idx);
		open_entry(idx);
	}
}

/* letters typed quickly jump to the first name that starts with them */
static void
type_ahead(const char *ch)
{
	long long now = rui_now_ms();
	size_t len;
	int i;

	if (now - fm.type_time > 900)
		fm.typed[0] = '\0';
	fm.type_time = now;
	len = strlen(fm.typed);
	if (len + strlen(ch) + 1 < sizeof fm.typed)
		strcat(fm.typed, ch);
	len = strlen(fm.typed);
	for (i = 0; i < fm.n; i++) {
		if (!strncasecmp(fm.ents[i].name, fm.typed, len)) {
			select_only(i);
			ensure_visible();
			return;
		}
	}
}

static void
move_cursor(int to, int extend)
{
	if (!fm.n)
		return;
	if (to < 0)
		to = 0;
	if (to >= fm.n)
		to = fm.n - 1;
	if (extend && fm.anchor >= 0) {
		select_range(fm.anchor, to);
		fm.cur = to;
	} else {
		select_only(to);
	}
	ensure_visible();
	set_msg("");
}

static void
on_key(XEvent *ev)
{
	KeySym ks = XLookupKeysym(&ev->xkey, 0);
	unsigned st = ev->xkey.state;
	int ctrl = (st & ControlMask) != 0, alt = (st & Mod1Mask) != 0;
	int shift = (st & ShiftMask) != 0, r;
	char buf[8] = "";

	if (fm.focus == FOCUS_PROMPT || fm.focus == FOCUS_PATH) {
		RuiInput *in = fm.focus == FOCUS_PROMPT ? &fm.prompt : &fm.path;

		r = rui_input_event(fm.app, in, ev);
		if (r & RUI_EV_ACTIVATE) {
			if (fm.focus == FOCUS_PROMPT) {
				finish_prompt();
			} else {
				fm.focus = FOCUS_LIST;
				go(fm.path.text, 1);
			}
		} else if (r & RUI_EV_CANCEL) {
			fm.prompt_kind = PR_NONE;
			fm.focus = FOCUS_LIST;
			rui_input_set(&fm.path, fm.cwd);
		}
		fm.dirty = 1;
		return;
	}
	if (ctrl) {
		switch (ks) {
		case XK_q: fm.running = 0; break;
		case XK_c: do_copy(0); break;
		case XK_x: do_copy(1); break;
		case XK_v: do_paste(); break;
		case XK_n: start_prompt(PR_NEWDIR, "New Folder"); break;
		case XK_t: terminal_here(); break;
		case XK_h:
			fm.hidden = !fm.hidden;
			reload(NULL);
			break;
		case XK_l:
			fm.focus = FOCUS_PATH;
			break;
		case XK_a: {
			int i;

			for (i = 0; i < fm.n; i++)
				fm.ents[i].sel = 1;
			break;
		}
		case XK_r: reload(NULL); break;
		default: break;
		}
		fm.dirty = 1;
		return;
	}
	if (alt) {
		if (ks == XK_Left)
			go_back();
		else if (ks == XK_Up)
			go_up();
		else if (ks == XK_Home)
			go("~", 1);
		return;
	}
	switch (ks) {
	case XK_Up: move_cursor(fm.cur - 1, shift); break;
	case XK_Down: move_cursor(fm.cur + 1, shift); break;
	case XK_Prior: move_cursor(fm.cur - fm.rows, shift); break;
	case XK_Next: move_cursor(fm.cur + fm.rows, shift); break;
	case XK_Home: move_cursor(0, shift); break;
	case XK_End: move_cursor(fm.n - 1, shift); break;
	case XK_Return:
	case XK_KP_Enter:
		open_entry(fm.cur);
		break;
	case XK_BackSpace: go_up(); break;
	case XK_F2:
		if (fm.cur >= 0)
			start_prompt(PR_RENAME, fm.ents[fm.cur].name);
		break;
	case XK_F5:
		reload(NULL);
		set_msg("");
		break;
	case XK_Delete: do_delete(); break;
	case XK_Escape:
		select_only(-1);
		set_msg("");
		break;
	case XK_slash:
		fm.focus = FOCUS_PATH;
		rui_input_set(&fm.path, "/");
		fm.dirty = 1;
		break;
	default:
		if (XLookupString(&ev->xkey, buf, sizeof buf - 1, NULL, NULL) == 1 &&
		    (unsigned char)buf[0] > ' ' && buf[0] != 127) {
			buf[1] = '\0';
			type_ahead(buf);
		}
		break;
	}
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(fm.s);
		break;
	case ConfigureNotify:
		if (ev->xconfigure.window == fm.win &&
		    (ev->xconfigure.width != fm.w || ev->xconfigure.height != fm.h)) {
			fm.w = ev->xconfigure.width;
			fm.h = ev->xconfigure.height;
			rui_surf_resize(fm.s, fm.w, fm.h);
			fm.dirty = 1;
		}
		break;
	case KeyPress:
		on_key(ev);
		break;
	case SelectionNotify:
		if (fm.focus != FOCUS_LIST && rui_input_event(fm.app,
		    fm.focus == FOCUS_PROMPT ? &fm.prompt : &fm.path, ev))
			fm.dirty = 1;
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		if (ev->xany.window != fm.win)
			break;
		if (fm.focus != FOCUS_PROMPT) {
			if (rui_button_event(&fm.b_back, ev) & RUI_EV_ACTIVATE)
				go_back();
			if (rui_button_event(&fm.b_up, ev) & RUI_EV_ACTIVATE)
				go_up();
			if (rui_button_event(&fm.b_home, ev) & RUI_EV_ACTIVATE)
				go("~", 1);
			if (rui_button_event(&fm.b_reload, ev) & RUI_EV_ACTIVATE) {
				reload(NULL);
				set_msg("");
			}
			if (fm.n > fm.rows) {
				r = rui_scrollbar_event(&fm.sb, ev);
				if (r) {
					fm.top = fm.sb.pos;
					fm.dirty = 1;
				}
			}
		}
		if (ev->type == ButtonPress)
			on_button(&ev->xbutton);
		else if (ev->type == ButtonRelease)
			fm.dirty = 1;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(fm.app, ev))
			fm.running = 0;
		break;
	default:
		break;
	}
}

static void
dispatch_cb(XEvent *ev, void *ud)
{
	(void)ud;
	handle_event(ev);
}

/* -------------------------------------------------------------------- main */

static void
files_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "files_hidden"))
		fm.hidden = rui_conf_bool(val);
	else if (!strcmp(key, "files_sort"))
		fm.sort = !strcmp(val, "size") ? SORT_SIZE : !strcmp(val, "date") ?
		    SORT_DATE : SORT_NAME;
}

static void
setup(const char *start)
{
	static const char *const pal[P_COUNT] = {
		"#e8c850", "#a08020", "#f8f8f0", "#bfe0bf", "#3050b0"
	};
	RuiApp *a = fm.app;
	RuiMonitor mon = rui_monitor_pick(a, "primary");
	XSizeHints sh;
	char *path;
	int i;

	fm.w = 680;
	fm.h = 460;
	fm.cur = fm.anchor = fm.last_idx = -1;
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, files_conf, NULL);
	free(path);
	fm.desc = fm.sort != SORT_NAME;
	for (i = 0; i < P_COUNT; i++) {
		XColor xc, exact;

		fm.pal[i] = a->color[RUI_C_FACE].pixel;
		if (XAllocNamedColor(a->dpy, a->cmap, pal[i], &xc, &exact))
			fm.pal[i] = xc.pixel;
	}
	fm.b_back.label = "Back";
	fm.b_up.label = "Up";
	fm.b_home.label = "Home";
	fm.b_reload.label = "Reload";
	fm.win = rui_window_new(a, a->root, mon.x + (mon.w - fm.w) / 2,
	    mon.y + (mon.h - fm.h) / 3, fm.w, fm.h, 0, ExposureMask | KeyPressMask |
	    ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	fm.s = rui_surf_new(a, fm.win, fm.w, fm.h);
	rui_set_title(a, fm.win, "Files", "retro-files");
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PSize;
	sh.width = fm.w;
	sh.height = fm.h;
	sh.min_width = 420;
	sh.min_height = 240;
	XSetWMNormalHints(a->dpy, fm.win, &sh);
	rui_input_init(a, &fm.path, fm.win);
	rui_input_init(a, &fm.prompt, fm.win);
	a->dispatch = dispatch_cb;
	init_places();
	layout();
	go(start && *start ? start : "~", 0);
	if (!fm.cwd[0])
		go("/", 0);
	XMapWindow(a->dpy, fm.win);
}

int
main(int argc, char **argv)
{
	long long t_check = 0;
	int i;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		puts("retro-files " RUI_VERSION);
		return 0;
	}
	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		fprintf(stderr, "usage: retro-files [-v] [DIRECTORY]\n");
		return 2;
	}
	rui_signals_init();
	fm.app = rui_open("retro-files");
	setup(argc > 1 ? argv[1] : NULL);
#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	fm.running = 1;
	fm.dirty = 1;
	while (fm.running && !rui_quit_requested()) {
		XEvent ev;
		long long now;

		while (fm.running && XPending(fm.app->dpy)) {
			XNextEvent(fm.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		now = rui_now_ms();
		if (now - t_check > 2000) {        /* follow changes made elsewhere */
			struct stat st;

			t_check = now;
			if (!fm.busy && fm.focus != FOCUS_PROMPT &&
			    stat(fm.cwd, &st) == 0 && st.st_mtime != fm.dir_mtime)
				reload(NULL);
		}
		if (fm.dirty && fm.running)
			draw();
		if (fm.running)
			rui_wait(fm.app, 1000);
	}
	free_ents();
	clip_free();
	for (i = 0; i < fm.nback; i++)
		free(fm.back[i]);
	rui_input_destroy(&fm.path);
	rui_input_destroy(&fm.prompt);
	rui_surf_free(fm.s);
	XDestroyWindow(fm.app->dpy, fm.win);
	rui_close(fm.app);
	return 0;
}
