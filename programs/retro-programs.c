/*
 * retro-programs - the programs folder as a window of icons, like the
 * Program Manager: categories are folders you open, programs start with a
 * double click.  It shows ~/.config/retro-de/programs (`programs_dir`),
 * the same folder as the Programs menu, and follows changes made to it
 * with a file manager, mv, ln -s or mkdir.
 *
 * Pictures: "# icon: SPEC" in a launcher, an image beside a program with
 * the same name (Chess.xpm), ".icon.xpm" in a category, an Icon= path in a
 * .desktop file, or a pictogram chosen from the command.  No XDG icon
 * themes and no D-Bus: plain files and POSIX calls only.
 *
 *   retro-programs [CATEGORY]   the window, opened at a category
 *   retro-programs -l [CATEGORY] lists the programs (and their icons)
 */
#include "../desk/image.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CELL_W 92
#define CELL_H 74
#define PAD 8
#define POLL_MS 1500
#define MAX_PATH_DEPTH 8

typedef struct Item {
	RuiProgram p;
	Image *img;
} Item;

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int w, h;
	int running, dirty;
	char *root;                        /* the programs folder           */
	char *dir;                         /* the category shown            */
	Item *items;
	int n, sel;
	int cols, rows, vis_rows;
	RuiRect field, grid;
	RuiScrollbar sb;
	RuiButton up, organize;
	int bar_h, status_h;
	long long stamp, last_poll;
	long long last_click;
	int last_idx;
	int single_click;
	int reload;
} pm;

/* ------------------------------------------------------------- contents */

static void
free_items(void)
{
	int i;

	for (i = 0; i < pm.n; i++) {
		image_free(pm.items[i].img);
		free(pm.items[i].p.label);
		free(pm.items[i].p.path);
		free(pm.items[i].p.cmd);
		free(pm.items[i].p.image);
	}
	free(pm.items);
	pm.items = NULL;
	pm.n = 0;
}

static void
load_items(void)
{
	RuiProgram *v;
	int n, i;

	free_items();
	n = rui_programs_read(pm.dir, &v);
	if (n)
		pm.items = rui_calloc((size_t)n, sizeof *pm.items);
	for (i = 0; i < n; i++) {
		pm.items[i].p = v[i];              /* takes over the strings */
		if (pm.app && v[i].image)
			pm.items[i].img = image_fit(image_load(pm.app, v[i].image),
			    RUI_ICON_SIZE);
	}
	free(v);                               /* the array, not the strings */
	pm.n = n;
	if (pm.sel >= pm.n)
		pm.sel = pm.n - 1;
	pm.stamp = rui_programs_stamp();
	pm.dirty = 1;
}

static int
at_root(void)
{
	return !strcmp(pm.dir, pm.root);
}

/* "Programs > Games > Board" */
static void
crumbs(char *buf, size_t n)
{
	const char *rel = pm.dir + strlen(pm.root);
	size_t o;

	rui_strlcpy(buf, "Programs", n);
	while (*rel == '/')
		rel++;
	while (*rel) {
		const char *e = rel + strcspn(rel, "/");
		char part[NAME_MAX + 1], *label;

		o = (size_t)(e - rel) < sizeof part - 1 ? (size_t)(e - rel) :
		    sizeof part - 1;
		memcpy(part, rel, o);
		part[o] = '\0';
		/* the same label the menu shows: without "NN-" */
		label = part;
		while (*label >= '0' && *label <= '9')
			label++;
		label = (label > part && *label == '-' && label[1]) ? label + 1 :
		    part;
		o = strlen(buf);
		snprintf(buf + o, n > o ? n - o : 0, " > %s", label);
		rel = e;
		while (*rel == '/')
			rel++;
	}
}

static void
set_title(void)
{
	char t[512];

	crumbs(t, sizeof t);
	rui_set_title(pm.app, pm.win, t, "retro-programs");
}

static void
go_to(const char *dir, const char *select_path)
{
	int i;

	free(pm.dir);
	pm.dir = rui_strdup(dir);
	pm.sel = -1;
	pm.sb.pos = 0;
	load_items();
	for (i = 0; select_path && i < pm.n; i++)
		if (!strcmp(pm.items[i].p.path, select_path))
			pm.sel = i;
	if (pm.sel < 0 && pm.n)
		pm.sel = 0;
	if (pm.win)
		set_title();
}

static void
go_up(void)
{
	char *parent, *slash, *from;

	if (at_root())
		return;
	from = rui_strdup(pm.dir);
	parent = rui_strdup(pm.dir);
	slash = strrchr(parent, '/');
	if (slash && slash > parent)
		*slash = '\0';
	if (strlen(parent) < strlen(pm.root))  /* never above the folder */
		strcpy(parent, pm.root);
	go_to(parent, from);
	free(parent);
	free(from);
}

static void
activate(int i)
{
	if (i < 0 || i >= pm.n)
		return;
	if (pm.items[i].p.is_dir)
		go_to(pm.items[i].p.path, NULL);
	else
		rui_run_command(pm.app, pm.items[i].p.cmd);
}

static void
organize(void)
{
	char *q = rui_shell_quote(pm.dir), *cmd;

	/* our own file explorer: no XDG default application needed */
	cmd = rui_have_cmd("retro-files") ? rui_strdupf("retro-files %s", q) :
	    rui_strdupf("@filemanager %s", q);
	rui_run_command(pm.app, cmd);
	free(cmd);
	free(q);
}

/* ---------------------------------------------------------------- layout */

static void
layout(void)
{
	RuiApp *a = pm.app;
	int fh = a->font->height, bw;

	pm.bar_h = fh + 18;
	pm.status_h = fh + 8;
	bw = rui_text_width(a, a->font, "Organize...", -1) + 24;
	pm.up.r.x = PAD;
	pm.up.r.y = 5;
	pm.up.r.w = rui_text_width(a, a->font, "Up", -1) + 36;
	pm.up.r.h = fh + 8;
	pm.up.label = "";                      /* drawn with its arrow below */
	pm.organize.r.x = pm.w - PAD - bw;
	pm.organize.r.y = 5;
	pm.organize.r.w = bw;
	pm.organize.r.h = fh + 8;
	pm.organize.label = "Organize...";
	pm.field.x = PAD;
	pm.field.y = pm.bar_h;
	pm.field.w = pm.w - 2 * PAD;
	pm.field.h = pm.h - pm.bar_h - pm.status_h - 4;
	if (pm.field.h < CELL_H)
		pm.field.h = CELL_H;
	pm.sb.r.w = 16;
	pm.sb.r.x = pm.field.x + pm.field.w - 2 - pm.sb.r.w;
	pm.sb.r.y = pm.field.y + 2;
	pm.sb.r.h = pm.field.h - 4;
	pm.grid.x = pm.field.x + 6;
	pm.grid.y = pm.field.y + 6;
	pm.grid.w = pm.sb.r.x - pm.grid.x - 2;
	pm.grid.h = pm.field.h - 12;
	pm.cols = pm.grid.w / CELL_W;
	if (pm.cols < 1)
		pm.cols = 1;
	pm.vis_rows = pm.grid.h / CELL_H;
	if (pm.vis_rows < 1)
		pm.vis_rows = 1;
	pm.dirty = 1;
}

static void
update_scroll(void)
{
	pm.rows = (pm.n + pm.cols - 1) / pm.cols;
	pm.sb.total = pm.rows;
	pm.sb.page = pm.vis_rows;
	if (pm.sb.pos > pm.rows - pm.vis_rows)
		pm.sb.pos = pm.rows - pm.vis_rows;
	if (pm.sb.pos < 0)
		pm.sb.pos = 0;
}

static RuiRect
cell_rect(int i)
{
	RuiRect r;

	r.x = pm.grid.x + (i % pm.cols) * CELL_W;
	r.y = pm.grid.y + (i / pm.cols - pm.sb.pos) * CELL_H;
	r.w = CELL_W;
	r.h = CELL_H;
	return r;
}

static int
hit(int x, int y)
{
	int i;

	if (!rui_in_rect(pm.grid, x, y))
		return -1;
	for (i = 0; i < pm.n; i++)
		if (rui_in_rect(cell_rect(i), x, y))
			return i;
	return -1;
}

static void
show_sel(void)
{
	int row;

	if (pm.sel < 0)
		return;
	row = pm.sel / pm.cols;
	if (row < pm.sb.pos)
		pm.sb.pos = row;
	else if (row >= pm.sb.pos + pm.vis_rows)
		pm.sb.pos = row - pm.vis_rows + 1;
}

/* ------------------------------------------------------------------ draw */

/* the label under an icon: up to two lines, elided after that */
static void
draw_label(RuiSurf *s, RuiRect cell, int y, const char *label, int sel)
{
	RuiApp *a = pm.app;
	int maxw = CELL_W - 6, len = (int)strlen(label), cut = len, lines = 1;
	int w1, w2 = 0, fh = a->font->height;
	RuiRect t;

	if (rui_text_width(a, a->font, label, len) > maxw) {
		/* break at the last space that fits */
		int i;

		cut = -1;
		for (i = len - 1; i > 0; i--)
			if (label[i] == ' ' && rui_text_width(a, a->font, label, i) <=
			    maxw) {
				cut = i;
				break;
			}
		if (cut > 0)
			lines = 2;
		else
			cut = len;
	}
	w1 = rui_text_width(a, a->font, label, cut);
	if (lines == 2)
		w2 = rui_text_width(a, a->font, label + cut + 1, -1);
	if (w1 > maxw)
		w1 = maxw;
	if (w2 > maxw)
		w2 = maxw;
	t.x = cell.x + (CELL_W - (w1 > w2 ? w1 : w2) - 6) / 2;
	t.y = y;
	t.w = (w1 > w2 ? w1 : w2) + 6;
	t.h = lines * fh + 2;
	if (sel)
		rui_fill(s, t.x, t.y, t.w, t.h, RUI_C_SEL_BG);
	t.h = fh + 1;
	{
		char first[512];

		snprintf(first, sizeof first, "%.*s", cut, label);
		rui_text_fit(s, a->font, t, first, sel ? RUI_C_SEL_FG : RUI_C_TEXT,
		    RUI_CENTER);
	}
	if (lines == 2) {
		t.y += fh;
		rui_text_fit(s, a->font, t, label + cut + 1, sel ? RUI_C_SEL_FG :
		    RUI_C_TEXT, RUI_CENTER);
	}
}

static void
draw(void)
{
	RuiSurf *s = pm.s;
	RuiApp *a = pm.app;
	char buf[600];
	RuiRect r;
	int i, first, last;

	update_scroll();
	rui_fill(s, 0, 0, pm.w, pm.h, RUI_C_FACE);
	rui_fill(s, pm.field.x, pm.field.y, pm.field.w, pm.field.h, RUI_C_FIELD);

	first = pm.sb.pos * pm.cols;
	last = (pm.sb.pos + pm.vis_rows + 1) * pm.cols;
	for (i = first; i < pm.n && i < last; i++) {
		Item *it = &pm.items[i];
		RuiRect c = cell_rect(i);
		int iy = c.y + 4;

		if (it->img)
			image_draw(a, it->img, s->buf, c.x + (CELL_W - it->img->w) / 2,
			    iy + (RUI_ICON_SIZE - it->img->h) / 2);
		else
			rui_icon_draw(s, it->p.icon, c.x + (CELL_W - RUI_ICON_SIZE) / 2,
			    iy);
		draw_label(s, c, iy + RUI_ICON_SIZE + 3, it->p.label, i == pm.sel);
	}
	if (!pm.n) {
		r = pm.grid;
		r.h = a->font->height + 4;
		rui_text_fit(s, a->font, r, at_root() ? "The programs folder is "
		    "empty: Organize... opens it." : "(empty)", RUI_C_DISABLED,
		    RUI_LEFT);
	}
	/* the frame and the bars cover whatever a partial row spilled */
	rui_fill(s, 0, 0, pm.w, pm.field.y, RUI_C_FACE);
	rui_fill(s, 0, pm.field.y + pm.field.h, pm.w, pm.h - pm.field.y -
	    pm.field.h, RUI_C_FACE);
	rui_fill(s, pm.field.x + 2, pm.field.y + 2, pm.field.w - 4, 4,
	    RUI_C_FIELD);
	rui_fill(s, pm.field.x + 2, pm.grid.y + pm.vis_rows * CELL_H,
	    pm.field.w - 4, pm.field.y + pm.field.h - 2 - pm.grid.y -
	    pm.vis_rows * CELL_H, RUI_C_FIELD);
	rui_bevel(s, pm.field.x, pm.field.y, pm.field.w, pm.field.h, RUI_SUNKEN);
	rui_scrollbar_draw(s, &pm.sb);

	pm.up.disabled = at_root();
	rui_button_draw(s, &pm.up);
	{
		int off = pm.up.pressed && pm.up.armed, c = pm.up.disabled ?
		    RUI_C_DISABLED : RUI_C_TEXT;

		rui_arrow(s, pm.up.r.x + 12 + off, pm.up.r.y + pm.up.r.h / 2 + off,
		    5, 0, c);
		r = pm.up.r;
		r.x += 21 + off;
		r.y += off;
		r.w -= 23;
		rui_text_fit(s, a->font, r, "Up", c, RUI_LEFT);
	}
	rui_button_draw(s, &pm.organize);
	crumbs(buf, sizeof buf);
	r.x = pm.up.r.x + pm.up.r.w + 10;
	r.y = pm.up.r.y;
	r.w = pm.organize.r.x - 10 - r.x;
	r.h = pm.up.r.h;
	rui_text_fit(s, a->bold, r, buf, RUI_C_TEXT, RUI_LEFT);

	r.x = PAD;
	r.y = pm.h - pm.status_h;
	r.w = pm.w - 2 * PAD;
	r.h = pm.status_h - 2;
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	{
		int ndir = 0;

		for (i = 0; i < pm.n; i++)
			ndir += pm.items[i].p.is_dir;
		if (pm.sel >= 0 && pm.sel < pm.n && !pm.items[pm.sel].p.is_dir)
			snprintf(buf, sizeof buf, "%s", pm.items[pm.sel].p.cmd);
		else
			snprintf(buf, sizeof buf, "%d categor%s, %d program%s", ndir,
			    ndir == 1 ? "y" : "ies", pm.n - ndir, pm.n - ndir == 1 ?
			    "" : "s");
	}
	r.x += 6;
	r.w -= 12;
	rui_text_fit(s, a->font, r, buf, RUI_C_TEXT, RUI_LEFT);
	rui_surf_flush(s);
	pm.dirty = 0;
}

/* ---------------------------------------------------------------- events */

static void
item_menu(int i, int x, int y)
{
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	int id = 0;

	if (i >= 0) {
		rui_menu_add(m, pm.items[i].p.is_dir ? "Open" : "Run", NULL, 1);
		if (!pm.items[i].p.is_dir)
			rui_menu_add(m, "Edit Launcher...", NULL, 2);
		rui_menu_add_sep(m);
	}
	rui_menu_add(m, "Up", NULL, 3)->disabled = at_root();
	rui_menu_add(m, "Refresh", NULL, 4);
	rui_menu_add(m, "Organize...", NULL, 5);
	rui_menu_add_sep(m);
	rui_menu_add(m, "Close", NULL, 6);
	it = rui_menu_popup(pm.app, m, x, y, 0);
	if (it)
		id = it->id;
	rui_menu_free(m);
	switch (id) {
	case 1:
		activate(i);
		break;
	case 2: {
		char *q = rui_shell_quote(pm.items[i].p.path), *cmd;

		cmd = rui_strdupf("@editor %s", q);
		rui_run_command(pm.app, cmd);
		free(cmd);
		free(q);
		break;
	}
	case 3:
		go_up();
		break;
	case 4:
		load_items();
		break;
	case 5:
		organize();
		break;
	case 6:
		pm.running = 0;
		break;
	default:
		break;
	}
	pm.dirty = 1;
}

static void
move_sel(int d)
{
	if (!pm.n)
		return;
	if (pm.sel < 0)
		pm.sel = 0;
	else if (pm.sel + d >= 0 && pm.sel + d < pm.n)
		pm.sel += d;
	else if (d > 0)
		pm.sel = pm.n - 1;
	else
		pm.sel = 0;
	show_sel();
	pm.dirty = 1;
}

/* typing jumps to the next label starting with that letter */
static void
jump_to(char c)
{
	int i, k;

	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	for (k = 1; k <= pm.n; k++) {
		char f;

		i = (pm.sel + k + pm.n) % pm.n;
		f = pm.items[i].p.label[0];
		if (f >= 'A' && f <= 'Z')
			f = (char)(f - 'A' + 'a');
		if (f == c) {
			pm.sel = i;
			show_sel();
			pm.dirty = 1;
			return;
		}
	}
}

static void
on_key(XKeyEvent *e)
{
	KeySym ks = XLookupKeysym(e, 0);
	char buf[8] = "";
	int n = XLookupString(e, buf, sizeof buf - 1, NULL, NULL);

	if ((e->state & ControlMask) && (ks == XK_q || ks == XK_w)) {
		pm.running = 0;
		return;
	}
	switch (ks) {
	case XK_Escape:
		if (at_root())
			pm.running = 0;
		else
			go_up();
		break;
	case XK_BackSpace:
		go_up();
		break;
	case XK_Up:
		if (e->state & Mod1Mask)
			go_up();
		else
			move_sel(-pm.cols);
		break;
	case XK_Down:
		move_sel(pm.cols);
		break;
	case XK_Left:
		move_sel(-1);
		break;
	case XK_Right:
		move_sel(1);
		break;
	case XK_Home:
		move_sel(-pm.n);
		break;
	case XK_End:
		move_sel(pm.n);
		break;
	case XK_Page_Up:
		move_sel(-pm.cols * pm.vis_rows);
		break;
	case XK_Page_Down:
		move_sel(pm.cols * pm.vis_rows);
		break;
	case XK_Return:
	case XK_KP_Enter:
		activate(pm.sel);
		break;
	case XK_F5:
		load_items();
		break;
	case XK_Menu:
		if (pm.sel >= 0) {
			RuiRect c = cell_rect(pm.sel);
			Window child;
			int rx, ry;

			XTranslateCoordinates(pm.app->dpy, pm.win, pm.app->root, c.x +
			    CELL_W / 2, c.y + CELL_H / 2, &rx, &ry, &child);
			item_menu(pm.sel, rx, ry);
		}
		break;
	default:
		if (n == 1 && (unsigned char)buf[0] > ' ' && buf[0] != 127)
			jump_to(buf[0]);
		break;
	}
}

static void
on_button(XEvent *ev)
{
	XButtonEvent *e = &ev->xbutton;
	int r, i;
	long long now;

	r = rui_button_event(&pm.up, ev);
	if (r & RUI_EV_ACTIVATE)
		go_up();
	pm.dirty |= r != 0;
	r = rui_button_event(&pm.organize, ev);
	if (r & RUI_EV_ACTIVATE)
		organize();
	pm.dirty |= r != 0;
	r = rui_scrollbar_event(&pm.sb, ev);
	pm.dirty |= r != 0;
	if (r || ev->type != ButtonPress)
		return;
	if (e->button == Button4 || e->button == Button5) {
		if (rui_in_rect(pm.field, e->x, e->y)) {
			pm.sb.pos += e->button == Button4 ? -1 : 1;
			update_scroll();
			pm.dirty = 1;
		}
		return;
	}
	if (!rui_in_rect(pm.field, e->x, e->y))
		return;
	i = hit(e->x, e->y);
	now = rui_now_ms();
	if (i != pm.sel) {
		pm.sel = i;
		pm.dirty = 1;
	}
	if (e->button == Button3) {
		item_menu(i, e->x_root, e->y_root);
		return;
	}
	if (e->button == Button2 && i >= 0) {
		activate(i);
		return;
	}
	if (e->button != Button1)
		return;
	if (i >= 0 && (pm.single_click || (i == pm.last_idx &&
	    now - pm.last_click < 400))) {
		pm.last_idx = -1;
		activate(i);
	} else {
		pm.last_idx = i;
		pm.last_click = now;
	}
}

static void
handle_event(XEvent *ev)
{
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(pm.s);
		break;
	case ConfigureNotify:
		if (ev->xconfigure.width != pm.w || ev->xconfigure.height != pm.h) {
			pm.w = ev->xconfigure.width;
			pm.h = ev->xconfigure.height;
			rui_surf_resize(pm.s, pm.w, pm.h);
			layout();
			show_sel();
		}
		break;
	case KeyPress:
		on_key(&ev->xkey);
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		on_button(ev);
		break;
	case PropertyNotify:
		if (ev->xproperty.window == pm.app->root &&
		    ev->xproperty.atom == pm.app->atom[A__RETRO_RELOAD]) {
			pm.reload = 1;             /* new settings: start afresh */
			pm.running = 0;
		}
		break;
	case ClientMessage:
		if (rui_wm_delete_event(pm.app, ev))
			pm.running = 0;
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

static void
pm_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "icon_single_click"))
		pm.single_click = rui_conf_bool(val);
}

/* ------------------------------------------------------------- start up */

/* the folder to open: the programs folder, or a category inside it */
static int
open_dir(const char *category)
{
	char real[PATH_MAX], *d = rui_programs_dir(), *sub;
	struct stat st;

	rui_programs_init();                   /* first use: the defaults */
	if (realpath(d, real)) {
		free(d);
		d = rui_strdup(real);
	}
	pm.root = d;
	if (!category || !*category) {
		pm.dir = rui_strdup(d);
		return 0;
	}
	sub = category[0] == '/' ? rui_strdup(category) :
	    rui_strdupf("%s/%s", d, category);
	if (realpath(sub, real) && stat(real, &st) == 0 && S_ISDIR(st.st_mode) &&
	    !strncmp(real, d, strlen(d)) && (real[strlen(d)] == '/' ||
	    real[strlen(d)] == '\0')) {
		pm.dir = rui_strdup(real);
		free(sub);
		return 0;
	}
	free(sub);
	pm.dir = rui_strdup(d);
	return -1;
}

/* -l: the tree as text, for scripts and for checking icons */
static void
list_dir(const char *dir, int depth)
{
	RuiProgram *v;
	int n = rui_programs_read(dir, &v), i;

	for (i = 0; i < n; i++) {
		printf("%*s%s%s\t%s\t%s\n", depth * 2, "", v[i].label,
		    v[i].is_dir ? "/" : "", v[i].is_dir ? "" : v[i].cmd,
		    v[i].image ? v[i].image : rui_icon_name(v[i].icon));
		if (v[i].is_dir && depth < MAX_PATH_DEPTH)
			list_dir(v[i].path, depth + 1);
	}
	rui_programs_free(v, n);
}

static void
setup(void)
{
	RuiApp *a = pm.app;
	XSizeHints sh;
	XWMHints wmh;
	char *path;

	path = rui_config_path("retro.conf");
	rui_conf_parse(path, pm_conf, NULL);
	free(path);
	pm.w = CELL_W * 5 + 2 * PAD + 34;
	pm.h = CELL_H * 4 + a->font->height * 2 + 50;
	pm.sel = pm.last_idx = -1;
	pm.win = rui_window_new(a, a->root, 80, 80, pm.w, pm.h, 0,
	    ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
	    ButtonMotionMask | StructureNotifyMask);
	pm.s = rui_surf_new(a, pm.win, pm.w, pm.h);
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PSize;
	sh.width = pm.w;
	sh.height = pm.h;
	sh.min_width = CELL_W * 2 + 2 * PAD + 34;
	sh.min_height = CELL_H + a->font->height * 2 + 50;
	XSetWMNormalHints(a->dpy, pm.win, &sh);
	memset(&wmh, 0, sizeof wmh);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(a->dpy, pm.win, &wmh);
	XSelectInput(a->dpy, a->root, PropertyChangeMask);
	a->dispatch = dispatch_cb;
	layout();
	load_items();
	if (pm.n)
		pm.sel = 0;
	set_title();
	XMapWindow(a->dpy, pm.win);
}

int
main(int argc, char **argv)
{
	const char *category = NULL;
	int list = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("retro-programs " RUI_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-l")) {
			list = 1;
		} else if (argv[i][0] != '-' && !category) {
			category = argv[i];
		} else {
			fprintf(stderr, "usage: retro-programs [-v] [-l] [CATEGORY]\n");
			return 2;
		}
	}
	if (open_dir(category) < 0)
		rui_warn("no category '%s' in %s", category, pm.root);
	if (list) {
		list_dir(pm.dir, 0);
		return 0;
	}
	rui_signals_init();
	pm.app = rui_open("retro-programs");
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	pm.running = 1;
	pm.last_poll = rui_now_ms();
	while (pm.running && !rui_quit_requested()) {
		XEvent ev;
		long long now;

		while (pm.running && XPending(pm.app->dpy)) {
			XNextEvent(pm.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		now = rui_now_ms();
		if (now - pm.last_poll >= POLL_MS) {
			/* follow what a file manager does to the folder */
			struct stat st;

			pm.last_poll = now;
			if (stat(pm.dir, &st) < 0)
				go_to(pm.root, NULL);
			else if (rui_programs_stamp() != pm.stamp)
				load_items();
		}
		if (pm.dirty && pm.running)
			draw();
		if (pm.running)
			rui_wait(pm.app, POLL_MS);
	}
	free_items();
	rui_surf_free(pm.s);
	XDestroyWindow(pm.app->dpy, pm.win);
	rui_close(pm.app);
	if (pm.reload && !rui_quit_requested()) {
		char *args[3];

		args[0] = argv[0];
		args[1] = pm.dir;                  /* reopen where we were */
		args[2] = NULL;
		execvp(argv[0], args);
		perror("retro-programs: reload");
		return 1;
	}
	free(pm.dir);
	free(pm.root);
	return 0;
}
