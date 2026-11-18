/* libretroui menus: menu.conf parser and modal cascading popup menus. */
#include "retroui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_LEVELS 8

const char rui_default_menu[] =
	"# retro-de fallback menu (used when menu.conf is missing or empty)\n"
	"programs \"Programs\"\n"
	"applications \"Applications\"\n"
	"menu \"Settings\" {\n"
	"    item \"Style Manager\" retro-settings\n"
	"    item \"Wallpaper\" retro-wallpaper\n"
	"    item \"Display\" retro-display\n"
	"    item \"Screen Saver\" @screensaver\n"
	"    item \"Do Not Disturb (on/off)\" @dnd\n"
	"    item \"Keep Screen On (on/off)\" @awake\n"
	"    item \"Bluetooth\" retro-bt\n"
	"    item \"Network\" retro-net\n"
	"    item \"Edit Menu\" @terminal -e ${EDITOR:-vi} \"${RETRO_DE_CONFIG_DIR:-$HOME/.config/retro-de}/menu.conf\"\n"
	"}\n"
	"separator\n"
	"item \"Run...\" retro-run\n"
	"item \"About retro-de\" retro-about\n"
	"separator\n"
	"item \"Restart WM\" @restart\n"
	"item \"Lock Screen\" @lock\n"
	"item \"Log Out\" @logout\n";

/* ------------------------------------------------------------ menu trees */

RuiMenu *
rui_menu_new(void)
{
	return rui_calloc(1, sizeof(RuiMenu));
}

static RuiMenuItem *
menu_slot(RuiMenu *m)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 8;
		m->items = rui_realloc(m->items, (size_t)m->cap * sizeof *m->items);
	}
	memset(&m->items[m->n], 0, sizeof *m->items);
	return &m->items[m->n++];
}

RuiMenuItem *
rui_menu_add(RuiMenu *m, const char *label, const char *cmd, int id)
{
	RuiMenuItem *it = menu_slot(m);

	it->label = rui_strdup(label);
	it->cmd = cmd ? rui_strdup(cmd) : NULL;
	it->id = id;
	return it;
}

RuiMenuItem *
rui_menu_add_sub(RuiMenu *m, const char *label, RuiMenu *sub)
{
	RuiMenuItem *it = menu_slot(m);

	it->label = rui_strdup(label);
	it->sub = sub;
	return it;
}

void
rui_menu_add_sep(RuiMenu *m)
{
	RuiMenuItem *it = menu_slot(m);

	it->separator = 1;
}

void
rui_menu_free(RuiMenu *m)
{
	int i;

	if (!m)
		return;
	for (i = 0; i < m->n; i++) {
		free(m->items[i].label);
		free(m->items[i].cmd);
		rui_menu_free(m->items[i].sub);
	}
	free(m->items);
	free(m);
}

int
rui_menu_count(const RuiMenu *m)
{
	int i, n = 0;

	for (i = 0; i < m->n; i++) {
		if (m->items[i].sub)
			n += rui_menu_count(m->items[i].sub);
		else if (!m->items[i].separator)
			n++;
	}
	return n;
}

/* moves all items of src to the end of dst and frees src */
static void
menu_take(RuiMenu *dst, RuiMenu *src)
{
	int i;

	for (i = 0; i < src->n; i++) {
		RuiMenuItem *it = menu_slot(dst);

		*it = src->items[i];
	}
	free(src->items);
	free(src);
}

static const char *
skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* reads a "quoted label" or bare word; returns pointer past it or NULL */
static const char *
parse_label(const char *p, char *out, size_t n)
{
	size_t o = 0;

	p = skip_ws(p);
	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
				p++;
			if (o + 1 < n)
				out[o++] = *p;
			p++;
		}
		if (*p != '"')
			return NULL;
		p++;
	} else {
		while (*p && *p != ' ' && *p != '\t' && *p != '{') {
			if (o + 1 < n)
				out[o++] = *p;
			p++;
		}
	}
	out[o] = '\0';
	return o ? p : NULL;
}

RuiMenu *
rui_menu_parse(const char *text, const char *origin)
{
	RuiMenu *stack[MAX_LEVELS];
	int depth = 0, skipped = 0, lineno = 0;
	const char *p = text;

	stack[0] = rui_menu_new();
	while (*p) {
		char line[1024], label[256];
		size_t len = strcspn(p, "\n"), c = len < sizeof line - 1 ? len :
		    sizeof line - 1;
		const char *s, *rest;
		char *e;

		memcpy(line, p, c);
		line[c] = '\0';
		p += len;
		if (*p == '\n')
			p++;
		lineno++;
		e = line + strlen(line);
		while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
			*--e = '\0';
		s = skip_ws(line);
		if (!*s || *s == '#')
			continue;
		if (*s == '}') {
			if (skipped)
				skipped--;
			else if (depth > 0)
				depth--;
			else
				rui_warn("%s:%d: unmatched '}'", origin, lineno);
		} else if (!strncmp(s, "menu", 4) && (s[4] == ' ' || s[4] == '\t')) {
			rest = parse_label(s + 4, label, sizeof label);
			if (!rest || *skip_ws(rest) != '{') {
				rui_warn("%s:%d: expected: menu \"Label\" {", origin, lineno);
				continue;
			}
			if (skipped || depth + 1 >= MAX_LEVELS) {
				if (!skipped)
					rui_warn("%s:%d: menus nested too deeply", origin,
					    lineno);
				skipped++;
			} else {
				RuiMenu *sub = rui_menu_new();

				rui_menu_add_sub(stack[depth], label, sub);
				stack[++depth] = sub;
			}
		} else if (!strncmp(s, "item", 4) && (s[4] == ' ' || s[4] == '\t')) {
			rest = parse_label(s + 4, label, sizeof label);
			if (!rest || !*skip_ws(rest)) {
				rui_warn("%s:%d: expected: item \"Label\" command", origin,
				    lineno);
				continue;
			}
			if (!skipped)
				rui_menu_add(stack[depth], label, skip_ws(rest), 0);
		} else if (!strncmp(s, "applications", 12) && (!s[12] || s[12] == ' ' ||
		    s[12] == '\t')) {
			/* freedesktop applications, grouped by main category */
			RuiAppList *apps;
			RuiMenu *am;

			if (skipped || !rui_xdg_menus())
				continue;
			rest = parse_label(s + 12, label, sizeof label);
			apps = rui_apps_scan();
			am = rui_apps_menu(apps);
			rui_apps_free(apps);
			if (am->n == 0)
				rui_menu_free(am);
			else if (rest)
				rui_menu_add_sub(stack[depth], label, am);
			else
				menu_take(stack[depth], am);
		} else if (!strncmp(s, "programs", 8) && (!s[8] || s[8] == ' ' ||
		    s[8] == '\t')) {
			/* the user's programs folder: sub-folders are categories */
			RuiMenu *pm;

			if (skipped)
				continue;
			rest = parse_label(s + 8, label, sizeof label);
			pm = rui_programs_menu();
			if (rest)
				rui_menu_add_sub(stack[depth], label, pm);
			else
				menu_take(stack[depth], pm);
		} else if (!strcmp(s, "separator")) {
			if (!skipped)
				rui_menu_add_sep(stack[depth]);
		} else {
			rui_warn("%s:%d: unknown directive '%s'", origin, lineno, s);
		}
	}
	if (depth > 0 || skipped)
		rui_warn("%s: missing '}' at end of file", origin);
	return stack[0];
}

RuiMenu *
rui_menu_load(const char *path)
{
	RuiMenu *m = NULL;
	FILE *f = path ? fopen(path, "r") : NULL;

	if (f) {
		size_t cap = 4096, len = 0, r;
		char *buf = rui_malloc(cap);

		while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
			len += r;
			if (len + 1 >= cap) {
				cap *= 2;
				buf = rui_realloc(buf, cap);
			}
		}
		buf[len] = '\0';
		fclose(f);
		m = rui_menu_parse(buf, path);
		free(buf);
		if (m->n == 0) {
			rui_warn("%s: no entries, using built-in menu", path);
			rui_menu_free(m);
			m = NULL;
		}
	}
	if (!m)
		m = rui_menu_parse(rui_default_menu, "(built-in menu)");
	return m;
}

long long
rui_menu_stamp(const char *path)
{
	struct stat st;
	long long stamp = (rui_xdg_menus() ? rui_apps_stamp() : 0) +
	    rui_programs_stamp();

	if (path && stat(path, &st) == 0)
		stamp += (long long)st.st_mtime * 7919 + (long long)st.st_size;
	return stamp;
}

/* ----------------------------------------------------------- popup menus */

typedef struct Level {
	RuiMenu *m;
	Window win;
	RuiSurf *s;
	int x, y, w, h;
	int sel;
} Level;

typedef struct Popup {
	RuiApp *app;
	Level lv[MAX_LEVELS];
	int n;
} Popup;

#define MENU_BORDER 3
#define MENU_LPAD 20
#define MENU_RPAD 24

static int
item_h(RuiApp *a, const RuiMenuItem *it)
{
	return it->separator ? 8 : a->font->height + 6;
}

static void
level_draw(Popup *p, int idx)
{
	RuiApp *a = p->app;
	Level *l = &p->lv[idx];
	int i, y = MENU_BORDER;

	rui_fill(l->s, 0, 0, l->w, l->h, RUI_C_FACE);
	rui_bevel(l->s, 0, 0, l->w, l->h, RUI_RAISED);
	for (i = 0; i < l->m->n; i++) {
		const RuiMenuItem *it = &l->m->items[i];
		int h = item_h(a, it), fg = RUI_C_TEXT;
		RuiRect r;

		if (it->separator) {
			rui_fill(l->s, MENU_BORDER + 2, y + 3, l->w - 2 * MENU_BORDER - 4,
			    1, RUI_C_SHADOW);
			rui_fill(l->s, MENU_BORDER + 2, y + 4, l->w - 2 * MENU_BORDER - 4,
			    1, RUI_C_LIGHT);
			y += h;
			continue;
		}
		if (i == l->sel && !it->disabled && a->style == RUI_STYLE_CDE) {
			/* Motif: the item under the pointer is drawn raised */
			rui_bevel(l->s, MENU_BORDER, y, l->w - 2 * MENU_BORDER, h,
			    RUI_RAISED);
		} else if (i == l->sel && !it->disabled) {
			rui_fill(l->s, MENU_BORDER, y, l->w - 2 * MENU_BORDER, h,
			    RUI_C_SEL_BG);
			fg = RUI_C_SEL_FG;
		} else if (it->disabled) {
			fg = RUI_C_DISABLED;
		}
		r.x = MENU_BORDER + MENU_LPAD;
		r.y = y;
		r.w = l->w - 2 * MENU_BORDER - MENU_LPAD - 4;
		r.h = h;
		rui_text_fit(l->s, a->font, r, it->label, fg, RUI_LEFT);
		if (it->checked) {
			rui_fill(l->s, MENU_BORDER + 7, y + h / 2 - 3, 6, 6, fg);
		}
		if (it->sub)
			rui_arrow(l->s, l->w - MENU_BORDER - 10, y + h / 2, 4, 3, fg);
		y += h;
	}
	rui_surf_flush(l->s);
}

static int
item_y(Popup *p, int idx, int item)
{
	Level *l = &p->lv[idx];
	int i, y = MENU_BORDER;

	for (i = 0; i < item && i < l->m->n; i++)
		y += item_h(p->app, &l->m->items[i]);
	return y;
}

static void
level_open(Popup *p, RuiMenu *m, int x, int y, int flags)
{
	RuiApp *a = p->app;
	Level *l;
	int i, w = 60, h = 2 * MENU_BORDER;

	if (p->n >= MAX_LEVELS)
		return;
	l = &p->lv[p->n];
	for (i = 0; i < m->n; i++) {
		const RuiMenuItem *it = &m->items[i];

		h += item_h(a, it);
		if (!it->separator) {
			int tw = rui_text_width(a, a->font, it->label, -1);

			if (tw > w)
				w = tw;
		}
	}
	if (w > a->sw / 2)
		w = a->sw / 2;
	w += 2 * MENU_BORDER + MENU_LPAD + MENU_RPAD;
	if (flags & RUI_MENU_UPWARD)
		y -= h;
	if (x + w > a->sw)
		x = p->n > 0 ? p->lv[p->n - 1].x - w + MENU_BORDER : a->sw - w;
	if (x < 0)
		x = 0;
	if (y + h > a->sh)
		y = a->sh - h;
	if (y < 0)
		y = 0;
	l->m = m;
	l->x = x;
	l->y = y;
	l->w = w;
	l->h = h;
	l->sel = -1;
	l->win = rui_window_new(a, a->root, x, y, w, h, 1, ExposureMask |
	    ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
	l->s = rui_surf_new(a, l->win, w, h);
	p->n++;
	XMapRaised(a->dpy, l->win);
	level_draw(p, p->n - 1);
}

static void
levels_close(Popup *p, int keep)
{
	while (p->n > keep) {
		Level *l = &p->lv[--p->n];

		rui_surf_free(l->s);
		XDestroyWindow(p->app->dpy, l->win);
	}
}

static int
level_at(Popup *p, int xr, int yr)
{
	int i;

	for (i = p->n - 1; i >= 0; i--) {
		Level *l = &p->lv[i];

		if (xr >= l->x && yr >= l->y && xr < l->x + l->w && yr < l->y + l->h)
			return i;
	}
	return -1;
}

static int
item_at(Popup *p, int idx, int yr)
{
	Level *l = &p->lv[idx];
	int i, y = l->y + MENU_BORDER;

	for (i = 0; i < l->m->n; i++) {
		int h = item_h(p->app, &l->m->items[i]);

		if (yr >= y && yr < y + h)
			return l->m->items[i].separator ? -1 : i;
		y += h;
	}
	return -1;
}

static void
open_sub(Popup *p, int idx, int item)
{
	Level *l = &p->lv[idx];
	RuiMenuItem *it = &l->m->items[item];

	if (!it->sub || it->disabled || it->sub->n == 0)
		return;
	level_open(p, it->sub, l->x + l->w - MENU_BORDER,
	    l->y + item_y(p, idx, item) - MENU_BORDER, 0);
}

static void
select_item(Popup *p, int idx, int item, int open)
{
	Level *l = &p->lv[idx];

	if (item == l->sel && (p->n > idx + 1 || !open))
		return;
	levels_close(p, idx + 1);
	l->sel = item;
	level_draw(p, idx);
	if (open && item >= 0)
		open_sub(p, idx, item);
}

static int
next_selectable(const RuiMenu *m, int from, int dir)
{
	int i, idx = from;

	for (i = 0; i < m->n; i++) {
		idx += dir;
		if (idx < 0)
			idx = m->n - 1;
		if (idx >= m->n)
			idx = 0;
		if (!m->items[idx].separator && !m->items[idx].disabled)
			return idx;
	}
	return from;
}

int rui_menu_outside_x = -1, rui_menu_outside_y = -1;
long long rui_menu_outside_ms;

const RuiMenuItem *
rui_menu_popup(RuiApp *app, RuiMenu *m, int x, int y, int flags)
{
	Popup p;
	const RuiMenuItem *result = NULL;
	long long t0 = rui_now_ms();
	int done = 0, i, moved = 0, grabbed = 0, start_x = -1, start_y = -1;

	if (!m || m->n == 0)
		return NULL;
	memset(&p, 0, sizeof p);
	p.app = app;
	level_open(&p, m, x, y, flags);
	XSync(app->dpy, False);
	for (i = 0; i < 50 && !grabbed; i++) {
		if (XGrabPointer(app->dpy, p.lv[0].win, True, ButtonPressMask |
		    ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
		    GrabModeAsync, None, app->cur_arrow, CurrentTime) == GrabSuccess)
			grabbed = 1;
		else
			rui_wait(app, 10);
	}
	if (!grabbed) {
		levels_close(&p, 0);
		return NULL;
	}
	XGrabKeyboard(app->dpy, p.lv[0].win, True, GrabModeAsync, GrabModeAsync,
	    CurrentTime);

	while (!done) {
		XEvent ev;
		int lvl, it;

		if (rui_quit_requested())
			break;
		if (!XPending(app->dpy)) {
			rui_wait(app, 250);
			continue;
		}
		XNextEvent(app->dpy, &ev);
		switch (ev.type) {
		case Expose:
			for (i = 0; i < p.n; i++)
				if (ev.xexpose.window == p.lv[i].win)
					break;
			if (i < p.n) {
				if (ev.xexpose.count == 0)
					rui_surf_flush(p.lv[i].s);
			} else if (app->dispatch) {
				app->dispatch(&ev, app->dispatch_ud);
			}
			break;
		case MotionNotify:
			while (XCheckTypedEvent(app->dpy, MotionNotify, &ev))
				;
			if (start_x < 0) {
				start_x = ev.xmotion.x_root;
				start_y = ev.xmotion.y_root;
			} else if (abs(ev.xmotion.x_root - start_x) > 4 ||
			    abs(ev.xmotion.y_root - start_y) > 4) {
				moved = 1;
			}
			lvl = level_at(&p, ev.xmotion.x_root, ev.xmotion.y_root);
			if (lvl >= 0)
				select_item(&p, lvl, item_at(&p, lvl, ev.xmotion.y_root), 1);
			break;
		case ButtonPress:
			lvl = level_at(&p, ev.xbutton.x_root, ev.xbutton.y_root);
			if (lvl < 0) {
				rui_menu_outside_x = ev.xbutton.x_root;
				rui_menu_outside_y = ev.xbutton.y_root;
				rui_menu_outside_ms = rui_now_ms();
				done = 1;
			}
			else {
				moved = 1;
				select_item(&p, lvl, item_at(&p, lvl, ev.xbutton.y_root), 1);
			}
			break;
		case ButtonRelease:
			if (ev.xbutton.button > Button3)
				break;
			lvl = level_at(&p, ev.xbutton.x_root, ev.xbutton.y_root);
			if (lvl < 0)
				break;
			if (!moved && rui_now_ms() - t0 < 300)
				break;
			it = item_at(&p, lvl, ev.xbutton.y_root);
			if (it >= 0 && !p.lv[lvl].m->items[it].sub &&
			    !p.lv[lvl].m->items[it].disabled) {
				result = &p.lv[lvl].m->items[it];
				done = 1;
			}
			break;
		case KeyPress: {
			KeySym ks = XLookupKeysym(&ev.xkey, 0);
			Level *l = &p.lv[p.n - 1];

			lvl = p.n - 1;
			switch (ks) {
			case XK_Escape:
				if (p.n > 1)
					levels_close(&p, p.n - 1);
				else
					done = 1;
				break;
			case XK_Left:
				if (p.n > 1)
					levels_close(&p, p.n - 1);
				break;
			case XK_Up:
				select_item(&p, lvl, next_selectable(l->m, l->sel < 0 ?
				    0 : l->sel, -1), 0);
				break;
			case XK_Down:
				select_item(&p, lvl, next_selectable(l->m, l->sel, 1), 0);
				break;
			case XK_Right:
			case XK_Return:
			case XK_KP_Enter:
			case XK_space:
				if (l->sel < 0)
					break;
				if (l->m->items[l->sel].sub) {
					open_sub(&p, lvl, l->sel);
					if (p.n > lvl + 1)
						select_item(&p, p.n - 1,
						    next_selectable(p.lv[p.n - 1].m, -1, 1), 0);
				} else if (ks != XK_Right && !l->m->items[l->sel].disabled) {
					result = &l->m->items[l->sel];
					done = 1;
				}
				break;
			default:
				break;
			}
			break;
		}
		case KeyRelease:
			break;
		default:
			if (app->dispatch)
				app->dispatch(&ev, app->dispatch_ud);
			break;
		}
	}
	XUngrabKeyboard(app->dpy, CurrentTime);
	XUngrabPointer(app->dpy, CurrentTime);
	levels_close(&p, 0);
	XFlush(app->dpy);
	return result;
}

/* ------------------------------------------------------------ confirmation */

typedef struct Confirm {
	RuiApp *app;
	RuiSurf *s;
	const char *title, *text;
	RuiButton ok, cancel;
	int focus;                             /* 0 = OK, 1 = Cancel */
} Confirm;

static void
confirm_draw(Confirm *c)
{
	RuiApp *a = c->app;
	RuiSurf *s = c->s;
	int fh = a->font->height, th = a->bold->height + 6;
	RuiRect r;

	rui_fill(s, 0, 0, s->w, s->h, RUI_C_FACE);
	rui_bevel(s, 0, 0, s->w, s->h, RUI_RAISED);
	rui_fill(s, 3, 3, s->w - 6, th, RUI_C_TITLE_ACTIVE);
	r.x = 3;
	r.y = 3;
	r.w = s->w - 6;
	r.h = th;
	rui_text_fit(s, a->bold, r, c->title, RUI_C_TITLE_FG, RUI_CENTER);
	/* question mark "icon" */
	rui_fill(s, 16, th + 14, 34, 34, RUI_C_FIELD);
	rui_bevel(s, 16, th + 14, 34, 34, RUI_SUNKEN);
	r.x = 16;
	r.y = th + 14;
	r.w = 34;
	r.h = 34;
	rui_text_fit(s, a->bold, r, "?", RUI_C_SEL_BG, RUI_CENTER);
	r.x = 64;
	r.y = th + 14;
	r.w = s->w - 76;
	r.h = 34;
	rui_text_fit(s, a->font, r, c->text, RUI_C_TEXT, RUI_LEFT);
	rui_fill(s, 8, s->h - fh - 30, s->w - 16, 2, RUI_C_SHADOW);
	rui_fill(s, 8, s->h - fh - 29, s->w - 16, 1, RUI_C_LIGHT);
	rui_button_draw(s, &c->ok);
	rui_button_draw(s, &c->cancel);
	/* keyboard focus: dotted-looking frame inside the focused button */
	r = c->focus ? c->cancel.r : c->ok.r;
	rui_fill(s, r.x - 2, r.y - 2, r.w + 4, 1, RUI_C_DARK);
	rui_fill(s, r.x - 2, r.y + r.h + 1, r.w + 4, 1, RUI_C_DARK);
	rui_fill(s, r.x - 2, r.y - 2, 1, r.h + 4, RUI_C_DARK);
	rui_fill(s, r.x + r.w + 1, r.y - 2, 1, r.h + 4, RUI_C_DARK);
	rui_surf_flush(s);
}

int
rui_confirm(RuiApp *app, const char *title, const char *text,
    const char *ok_label)
{
	Confirm c;
	Window win;
	RuiMonitor mon;
	int fh = app->font->height, th = app->bold->height + 6, w, h, bw, i;
	int done = 0, result = 0, grabbed = 0;

	memset(&c, 0, sizeof c);
	c.app = app;
	c.title = title;
	c.text = text;
	w = rui_text_width(app, app->font, text, -1) + 90;
	if (w < 320)
		w = 320;
	if (w > app->sw - 40)
		w = app->sw - 40;
	h = th + 14 + 34 + 20 + fh + 30;
	bw = 96;
	c.ok.label = ok_label ? ok_label : "OK";
	c.cancel.label = "Cancel";
	c.ok.r.x = w / 2 - bw - 8;
	c.cancel.r.x = w / 2 + 8;
	c.ok.r.y = c.cancel.r.y = h - fh - 22;
	c.ok.r.w = c.cancel.r.w = bw;
	c.ok.r.h = c.cancel.r.h = fh + 12;
	c.ok.bold = 1;
	mon = rui_monitor_pick(app, "primary");    /* not across two screens */
	if (w > mon.w - 40)
		w = mon.w - 40;
	win = rui_window_new(app, app->root, mon.x + (mon.w - w) / 2,
	    mon.y + (mon.h - h) / 3, w, h, 1, ExposureMask | ButtonPressMask | ButtonReleaseMask |
	    PointerMotionMask | KeyPressMask);
	rui_set_title(app, win, title, "retro-confirm");
	c.s = rui_surf_new(app, win, w, h);
	XMapRaised(app->dpy, win);
	XSync(app->dpy, False);
	for (i = 0; i < 50 && !grabbed; i++) {
		if (XGrabKeyboard(app->dpy, win, True, GrabModeAsync, GrabModeAsync,
		    CurrentTime) == GrabSuccess)
			grabbed = 1;
		else
			rui_wait(app, 10);
	}
	XGrabPointer(app->dpy, win, True, ButtonPressMask | ButtonReleaseMask |
	    PointerMotionMask, GrabModeAsync, GrabModeAsync, None, app->cur_arrow,
	    CurrentTime);
	confirm_draw(&c);
	while (!done && grabbed) {
		XEvent ev;
		int r;

		if (rui_quit_requested())
			break;
		if (!XPending(app->dpy)) {
			rui_wait(app, 250);
			continue;
		}
		XNextEvent(app->dpy, &ev);
		/* keys count wherever they land: the asking program may itself
		 * have the focus, and then they arrive at its own window */
		if (ev.xany.window != win && ev.type != KeyPress) {
			if (ev.type != KeyPress && ev.type != ButtonPress &&
			    ev.type != ButtonRelease && ev.type != MotionNotify &&
			    app->dispatch)
				app->dispatch(&ev, app->dispatch_ud);
			continue;
		}
		switch (ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				rui_surf_flush(c.s);
			break;
		case KeyPress: {
			KeySym ks = XLookupKeysym(&ev.xkey, 0);

			if (ks == XK_Escape) {
				done = 1;
			} else if (ks == XK_Return || ks == XK_KP_Enter ||
			    ks == XK_space) {
				result = c.focus == 0;
				done = 1;
			} else if (ks == XK_Tab || ks == XK_Left || ks == XK_Right) {
				c.focus = !c.focus;
				confirm_draw(&c);
			}
			break;
		}
		case ButtonPress:
		case ButtonRelease:
		case MotionNotify:
			r = rui_button_event(&c.ok, &ev);
			if (r & RUI_EV_ACTIVATE) {
				result = 1;
				done = 1;
			}
			i = rui_button_event(&c.cancel, &ev);
			if (i & RUI_EV_ACTIVATE)
				done = 1;
			if ((r | i) && !done)
				confirm_draw(&c);
			break;
		default:
			break;
		}
	}
	XUngrabPointer(app->dpy, CurrentTime);
	XUngrabKeyboard(app->dpy, CurrentTime);
	rui_surf_free(c.s);
	XDestroyWindow(app->dpy, win);
	XFlush(app->dpy);
	return result;
}
