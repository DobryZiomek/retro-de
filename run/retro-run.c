/*
 * retro-run - Alt+F2 command runner with history and $PATH completion.
 *
 * Tab completes the command name, Up/Down walk the completion list (or the
 * history when no list is shown), Enter runs, Escape closes.
 */
#include "../lib/retroui.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define HIST_MAX 200
#define WIN_W 440

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int w, h, running, dirty;
	RuiInput input;
	RuiList list;
	RuiButton ok, cancel;
	char **hist;                   /* oldest first */
	int nhist, hist_pos;
	char **cmds;                   /* every executable in $PATH, sorted */
	int ncmds, cmds_loaded;
	char **matches;                /* borrowed pointers shown in the list */
	int nmatches;
	int list_is_history;
} run;

/* ---------------------------------------------------------------- history */

static void
hist_load(void)
{
	char *path = rui_config_path("history"), line[RUI_INPUT_MAX];
	FILE *f = fopen(path, "r");

	free(path);
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!line[0])
			continue;
		if (run.nhist == HIST_MAX) {
			free(run.hist[0]);
			memmove(&run.hist[0], &run.hist[1],
			    (HIST_MAX - 1) * sizeof *run.hist);
			run.nhist--;
		}
		run.hist = rui_realloc(run.hist, (size_t)(run.nhist + 1) *
		    sizeof *run.hist);
		run.hist[run.nhist++] = rui_strdup(line);
	}
	fclose(f);
}

static void
hist_save(const char *cmd)
{
	char *path = rui_config_path("history");
	FILE *f;
	int i;

	if (mkdir(rui_config_dir(), 0755) < 0 && errno != EEXIST) {
		free(path);
		return;
	}
	f = fopen(path, "w");
	free(path);
	if (!f)
		return;
	for (i = 0; i < run.nhist; i++)
		if (strcmp(run.hist[i], cmd))
			fprintf(f, "%s\n", run.hist[i]);
	fprintf(f, "%s\n", cmd);
	fclose(f);
}

/* ------------------------------------------------------------- completion */

static int
cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
cmds_load(void)
{
	const char *path = getenv("PATH");
	int cap = 0, i, k;

	run.cmds_loaded = 1;
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	while (*path) {
		size_t len = strcspn(path, ":");
		char dir[1024];
		DIR *d;
		struct dirent *de;

		if (len && len < sizeof dir) {
			memcpy(dir, path, len);
			dir[len] = '\0';
			if ((d = opendir(dir))) {
				while ((de = readdir(d))) {
					char full[2048];
					struct stat st;

					if (de->d_name[0] == '.')
						continue;
					snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
					if (stat(full, &st) < 0 || !S_ISREG(st.st_mode) ||
					    access(full, X_OK) < 0)
						continue;
					if (run.ncmds == cap) {
						cap = cap ? cap * 2 : 512;
						run.cmds = rui_realloc(run.cmds, (size_t)cap *
						    sizeof *run.cmds);
					}
					run.cmds[run.ncmds++] = rui_strdup(de->d_name);
				}
				closedir(d);
			}
		}
		path += len;
		if (*path == ':')
			path++;
	}
	if (run.ncmds > 1)
		qsort(run.cmds, (size_t)run.ncmds, sizeof *run.cmds, cmp_str);
	for (i = 1, k = 0; i < run.ncmds; i++) {   /* drop duplicates */
		if (strcmp(run.cmds[i], run.cmds[k])) {
			run.cmds[++k] = run.cmds[i];
		} else {
			free(run.cmds[i]);
		}
	}
	if (run.ncmds)
		run.ncmds = k + 1;
}

static void
set_list(char **items, int n, int is_history)
{
	run.list.items = items;
	run.list.n = n;
	run.list.sel = -1;
	run.list.top = 0;
	run.list_is_history = is_history;
	run.dirty = 1;
}

static void
complete(void)
{
	const char *text = run.input.text;
	size_t plen = strlen(text), common;
	int i;

	if (strchr(text, ' ') || strchr(text, '/'))
		return;                            /* only the command word */
	if (!run.cmds_loaded)
		cmds_load();
	free(run.matches);
	run.matches = rui_malloc((size_t)(run.ncmds + 1) * sizeof *run.matches);
	run.nmatches = 0;
	for (i = 0; i < run.ncmds; i++)
		if (!strncmp(run.cmds[i], text, plen))
			run.matches[run.nmatches++] = run.cmds[i];
	if (run.nmatches == 0) {
		set_list(NULL, 0, 0);
		return;
	}
	common = strlen(run.matches[0]);
	for (i = 1; i < run.nmatches; i++) {
		size_t k = 0;

		while (k < common && run.matches[i][k] == run.matches[0][k])
			k++;
		common = k;
	}
	if (common > plen && common < RUI_INPUT_MAX - 2) {
		char buf[RUI_INPUT_MAX];

		memcpy(buf, run.matches[0], common);
		buf[common] = '\0';
		if (run.nmatches == 1)
			strcat(buf, " ");
		rui_input_set(&run.input, buf);
	} else if (run.nmatches == 1) {
		char buf[RUI_INPUT_MAX];

		snprintf(buf, sizeof buf, "%s ", run.matches[0]);
		rui_input_set(&run.input, buf);
	}
	if (run.nmatches > 1)
		set_list(run.matches, run.nmatches, 0);
	else
		set_list(NULL, 0, 0);
	run.dirty = 1;
}

/* -------------------------------------------------------------------- UI */

static void
draw(void)
{
	RuiSurf *s = run.s;
	RuiApp *a = run.app;
	RuiRect r;

	rui_fill(s, 0, 0, run.w, run.h, RUI_C_FACE);
	r.x = 12;
	r.y = 8;
	r.w = run.w - 24;
	r.h = a->font->height + 4;
	rui_text_fit(s, a->font, r, "Type the name of a program to run:",
	    RUI_C_TEXT, RUI_LEFT);
	rui_input_draw(s, &run.input);
	rui_list_draw(s, &run.list);
	rui_button_draw(s, &run.ok);
	rui_button_draw(s, &run.cancel);
	rui_surf_flush(s);
	run.dirty = 0;
}

static void
execute(void)
{
	char cmd[RUI_INPUT_MAX];
	char *s = run.input.text, *e;

	while (*s == ' ')
		s++;
	rui_strlcpy(cmd, s, sizeof cmd);
	e = cmd + strlen(cmd);
	while (e > cmd && e[-1] == ' ')
		*--e = '\0';
	if (!cmd[0])
		return;
	hist_save(cmd);
	rui_run_command(run.app, cmd);
	run.running = 0;
}

static void
list_pick(void)
{
	if (run.list.sel >= 0 && run.list.sel < run.list.n)
		rui_input_set(&run.input, run.list.items[run.list.sel]);
	run.dirty = 1;
}

static void
on_key(XEvent *ev)
{
	KeySym ks = XLookupKeysym(&ev->xkey, 0);
	int r;

	if (ks == XK_Tab) {
		complete();
		return;
	}
	if (ks == XK_Up || ks == XK_Down || ks == XK_Prior || ks == XK_Next) {
		if (run.list.n > 0 && !run.list_is_history) {
			if (run.list.sel < 0 && (ks == XK_Down || ks == XK_Next)) {
				run.list.sel = 0;
				rui_list_key(run.app, &run.list, XK_Home);
			} else {
				rui_list_key(run.app, &run.list, ks);
			}
			list_pick();
		} else if (run.nhist > 0) {
			/* history: newest is at the end */
			if (ks == XK_Up || ks == XK_Prior)
				run.hist_pos = run.hist_pos > 0 ? run.hist_pos - 1 : 0;
			else
				run.hist_pos = run.hist_pos < run.nhist ? run.hist_pos + 1 :
				    run.nhist;
			rui_input_set(&run.input, run.hist_pos < run.nhist ?
			    run.hist[run.hist_pos] : "");
			if (!run.list_is_history)
				set_list(run.hist, run.nhist, 1);
			run.list.sel = run.hist_pos < run.nhist ? run.hist_pos : -1;
			run.list.top = run.list.sel - 3;   /* clamped when drawn */
			run.dirty = 1;
		}
		return;
	}
	r = rui_input_event(run.app, &run.input, ev);
	if (r & RUI_EV_ACTIVATE)
		execute();
	else if (r & RUI_EV_CANCEL)
		run.running = 0;
	if (r & RUI_EV_CHANGED) {
		set_list(NULL, 0, 0);
		run.hist_pos = run.nhist;
	}
	if (r)
		run.dirty = 1;
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(run.s);
		break;
	case KeyPress:
		on_key(ev);
		break;
	case SelectionNotify:
		if (rui_input_event(run.app, &run.input, ev))
			run.dirty = 1;
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		r = rui_button_event(&run.ok, ev);
		if (r & RUI_EV_ACTIVATE)
			execute();
		run.dirty |= r != 0;
		r = rui_button_event(&run.cancel, ev);
		if (r & RUI_EV_ACTIVATE)
			run.running = 0;
		run.dirty |= r != 0;
		r = rui_list_event(run.app, &run.list, ev);
		if (r & (RUI_EV_CHANGED | RUI_EV_ACTIVATE))
			list_pick();
		if (r & RUI_EV_ACTIVATE)
			execute();
		run.dirty |= r != 0;
		if (ev->type == ButtonPress &&
		    rui_input_event(run.app, &run.input, ev))
			run.dirty = 1;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(run.app, ev))
			run.running = 0;
		break;
	default:
		break;
	}
}

static void
setup(void)
{
	RuiApp *a = run.app;
	XSizeHints sh;
	XWMHints wmh;
	int fh = a->font->height, y, bw = 84, rows = 7;

	run.w = WIN_W;
	y = 8 + fh + 8;
	run.input.r.x = 12;
	run.input.r.y = y;
	run.input.r.w = run.w - 24;
	run.input.r.h = fh + 10;
	run.input.focused = 1;
	y += fh + 10 + 8;
	run.list.r.x = 12;
	run.list.r.y = y;
	run.list.r.w = run.w - 24;
	run.list.r.h = rows * (fh + 2) + 4;
	run.list.sel = -1;
	run.list.last_idx = -1;
	y += run.list.r.h + 10;
	run.ok.label = "Run";
	run.ok.r.x = run.w - 12 - 2 * bw - 8;
	run.ok.r.y = y;
	run.ok.r.w = bw;
	run.ok.r.h = fh + 12;
	run.cancel.label = "Cancel";
	run.cancel.r = run.ok.r;
	run.cancel.r.x = run.w - 12 - bw;
	run.h = y + fh + 12 + 10;

	run.win = rui_window_new(a, a->root, (a->sw - run.w) / 2,
	    (a->sh - run.h) / 3, run.w, run.h, 0, ExposureMask | KeyPressMask |
	    ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	run.s = rui_surf_new(a, run.win, run.w, run.h);
	rui_set_title(a, run.win, "Run", "retro-run");
	rui_set_window_type(a, run.win, A__NET_WM_WINDOW_TYPE_DIALOG);
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = run.w;
	sh.height = sh.min_height = sh.max_height = run.h;
	XSetWMNormalHints(a->dpy, run.win, &sh);
	memset(&wmh, 0, sizeof wmh);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(a->dpy, run.win, &wmh);
	rui_input_init(a, &run.input, run.win);
	hist_load();
	run.hist_pos = run.nhist;
	XMapWindow(a->dpy, run.win);
}

int
main(int argc, char **argv)
{
	int i;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-run " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-run [-v]\n");
		return 2;
	}
	rui_signals_init();
	run.app = rui_open("retro-run");
	setup();
#ifdef __OpenBSD__
	if (unveil("/", "rx") == -1 || unveil(rui_config_dir(), "rwc") == -1 ||
	    unveil(NULL, NULL) == -1)
		rui_die("unveil failed");
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	run.running = 1;
	run.dirty = 1;
	while (run.running && !rui_quit_requested()) {
		XEvent ev;

		while (run.running && XPending(run.app->dpy)) {
			XNextEvent(run.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		if (run.dirty && run.running)
			draw();
		if (run.running)
			rui_wait(run.app, -1);
	}
	rui_input_destroy(&run.input);
	for (i = 0; i < run.nhist; i++)
		free(run.hist[i]);
	free(run.hist);
	for (i = 0; i < run.ncmds; i++)
		free(run.cmds[i]);
	free(run.cmds);
	free(run.matches);
	rui_surf_free(run.s);
	XDestroyWindow(run.app->dpy, run.win);
	rui_close(run.app);
	return 0;
}
