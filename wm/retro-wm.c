/*
 * retro-wm - re-parenting floating/tiling window manager with 90s looks.
 *
 * ICCCM + EWMH, virtual desktops, shade/maximize/minimize/sticky, Alt+Tab,
 * key bindings and per-workspace layouts (float, tile, monocle) chosen in
 * config.h like dwm, overridable in retro.conf.
 */
#include "../lib/retroui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/cursorfont.h>
#include <X11/extensions/dpms.h>

enum { T_NORMAL, T_DIALOG, T_DOCK, T_DESKTOP, T_SPLASH, T_UTILITY };
enum { L_DESKTOP, L_BELOW, L_NORMAL, L_ABOVE, L_FULL, L_COUNT };
enum { BTN_NONE, BTN_MENU, BTN_MIN, BTN_MAX, BTN_CLOSE };
enum { E_LEFT = 1, E_RIGHT = 2, E_TOP = 4, E_BOTTOM = 8 };
enum { MAX_VERT = 1, MAX_HORZ = 2, MAX_BOTH = 3 };
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

enum {
	ACT_NONE, ACT_CLOSE, ACT_MAXIMIZE, ACT_MINIMIZE, ACT_SHADE, ACT_STICKY,
	ACT_FULLSCREEN, ACT_ABOVE, ACT_CYCLE, ACT_CYCLE_BACK, ACT_WORKSPACE,
	ACT_SENDTO, ACT_NEXT_WS, ACT_PREV_WS, ACT_EXEC, ACT_TOGGLE_TILE,
	ACT_TOGGLE_FLOAT, ACT_MASTER_GROW, ACT_MASTER_SHRINK, ACT_SWAP_MASTER,
	ACT_LOWER, ACT_WINDOW_MENU, ACT_QUIT, ACT_RESTART, ACT_LAYOUT,
	ACT_NEXT_LAYOUT, ACT_SNAP, ACT_MOVE, ACT_RESIZE, ACT_CENTER,
	ACT_TO_MONITOR, ACT_DND, ACT_AWAKE
};
/* where a window is snapped: a half or a quarter of its monitor */
enum { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_TOP, SNAP_BOTTOM, SNAP_TL,
       SNAP_TR, SNAP_BL, SNAP_BR, SNAP_COUNT };
enum { PLACE_CASCADE, PLACE_CENTER, PLACE_POINTER, PLACE_SMART };
enum { LA_FLOAT, LA_TILE, LA_MONOCLE };
#define MAX_LAYOUTS 16

static const struct {
	const char *name;
	int action;
} action_names[] = {
	{ "close", ACT_CLOSE }, { "maximize", ACT_MAXIMIZE },
	{ "minimize", ACT_MINIMIZE }, { "shade", ACT_SHADE },
	{ "sticky", ACT_STICKY }, { "fullscreen", ACT_FULLSCREEN },
	{ "above", ACT_ABOVE }, { "cycle", ACT_CYCLE },
	{ "cycle_back", ACT_CYCLE_BACK }, { "workspace", ACT_WORKSPACE },
	{ "sendto", ACT_SENDTO }, { "next_workspace", ACT_NEXT_WS },
	{ "prev_workspace", ACT_PREV_WS }, { "exec", ACT_EXEC },
	{ "toggle_tile", ACT_TOGGLE_TILE }, { "toggle_float", ACT_TOGGLE_FLOAT },
	{ "master_grow", ACT_MASTER_GROW }, { "master_shrink", ACT_MASTER_SHRINK },
	{ "swap_master", ACT_SWAP_MASTER }, { "lower", ACT_LOWER },
	{ "window_menu", ACT_WINDOW_MENU }, { "quit", ACT_QUIT },
	{ "restart", ACT_RESTART }, { "layout", ACT_LAYOUT },
	{ "next_layout", ACT_NEXT_LAYOUT },
	/* snap left|right|top|bottom|topleft|topright|bottomleft|bottomright,
	 * move DX DY, resize DW DH, center, to_monitor next|prev|N,
	 * dnd [on|off], awake [on|off] */
	{ "snap", ACT_SNAP }, { "move", ACT_MOVE }, { "resize", ACT_RESIZE },
	{ "center", ACT_CENTER }, { "to_monitor", ACT_TO_MONITOR },
	{ "dnd", ACT_DND }, { "awake", ACT_AWAKE },
};

static void bus_event(const char *topic, const char *fmt, ...);

static const char *const snap_names[SNAP_COUNT] = {
	"", "left", "right", "top", "bottom", "topleft", "topright",
	"bottomleft", "bottomright"
};

/* rule = class=Firefox title=*Private* => workspace=2 maximized
 * The left side matches a new window (class, name, title, type; globs,
 * case-insensitive); the right side says how it starts. */
typedef struct Rule {
	char *cls, *name, *title, *type;
	int desk;                      /* 1..N, -1 = all, 0 = unset           */
	int monitor;                   /* 1..N, 0 = unset                     */
	int maximized, fullscreen, minimized, sticky, above, below, shaded;
	int floating;                  /* 1 float, -1 tile, 0 unset           */
	int border;                    /* 1 force, -1 none, 0 unset           */
	int skip_taskbar, skip_pager, nofocus, center, snap;
	int has_size, has_pos, gw, gh, gx, gy, gneg_x, gneg_y;
} Rule;

typedef struct Bind {
	unsigned mod;
	KeySym ks;
	int action, arg;
	char *cmd;
} Bind;

typedef struct Client {
	Window win, frame;
	RuiSurf *deco;                 /* back buffer for the title strip      */
	char *name;
	int x, y, w, h;                /* frame position, client size          */
	int sx, sy, sw, sh;            /* geometry before maximize             */
	int fx, fy, fw, fh;            /* geometry before fullscreen           */
	int tx, ty, tw, th;            /* geometry before tiling               */
	int old_bw;
	unsigned long desk;
	int type;
	int decorated, shaded, maxed, fullscreen, hidden, sticky, above, below;
	int skip_taskbar, skip_pager, urgent, modal, floating, tiled;
	int no_input, take_focus, can_delete;
	int snap;                      /* SNAP_*: half/quarter of the monitor  */
	int nofocus;                   /* rule: never focused when it opens    */
	int minw, minh, maxw, maxh, incw, inch, basew, baseh;
	Window transient;
	int strut[4];                  /* left right top bottom                */
	int cur_edges;
	Window iconwin;                /* CDE: iconified window on the desktop */
	RuiSurf *isurf;
	int islot;
} Client;

typedef struct Vec {
	Client **v;
	int n, cap;
} Vec;

static struct {
	RuiApp *app;
	Display *dpy;
	Window root, check;
	int running, restart, exiting;
	int ndesk;
	unsigned long cur;
	Vec clients, stack, mru;       /* map order, bottom->top, most recent  */
	Client *focused;
	RuiRect mons[16];
	int nmons, prim;               /* prim: index of the primary monitor   */
	RuiRect workarea;
	int bw, th;                    /* border width, title height           */
	int sloppy, dclick_shade;
	unsigned mousemod, numlock;
	Bind *binds;
	int nbinds;
	int layout[32], mfact[32];         /* per workspace                  */
	int algo[MAX_LAYOUTS], nlayouts;   /* config.h layouts -> algorithm  */
	Cursor curs[16], cur_move;
	Time last_time;
	/* title button being pressed */
	Client *btn_c;
	int btn, btn_in;
	/* double click detection */
	Window dc_win;
	Time dc_time;
	/* move / resize */
	struct {
		int mode, edges, px, py, ox, oy, ow, oh;
		Client *c;
	} drag;
	/* Alt+Tab */
	struct {
		int active, sel, n;
		unsigned mod;
		Client **cand;
		Window win;
		RuiSurf *s;
	} cyc;
	Rule *rules;
	int nrules;
	int snap_dist;                 /* edges stick within this many pixels */
	int edge_snap;                 /* drag to a screen edge: half / max   */
	int move_step;                 /* keyboard move/resize step           */
	int placement, focus_new;
	int zone;                      /* SNAP_* or -1 (maximize) while dragging */
	Window zone_win[4];            /* outline of where it would snap      */
	int dnd, awake;                /* do not disturb, keep the screen on  */
	int saver_secs, dpms_secs;     /* retro.conf timeouts                 */
	int xss;                       /* xscreensaver is the screen saver    */
	long long awake_poke;
	char *dnd_cmd[2];              /* run when DND goes off / on          */
	int motif;                     /* CDE style: Motif decorations       */
	int icons_on;                  /* minimized windows become icons     */
	int cascade;
	char **argv;
} wm;

#define A(n) (wm.app->atom[A_##n])
#define FRAME_MASK (SubstructureRedirectMask | SubstructureNotifyMask | \
	ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask | \
	EnterWindowMask)
#define CLEANMASK(m) ((m) & ~(wm.numlock | LockMask) & (ShiftMask | \
	ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask))

static void focus(Client *c);
static void arrange(void);
static void restack(void);
static void update_client_list(void);
static void handle_event(XEvent *ev);

/* ---------------------------------------------------------------- vectors */

static void
vec_push(Vec *v, Client *c)
{
	if (v->n == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 16;
		v->v = rui_realloc(v->v, (size_t)v->cap * sizeof *v->v);
	}
	v->v[v->n++] = c;
}

static void
vec_remove(Vec *v, Client *c)
{
	int i;

	for (i = 0; i < v->n; i++) {
		if (v->v[i] == c) {
			memmove(&v->v[i], &v->v[i + 1],
			    (size_t)(v->n - i - 1) * sizeof *v->v);
			v->n--;
			return;
		}
	}
}

static void
vec_front(Vec *v, Client *c)
{
	vec_remove(v, c);
	vec_push(v, c);
	memmove(&v->v[1], &v->v[0], (size_t)(v->n - 1) * sizeof *v->v);
	v->v[0] = c;
}

static Client *
client_from_win(Window w)
{
	int i;

	for (i = 0; i < wm.clients.n; i++)
		if (wm.clients.v[i]->win == w || wm.clients.v[i]->frame == w)
			return wm.clients.v[i];
	return NULL;
}

static int
client_alive(Client *c)
{
	int i;

	for (i = 0; i < wm.clients.n; i++)
		if (wm.clients.v[i] == c)
			return 1;
	return 0;
}

/* --------------------------------------------------------------- geometry */

static int
has_deco(const Client *c)
{
	return c->decorated && !c->fullscreen;
}

static int
bw_of(const Client *c)
{
	return has_deco(c) ? wm.bw : 0;
}

static int
th_of(const Client *c)
{
	return has_deco(c) ? wm.th : 0;
}

static int
frame_w(const Client *c)
{
	return c->w + 2 * bw_of(c);
}

static int
frame_h(const Client *c)
{
	if (c->shaded && has_deco(c))
		return th_of(c) + 2 * bw_of(c);
	return c->h + th_of(c) + 2 * bw_of(c);
}

static int
is_visible(const Client *c)
{
	return !c->hidden && (c->sticky || c->desk == wm.cur);
}

static int
focusable(const Client *c)
{
	return c->type != T_DOCK && c->type != T_DESKTOP;
}

static void
update_monitors(void)
{
	RuiMonitor m[16];
	int n = rui_monitors(wm.app, m, 16), i;

	wm.nmons = n;
	wm.prim = 0;
	for (i = 0; i < n; i++) {
		wm.mons[i].x = m[i].x;
		wm.mons[i].y = m[i].y;
		wm.mons[i].w = m[i].w;
		wm.mons[i].h = m[i].h;
		if (m[i].primary)
			wm.prim = i;
	}
}

static RuiRect
monitor_at(int x, int y)
{
	int i;

	for (i = 0; i < wm.nmons; i++)
		if (rui_in_rect(wm.mons[i], x, y))
			return wm.mons[i];
	return wm.mons[0];
}

static RuiRect
monitor_of(const Client *c)
{
	return monitor_at(c->x + frame_w(c) / 2, c->y + frame_h(c) / 2);
}

/* Usable area of one monitor: the monitor minus the struts of the panels
 * that are on it.  Struts are measured from the edges of the whole screen,
 * so with monitors of different sizes they are clipped per monitor - a
 * taskbar on one screen takes no room away from the others. */
static RuiRect
mon_area(RuiRect m)
{
	int x1 = m.x, y1 = m.y, x2 = m.x + m.w, y2 = m.y + m.h, i;
	RuiRect r;

	for (i = 0; i < wm.clients.n; i++) {
		const Client *c = wm.clients.v[i];
		int ox, oy;

		if (!c->strut[0] && !c->strut[1] && !c->strut[2] && !c->strut[3])
			continue;
		ox = c->x < m.x + m.w && c->x + c->w > m.x;    /* shares columns */
		oy = c->y < m.y + m.h && c->y + c->h > m.y;    /* shares rows */
		if (c->strut[0] && oy && c->strut[0] > x1)
			x1 = c->strut[0];
		if (c->strut[1] && oy && wm.app->sw - c->strut[1] < x2)
			x2 = wm.app->sw - c->strut[1];
		if (c->strut[2] && ox && c->strut[2] > y1)
			y1 = c->strut[2];
		if (c->strut[3] && ox && wm.app->sh - c->strut[3] < y2)
			y2 = wm.app->sh - c->strut[3];
	}
	r.x = x1;
	r.y = y1;
	r.w = x2 - x1;
	r.h = y2 - y1;
	if (r.w < 50 || r.h < 50)
		return m;
	return r;
}

/* usable area for a client: that of the monitor it is on */
static RuiRect
area_of(const Client *c)
{
	return mon_area(monitor_of(c));
}

static void
apply_size_hints(const Client *c, int *w, int *h)
{
	if (c->incw > 1)
		*w -= (*w - c->basew) % c->incw;
	if (c->inch > 1)
		*h -= (*h - c->baseh) % c->inch;
	if (c->maxw > 0 && *w > c->maxw)
		*w = c->maxw;
	if (c->maxh > 0 && *h > c->maxh)
		*h = c->maxh;
	if (*w < c->minw)
		*w = c->minw;
	if (*h < c->minh)
		*h = c->minh;
	if (*w < 1)
		*w = 1;
	if (*h < 1)
		*h = 1;
}

static int
fixed_size(const Client *c)
{
	return c->maxw > 0 && c->maxh > 0 && c->maxw == c->minw &&
	    c->maxh == c->minh;
}

/* ------------------------------------------------------------- decoration */

static RuiRect
btn_rect(const Client *c, int which)
{
	RuiRect r;
	int bs = wm.th - 5, fw = frame_w(c);
	int simple = c->type == T_DIALOG || c->transient != None;

	r.x = -1000;
	if (wm.motif) {
		/* Motif: menu button left; minimize and maximize right; no close */
		r.y = wm.bw;
		r.w = r.h = wm.th;
		if (which == BTN_MENU)
			r.x = wm.bw;
		else if (which == BTN_MAX && !simple)
			r.x = fw - wm.bw - wm.th;
		else if (which == BTN_MIN && !simple)
			r.x = fw - wm.bw - 2 * wm.th;
		return r;
	}
	r.y = wm.bw + 2;
	r.w = r.h = bs;
	switch (which) {
	case BTN_CLOSE:
		r.x = fw - wm.bw - 2 - bs;
		break;
	case BTN_MAX:
		if (!simple)
			r.x = fw - wm.bw - 4 - 2 * bs;
		break;
	case BTN_MIN:
		if (!simple)
			r.x = fw - wm.bw - 4 - 3 * bs;
		break;
	case BTN_MENU:
		r.x = wm.bw + 2;
		break;
	default:
		break;
	}
	return r;
}

static int
button_at(const Client *c, int x, int y)
{
	int b;

	if (!has_deco(c))
		return BTN_NONE;
	for (b = BTN_MENU; b <= BTN_CLOSE; b++)
		if (rui_in_rect(btn_rect(c, b), x, y))
			return b;
	return BTN_NONE;
}

static void
draw_button(Client *c, int which)
{
	RuiSurf *s = c->deco;
	RuiRect r = btn_rect(c, which);
	int down = wm.btn_c == c && wm.btn == which && wm.btn_in;
	int o = down ? 1 : 0, g = r.w - 8, i;
	int x = r.x + o, y = r.y + o;

	if (r.x < 0)
		return;
	rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_FACE);
	rui_bevel(s, r.x, r.y, r.w, r.h, down ? RUI_PRESSED : RUI_RAISED);
	switch (which) {
	case BTN_CLOSE:
		for (i = 0; i < g; i++) {
			rui_fill(s, x + 4 + i, y + 4 + i, 2, 1, RUI_C_TEXT);
			rui_fill(s, x + 4 + g - 1 - i, y + 4 + i, 2, 1, RUI_C_TEXT);
		}
		break;
	case BTN_MAX:
		if (c->maxed == MAX_BOTH) {
			rui_fill(s, x + 5, y + 3, r.w - 9, 2, RUI_C_TEXT);
			rui_fill(s, x + r.w - 5, y + 3, 1, r.h - 9, RUI_C_TEXT);
			rui_fill(s, x + 3, y + 6, r.w - 9, 2, RUI_C_TEXT);
			rui_fill(s, x + 3, y + 6, 1, r.h - 10, RUI_C_TEXT);
			rui_fill(s, x + r.w - 7, y + 6, 1, r.h - 10, RUI_C_TEXT);
			rui_fill(s, x + 3, y + r.h - 5, r.w - 9, 1, RUI_C_TEXT);
		} else {
			rui_fill(s, x + 3, y + 3, r.w - 7, 2, RUI_C_TEXT);
			rui_fill(s, x + 3, y + 3, 1, r.h - 7, RUI_C_TEXT);
			rui_fill(s, x + r.w - 5, y + 3, 1, r.h - 7, RUI_C_TEXT);
			rui_fill(s, x + 3, y + r.h - 5, r.w - 7, 1, RUI_C_TEXT);
		}
		break;
	case BTN_MIN:
		rui_fill(s, x + 4, y + r.h - 6, r.w - 10, 2, RUI_C_TEXT);
		break;
	case BTN_MENU:
		rui_fill(s, x + 3, y + r.h / 2 - 1, r.w - 7, 3, RUI_C_TEXT);
		rui_fill(s, x + 4, y + r.h / 2, r.w - 9, 1, RUI_C_LIGHT);
		break;
	default:
		break;
	}
}

static void
direct_fill(Window w, int x, int y, int wd, int ht, int color)
{
	if (wd <= 0 || ht <= 0)
		return;
	XSetForeground(wm.dpy, wm.app->gc, wm.app->color[color].pixel);
	XFillRectangle(wm.dpy, w, wm.app->gc, x, y, (unsigned)wd, (unsigned)ht);
}

/* one-pixel bevel lines straight onto a window */
static void
direct_bevel(Window w, int x, int y, int wd, int ht, int light, int shadow)
{
	direct_fill(w, x, y, wd, 1, light);
	direct_fill(w, x, y, 1, ht, light);
	direct_fill(w, x, y + ht - 1, wd, 1, shadow);
	direct_fill(w, x + wd - 1, y, 1, ht, shadow);
}

static void
motif_button(Client *c, int which, int face, int lt, int sh)
{
	RuiSurf *s = c->deco;
	RuiRect r = btn_rect(c, which);
	int down = wm.btn_c == c && wm.btn == which && wm.btn_in;
	int cx, cy, k;

	if (r.x < 0)
		return;
	rui_fill(s, r.x, r.y, r.w, r.h, face);
	rui_bevel_c(s, r.x, r.y, r.w, r.h, !down, lt, sh, 1);
	cx = r.x + r.w / 2;
	cy = r.y + r.h / 2;
	switch (which) {
	case BTN_MENU:                         /* horizontal bar */
		k = r.w / 2;
		rui_bevel_c(s, cx - k / 2, cy - 2, k, 4, !down, lt, sh, 1);
		break;
	case BTN_MIN:                          /* small square */
		rui_bevel_c(s, cx - 2, cy - 2, 5, 5, !down, lt, sh, 1);
		break;
	case BTN_MAX:                          /* large square */
		k = r.w - 8;
		if (c->maxed == MAX_BOTH)
			k -= 4;
		rui_bevel_c(s, cx - k / 2, cy - k / 2, k, k, !down, lt, sh, 1);
		break;
	default:
		break;
	}
}

/* Motif/CDE frame: colour follows focus, separate title box and buttons,
 * resize handles marked off at the corners */
static void
draw_frame_motif(Client *c)
{
	RuiSurf *s = c->deco;
	int fw = frame_w(c), fh = frame_h(c), b = wm.bw, t = wm.th;
	int act = c == wm.focused, top = b + t, cs = b + t;
	int face = act ? RUI_C_TITLE_ACTIVE : c->urgent ? RUI_C_ALERT :
	    RUI_C_TITLE_INACTIVE;
	int lt = act ? RUI_C_ACT_LIGHT : RUI_C_INACT_LIGHT;
	int sh = act ? RUI_C_ACT_SHADOW : RUI_C_INACT_SHADOW;
	int fg = act || c->urgent ? RUI_C_TITLE_FG : RUI_C_TITLE_FG_INACTIVE;
	RuiRect mu = btn_rect(c, BTN_MENU), mn = btn_rect(c, BTN_MIN), r;

	rui_fill(s, 0, 0, fw, top, face);
	rui_fill(s, 0, 0, fw, 1, lt);
	rui_fill(s, 0, 0, 1, top, lt);
	rui_fill(s, fw - 1, 0, 1, top, sh);
	/* corner handle marks on the top border */
	rui_fill(s, cs, 1, 1, b - 1, sh);
	rui_fill(s, cs + 1, 1, 1, b - 1, lt);
	rui_fill(s, fw - cs - 1, 1, 1, b - 1, sh);
	rui_fill(s, fw - cs, 1, 1, b - 1, lt);
	motif_button(c, BTN_MENU, face, lt, sh);
	motif_button(c, BTN_MIN, face, lt, sh);
	motif_button(c, BTN_MAX, face, lt, sh);
	r.x = mu.x + mu.w;
	r.y = b;
	r.w = (mn.x >= 0 ? mn.x : fw - b) - r.x;
	r.h = t;
	rui_bevel_c(s, r.x, r.y, r.w, r.h, 1, lt, sh, 1);
	r.x += 6;
	r.w -= 12;
	if (c->name)
		rui_text_fit(s, wm.app->bold, r, c->name, fg, RUI_CENTER);
	if (c->sticky && r.w > 40)
		rui_fill(s, r.x + r.w - 4, b + t / 2 - 2, 4, 4, fg);
	rui_surf_flush(s);

	/* sides and bottom go straight onto the frame window */
	direct_fill(c->frame, 0, top, b, fh - top, face);
	direct_fill(c->frame, fw - b, top, b, fh - top, face);
	direct_fill(c->frame, 0, fh - b, fw, b, face);
	direct_fill(c->frame, 0, top, 1, fh - top, lt);
	direct_fill(c->frame, fw - 1, top, 1, fh - top, sh);
	direct_fill(c->frame, 0, fh - 1, fw, 1, sh);
	if (!c->shaded) {
		/* sunken edge around the client */
		direct_fill(c->frame, b - 1, top, 1, fh - top - b + 1, sh);
		direct_fill(c->frame, fw - b, top, 1, fh - top - b + 1, lt);
		direct_fill(c->frame, b - 1, fh - b, fw - 2 * b + 2, 1, lt);
		/* corner handle marks on the sides and the bottom */
		if (fh > 2 * cs) {
			direct_bevel(c->frame, 1, fh - cs, b - 2, 2, sh, lt);
			direct_bevel(c->frame, fw - b + 1, fh - cs, b - 2, 2, sh, lt);
			direct_bevel(c->frame, 1, cs, b - 2, 2, sh, lt);
			direct_bevel(c->frame, fw - b + 1, cs, b - 2, 2, sh, lt);
		}
		direct_fill(c->frame, cs, fh - b + 1, 1, b - 2, sh);
		direct_fill(c->frame, cs + 1, fh - b + 1, 1, b - 2, lt);
		direct_fill(c->frame, fw - cs - 1, fh - b + 1, 1, b - 2, sh);
		direct_fill(c->frame, fw - cs, fh - b + 1, 1, b - 2, lt);
	}
}

static void
draw_frame(Client *c)
{
	RuiSurf *s = c->deco;
	int fw, fh, b = wm.bw, t = wm.th, active = c == wm.focused, bg, fg;
	RuiRect r, left, right;

	if (!has_deco(c))
		return;
	if (wm.motif) {
		draw_frame_motif(c);
		return;
	}
	fw = frame_w(c);
	fh = frame_h(c);
	rui_fill(s, 0, 0, fw, b + t, RUI_C_FACE);
	rui_bevel(s, 0, 0, fw, fh, RUI_RAISED);
	bg = active ? RUI_C_TITLE_ACTIVE : c->urgent ? RUI_C_ALERT :
	    RUI_C_TITLE_INACTIVE;
	fg = active || c->urgent ? RUI_C_TITLE_FG : RUI_C_TITLE_FG_INACTIVE;
	rui_fill(s, b, b, fw - 2 * b, t - 1, bg);
	draw_button(c, BTN_MENU);
	draw_button(c, BTN_MIN);
	draw_button(c, BTN_MAX);
	draw_button(c, BTN_CLOSE);
	left = btn_rect(c, BTN_MENU);
	right = btn_rect(c, BTN_MIN);
	if (right.x < 0)
		right = btn_rect(c, BTN_CLOSE);
	r.x = left.x + left.w + 5;
	r.y = b;
	r.w = right.x - 5 - r.x;
	r.h = t - 1;
	if (c->name)
		rui_text_fit(s, wm.app->bold, r, c->name, fg, RUI_LEFT);
	if (c->sticky && r.w > 40)
		rui_fill(s, right.x - 9, b + t / 2 - 2, 4, 4, fg);
	rui_surf_flush(s);
	/* the rest of the border is plain lines on the face-coloured frame */
	direct_fill(c->frame, 0, b + t, 1, fh - b - t, RUI_C_LIGHT);
	direct_fill(c->frame, fw - 1, b + t, 1, fh - b - t, RUI_C_DARK);
	direct_fill(c->frame, fw - 2, b + t, 1, fh - b - t - 1, RUI_C_SHADOW);
	direct_fill(c->frame, 0, fh - 1, fw, 1, RUI_C_DARK);
	direct_fill(c->frame, 1, fh - 2, fw - 2, 1, RUI_C_SHADOW);
}

static int
edges_at(const Client *c, int x, int y)
{
	int e = 0, fw = frame_w(c), fh = frame_h(c), b = wm.bw;
	int corner = wm.motif ? wm.bw + wm.th : 18;

	if (!has_deco(c) || c->maxed == MAX_BOTH || fixed_size(c) || c->tiled)
		return 0;
	if (x < b)
		e |= E_LEFT;
	else if (x >= fw - b)
		e |= E_RIGHT;
	if (y < b)
		e |= E_TOP;
	else if (y >= fh - b)
		e |= E_BOTTOM;
	if (!e)
		return 0;
	if (e & (E_TOP | E_BOTTOM)) {
		if (x < corner)
			e |= E_LEFT;
		else if (x >= fw - corner)
			e |= E_RIGHT;
	}
	if (e & (E_LEFT | E_RIGHT)) {
		if (y < corner)
			e |= E_TOP;
		else if (y >= fh - corner)
			e |= E_BOTTOM;
	}
	if (c->shaded)
		e &= ~(E_TOP | E_BOTTOM);
	return e;
}

/* --------------------------------------------------------- ICCCM / EWMH */

static void
send_configure(Client *c)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xconfigure.type = ConfigureNotify;
	ev.xconfigure.display = wm.dpy;
	ev.xconfigure.event = c->win;
	ev.xconfigure.window = c->win;
	ev.xconfigure.x = c->x + bw_of(c);
	ev.xconfigure.y = c->y + bw_of(c) + th_of(c);
	ev.xconfigure.width = c->w;
	ev.xconfigure.height = c->h;
	ev.xconfigure.border_width = 0;
	ev.xconfigure.above = None;
	ev.xconfigure.override_redirect = False;
	XSendEvent(wm.dpy, c->win, False, StructureNotifyMask, &ev);
}

static void
update_frame_extents(Client *c)
{
	unsigned long v[4];

	v[0] = v[1] = v[3] = (unsigned long)bw_of(c);
	v[2] = (unsigned long)(bw_of(c) + th_of(c));
	rui_set_prop32(wm.app, c->win, A(_NET_FRAME_EXTENTS), XA_CARDINAL, v, 4);
}

static void
apply_geometry(Client *c)
{
	int fw = frame_w(c), fh = frame_h(c);

	XMoveResizeWindow(wm.dpy, c->frame, c->x, c->y, (unsigned)fw, (unsigned)fh);
	XMoveResizeWindow(wm.dpy, c->win, bw_of(c), bw_of(c) + th_of(c),
	    (unsigned)c->w, (unsigned)c->h);
	rui_surf_resize(c->deco, fw, has_deco(c) ? wm.bw + wm.th : 1);
	update_frame_extents(c);
	send_configure(c);
	draw_frame(c);
}

static void
set_wm_state(Client *c, long state)
{
	unsigned long v[2];

	v[0] = (unsigned long)state;
	v[1] = None;
	rui_set_prop32(wm.app, c->win, A(WM_STATE), A(WM_STATE), v, 2);
}

static void
update_net_state(Client *c)
{
	unsigned long v[16];
	int n = 0;

	if (c->modal)
		v[n++] = A(_NET_WM_STATE_MODAL);
	if (c->sticky)
		v[n++] = A(_NET_WM_STATE_STICKY);
	if (c->maxed & MAX_VERT)
		v[n++] = A(_NET_WM_STATE_MAXIMIZED_VERT);
	if (c->maxed & MAX_HORZ)
		v[n++] = A(_NET_WM_STATE_MAXIMIZED_HORZ);
	if (c->shaded)
		v[n++] = A(_NET_WM_STATE_SHADED);
	if (c->skip_taskbar)
		v[n++] = A(_NET_WM_STATE_SKIP_TASKBAR);
	if (c->skip_pager)
		v[n++] = A(_NET_WM_STATE_SKIP_PAGER);
	if (c->hidden)
		v[n++] = A(_NET_WM_STATE_HIDDEN);
	if (c->fullscreen)
		v[n++] = A(_NET_WM_STATE_FULLSCREEN);
	if (c->above)
		v[n++] = A(_NET_WM_STATE_ABOVE);
	if (c->below)
		v[n++] = A(_NET_WM_STATE_BELOW);
	if (c->urgent)
		v[n++] = A(_NET_WM_STATE_DEMANDS_ATTENTION);
	if (c == wm.focused)
		v[n++] = A(_NET_WM_STATE_FOCUSED);
	rui_set_prop32(wm.app, c->win, A(_NET_WM_STATE), XA_ATOM, v, n);
}

static void
update_desktop_prop(Client *c)
{
	unsigned long v = c->sticky ? 0xffffffffUL : c->desk;

	rui_set_prop32(wm.app, c->win, A(_NET_WM_DESKTOP), XA_CARDINAL, &v, 1);
}

static void
update_allowed(Client *c)
{
	unsigned long v[12] = { 0 };
	int n = 0;

	if (c->type == T_DOCK || c->type == T_DESKTOP) {
		rui_set_prop32(wm.app, c->win, A(_NET_WM_ALLOWED_ACTIONS), XA_ATOM, v,
		    0);
		return;
	}
	v[n++] = A(_NET_WM_ACTION_MOVE);
	v[n++] = A(_NET_WM_ACTION_MINIMIZE);
	v[n++] = A(_NET_WM_ACTION_SHADE);
	v[n++] = A(_NET_WM_ACTION_STICK);
	v[n++] = A(_NET_WM_ACTION_CHANGE_DESKTOP);
	v[n++] = A(_NET_WM_ACTION_CLOSE);
	v[n++] = A(_NET_WM_ACTION_FULLSCREEN);
	if (!fixed_size(c)) {
		v[n++] = A(_NET_WM_ACTION_RESIZE);
		v[n++] = A(_NET_WM_ACTION_MAXIMIZE_HORZ);
		v[n++] = A(_NET_WM_ACTION_MAXIMIZE_VERT);
	}
	rui_set_prop32(wm.app, c->win, A(_NET_WM_ALLOWED_ACTIONS), XA_ATOM, v, n);
}

static void
send_protocol(Client *c, Atom proto)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = c->win;
	ev.xclient.message_type = A(WM_PROTOCOLS);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)proto;
	ev.xclient.data.l[1] = (long)wm.last_time;
	XSendEvent(wm.dpy, c->win, False, NoEventMask, &ev);
}

static void
fetch_name(Client *c)
{
	free(c->name);
	c->name = rui_window_title(wm.app, c->win);
}

static void
fetch_size_hints(Client *c)
{
	XSizeHints h;
	long supplied;

	c->minw = c->minh = c->maxw = c->maxh = c->basew = c->baseh = 0;
	c->incw = c->inch = 1;
	memset(&h, 0, sizeof h);
	if (!XGetWMNormalHints(wm.dpy, c->win, &h, &supplied))
		return;
	if (h.flags & PMinSize) {
		c->minw = h.min_width;
		c->minh = h.min_height;
	}
	if (h.flags & PBaseSize) {
		c->basew = h.base_width;
		c->baseh = h.base_height;
		if (!(h.flags & PMinSize)) {
			c->minw = c->basew;
			c->minh = c->baseh;
		}
	} else {
		c->basew = c->minw;
		c->baseh = c->minh;
	}
	if (h.flags & PMaxSize) {
		c->maxw = h.max_width;
		c->maxh = h.max_height;
	}
	if (h.flags & PResizeInc) {
		c->incw = h.width_inc > 0 ? h.width_inc : 1;
		c->inch = h.height_inc > 0 ? h.height_inc : 1;
	}
}

static void
fetch_wm_hints(Client *c, int *iconic)
{
	XWMHints *h = XGetWMHints(wm.dpy, c->win);

	c->no_input = 0;
	if (!h)
		return;
	if ((h->flags & InputHint) && !h->input)
		c->no_input = 1;
	c->urgent = (h->flags & XUrgencyHint) && c != wm.focused && !wm.dnd;
	if (iconic && (h->flags & StateHint) && h->initial_state == IconicState)
		*iconic = 1;
	XFree(h);
}

static void
fetch_protocols(Client *c)
{
	Atom *protos = NULL;
	int n = 0, i;

	c->take_focus = c->can_delete = 0;
	if (!XGetWMProtocols(wm.dpy, c->win, &protos, &n))
		return;
	for (i = 0; i < n; i++) {
		if (protos[i] == A(WM_TAKE_FOCUS))
			c->take_focus = 1;
		else if (protos[i] == A(WM_DELETE_WINDOW))
			c->can_delete = 1;
	}
	if (protos)
		XFree(protos);
}

static void
fetch_transient(Client *c)
{
	Window t = None;

	if (XGetTransientForHint(wm.dpy, c->win, &t) && t != c->win)
		c->transient = t;
	else
		c->transient = None;
}

static void
fetch_type(Client *c)
{
	unsigned long *v;
	int n = rui_get_prop32(wm.app, c->win, A(_NET_WM_WINDOW_TYPE), &v), i;

	c->type = c->transient ? T_DIALOG : T_NORMAL;
	for (i = 0; i < n; i++) {
		if (v[i] == A(_NET_WM_WINDOW_TYPE_DOCK))
			c->type = T_DOCK;
		else if (v[i] == A(_NET_WM_WINDOW_TYPE_DESKTOP))
			c->type = T_DESKTOP;
		else if (v[i] == A(_NET_WM_WINDOW_TYPE_DIALOG))
			c->type = T_DIALOG;
		else if (v[i] == A(_NET_WM_WINDOW_TYPE_SPLASH))
			c->type = T_SPLASH;
		else if (v[i] == A(_NET_WM_WINDOW_TYPE_UTILITY) ||
		    v[i] == A(_NET_WM_WINDOW_TYPE_TOOLBAR) ||
		    v[i] == A(_NET_WM_WINDOW_TYPE_MENU))
			c->type = T_UTILITY;
		else if (v[i] == A(_NET_WM_WINDOW_TYPE_NORMAL))
			c->type = T_NORMAL;
		else
			continue;
		break;
	}
	free(v);
}

static void
fetch_motif(Client *c)
{
	unsigned long *v;
	int n = rui_get_prop32(wm.app, c->win, A(_MOTIF_WM_HINTS), &v);

	c->decorated = !(c->type == T_DOCK || c->type == T_DESKTOP ||
	    c->type == T_SPLASH);
	/* flags bit 1: decorations field valid; decorations == 0: none */
	if (n >= 3 && (v[0] & 2) && v[2] == 0)
		c->decorated = 0;
	free(v);
}

static int
fetch_strut(Client *c)
{
	unsigned long *v;
	int n, i, changed = 0, s[4] = { 0, 0, 0, 0 };

	n = rui_get_prop32(wm.app, c->win, A(_NET_WM_STRUT_PARTIAL), &v);
	if (n < 4) {
		free(v);
		n = rui_get_prop32(wm.app, c->win, A(_NET_WM_STRUT), &v);
	}
	for (i = 0; i < 4 && i < n; i++)
		s[i] = (int)v[i];
	free(v);
	for (i = 0; i < 4; i++) {
		if (s[i] != c->strut[i])
			changed = 1;
		c->strut[i] = s[i];
	}
	return changed;
}

static void
update_workarea(void)
{
	int l = 0, r = 0, t = 0, b = 0, i;
	unsigned long *v;
	RuiRect old = wm.workarea;

	for (i = 0; i < wm.clients.n; i++) {
		Client *c = wm.clients.v[i];

		if (c->strut[0] > l)
			l = c->strut[0];
		if (c->strut[1] > r)
			r = c->strut[1];
		if (c->strut[2] > t)
			t = c->strut[2];
		if (c->strut[3] > b)
			b = c->strut[3];
	}
	if (l + r > wm.app->sw - 100)
		l = r = 0;
	if (t + b > wm.app->sh - 100)
		t = b = 0;
	wm.workarea.x = l;
	wm.workarea.y = t;
	wm.workarea.w = wm.app->sw - l - r;
	wm.workarea.h = wm.app->sh - t - b;
	/* several monitors: the work area (desktop icons, tiling, minimized
	 * icons) is the primary monitor's */
	if (wm.nmons > 1)
		wm.workarea = mon_area(wm.mons[wm.prim]);
	v = rui_malloc((size_t)wm.ndesk * 4 * sizeof *v);
	for (i = 0; i < wm.ndesk; i++) {
		v[i * 4] = (unsigned long)wm.workarea.x;
		v[i * 4 + 1] = (unsigned long)wm.workarea.y;
		v[i * 4 + 2] = (unsigned long)wm.workarea.w;
		v[i * 4 + 3] = (unsigned long)wm.workarea.h;
	}
	rui_set_prop32(wm.app, wm.root, A(_NET_WORKAREA), XA_CARDINAL, v,
	    wm.ndesk * 4);
	free(v);
	if (old.x == wm.workarea.x && old.y == wm.workarea.y &&
	    old.w == wm.workarea.w && old.h == wm.workarea.h)
		return;
	/* re-fit maximized windows and the tiled layout */
	for (i = 0; i < wm.clients.n; i++) {
		Client *c = wm.clients.v[i];

		if (c->maxed && !c->fullscreen && !c->tiled) {
			RuiRect a = area_of(c);

			if (c->maxed & MAX_HORZ) {
				c->x = a.x;
				c->w = a.w - 2 * bw_of(c);
			}
			if (c->maxed & MAX_VERT) {
				c->y = a.y;
				c->h = a.h - th_of(c) - 2 * bw_of(c);
			}
			apply_geometry(c);
		}
	}
	arrange();
}

static void
update_client_list(void)
{
	unsigned long *v = rui_malloc((size_t)(wm.clients.n + 1) * sizeof *v);
	int i;

	for (i = 0; i < wm.clients.n; i++)
		v[i] = wm.clients.v[i]->win;
	rui_set_prop32(wm.app, wm.root, A(_NET_CLIENT_LIST), XA_WINDOW, v,
	    wm.clients.n);
	free(v);
}

/* ---------------------------------------------------------------- stacking */

static int
base_layer(const Client *c)
{
	if (c->type == T_DESKTOP)
		return L_DESKTOP;
	if (c->below)
		return L_BELOW;
	if (c->fullscreen && (wm.focused == c || (wm.focused &&
	    wm.focused->transient == c->win)))
		return L_FULL;
	if (c->type == T_DOCK || c->above)
		return L_ABOVE;
	return L_NORMAL;
}

static int
layer_of(const Client *c)
{
	int l = base_layer(c);

	if (c->transient) {
		Client *p = client_from_win(c->transient);

		if (p && p != c && base_layer(p) > l)
			l = base_layer(p);
	}
	return l;
}

static void
restack(void)
{
	unsigned long *list;
	Window prev = None;
	int layer, i, n = 0;

	if (wm.stack.n == 0) {
		XDeleteProperty(wm.dpy, wm.root, A(_NET_CLIENT_LIST_STACKING));
		return;
	}
	list = rui_malloc((size_t)wm.stack.n * sizeof *list);
	for (layer = 0; layer < L_COUNT; layer++) {
		for (i = 0; i < wm.stack.n; i++) {
			Client *c = wm.stack.v[i];
			XWindowChanges wc;

			if (layer_of(c) != layer)
				continue;
			if (prev == None) {
				XLowerWindow(wm.dpy, c->frame);
			} else {
				wc.sibling = prev;
				wc.stack_mode = Above;
				XConfigureWindow(wm.dpy, c->frame, CWSibling | CWStackMode,
				    &wc);
			}
			prev = c->frame;
			list[n++] = c->win;
		}
		/* desktop icons sit just above the desktop window */
		for (i = 0; layer == L_DESKTOP && i < wm.clients.n; i++) {
			Client *c = wm.clients.v[i];
			XWindowChanges wc;

			if (!c->iconwin)
				continue;
			if (prev == None) {
				XLowerWindow(wm.dpy, c->iconwin);
			} else {
				wc.sibling = prev;
				wc.stack_mode = Above;
				XConfigureWindow(wm.dpy, c->iconwin, CWSibling | CWStackMode,
				    &wc);
			}
			prev = c->iconwin;
		}
	}
	rui_set_prop32(wm.app, wm.root, A(_NET_CLIENT_LIST_STACKING), XA_WINDOW,
	    list, n);
	free(list);
}

static void
raise_client(Client *c)
{
	int i, n = wm.stack.n;

	vec_remove(&wm.stack, c);
	vec_push(&wm.stack, c);
	/* keep transients above their parent */
	for (i = 0; i < n - 1; i++) {
		Client *t = wm.stack.v[i];

		if (t != c && t->transient == c->win) {
			vec_remove(&wm.stack, t);
			vec_push(&wm.stack, t);
			i--;
			n--;
		}
	}
	restack();
}

static void
lower_client(Client *c)
{
	vec_remove(&wm.stack, c);
	vec_push(&wm.stack, c);
	memmove(&wm.stack.v[1], &wm.stack.v[0],
	    (size_t)(wm.stack.n - 1) * sizeof *wm.stack.v);
	wm.stack.v[0] = c;
	restack();
}

/* ------------------------------------------------------------------ focus */

static void
set_active_prop(void)
{
	unsigned long v = wm.focused ? wm.focused->win : None;
	static unsigned long last = (unsigned long)-1;

	rui_set_prop32(wm.app, wm.root, A(_NET_ACTIVE_WINDOW), XA_WINDOW, &v, 1);
	if (v != last && v != None && wm.focused)
		bus_event("wm.focus", "0x%lx\t%s", v,
		    wm.focused->name ? wm.focused->name : "");
	last = v;
}

static void
focus(Client *c)
{
	Client *old = wm.focused;
	int i, any_full = 0;

	if (c && (!is_visible(c) || !focusable(c)))
		c = NULL;
	wm.focused = c;
	if (old && old != c && client_alive(old)) {
		draw_frame(old);
		update_net_state(old);
	}
	if (c) {
		vec_front(&wm.mru, c);
		if (!c->no_input)
			XSetInputFocus(wm.dpy, c->win, RevertToPointerRoot, CurrentTime);
		else
			XSetInputFocus(wm.dpy, wm.root, RevertToPointerRoot, CurrentTime);
		if (c->take_focus)
			send_protocol(c, A(WM_TAKE_FOCUS));
		c->urgent = 0;
		draw_frame(c);
		update_net_state(c);
	} else {
		XSetInputFocus(wm.dpy, wm.root, RevertToPointerRoot, CurrentTime);
	}
	set_active_prop();
	for (i = 0; i < wm.clients.n; i++)
		if (wm.clients.v[i]->fullscreen)
			any_full = 1;
	if (any_full && old != c)
		restack();
}

static void
focus_top(void)
{
	int i;

	for (i = 0; i < wm.mru.n; i++) {
		Client *c = wm.mru.v[i];

		if (is_visible(c) && focusable(c) && !c->skip_taskbar) {
			focus(c);
			return;
		}
	}
	for (i = 0; i < wm.mru.n; i++) {
		Client *c = wm.mru.v[i];

		if (is_visible(c) && focusable(c)) {
			focus(c);
			return;
		}
	}
	focus(NULL);
}

/* ---------------------------------------------------------- desktop icons */

#define ICON_W 80
#define ICON_PIC 36

static int
icon_h(void)
{
	return ICON_PIC + 6 + wm.app->font->height + 6;
}

static void
icon_draw(Client *c)
{
	RuiSurf *s = c->isurf;
	int h = icon_h(), px = (ICON_W - ICON_PIC) / 2;
	int focused = wm.focused == c;
	RuiRect r;

	if (!s)
		return;
	rui_fill(s, 0, 0, ICON_W, h, RUI_C_DESKTOP);
	/* picture: a little window in a raised square */
	rui_fill(s, px, 0, ICON_PIC, ICON_PIC, RUI_C_FACE);
	rui_bevel(s, px, 0, ICON_PIC, ICON_PIC, RUI_RAISED);
	rui_fill(s, px + 6, 7, ICON_PIC - 12, ICON_PIC - 14, RUI_C_DARK);
	rui_fill(s, px + 7, 8, ICON_PIC - 14, 5, focused ? RUI_C_TITLE_ACTIVE :
	    RUI_C_TITLE_INACTIVE);
	rui_fill(s, px + 7, 14, ICON_PIC - 14, ICON_PIC - 22, RUI_C_FIELD);
	/* label */
	r.x = 0;
	r.y = ICON_PIC + 4;
	r.w = ICON_W;
	r.h = h - ICON_PIC - 4;
	rui_fill(s, r.x, r.y, r.w, r.h, focused ? RUI_C_SEL_BG : RUI_C_FACE);
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_RAISED_THIN);
	r.x += 3;
	r.w -= 6;
	rui_text_fit(s, wm.app->font, r, c->name ? c->name : "",
	    focused ? RUI_C_SEL_FG : RUI_C_TEXT, RUI_CENTER);
	rui_surf_flush(s);
}

static void
icon_move(Client *c)
{
	RuiRect a = wm.workarea;
	int h = icon_h(), rows = (a.h - 8) / (h + 8), col, row;

	if (rows < 1)
		rows = 1;
	col = c->islot / rows;
	row = c->islot % rows;
	XMoveWindow(wm.dpy, c->iconwin, a.x + a.w - (col + 1) * (ICON_W + 8),
	    a.y + 8 + row * (h + 8));
}

/* create, place, show or remove the desktop icon of a minimized window */
static void
icon_update(Client *c)
{
	int want = wm.icons_on && c->hidden && (c->sticky || c->desk == wm.cur);

	if (c->iconwin && (!c->hidden || !wm.icons_on)) {
		rui_surf_free(c->isurf);
		XDestroyWindow(wm.dpy, c->iconwin);
		c->iconwin = None;
		c->isurf = NULL;
		return;
	}
	if (!wm.icons_on || !c->hidden)
		return;
	if (!c->iconwin) {
		XClassHint ch;
		int slot = 0, i, taken = 1;

		while (taken) {                    /* lowest free slot */
			taken = 0;
			for (i = 0; i < wm.clients.n; i++)
				if (wm.clients.v[i]->iconwin &&
				    wm.clients.v[i]->islot == slot)
					taken = 1;
			if (taken)
				slot++;
		}
		c->islot = slot;
		c->iconwin = rui_window_new(wm.app, wm.root, 0, 0, ICON_W, icon_h(),
		    1, ExposureMask | ButtonPressMask);
		c->isurf = rui_surf_new(wm.app, c->iconwin, ICON_W, icon_h());
		ch.res_name = (char *)"retro-icon";
		ch.res_class = (char *)"Retro-icon";
		XSetClassHint(wm.dpy, c->iconwin, &ch);
		XStoreName(wm.dpy, c->iconwin, c->name ? c->name : "");
	}
	icon_move(c);
	if (want) {
		XMapWindow(wm.dpy, c->iconwin);
		icon_draw(c);
	} else {
		XUnmapWindow(wm.dpy, c->iconwin);
	}
}

static Client *
client_from_icon(Window w)
{
	int i;

	for (i = 0; i < wm.clients.n && w != None; i++)
		if (wm.clients.v[i]->iconwin == w)
			return wm.clients.v[i];
	return NULL;
}

static void
sync_visibility(Client *c)
{
	if (is_visible(c)) {
		XMapWindow(wm.dpy, c->frame);
	} else {
		XUnmapWindow(wm.dpy, c->frame);
	}
	set_wm_state(c, c->hidden ? IconicState : NormalState);
	if (wm.icons_on) {
		Window had = c->iconwin;

		icon_update(c);
		if (c->iconwin && c->iconwin != had)
			restack();
	}
}

/* ----------------------------------------------------------------- tiling */

static int
layout_index(const char *name)
{
	int i;

	for (i = 0; i < wm.nlayouts; i++)
		if (!strcmp(rui_config.layouts[i].name, name))
			return i;
	return -1;
}

static void
init_layouts(void)
{
	int i;

	wm.nlayouts = rui_config.nlayouts > MAX_LAYOUTS ? MAX_LAYOUTS :
	    rui_config.nlayouts;
	for (i = 0; i < wm.nlayouts; i++) {
		const char *n = rui_config.layouts[i].name;

		if (!strcmp(n, "tile"))
			wm.algo[i] = LA_TILE;
		else if (!strcmp(n, "monocle"))
			wm.algo[i] = LA_MONOCLE;
		else {
			if (strcmp(n, "float"))
				rui_warn("config.h: unknown layout '%s', using float", n);
			wm.algo[i] = LA_FLOAT;
		}
	}
}

static int
desk_algo(unsigned long d)
{
	if (d >= (unsigned long)wm.ndesk || wm.nlayouts == 0)
		return LA_FLOAT;
	return wm.algo[wm.layout[d]];
}

static int
tileable(const Client *c)
{
	return c->type == T_NORMAL && !c->floating && !c->fullscreen &&
	    !c->hidden && !c->sticky && c->transient == None && !fixed_size(c) &&
	    c->decorated;
}

static void
untile(Client *c)
{
	if (!c->tiled)
		return;
	c->tiled = 0;
	c->x = c->tx;
	c->y = c->ty;
	c->w = c->tw;
	c->h = c->th;
	apply_geometry(c);
}

static void
tile_place(Client *c, int x, int y, int w, int h)
{
	if (!c->tiled) {
		c->tx = c->x;
		c->ty = c->y;
		c->tw = c->w;
		c->th = c->h;
		c->tiled = 1;
	}
	c->maxed = 0;
	c->shaded = 0;
	c->x = x;
	c->y = y;
	c->w = w - 2 * bw_of(c);
	c->h = h - th_of(c) - 2 * bw_of(c);
	if (c->w < 20)
		c->w = 20;
	if (c->h < 20)
		c->h = 20;
	apply_geometry(c);
	update_net_state(c);
}

static void
arrange(void)
{
	int d, i;

	for (i = 0; i < wm.clients.n; i++) {
		Client *c = wm.clients.v[i];

		if (c->tiled && (!tileable(c) || desk_algo(c->desk) == LA_FLOAT))
			untile(c);
	}
	for (d = 0; d < wm.ndesk; d++) {
		RuiRect a = wm.workarea;
		int n = 0, k = 0, mw, algo = desk_algo((unsigned long)d);

		if (algo == LA_FLOAT)
			continue;
		for (i = 0; i < wm.clients.n; i++)
			if (wm.clients.v[i]->desk == (unsigned long)d &&
			    tileable(wm.clients.v[i]))
				n++;
		if (n == 0)
			continue;
		for (i = 0; i < wm.clients.n; i++) {
			Client *c = wm.clients.v[i];

			if (c->desk != (unsigned long)d || !tileable(c))
				continue;
			if (algo == LA_MONOCLE) {
				tile_place(c, a.x, a.y, a.w, a.h);
				continue;
			}
			mw = n > 1 ? a.w * wm.mfact[d] / 100 : a.w;
			if (k == 0) {
				tile_place(c, a.x, a.y, mw, a.h);
			} else {
				int sh = a.h / (n - 1);
				int last = k == n - 1;

				tile_place(c, a.x + mw, a.y + (k - 1) * sh, a.w - mw,
				    last ? a.h - (k - 1) * sh : sh);
			}
			k++;
		}
	}
}

/* the panel shows this next to the pager (dwm-style layout symbol) */
static void
publish_layout(void)
{
	const char *sym = wm.nlayouts ?
	    rui_config.layouts[wm.layout[wm.cur]].symbol : "";

	XChangeProperty(wm.dpy, wm.root, A(_RETRO_LAYOUT), A(UTF8_STRING), 8,
	    PropModeReplace, (const unsigned char *)sym, (int)strlen(sym));
}

static void
set_layout(int idx)
{
	if (idx < 0 || idx >= wm.nlayouts)
		return;
	wm.layout[wm.cur] = idx;
	arrange();
	restack();
	publish_layout();
}

/* selecting the active layout again returns to the default (first) one */
static void
set_layout_named(const char *name)
{
	int idx = layout_index(name);

	if (idx < 0) {
		rui_warn("no layout '%s' in config.h", name);
		return;
	}
	set_layout(wm.layout[wm.cur] == idx ? 0 : idx);
}

/* ------------------------------------------------------------ window ops */

static void
set_maximized(Client *c, int bits)
{
	RuiRect a;

	if (c->type == T_DOCK || c->type == T_DESKTOP || fixed_size(c))
		return;
	bits &= MAX_BOTH;
	if (bits == c->maxed)
		return;
	if (c->tiled) {
		c->floating = 1;
		arrange();
	}
	if (c->fullscreen) {
		c->maxed = bits;           /* applied when fullscreen ends */
		update_net_state(c);
		return;
	}
	if (c->snap) {                         /* its free geometry is saved */
		c->snap = SNAP_NONE;
		c->x = c->sx;
		c->y = c->sy;
		c->w = c->sw;
		c->h = c->sh;
	}
	if (!c->maxed) {
		c->sx = c->x;
		c->sy = c->y;
		c->sw = c->w;
		c->sh = c->h;
	}
	/* restore dimensions that are no longer maximized */
	if ((c->maxed & MAX_HORZ) && !(bits & MAX_HORZ)) {
		c->x = c->sx;
		c->w = c->sw;
	}
	if ((c->maxed & MAX_VERT) && !(bits & MAX_VERT)) {
		c->y = c->sy;
		c->h = c->sh;
	}
	c->maxed = bits;
	a = area_of(c);
	if (bits & MAX_HORZ) {
		c->x = a.x;
		c->w = a.w - 2 * bw_of(c);
	}
	if (bits & MAX_VERT) {
		c->y = a.y;
		c->h = a.h - th_of(c) - 2 * bw_of(c);
		c->shaded = 0;
	}
	apply_geometry(c);
	update_net_state(c);
}

static void
set_fullscreen(Client *c, int on)
{
	if (c->type == T_DOCK || c->type == T_DESKTOP || !!on == c->fullscreen)
		return;
	if (on) {
		RuiRect m = monitor_of(c);

		if (c->tiled) {
			c->fullscreen = 1;
			arrange();             /* drops it out of the layout */
			c->fullscreen = 0;
		}
		c->fx = c->x;
		c->fy = c->y;
		c->fw = c->w;
		c->fh = c->h;
		c->fullscreen = 1;
		c->x = m.x;
		c->y = m.y;
		c->w = m.w;
		c->h = m.h;
	} else {
		c->fullscreen = 0;
		c->x = c->fx;
		c->y = c->fy;
		c->w = c->fw;
		c->h = c->fh;
	}
	apply_geometry(c);
	update_net_state(c);
	restack();
	arrange();
}

static void
set_shaded(Client *c, int on)
{
	if (!c->decorated || c->tiled || !!on == c->shaded)
		return;
	c->shaded = !!on;
	apply_geometry(c);
	update_net_state(c);
}

static void
set_hidden(Client *c, int on)
{
	if (c->type == T_DOCK || c->type == T_DESKTOP || !!on == c->hidden)
		return;
	c->hidden = !!on;
	sync_visibility(c);
	update_net_state(c);
	arrange();
	if (on && wm.focused == c)
		focus_top();
}

static void
set_sticky(Client *c, int on)
{
	if (!!on == c->sticky)
		return;
	c->sticky = !!on;
	if (!on)
		c->desk = wm.cur;
	update_desktop_prop(c);
	update_net_state(c);
	sync_visibility(c);
	draw_frame(c);
	arrange();
}

static void
set_desktop(Client *c, unsigned long d)
{
	if (d == 0xffffffffUL) {
		set_sticky(c, 1);
		return;
	}
	if (d >= (unsigned long)wm.ndesk || c->type == T_DOCK ||
	    c->type == T_DESKTOP)
		return;
	c->sticky = 0;
	c->desk = d;
	update_desktop_prop(c);
	update_net_state(c);
	sync_visibility(c);
	arrange();
	if (wm.focused == c && !is_visible(c))
		focus_top();
}

static void
close_client(Client *c)
{
	if (c->type == T_DESKTOP)
		return;
	if (c->can_delete)
		send_protocol(c, A(WM_DELETE_WINDOW));
	else
		XKillClient(wm.dpy, c->win);
}

static void
switch_desktop(unsigned long d)
{
	int i;
	unsigned long v = d;

	if (d >= (unsigned long)wm.ndesk || d == wm.cur)
		return;
	wm.cur = d;
	/* map the new desktop first, then unmap the old: less flashing */
	for (i = 0; i < wm.clients.n; i++)
		if (is_visible(wm.clients.v[i]))
			sync_visibility(wm.clients.v[i]);
	for (i = 0; i < wm.clients.n; i++)
		if (!is_visible(wm.clients.v[i]))
			sync_visibility(wm.clients.v[i]);
	rui_set_prop32(wm.app, wm.root, A(_NET_CURRENT_DESKTOP), XA_CARDINAL, &v, 1);
	publish_layout();
	arrange();
	focus_top();
	bus_event("wm.workspace", "%lu", d + 1);
}

static void
activate(Client *c)
{
	if (!c->sticky && c->desk != wm.cur && c->desk < (unsigned long)wm.ndesk)
		switch_desktop(c->desk);
	if (c->hidden)
		set_hidden(c, 0);
	raise_client(c);
	focus(c);
}

/* ------------------------------------------- snapping and moving windows */

static int
movable(const Client *c)
{
	return c && c->type != T_DOCK && c->type != T_DESKTOP && !c->fullscreen;
}

/* the half or quarter `where` of area a, as a frame rectangle */
static RuiRect
snap_rect(RuiRect a, int where)
{
	RuiRect r = a;
	int hw = a.w / 2, hh = a.h / 2;

	if (where == SNAP_LEFT || where == SNAP_TL || where == SNAP_BL)
		r.w = hw;
	if (where == SNAP_RIGHT || where == SNAP_TR || where == SNAP_BR) {
		r.x = a.x + hw;
		r.w = a.w - hw;
	}
	if (where == SNAP_TOP || where == SNAP_TL || where == SNAP_TR)
		r.h = hh;
	if (where == SNAP_BOTTOM || where == SNAP_BL || where == SNAP_BR) {
		r.y = a.y + hh;
		r.h = a.h - hh;
	}
	return r;
}

/* remembers the free geometry once, so un-snapping gives it back */
static void
save_free_geometry(Client *c)
{
	if (c->snap || c->maxed)
		return;
	c->sx = c->x;
	c->sy = c->y;
	c->sw = c->w;
	c->sh = c->h;
}

static void
unsnap(Client *c)
{
	if (!c->snap)
		return;
	c->snap = SNAP_NONE;
	c->w = c->sw;
	c->h = c->sh;
}

static void
fit_frame(Client *c, RuiRect r)
{
	c->x = r.x;
	c->y = r.y;
	c->w = r.w - 2 * bw_of(c);
	c->h = r.h - th_of(c) - 2 * bw_of(c);
	if (c->w < 1)
		c->w = 1;
	if (c->h < 1)
		c->h = 1;
	apply_geometry(c);
}

/* index of the monitor next to m in direction dx (-1 left, 1 right) */
static int
monitor_beside(RuiRect m, int dx)
{
	int i, best = -1;

	for (i = 0; i < wm.nmons; i++) {
		RuiRect o = wm.mons[i];

		if (o.y >= m.y + m.h || o.y + o.h <= m.y)
			continue;                      /* not side by side */
		if (dx > 0 && o.x >= m.x + m.w &&
		    (best < 0 || o.x < wm.mons[best].x))
			best = i;
		if (dx < 0 && o.x + o.w <= m.x &&
		    (best < 0 || o.x > wm.mons[best].x))
			best = i;
	}
	return best;
}

static void
snap_client(Client *c, int where)
{
	RuiRect m;

	if (!movable(c) || fixed_size(c) || where <= SNAP_NONE ||
	    where >= SNAP_COUNT)
		return;
	if (c->tiled) {
		c->floating = 1;
		arrange();
	}
	m = monitor_of(c);
	/* snapped left already and asked again: on to the monitor beside it */
	if (c->snap == where && (where == SNAP_LEFT || where == SNAP_RIGHT)) {
		int n = monitor_beside(m, where == SNAP_LEFT ? -1 : 1);

		if (n < 0)
			return;
		m = wm.mons[n];
		where = where == SNAP_LEFT ? SNAP_RIGHT : SNAP_LEFT;
	}
	save_free_geometry(c);
	if (c->maxed) {
		c->maxed = 0;
		update_net_state(c);
	}
	c->snap = where;
	fit_frame(c, snap_rect(mon_area(m), where));
	raise_client(c);
}

static void
center_client(Client *c)
{
	RuiRect a;

	if (!movable(c) || c->maxed || c->tiled)
		return;
	unsnap(c);
	a = area_of(c);
	c->x = a.x + (a.w - frame_w(c)) / 2;
	c->y = a.y + (a.h - frame_h(c)) / 2;
	if (c->y < a.y)
		c->y = a.y;
	apply_geometry(c);
}

/* keeps the window where it was, relative to its monitor */
static void
move_to_monitor(Client *c, int idx)
{
	RuiRect from, to;
	int snap, maxed;

	if (!movable(c) || idx < 0 || idx >= wm.nmons)
		return;
	if (c->tiled) {
		c->floating = 1;
		arrange();
	}
	from = mon_area(monitor_of(c));
	to = mon_area(wm.mons[idx]);
	snap = c->snap;
	maxed = c->maxed;
	if (maxed)
		set_maximized(c, 0);
	unsnap(c);
	c->x = to.x + (int)((long)(c->x - from.x) * to.w / (from.w ? from.w : 1));
	c->y = to.y + (int)((long)(c->y - from.y) * to.h / (from.h ? from.h : 1));
	if (c->x + frame_w(c) > to.x + to.w)
		c->x = to.x + to.w - frame_w(c);
	if (c->y + frame_h(c) > to.y + to.h)
		c->y = to.y + to.h - frame_h(c);
	if (c->x < to.x)
		c->x = to.x;
	if (c->y < to.y)
		c->y = to.y;
	apply_geometry(c);
	if (maxed)
		set_maximized(c, maxed);
	else if (snap)
		snap_client(c, snap);
	raise_client(c);
}

static void
to_monitor(Client *c, const char *arg)
{
	RuiRect m;
	int cur = 0, i, n;

	if (!c || wm.nmons < 2)
		return;
	m = monitor_of(c);
	for (i = 0; i < wm.nmons; i++)
		if (wm.mons[i].x == m.x && wm.mons[i].y == m.y)
			cur = i;
	if (!arg || !*arg || !strcmp(arg, "next"))
		n = (cur + 1) % wm.nmons;
	else if (!strcmp(arg, "prev"))
		n = (cur + wm.nmons - 1) % wm.nmons;
	else
		n = atoi(arg) - 1;
	if (n != cur)
		move_to_monitor(c, n);
}

/* move DX DY / resize DW DH from the keyboard */
static void
nudge(Client *c, const char *arg, int resize)
{
	int dx = 0, dy = 0;

	if (!movable(c) || c->maxed || c->tiled)
		return;
	if (!arg)
		return;
	/* left/right/up/down use move_step; else two numbers */
	if (!strcmp(arg, "left"))
		dx = -wm.move_step;
	else if (!strcmp(arg, "right"))
		dx = wm.move_step;
	else if (!strcmp(arg, "up"))
		dy = -wm.move_step;
	else if (!strcmp(arg, "down"))
		dy = wm.move_step;
	else if (sscanf(arg, "%d %d", &dx, &dy) < 1)
		return;
	unsnap(c);
	if (resize) {
		int w = c->w + dx, h = c->h + dy;

		if (fixed_size(c))
			return;
		if (w < 40)
			w = 40;
		if (h < 20)
			h = 20;
		apply_size_hints(c, &w, &h);
		c->w = w;
		c->h = h;
	} else {
		RuiRect a = area_of(c);

		c->x += dx;
		c->y += dy;
		/* part of the title always stays reachable */
		if (c->x > a.x + a.w - 40)
			c->x = a.x + a.w - 40;
		if (c->x + frame_w(c) < a.x + 40)
			c->x = a.x + 40 - frame_w(c);
		if (c->y > a.y + a.h - 20)
			c->y = a.y + a.h - 20;
		if (c->y < a.y)
			c->y = a.y;
	}
	apply_geometry(c);
}

/* sticks v (an edge at v or v + len) to target within the snap distance */
static void
stick(int *v, int len, int target, int *best)
{
	int d1 = abs(*v - target), d2 = abs(*v + len - target);

	if (d1 <= wm.snap_dist && d1 < *best) {
		*best = d1;
		*v = target;
	}
	if (d2 <= wm.snap_dist && d2 < *best) {
		*best = d2;
		*v = target - len;
	}
}

/* edge resistance: a moved window sticks to the monitor's edges and to
 * the edges of the other windows */
static void
snap_edges(const Client *c, int *x, int *y)
{
	RuiRect a = mon_area(monitor_at(*x + frame_w(c) / 2, *y + frame_h(c) / 2));
	int fw = frame_w(c), fh = frame_h(c), bx, by, i;

	if (wm.snap_dist <= 0)
		return;
	bx = by = wm.snap_dist + 1;
	stick(x, fw, a.x, &bx);
	stick(x, fw, a.x + a.w, &bx);
	stick(y, fh, a.y, &by);
	stick(y, fh, a.y + a.h, &by);
	for (i = 0; i < wm.clients.n; i++) {
		const Client *o = wm.clients.v[i];
		int ow, oh;

		if (o == c || !is_visible(o) || !focusable(o))
			continue;
		ow = frame_w(o);
		oh = frame_h(o);
		if (*y < o->y + oh && *y + fh > o->y) {        /* side by side */
			stick(x, fw, o->x, &bx);
			stick(x, fw, o->x + ow, &bx);
		}
		if (*x < o->x + ow && *x + fw > o->x) {        /* above / below */
			stick(y, fh, o->y, &by);
			stick(y, fh, o->y + oh, &by);
		}
	}
}

/* the outline shown while a window is dragged against a screen edge */
static void
zone_show(RuiRect r)
{
	int i, t = 3;
	RuiRect e[4];

	e[0].x = r.x;
	e[0].y = r.y;
	e[0].w = r.w;
	e[0].h = t;
	e[1] = e[0];
	e[1].y = r.y + r.h - t;
	e[2].x = r.x;
	e[2].y = r.y;
	e[2].w = t;
	e[2].h = r.h;
	e[3] = e[2];
	e[3].x = r.x + r.w - t;
	for (i = 0; i < 4; i++) {
		if (!wm.zone_win[i]) {
			XSetWindowAttributes swa;

			swa.override_redirect = True;
			swa.background_pixel = wm.app->color[RUI_C_TITLE_ACTIVE].pixel;
			swa.save_under = True;
			wm.zone_win[i] = XCreateWindow(wm.dpy, wm.root, 0, 0, 1, 1, 0,
			    CopyFromParent, InputOutput, CopyFromParent,
			    CWOverrideRedirect | CWBackPixel | CWSaveUnder, &swa);
		}
		XMoveResizeWindow(wm.dpy, wm.zone_win[i], e[i].x, e[i].y,
		    (unsigned)e[i].w, (unsigned)e[i].h);
		XMapRaised(wm.dpy, wm.zone_win[i]);
	}
}

static void
zone_hide(void)
{
	int i;

	for (i = 0; i < 4; i++)
		if (wm.zone_win[i])
			XUnmapWindow(wm.dpy, wm.zone_win[i]);
}

/* Which zone the pointer asks for while dragging: the left or right edge
 * of the screen snaps to that half (the corners to a quarter), the top
 * edge maximizes.  Only outer edges count, so a window can still be
 * carried from one monitor to the next. */
static int
zone_at(int xr, int yr, RuiRect *m)
{
	int left, right, top, bottom, corner;

	*m = monitor_at(xr, yr);
	left = xr <= m->x && monitor_beside(*m, -1) < 0;
	right = xr >= m->x + m->w - 1 && monitor_beside(*m, 1) < 0;
	top = yr <= m->y;
	bottom = yr >= m->y + m->h - 1;
	corner = m->h / 6;
	if (left)
		return yr < m->y + corner ? SNAP_TL : yr > m->y + m->h - corner ?
		    SNAP_BL : SNAP_LEFT;
	if (right)
		return yr < m->y + corner ? SNAP_TR : yr > m->y + m->h - corner ?
		    SNAP_BR : SNAP_RIGHT;
	if (top)
		return -1;                         /* maximize */
	(void)bottom;
	return SNAP_NONE;
}

/* --------------------------------------------- do not disturb, screen saver */

static void
publish_flags(void)
{
	unsigned long v;

	v = (unsigned long)wm.dnd;
	rui_set_prop32(wm.app, wm.root, A(_RETRO_DND), XA_CARDINAL, &v, 1);
	v = (unsigned long)wm.awake;
	rui_set_prop32(wm.app, wm.root, A(_RETRO_AWAKE), XA_CARDINAL, &v, 1);
}

/* the X server's own blanking and monitor power saving, from retro.conf;
 * both are held off while "keep the screen on" is set */
static void
apply_saver(void)
{
	int evb, erb, blank = wm.awake || wm.xss ? 0 : wm.saver_secs;
	int dpms = wm.awake ? 0 : wm.dpms_secs;

	XSetScreenSaver(wm.dpy, blank, 0, PreferBlanking, AllowExposures);
	if (DPMSQueryExtension(wm.dpy, &evb, &erb) && DPMSCapable(wm.dpy)) {
		if (dpms > 0) {
			DPMSSetTimeouts(wm.dpy, (CARD16)dpms, (CARD16)dpms, (CARD16)dpms);
			DPMSEnable(wm.dpy);
		} else {
			DPMSDisable(wm.dpy);
		}
	}
	if (wm.awake) {
		XResetScreenSaver(wm.dpy);
		wm.awake_poke = 0;                 /* poke xscreensaver at once */
	}
}

/* xscreensaver keeps its own idle timer: tell it the user is "active" */
static void
awake_tick(void)
{
	long long now = rui_now_ms();

	if (!wm.awake || now - wm.awake_poke < 50000)
		return;
	wm.awake_poke = now;
	XResetScreenSaver(wm.dpy);
	if (wm.xss)
		rui_spawn("xscreensaver-command -deactivate >/dev/null 2>&1");
}

/* how: 0 toggle, 1 on, 2 off */
static void
set_dnd(int how)
{
	int on = how == 1 ? 1 : how == 2 ? 0 : !wm.dnd, i;

	if (on == wm.dnd)
		return;
	wm.dnd = on;
	publish_flags();
	bus_event("wm.dnd", "%s", on ? "on" : "off");
	/* notification daemons are paused; windows stop asking for attention */
	if (wm.dnd_cmd[on] && *wm.dnd_cmd[on])
		rui_spawn(wm.dnd_cmd[on]);
	else if (rui_have_cmd("dunstctl"))
		rui_spawn(on ? "dunstctl set-paused true" : "dunstctl set-paused false");
	for (i = 0; on && i < wm.clients.n; i++) {
		Client *c = wm.clients.v[i];

		if (c->urgent) {
			c->urgent = 0;
			update_net_state(c);
			draw_frame(c);
		}
	}
}

static void
set_awake(int how)
{
	int on = how == 1 ? 1 : how == 2 ? 0 : !wm.awake;

	if (on == wm.awake)
		return;
	wm.awake = on;
	publish_flags();
	apply_saver();
}

static int
on_off(const char *arg)
{
	if (arg && (!strcmp(arg, "on") || !strcmp(arg, "1")))
		return 1;
	if (arg && (!strcmp(arg, "off") || !strcmp(arg, "2")))
		return 2;
	return 0;
}

/* ------------------------------------------------------------------ rules */

static char *
word_dup(const char *s, size_t n)
{
	char *w = rui_malloc(n + 1);

	/* quotes around a value with blanks are dropped */
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		s++;
		n -= 2;
	}
	memcpy(w, s, n);
	w[n] = '\0';
	return w;
}

/* next blank-separated word; "quoted values" may hold blanks */
static const char *
next_word(const char *p, const char **start, size_t *len)
{
	int q = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	*start = p;
	while (*p && (q || (*p != ' ' && *p != '\t'))) {
		if (*p == '"')
			q = !q;
		p++;
	}
	*len = (size_t)(p - *start);
	return p;
}

static int
add_rule(const char *spec, const char *origin)
{
	const char *arrow = strstr(spec, "=>"), *p, *w;
	char *tok, *val;
	size_t len;
	Rule r;
	int side, bad = 0, i;

	memset(&r, 0, sizeof r);
	if (!arrow) {
		rui_warn("%s: rule needs \"match => options\": %s", origin, spec);
		return 0;
	}
	for (side = 0; side < 2; side++) {
		const char *end = side ? spec + strlen(spec) : arrow;

		p = side ? arrow + 2 : spec;
		for (;;) {
			p = next_word(p, &w, &len);
			if (!len || w >= end)
				break;
			if (w + len > end)
				len = (size_t)(end - w);
			tok = word_dup(w, len);
			if ((val = strchr(tok, '='))) {
				*val++ = '\0';
				if (*val == '"' && val[strlen(val) - 1] == '"' && val[1]) {
					val[strlen(val) - 1] = '\0';
					val++;
				}
			}
			if (!side) {
				char **dst = !strcmp(tok, "class") ? &r.cls :
				    !strcmp(tok, "name") || !strcmp(tok, "instance") ?
				    &r.name : !strcmp(tok, "title") ? &r.title :
				    !strcmp(tok, "type") ? &r.type : NULL;

				if (dst && val && !*dst)
					*dst = rui_strdup(val);
				else
					bad = 1;
			} else if (!strcmp(tok, "workspace") && val) {
				r.desk = !strcmp(val, "all") ? -1 : atoi(val);
				bad |= r.desk == 0;
			} else if (!strcmp(tok, "monitor") && val) {
				r.monitor = atoi(val);
				bad |= r.monitor < 1;
			} else if (!strcmp(tok, "geometry") && val) {
				unsigned gw = 0, gh = 0;
				int m = XParseGeometry(val, &r.gx, &r.gy, &gw, &gh);

				r.has_size = (m & WidthValue) && (m & HeightValue);
				r.has_pos = (m & XValue) && (m & YValue);
				r.gneg_x = (m & XNegative) != 0;
				r.gneg_y = (m & YNegative) != 0;
				r.gw = (int)gw;
				r.gh = (int)gh;
				bad |= !r.has_size && !r.has_pos;
			} else if (!strcmp(tok, "snap") && val) {
				for (i = 1; i < SNAP_COUNT; i++)
					if (!strcmp(val, snap_names[i]))
						r.snap = i;
				bad |= !r.snap;
			} else if (!strcmp(tok, "maximized") || !strcmp(tok, "maximize")) {
				r.maximized = 1;
			} else if (!strcmp(tok, "fullscreen")) {
				r.fullscreen = 1;
			} else if (!strcmp(tok, "minimized") || !strcmp(tok, "minimize")) {
				r.minimized = 1;
			} else if (!strcmp(tok, "sticky")) {
				r.sticky = 1;
			} else if (!strcmp(tok, "above")) {
				r.above = 1;
			} else if (!strcmp(tok, "below")) {
				r.below = 1;
			} else if (!strcmp(tok, "shaded")) {
				r.shaded = 1;
			} else if (!strcmp(tok, "float")) {
				r.floating = 1;
			} else if (!strcmp(tok, "tile")) {
				r.floating = -1;
			} else if (!strcmp(tok, "noborder") || !strcmp(tok, "undecorated")) {
				r.border = -1;
			} else if (!strcmp(tok, "border")) {
				r.border = 1;
			} else if (!strcmp(tok, "skip_taskbar")) {
				r.skip_taskbar = 1;
			} else if (!strcmp(tok, "skip_pager")) {
				r.skip_pager = 1;
			} else if (!strcmp(tok, "nofocus")) {
				r.nofocus = 1;
			} else if (!strcmp(tok, "center")) {
				r.center = 1;
			} else {
				bad = 1;
			}
			if (bad)
				rui_warn("%s: rule: unknown or incomplete '%s' in: %s",
				    origin, tok, spec);
			free(tok);
			if (bad)
				break;
		}
	}
	if (bad || (!r.cls && !r.name && !r.title && !r.type)) {
		if (!bad)
			rui_warn("%s: rule matches nothing (class=, name=, title=, "
			    "type=): %s", origin, spec);
		free(r.cls);
		free(r.name);
		free(r.title);
		free(r.type);
		return 0;
	}
	wm.rules = rui_realloc(wm.rules, (size_t)(wm.nrules + 1) * sizeof *wm.rules);
	wm.rules[wm.nrules++] = r;
	return 1;
}

static const char *
type_name(int type)
{
	return type == T_DIALOG ? "dialog" : type == T_SPLASH ? "splash" :
	    type == T_DOCK ? "dock" : type == T_DESKTOP ? "desktop" :
	    type == T_NORMAL ? "normal" : "utility";
}

/* Settings from every matching rule, later rules winning; applied to a new
 * window before it is framed.  Returns bits for what must happen after. */
#define RULE_MAX 1
#define RULE_FULL 2
#define RULE_MIN 4
#define RULE_POS 8
#define RULE_CENTER 16
#define RULE_SHADE 32

static int
apply_rules(Client *c, int *snap)
{
	XClassHint ch = { NULL, NULL };
	int i, out = 0;

	*snap = 0;
	if (!wm.nrules)
		return 0;
	XGetClassHint(wm.dpy, c->win, &ch);
	for (i = 0; i < wm.nrules; i++) {
		const Rule *r = &wm.rules[i];

		if ((r->cls && !rui_glob(r->cls, ch.res_class ? ch.res_class : "")) ||
		    (r->name && !rui_glob(r->name, ch.res_name ? ch.res_name : "")) ||
		    (r->title && !rui_glob(r->title, c->name ? c->name : "")) ||
		    (r->type && !rui_glob(r->type, type_name(c->type))))
			continue;
		if (r->desk == -1)
			c->sticky = 1;
		else if (r->desk > 0 && r->desk <= wm.ndesk)
			c->desk = (unsigned long)(r->desk - 1);
		c->sticky |= r->sticky;
		c->above |= r->above;
		c->below |= r->below;
		c->skip_taskbar |= r->skip_taskbar;
		c->skip_pager |= r->skip_pager;
		c->nofocus |= r->nofocus;
		if (r->floating)
			c->floating = r->floating > 0;
		if (r->border)
			c->decorated = r->border > 0;
		if (r->has_size) {
			c->w = r->gw;
			c->h = r->gh;
		}
		if (r->has_pos || r->monitor) {
			RuiRect a = mon_area(wm.mons[r->monitor >= 1 &&
			    r->monitor <= wm.nmons ? r->monitor - 1 : wm.prim]);

			/* positions count from the monitor; negative ones from its
			 * right and bottom edges, like X geometry strings */
			c->x = a.x + (r->has_pos ? (r->gneg_x ? a.w - frame_w(c) + r->gx :
			    r->gx) : 30);
			c->y = a.y + (r->has_pos ? (r->gneg_y ? a.h - frame_h(c) + r->gy :
			    r->gy) : 30);
			out |= RULE_POS;
		}
		if (r->center)
			out |= RULE_CENTER;
		if (r->maximized)
			out |= RULE_MAX;
		if (r->fullscreen)
			out |= RULE_FULL;
		if (r->minimized)
			out |= RULE_MIN;
		if (r->shaded)
			out |= RULE_SHADE;
		if (r->snap)
			*snap = r->snap;
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
	return out;
}

/* -------------------------------------------------------- manage/unmanage */

/* area of r covered by the other windows of the current workspace */
static long
overlap_at(const Client *c, int x, int y)
{
	int fw = frame_w(c), fh = frame_h(c), i;
	long sum = 0;

	for (i = 0; i < wm.clients.n; i++) {
		const Client *o = wm.clients.v[i];
		int x1, y1, x2, y2;

		if (o == c || !is_visible(o) || !focusable(o))
			continue;
		x1 = x > o->x ? x : o->x;
		y1 = y > o->y ? y : o->y;
		x2 = x + fw < o->x + frame_w(o) ? x + fw : o->x + frame_w(o);
		y2 = y + fh < o->y + frame_h(o) ? y + fh : o->y + frame_h(o);
		if (x2 > x1 && y2 > y1)
			sum += (long)(x2 - x1) * (y2 - y1);
	}
	return sum;
}

static void
place(Client *c)
{
	RuiRect a;
	Client *p = c->transient ? client_from_win(c->transient) : NULL;
	int fw = frame_w(c), fh = frame_h(c);

	if (p && p != c) {
		c->x = p->x + (frame_w(p) - fw) / 2;
		c->y = p->y + (frame_h(p) - fh) / 2;
	} else {
		Window rw, cw;
		int px = 0, py = 0, wx, wy;
		unsigned mask;

		XQueryPointer(wm.dpy, wm.root, &rw, &cw, &px, &py, &wx, &wy, &mask);
		c->x = px;
		c->y = py;                         /* pick the pointer's monitor */
		a = area_of(c);
		if (c->type == T_DIALOG || c->type == T_SPLASH) {
			c->x = a.x + (a.w - fw) / 2;
			c->y = a.y + (a.h - fh) / 2;
		} else if (wm.placement == PLACE_CENTER) {
			c->x = a.x + (a.w - fw) / 2;
			c->y = a.y + (a.h - fh) / 2;
		} else if (wm.placement == PLACE_POINTER) {
			c->x = px - fw / 2;
			c->y = py - fh / 2;
			if (c->x + fw > a.x + a.w)
				c->x = a.x + a.w - fw;
			if (c->y + fh > a.y + a.h)
				c->y = a.y + a.h - fh;
		} else if (wm.placement == PLACE_SMART) {
			/* the spot where it covers the least of the other windows */
			int ix, iy, bx = a.x, by = a.y, n = 12;
			long best = -1;

			for (iy = 0; iy <= n; iy++) {
				for (ix = 0; ix <= n; ix++) {
					int tx = a.x + (a.w > fw ? (a.w - fw) * ix / n : 0);
					int ty = a.y + (a.h > fh ? (a.h - fh) * iy / n : 0);
					long o = overlap_at(c, tx, ty);

					if (best < 0 || o < best) {
						best = o;
						bx = tx;
						by = ty;
					}
				}
			}
			c->x = bx;
			c->y = by;
		} else {
			int step = wm.cascade++ % 8;

			c->x = a.x + 30 + step * (wm.th + wm.bw);
			c->y = a.y + 30 + step * (wm.th + wm.bw);
			if (c->x + fw > a.x + a.w)
				c->x = a.x + a.w - fw;
			if (c->y + fh > a.y + a.h)
				c->y = a.y + a.h - fh;
		}
	}
	a = area_of(c);
	/* a window larger than the monitor (a 640x480 or smaller screen) is
	 * shrunk to fit, as far as its minimum size allows */
	if (frame_w(c) > a.w || frame_h(c) > a.h) {
		int w = c->w, h = c->h;

		if (frame_w(c) > a.w)
			w -= frame_w(c) - a.w;
		if (frame_h(c) > a.h)
			h -= frame_h(c) - a.h;
		apply_size_hints(c, &w, &h);
		c->w = w;
		c->h = h;
	}
	if (c->x + frame_w(c) > a.x + a.w)
		c->x = a.x + a.w - frame_w(c);
	if (c->y + frame_h(c) > a.y + a.h)
		c->y = a.y + a.h - frame_h(c);
	if (c->x < a.x)
		c->x = a.x;
	if (c->y < a.y)
		c->y = a.y;
}

static Client *
manage(Window w, int startup)
{
	XWindowAttributes wa;
	XSetWindowAttributes swa;
	XSizeHints sh;
	long supplied;
	Client *c, *p;
	unsigned long *v;
	int n, i, iconic = 0, positioned, maxbits = 0, want_full = 0;
	int ruled = 0, rule_snap = 0;

	if (client_from_win(w))
		return NULL;
	if (!XGetWindowAttributes(wm.dpy, w, &wa) || wa.override_redirect ||
	    wa.class == InputOnly)
		return NULL;
	c = rui_calloc(1, sizeof *c);
	c->win = w;
	c->w = wa.width;
	c->h = wa.height;
	c->x = wa.x;
	c->y = wa.y;
	c->old_bw = wa.border_width;
	c->desk = wm.cur;
	fetch_name(c);
	fetch_transient(c);
	fetch_type(c);
	fetch_motif(c);
	fetch_size_hints(c);
	fetch_wm_hints(c, &iconic);
	fetch_protocols(c);
	fetch_strut(c);
	if (c->type == T_DOCK || c->type == T_DESKTOP) {
		c->sticky = 1;
		c->skip_taskbar = c->skip_pager = 1;
	}

	n = rui_get_prop32(wm.app, w, A(_NET_WM_DESKTOP), &v);
	p = c->transient ? client_from_win(c->transient) : NULL;
	if (n > 0 && v[0] == 0xffffffffUL)
		c->sticky = 1;
	else if (n > 0 && v[0] < (unsigned long)wm.ndesk)
		c->desk = v[0];
	else if (p) {
		c->desk = p->desk;
		c->sticky = c->sticky || p->sticky;
	}
	free(v);

	n = rui_get_prop32(wm.app, w, A(_NET_WM_STATE), &v);
	for (i = 0; i < n; i++) {
		if (v[i] == A(_NET_WM_STATE_STICKY))
			c->sticky = 1;
		else if (v[i] == A(_NET_WM_STATE_SHADED))
			c->shaded = c->decorated;
		else if (v[i] == A(_NET_WM_STATE_MAXIMIZED_VERT))
			maxbits |= MAX_VERT;
		else if (v[i] == A(_NET_WM_STATE_MAXIMIZED_HORZ))
			maxbits |= MAX_HORZ;
		else if (v[i] == A(_NET_WM_STATE_FULLSCREEN))
			want_full = 1;
		else if (v[i] == A(_NET_WM_STATE_ABOVE))
			c->above = 1;
		else if (v[i] == A(_NET_WM_STATE_BELOW))
			c->below = 1;
		else if (v[i] == A(_NET_WM_STATE_SKIP_TASKBAR))
			c->skip_taskbar = 1;
		else if (v[i] == A(_NET_WM_STATE_SKIP_PAGER))
			c->skip_pager = 1;
		else if (v[i] == A(_NET_WM_STATE_MODAL))
			c->modal = 1;
		else if (v[i] == A(_NET_WM_STATE_HIDDEN) && startup)
			iconic = 1;
	}
	free(v);
	if (startup) {
		n = rui_get_prop32(wm.app, w, A(WM_STATE), &v);
		if (n > 0 && v[0] == IconicState)
			iconic = 1;
		free(v);
	}
	if (iconic && c->type != T_DOCK && c->type != T_DESKTOP)
		c->hidden = 1;
	/* programs.conf: how this program's windows start */
	if (!startup && c->type != T_DOCK && c->type != T_DESKTOP) {
		ruled = apply_rules(c, &rule_snap);
		if (ruled & RULE_MAX)
			maxbits = MAX_BOTH;
		if (ruled & RULE_FULL)
			want_full = 1;
		if (ruled & RULE_MIN)
			c->hidden = 1;
	}

	memset(&sh, 0, sizeof sh);
	positioned = startup || c->type == T_DOCK || c->type == T_DESKTOP ||
	    (ruled & RULE_POS) ||
	    (XGetWMNormalHints(wm.dpy, w, &sh, &supplied) &&
	    (sh.flags & (USPosition | PPosition)));
	if (!positioned)
		place(c);
	else if (c->type != T_DOCK && c->type != T_DESKTOP) {
		/* never let a title bar start off-screen */
		if (c->y < 0)
			c->y = 0;
		if (c->x + frame_w(c) < 40)
			c->x = 0;
		if (c->x > wm.app->sw - 40)
			c->x = wm.app->sw - 40;
		if (c->y > wm.app->sh - 40)
			c->y = wm.app->sh - 40;
	}

	swa.override_redirect = True;
	swa.background_pixel = wm.app->color[RUI_C_FACE].pixel;
	swa.event_mask = FRAME_MASK;
	swa.cursor = wm.curs[0];
	c->frame = XCreateWindow(wm.dpy, wm.root, c->x, c->y,
	    (unsigned)frame_w(c), (unsigned)frame_h(c), 0, CopyFromParent,
	    InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel |
	    CWEventMask | CWCursor, &swa);
	c->deco = rui_surf_new(wm.app, c->frame, frame_w(c),
	    has_deco(c) ? wm.bw + wm.th : 1);

	XAddToSaveSet(wm.dpy, w);
	XSetWindowBorderWidth(wm.dpy, w, 0);
	XSelectInput(wm.dpy, w, PropertyChangeMask |
	    (wm.sloppy ? EnterWindowMask : 0));
	XReparentWindow(wm.dpy, w, c->frame, bw_of(c), bw_of(c) + th_of(c));
	XGrabButton(wm.dpy, AnyButton, AnyModifier, w, False, ButtonPressMask,
	    GrabModeSync, GrabModeAsync, None, None);

	vec_push(&wm.clients, c);
	vec_push(&wm.stack, c);
	vec_push(&wm.mru, c);

	apply_geometry(c);
	if (ruled & RULE_CENTER)
		center_client(c);
	if (rule_snap)
		snap_client(c, rule_snap);
	if (maxbits)
		set_maximized(c, maxbits);
	if (want_full)
		set_fullscreen(c, 1);
	if (ruled & RULE_SHADE)
		c->shaded = c->decorated;
	XMapWindow(wm.dpy, w);
	update_desktop_prop(c);
	update_net_state(c);
	update_allowed(c);
	if (c->strut[0] || c->strut[1] || c->strut[2] || c->strut[3])
		update_workarea();
	arrange();
	sync_visibility(c);
	if (p)
		raise_client(c);
	else
		restack();
	update_client_list();
	if (is_visible(c) && focusable(c) && c->type != T_SPLASH && !startup &&
	    !c->nofocus && (wm.focus_new || !wm.focused || p))
		focus(c);
	return c;
}

static void
cycle_end(int commit);

static void
unmanage(Client *c, int destroyed, int withdrawn)
{
	int had_strut = c->strut[0] || c->strut[1] || c->strut[2] || c->strut[3];

	if (wm.cyc.active)
		cycle_end(0);
	if (wm.drag.c == c) {
		wm.drag.mode = DRAG_NONE;
		wm.drag.c = NULL;
		XUngrabPointer(wm.dpy, CurrentTime);
	}
	if (wm.btn_c == c)
		wm.btn_c = NULL;
	vec_remove(&wm.clients, c);
	vec_remove(&wm.stack, c);
	vec_remove(&wm.mru, c);
	if (!destroyed) {
		XSelectInput(wm.dpy, c->win, NoEventMask);
		XUngrabButton(wm.dpy, AnyButton, AnyModifier, c->win);
		XReparentWindow(wm.dpy, c->win, wm.root, c->x, c->y);
		XSetWindowBorderWidth(wm.dpy, c->win, (unsigned)c->old_bw);
		XRemoveFromSaveSet(wm.dpy, c->win);
		if (withdrawn) {
			unsigned long v[2];

			v[0] = WithdrawnState;
			v[1] = None;
			rui_set_prop32(wm.app, c->win, A(WM_STATE), A(WM_STATE), v, 2);
			XDeleteProperty(wm.dpy, c->win, A(_NET_WM_STATE));
			XDeleteProperty(wm.dpy, c->win, A(_NET_WM_DESKTOP));
			XDeleteProperty(wm.dpy, c->win, A(_NET_FRAME_EXTENTS));
		} else {
			XMapWindow(wm.dpy, c->win);    /* WM is going away */
		}
	}
	if (c->iconwin) {
		rui_surf_free(c->isurf);
		XDestroyWindow(wm.dpy, c->iconwin);
	}
	XDestroyWindow(wm.dpy, c->frame);
	rui_surf_free(c->deco);
	free(c->name);
	if (wm.focused == c) {
		wm.focused = NULL;
		if (!wm.exiting)
			focus_top();
	}
	free(c);
	if (wm.exiting)
		return;
	if (had_strut)
		update_workarea();
	arrange();
	update_client_list();
	restack();
}

/* ------------------------------------------------------------ move/resize */

static void
drag_start(Client *c, int mode, int edges, int xr, int yr, Time t)
{
	Cursor cur = mode == DRAG_MOVE ? wm.cur_move : wm.curs[edges & 15];

	if (c->fullscreen || c->type == T_DOCK || c->type == T_DESKTOP)
		return;
	if (mode == DRAG_RESIZE && (fixed_size(c) || !edges))
		return;
	if (c->tiled) {
		c->floating = 1;
		arrange();
	}
	if (c->maxed) {
		if (mode == DRAG_MOVE)
			return;
		c->maxed = 0;
		update_net_state(c);
	}
	if (c->snap && mode == DRAG_MOVE) {
		/* pulled off its half: back to its own size, under the pointer */
		int ow = frame_w(c);

		unsnap(c);
		c->x = xr - (int)((long)(xr - c->x) * frame_w(c) / (ow ? ow : 1));
		apply_geometry(c);
	}
	c->snap = SNAP_NONE;
	wm.zone = SNAP_NONE;
	if (XGrabPointer(wm.dpy, wm.root, False, PointerMotionMask |
	    ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, cur, t) !=
	    GrabSuccess)
		return;
	wm.drag.mode = mode;
	wm.drag.edges = edges;
	wm.drag.c = c;
	wm.drag.px = xr;
	wm.drag.py = yr;
	wm.drag.ox = c->x;
	wm.drag.oy = c->y;
	wm.drag.ow = c->w;
	wm.drag.oh = c->h;
}

static void
drag_motion(int xr, int yr)
{
	Client *c = wm.drag.c;
	int dx = xr - wm.drag.px, dy = yr - wm.drag.py;

	if (!c)
		return;
	if (wm.drag.mode == DRAG_MOVE) {
		c->x = wm.drag.ox + dx;
		c->y = wm.drag.oy + dy;
		snap_edges(c, &c->x, &c->y);
		XMoveWindow(wm.dpy, c->frame, c->x, c->y);
		send_configure(c);
		if (wm.edge_snap && !fixed_size(c)) {
			RuiRect m;
			int z = zone_at(xr, yr, &m);

			if (z != wm.zone) {
				wm.zone = z;
				if (z == SNAP_NONE)
					zone_hide();
				else
					zone_show(z < 0 ? mon_area(m) : snap_rect(mon_area(m), z));
			}
		}
	} else {
		int w = wm.drag.ow, h = wm.drag.oh;

		if (wm.drag.edges & E_RIGHT)
			w += dx;
		if (wm.drag.edges & E_LEFT)
			w -= dx;
		if (wm.drag.edges & E_BOTTOM)
			h += dy;
		if (wm.drag.edges & E_TOP)
			h -= dy;
		apply_size_hints(c, &w, &h);
		if (wm.drag.edges & E_LEFT)
			c->x = wm.drag.ox + wm.drag.ow - w;
		if (wm.drag.edges & E_TOP)
			c->y = wm.drag.oy + wm.drag.oh - h;
		if (w != c->w || h != c->h) {
			c->w = w;
			c->h = h;
			apply_geometry(c);
		}
	}
}

static void
drag_end(void)
{
	Client *c = wm.drag.c;
	int zone = wm.zone;

	if (wm.drag.mode == DRAG_NONE)
		return;
	XUngrabPointer(wm.dpy, CurrentTime);
	zone_hide();
	wm.zone = SNAP_NONE;
	/* dropped against a screen edge: half, quarter or maximized */
	if (wm.drag.mode == DRAG_MOVE && c && client_alive(c) && zone) {
		if (zone < 0)
			set_maximized(c, MAX_BOTH);
		else
			snap_client(c, zone);
	}
	wm.drag.mode = DRAG_NONE;
	wm.drag.c = NULL;
}

/* ---------------------------------------------------------------- Alt+Tab */

static void
cycle_draw(void)
{
	RuiSurf *s = wm.cyc.s;
	RuiApp *a = wm.app;
	int rh = a->font->height + 8, i;

	rui_fill(s, 0, 0, s->w, s->h, RUI_C_FACE);
	rui_bevel(s, 0, 0, s->w, s->h, RUI_RAISED);
	for (i = 0; i < wm.cyc.n; i++) {
		Client *c = wm.cyc.cand[i];
		RuiRect r;
		char label[300];

		r.x = 8;
		r.y = 8 + i * rh;
		r.w = s->w - 16;
		r.h = rh;
		if (i == wm.cyc.sel) {
			rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_SEL_BG);
			rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
		}
		if (c->hidden)
			snprintf(label, sizeof label, "[%s]", c->name ? c->name : "");
		else
			snprintf(label, sizeof label, "%s", c->name ? c->name : "");
		r.x += 8;
		r.w -= 16;
		rui_text_fit(s, i == wm.cyc.sel ? a->bold : a->font, r, label,
		    i == wm.cyc.sel ? RUI_C_SEL_FG : RUI_C_TEXT, RUI_LEFT);
	}
	rui_surf_flush(s);
}

static void
cycle_end(int commit)
{
	Client *c = NULL;

	if (!wm.cyc.active)
		return;
	if (commit && wm.cyc.sel >= 0 && wm.cyc.sel < wm.cyc.n)
		c = wm.cyc.cand[wm.cyc.sel];
	XUngrabKeyboard(wm.dpy, CurrentTime);
	if (wm.cyc.win) {
		rui_surf_free(wm.cyc.s);
		XDestroyWindow(wm.dpy, wm.cyc.win);
	}
	free(wm.cyc.cand);
	memset(&wm.cyc, 0, sizeof wm.cyc);
	if (c && client_alive(c))
		activate(c);
}

static void
cycle(int dir, unsigned mod)
{
	int i;

	if (!wm.cyc.active) {
		RuiRect m;
		int w, h, n = 0;
		Client **cand = rui_malloc((size_t)(wm.mru.n + 1) * sizeof *cand);

		for (i = 0; i < wm.mru.n; i++) {
			Client *c = wm.mru.v[i];

			if (focusable(c) && !c->skip_taskbar &&
			    (c->sticky || c->desk == wm.cur))
				cand[n++] = c;
		}
		if (n == 0) {
			free(cand);
			return;
		}
		wm.cyc.cand = cand;
		wm.cyc.n = n;
		wm.cyc.sel = (wm.focused && cand[0] == wm.focused) ? 0 : -1;
		wm.cyc.mod = mod;
		wm.cyc.active = 1;
		if (mod == 0 || XGrabKeyboard(wm.dpy, wm.root, True, GrabModeAsync,
		    GrabModeAsync, CurrentTime) != GrabSuccess) {
			/* cannot track the modifier: switch immediately */
			wm.cyc.sel = (wm.cyc.sel + dir + n) % n;
			wm.cyc.mod = 0;
			cycle_end(1);
			return;
		}
		m = wm.focused ? monitor_of(wm.focused) : wm.mons[0];
		w = m.w < 520 ? m.w - 40 : 480;
		h = 16 + n * (wm.app->font->height + 8);
		wm.cyc.win = rui_window_new(wm.app, wm.root, m.x + (m.w - w) / 2,
		    m.y + (m.h - h) / 2, w, h, 1, ExposureMask);
		wm.cyc.s = rui_surf_new(wm.app, wm.cyc.win, w, h);
		XMapRaised(wm.dpy, wm.cyc.win);
	}
	wm.cyc.sel = (wm.cyc.sel + dir + wm.cyc.n) % wm.cyc.n;
	if (wm.cyc.sel < 0)
		wm.cyc.sel = 0;
	cycle_draw();
}

static int
keycode_is_mod(KeyCode kc, unsigned mask)
{
	XModifierKeymap *mk = XGetModifierMapping(wm.dpy);
	int i, k, res = 0;

	if (!mk)
		return 0;
	for (i = 0; i < 8; i++) {
		if (!(mask & (1u << i)))
			continue;
		for (k = 0; k < mk->max_keypermod; k++)
			if (mk->modifiermap[i * mk->max_keypermod + k] == kc && kc)
				res = 1;
	}
	XFreeModifiermap(mk);
	return res;
}

/* ------------------------------------------------------------- menus */

static void
window_menu(Client *c, int x, int y)
{
	RuiMenu *m, *sub;
	const RuiMenuItem *it;
	char label[64];
	int i, id;

	if (c->type == T_DOCK || c->type == T_DESKTOP)
		return;
	m = rui_menu_new();
	rui_menu_add(m, c->maxed == MAX_BOTH ? "Restore" : "Maximize", NULL, 1);
	rui_menu_add(m, c->hidden ? "Restore" : "Minimize", NULL, 2);
	rui_menu_add(m, c->shaded ? "Unshade" : "Shade", NULL, 3);
	rui_menu_add(m, "Sticky", NULL, 4)->checked = c->sticky;
	rui_menu_add(m, "Always on Top", NULL, 5)->checked = c->above;
	rui_menu_add(m, "Floating (tiling)", NULL, 7)->checked = c->floating;
	sub = rui_menu_new();
	for (i = 0; i < wm.ndesk; i++) {
		snprintf(label, sizeof label, "Workspace %d", i + 1);
		rui_menu_add(sub, label, NULL, 100 + i)->checked =
		    !c->sticky && c->desk == (unsigned long)i;
	}
	rui_menu_add_sub(m, "Send To", sub);
	sub = rui_menu_new();
	rui_menu_add(sub, "Left Half", NULL, 200 + SNAP_LEFT);
	rui_menu_add(sub, "Right Half", NULL, 200 + SNAP_RIGHT);
	rui_menu_add(sub, "Top Half", NULL, 200 + SNAP_TOP);
	rui_menu_add(sub, "Bottom Half", NULL, 200 + SNAP_BOTTOM);
	rui_menu_add_sep(sub);
	rui_menu_add(sub, "Top Left", NULL, 200 + SNAP_TL);
	rui_menu_add(sub, "Top Right", NULL, 200 + SNAP_TR);
	rui_menu_add(sub, "Bottom Left", NULL, 200 + SNAP_BL);
	rui_menu_add(sub, "Bottom Right", NULL, 200 + SNAP_BR);
	rui_menu_add_sep(sub);
	rui_menu_add(sub, "Center", NULL, 250);
	if (wm.nmons > 1)
		rui_menu_add(sub, "Next Monitor", NULL, 251);
	rui_menu_add_sub(m, "Move", sub);
	rui_menu_add_sep(m);
	rui_menu_add(m, "Close", NULL, 6);
	it = rui_menu_popup(wm.app, m, x, y, 0);
	id = it ? it->id : 0;
	rui_menu_free(m);
	if (!id || !client_alive(c))
		return;
	switch (id) {
	case 1:
		set_maximized(c, c->maxed == MAX_BOTH ? 0 : MAX_BOTH);
		break;
	case 2:
		if (c->hidden)
			activate(c);
		else
			set_hidden(c, 1);
		break;
	case 3:
		set_shaded(c, !c->shaded);
		break;
	case 4:
		set_sticky(c, !c->sticky);
		break;
	case 5:
		c->above = !c->above;
		update_net_state(c);
		restack();
		break;
	case 6:
		close_client(c);
		break;
	case 7:
		c->floating = !c->floating;
		arrange();
		break;
	default:
		if (id == 250)
			center_client(c);
		else if (id == 251)
			to_monitor(c, "next");
		else if (id >= 200)
			snap_client(c, id - 200);
		else if (id >= 100)
			set_desktop(c, (unsigned long)(id - 100));
		break;
	}
}

static void
root_menu(int x, int y)
{
	char *path = rui_config_path("menu.conf");
	RuiMenu *m = rui_menu_load(path);
	const RuiMenuItem *it = rui_menu_popup(wm.app, m, x, y, 0);
	char *cmd = it && it->cmd ? rui_strdup(it->cmd) : NULL;

	free(path);
	rui_menu_free(m);
	if (cmd)
		rui_run_command(wm.app, cmd);
	free(cmd);
}

/* ---------------------------------------------------------------- actions */

static void
do_action(int action, int arg, const char *cmd, unsigned mod)
{
	Client *c = wm.focused;

	switch (action) {
	case ACT_CLOSE:
		if (c)
			close_client(c);
		break;
	case ACT_MAXIMIZE:
		if (c)
			set_maximized(c, c->maxed == MAX_BOTH ? 0 : MAX_BOTH);
		break;
	case ACT_MINIMIZE:
		if (c)
			set_hidden(c, 1);
		break;
	case ACT_SHADE:
		if (c)
			set_shaded(c, !c->shaded);
		break;
	case ACT_STICKY:
		if (c)
			set_sticky(c, !c->sticky);
		break;
	case ACT_FULLSCREEN:
		if (c)
			set_fullscreen(c, !c->fullscreen);
		break;
	case ACT_ABOVE:
		if (c) {
			c->above = !c->above;
			update_net_state(c);
			restack();
		}
		break;
	case ACT_LOWER:
		if (c) {
			lower_client(c);
			focus_top();
		}
		break;
	case ACT_WINDOW_MENU:
		if (c)
			window_menu(c, c->x + bw_of(c), c->y + bw_of(c) + th_of(c));
		break;
	case ACT_CYCLE:
		cycle(1, mod);
		break;
	case ACT_CYCLE_BACK:
		cycle(-1, mod & ~(unsigned)ShiftMask);
		break;
	case ACT_WORKSPACE:
		switch_desktop((unsigned long)(arg - 1));
		break;
	case ACT_SENDTO:
		if (c && arg >= 1)
			set_desktop(c, (unsigned long)(arg - 1));
		break;
	case ACT_NEXT_WS:
		switch_desktop((wm.cur + 1) % (unsigned long)wm.ndesk);
		break;
	case ACT_PREV_WS:
		switch_desktop((wm.cur + (unsigned long)wm.ndesk - 1) %
		    (unsigned long)wm.ndesk);
		break;
	case ACT_EXEC:
		rui_run_command(wm.app, cmd);
		break;
	case ACT_TOGGLE_TILE:
		set_layout_named("tile");
		break;
	case ACT_LAYOUT:
		if (cmd)
			set_layout_named(cmd);
		break;
	case ACT_NEXT_LAYOUT:
		if (wm.nlayouts)
			set_layout((wm.layout[wm.cur] + 1) % wm.nlayouts);
		break;
	case ACT_TOGGLE_FLOAT:
		if (c) {
			c->floating = !c->floating;
			arrange();
		}
		break;
	case ACT_MASTER_GROW:
	case ACT_MASTER_SHRINK:
		wm.mfact[wm.cur] += action == ACT_MASTER_GROW ? 5 : -5;
		if (wm.mfact[wm.cur] < 15)
			wm.mfact[wm.cur] = 15;
		if (wm.mfact[wm.cur] > 85)
			wm.mfact[wm.cur] = 85;
		arrange();
		break;
	case ACT_SWAP_MASTER:
		if (c && c->tiled) {
			vec_front(&wm.clients, c);
			arrange();
			update_client_list();
		}
		break;
	case ACT_SNAP: {
		int i, where = arg > 0 && arg < SNAP_COUNT ? arg : 0;

		for (i = 1; cmd && i < SNAP_COUNT; i++)
			if (!strcmp(cmd, snap_names[i]))
				where = i;
		if (c && where)
			snap_client(c, where);
		break;
	}
	case ACT_MOVE:
	case ACT_RESIZE:
		if (c)
			nudge(c, cmd, action == ACT_RESIZE);
		break;
	case ACT_CENTER:
		if (c)
			center_client(c);
		break;
	case ACT_TO_MONITOR:
		to_monitor(c, cmd);
		break;
	case ACT_DND:
		set_dnd(cmd ? on_off(cmd) : arg);
		break;
	case ACT_AWAKE:
		set_awake(cmd ? on_off(cmd) : arg);
		break;
	case ACT_QUIT:
		wm.running = 0;
		break;
	case ACT_RESTART:
		wm.running = 0;
		wm.restart = 1;
		break;
	default:
		break;
	}
}

/* ----------------------------------------------------------- key bindings */

static int
parse_bind(const char *spec, Bind *b)
{
	char buf[512], *tok, *rest, *plus, *act;
	size_t i;

	memset(b, 0, sizeof *b);
	rui_strlcpy(buf, spec, sizeof buf);
	tok = buf + strspn(buf, " \t");
	rest = tok + strcspn(tok, " \t");
	if (*rest)
		*rest++ = '\0';
	rest += strspn(rest, " \t");
	while ((plus = strchr(tok, '+')) && plus[1]) {
		*plus = '\0';
		if (!strcmp(tok, "Shift"))
			b->mod |= ShiftMask;
		else if (!strcmp(tok, "Control") || !strcmp(tok, "Ctrl"))
			b->mod |= ControlMask;
		else if (!strcmp(tok, "Mod1") || !strcmp(tok, "Alt"))
			b->mod |= Mod1Mask;
		else if (!strcmp(tok, "Mod4") || !strcmp(tok, "Super") ||
		    !strcmp(tok, "Win"))
			b->mod |= Mod4Mask;
		else if (!strcmp(tok, "Mod2"))
			b->mod |= Mod2Mask;
		else if (!strcmp(tok, "Mod3"))
			b->mod |= Mod3Mask;
		else if (!strcmp(tok, "Mod5"))
			b->mod |= Mod5Mask;
		else
			return 0;
		tok = plus + 1;
	}
	b->ks = XStringToKeysym(tok);
	if (b->ks == NoSymbol)
		return 0;
	act = rest;
	rest = act + strcspn(act, " \t");
	if (*rest)
		*rest++ = '\0';
	rest += strspn(rest, " \t");
	for (i = 0; i < sizeof action_names / sizeof action_names[0]; i++)
		if (!strcmp(act, action_names[i].name))
			b->action = action_names[i].action;
	if (b->action == ACT_NONE)
		return 0;
	if (b->action == ACT_EXEC || b->action == ACT_LAYOUT ||
	    b->action == ACT_SNAP || b->action == ACT_MOVE ||
	    b->action == ACT_RESIZE) {
		if (!*rest)
			return 0;
		b->cmd = rui_strdup(rest);
	} else if (b->action == ACT_TO_MONITOR || b->action == ACT_DND ||
	    b->action == ACT_AWAKE) {
		if (*rest)
			b->cmd = rui_strdup(rest);     /* optional argument */
	} else if (b->action == ACT_WORKSPACE || b->action == ACT_SENDTO) {
		b->arg = atoi(rest);
		if (b->arg < 1)
			return 0;
	}
	return 1;
}

static void
remove_bind(unsigned mod, KeySym ks)
{
	int i;

	for (i = 0; i < wm.nbinds; i++) {
		if (wm.binds[i].mod == mod && wm.binds[i].ks == ks) {
			free(wm.binds[i].cmd);
			memmove(&wm.binds[i], &wm.binds[i + 1],
			    (size_t)(wm.nbinds - i - 1) * sizeof *wm.binds);
			wm.nbinds--;
			i--;
		}
	}
}

static void
add_bind(const char *spec, const char *origin)
{
	Bind b;

	if (!parse_bind(spec, &b)) {
		rui_warn("%s: bad key binding '%s'", origin, spec);
		return;
	}
	remove_bind(b.mod, b.ks);
	wm.binds = rui_realloc(wm.binds, (size_t)(wm.nbinds + 1) * sizeof *wm.binds);
	wm.binds[wm.nbinds++] = b;
}

static unsigned
parse_mod(const char *s)
{
	if (!strcmp(s, "Mod4") || !strcmp(s, "Super") || !strcmp(s, "Win"))
		return Mod4Mask;
	if (!strcmp(s, "Control") || !strcmp(s, "Ctrl"))
		return ControlMask;
	return Mod1Mask;
}

static void
grab_keys(void)
{
	unsigned locks[4];
	int i, j;
	XModifierKeymap *mk = XGetModifierMapping(wm.dpy);
	KeyCode nl = XKeysymToKeycode(wm.dpy, XK_Num_Lock);

	wm.numlock = 0;
	if (mk) {
		for (i = 0; i < 8; i++)
			for (j = 0; j < mk->max_keypermod; j++)
				if (nl && mk->modifiermap[i * mk->max_keypermod + j] == nl)
					wm.numlock = 1u << i;
		XFreeModifiermap(mk);
	}
	locks[0] = 0;
	locks[1] = LockMask;
	locks[2] = wm.numlock;
	locks[3] = wm.numlock | LockMask;
	XUngrabKey(wm.dpy, AnyKey, AnyModifier, wm.root);
	for (i = 0; i < wm.nbinds; i++) {
		KeyCode kc = XKeysymToKeycode(wm.dpy, wm.binds[i].ks);

		if (!kc)
			continue;
		for (j = 0; j < 4; j++)
			XGrabKey(wm.dpy, kc, wm.binds[i].mod | locks[j], wm.root, True,
			    GrabModeAsync, GrabModeAsync);
	}
}

static void
wm_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "workspaces")) {
		wm.ndesk = atoi(val);
		if (wm.ndesk < 1)
			wm.ndesk = 1;
		if (wm.ndesk > 32)
			wm.ndesk = 32;
	} else if (!strcmp(key, "desktop_icons")) {
		wm.icons_on = !strcmp(val, "auto") ? wm.motif : rui_conf_bool(val);
	} else if (!strcmp(key, "focus")) {
		wm.sloppy = !strcmp(val, "sloppy");
	} else if (!strcmp(key, "titlebar_doubleclick")) {
		wm.dclick_shade = !strcmp(val, "shade");
	} else if (!strcmp(key, "border_width")) {
		wm.bw = atoi(val);
		if (wm.bw < 2)
			wm.bw = 2;
		if (wm.bw > 16)
			wm.bw = 16;
	} else if (!strcmp(key, "mouse_mod")) {
		wm.mousemod = parse_mod(val);
	} else if (!strcmp(key, "tile_master")) {
		int i, f = atoi(val);

		if (f < 15)
			f = 15;
		if (f > 85)
			f = 85;
		for (i = 0; i < 32; i++)
			wm.mfact[i] = f;
	} else if (!strcmp(key, "tile") || !strcmp(key, "layout")) {
		int i, idx = strcmp(key, "tile") ? layout_index(val) :
		    rui_conf_bool(val) ? layout_index("tile") : 0;

		if (idx < 0) {
			rui_warn("retro.conf: unknown layout '%s'", val);
			return;
		}
		for (i = 0; i < 32; i++)
			wm.layout[i] = idx;
	} else if (!strcmp(key, "snap_distance")) {
		wm.snap_dist = atoi(val);
		if (wm.snap_dist < 0)
			wm.snap_dist = 0;
		if (wm.snap_dist > 64)
			wm.snap_dist = 64;
	} else if (!strcmp(key, "edge_snap")) {
		wm.edge_snap = rui_conf_bool(val);
	} else if (!strcmp(key, "move_step")) {
		wm.move_step = atoi(val) > 0 ? atoi(val) : 32;
	} else if (!strcmp(key, "placement")) {
		wm.placement = !strcmp(val, "center") ? PLACE_CENTER :
		    !strcmp(val, "pointer") ? PLACE_POINTER :
		    !strcmp(val, "smart") ? PLACE_SMART : PLACE_CASCADE;
	} else if (!strcmp(key, "focus_new")) {
		wm.focus_new = rui_conf_bool(val);
	} else if (!strcmp(key, "rule")) {
		add_rule(val, "config");
	} else if (!strcmp(key, "dnd")) {
		wm.dnd = rui_conf_bool(val);       /* the state at login */
	} else if (!strcmp(key, "keep_awake")) {
		wm.awake = rui_conf_bool(val);
	} else if (!strcmp(key, "dnd_on_cmd") || !strcmp(key, "dnd_off_cmd")) {
		int on = !strcmp(key, "dnd_on_cmd");

		free(wm.dnd_cmd[on]);
		wm.dnd_cmd[on] = rui_strdup(val);
	} else if (!strcmp(key, "bind")) {
		add_bind(val, "retro.conf");
	} else if (!strcmp(key, "unbind")) {
		Bind b;
		char spec[300];

		snprintf(spec, sizeof spec, "%s close", val);
		if (parse_bind(spec, &b))
			remove_bind(b.mod, b.ks);
		else
			rui_warn("retro.conf: bad unbind '%s'", val);
	}
}

/* ----------------------------------------------------------------- events */

static void
on_button_press(XButtonEvent *e)
{
	Client *c = client_from_win(e->window);
	int btn, edges, in_title;

	wm.last_time = e->time;
	if (!c) {
		if (e->window == wm.root && e->subwindow == None &&
		    e->button == Button3)
			root_menu(e->x_root, e->y_root);
		return;
	}
	if (e->window == c->win) {
		/* passive grab on the client: click-to-focus and Mod+drag */
		if (CLEANMASK(e->state) == wm.mousemod && focusable(c) &&
		    (e->button == Button1 || e->button == Button3)) {
			raise_client(c);
			focus(c);
			if (e->button == Button1) {
				drag_start(c, DRAG_MOVE, 0, e->x_root, e->y_root, e->time);
			} else {
				edges = (e->x < c->w / 2 ? E_LEFT : E_RIGHT) |
				    (e->y < c->h / 2 ? E_TOP : E_BOTTOM);
				drag_start(c, DRAG_RESIZE, edges, e->x_root, e->y_root,
				    e->time);
			}
			if (wm.drag.mode == DRAG_NONE)
				XAllowEvents(wm.dpy, AsyncPointer, e->time);
			return;
		}
		if (e->button <= Button3) {
			if (c->type == T_DESKTOP) {
				focus(NULL);
			} else if (c->type != T_DOCK) {
				if (wm.focused != c)
					focus(c);
				raise_client(c);
			}
		}
		XAllowEvents(wm.dpy, ReplayPointer, e->time);
		return;
	}
	/* press on the frame decoration */
	if (e->button <= Button3 && focusable(c)) {
		if (e->button != Button2)
			raise_client(c);
		if (wm.focused != c)
			focus(c);
	}
	btn = button_at(c, e->x, e->y);
	edges = edges_at(c, e->x, e->y);
	in_title = has_deco(c) && e->y >= wm.bw && e->y < wm.bw + wm.th && !edges;
	switch (e->button) {
	case Button1:
		if (btn != BTN_NONE) {
			wm.btn_c = c;
			wm.btn = btn;
			wm.btn_in = 1;
			draw_frame(c);
		} else if (edges) {
			drag_start(c, DRAG_RESIZE, edges, e->x_root, e->y_root, e->time);
		} else if (in_title) {
			if (wm.dc_win == c->frame && e->time - wm.dc_time < 400) {
				wm.dc_win = None;
				if (wm.dclick_shade)
					set_shaded(c, !c->shaded);
				else
					set_maximized(c, c->maxed == MAX_BOTH ? 0 : MAX_BOTH);
			} else {
				wm.dc_win = c->frame;
				wm.dc_time = e->time;
				drag_start(c, DRAG_MOVE, 0, e->x_root, e->y_root, e->time);
			}
		}
		break;
	case Button2:
		if (in_title) {
			lower_client(c);
			focus_top();
		}
		break;
	case Button3:
		if (in_title && btn == BTN_NONE)
			window_menu(c, e->x_root, e->y_root);
		break;
	case Button4:
		if (in_title)
			set_shaded(c, 1);
		break;
	case Button5:
		if (in_title)
			set_shaded(c, 0);
		break;
	default:
		break;
	}
}

static void
on_button_release(XButtonEvent *e)
{
	wm.last_time = e->time;
	if (wm.drag.mode != DRAG_NONE) {
		drag_end();
		return;
	}
	if (wm.btn_c && e->button == Button1) {
		Client *c = wm.btn_c;
		int btn = wm.btn, hit;

		wm.btn_c = NULL;
		wm.btn = BTN_NONE;
		if (!client_alive(c))
			return;
		hit = e->window == c->frame && button_at(c, e->x, e->y) == btn;
		draw_frame(c);
		if (!hit)
			return;
		switch (btn) {
		case BTN_CLOSE:
			close_client(c);
			break;
		case BTN_MAX:
			set_maximized(c, c->maxed == MAX_BOTH ? 0 : MAX_BOTH);
			break;
		case BTN_MIN:
			set_hidden(c, 1);
			break;
		case BTN_MENU: {
			/* Motif: a second click on the menu button closes the window */
			RuiRect br = btn_rect(c, BTN_MENU);
			long long t0 = rui_now_ms();

			rui_menu_outside_ms = 0;
			window_menu(c, c->x + wm.bw, c->y + wm.bw + wm.th);
			if (wm.motif && client_alive(c) && rui_menu_outside_ms &&
			    rui_menu_outside_ms - t0 < 600 &&
			    rui_menu_outside_x >= c->x + br.x &&
			    rui_menu_outside_x < c->x + br.x + br.w &&
			    rui_menu_outside_y >= c->y + br.y &&
			    rui_menu_outside_y < c->y + br.y + br.h)
				close_client(c);
			break;
		}
		default:
			break;
		}
	}
}

static void
on_icon_press(Client *c, XButtonEvent *e)
{
	wm.last_time = e->time;
	if (e->button == Button1) {
		if (wm.dc_win == c->iconwin && e->time - wm.dc_time < 400) {
			wm.dc_win = None;
			activate(c);                       /* double click: restore */
		} else {
			wm.dc_win = c->iconwin;
			wm.dc_time = e->time;
		}
	} else if (e->button == Button3) {
		window_menu(c, e->x_root, e->y_root);
	}
}

static void
on_motion(XMotionEvent *e)
{
	Client *c;

	while (XCheckTypedEvent(wm.dpy, MotionNotify, (XEvent *)e))
		;
	wm.last_time = e->time;
	if (wm.drag.mode != DRAG_NONE) {
		drag_motion(e->x_root, e->y_root);
		return;
	}
	c = client_from_win(e->window);
	if (!c || e->window != c->frame)
		return;
	if (wm.btn_c == c) {
		int in = button_at(c, e->x, e->y) == wm.btn;

		if (in != wm.btn_in) {
			wm.btn_in = in;
			draw_frame(c);
		}
		return;
	}
	if (!(e->state & (Button1Mask | Button2Mask | Button3Mask))) {
		int edges = edges_at(c, e->x, e->y);

		if (edges != c->cur_edges) {
			c->cur_edges = edges;
			XDefineCursor(wm.dpy, c->frame, wm.curs[edges & 15]);
		}
	}
}

static void
on_key_press(XKeyEvent *e)
{
	KeySym ks = XLookupKeysym(e, 0);
	unsigned mod = CLEANMASK(e->state);
	int i;

	wm.last_time = e->time;
	if (wm.cyc.active && ks == XK_Escape) {
		cycle_end(0);
		return;
	}
	for (i = 0; i < wm.nbinds; i++) {
		Bind *b = &wm.binds[i];

		if (b->ks != ks || b->mod != mod)
			continue;
		if (wm.cyc.active && b->action != ACT_CYCLE &&
		    b->action != ACT_CYCLE_BACK)
			return;
		do_action(b->action, b->arg, b->cmd, b->mod);
		return;
	}
}

static void
on_key_release(XKeyEvent *e)
{
	wm.last_time = e->time;
	if (wm.cyc.active && keycode_is_mod((KeyCode)e->keycode, wm.cyc.mod))
		cycle_end(1);
}

static Bool
is_destroy_of(Display *dpy, XEvent *ev, XPointer arg)
{
	Window w;

	(void)dpy;
	memcpy(&w, arg, sizeof w);
	return ev->type == DestroyNotify && ev->xdestroywindow.window == w;
}

static void
on_unmap(XUnmapEvent *e)
{
	Client *c = client_from_win(e->window);
	XEvent d;
	int destroyed;

	if (!c || e->window != c->win)
		return;
	/* real unmaps arrive via the frame; synthetic ones ask to withdraw */
	if (e->event != c->frame && !e->send_event)
		return;
	XGrabServer(wm.dpy);
	XSync(wm.dpy, False);
	destroyed = XCheckIfEvent(wm.dpy, &d, is_destroy_of, (XPointer)&c->win);
	unmanage(c, destroyed, 1);
	XUngrabServer(wm.dpy);
}

static void
on_configure_request(XConfigureRequestEvent *e)
{
	Client *c = client_from_win(e->window);
	XWindowChanges wc;

	if (!c || e->window != c->win) {
		if (c)
			return;
		wc.x = e->x;
		wc.y = e->y;
		wc.width = e->width;
		wc.height = e->height;
		wc.border_width = e->border_width;
		wc.sibling = e->above;
		wc.stack_mode = e->detail;
		XConfigureWindow(wm.dpy, e->window, (unsigned)e->value_mask, &wc);
		return;
	}
	if (e->value_mask & CWBorderWidth)
		c->old_bw = e->border_width;
	if (!c->fullscreen && !c->maxed && !c->tiled) {
		if (e->value_mask & CWX)
			c->x = e->x;
		if (e->value_mask & CWY)
			c->y = e->y;
		if (e->value_mask & CWWidth)
			c->w = e->width > 0 ? e->width : 1;
		if (e->value_mask & CWHeight)
			c->h = e->height > 0 ? e->height : 1;
		apply_geometry(c);
		/* a panel moved (to another monitor, say): its strut now eats
		 * into a different monitor even if the numbers stayed the same */
		if (c->strut[0] || c->strut[1] || c->strut[2] || c->strut[3]) {
			wm.workarea.w = -1;
			update_workarea();
		}
	} else {
		send_configure(c);
	}
	if ((e->value_mask & CWStackMode) && !(e->value_mask & CWSibling)) {
		if (e->detail == Above)
			raise_client(c);
		else if (e->detail == Below)
			lower_client(c);
	}
}

static void
on_property(XPropertyEvent *e)
{
	Client *c = client_from_win(e->window);

	if (!c || e->window != c->win)
		return;
	if (e->atom == XA_WM_NAME || e->atom == A(_NET_WM_NAME)) {
		fetch_name(c);
		draw_frame(c);
		if (c->iconwin) {
			XStoreName(wm.dpy, c->iconwin, c->name);
			icon_draw(c);
		}
	} else if (e->atom == XA_WM_NORMAL_HINTS) {
		fetch_size_hints(c);
		update_allowed(c);
	} else if (e->atom == XA_WM_HINTS) {
		int was = c->urgent;

		fetch_wm_hints(c, NULL);
		if (was != c->urgent) {
			update_net_state(c);
			draw_frame(c);
		}
	} else if (e->atom == A(WM_PROTOCOLS)) {
		fetch_protocols(c);
	} else if (e->atom == XA_WM_TRANSIENT_FOR) {
		fetch_transient(c);
		arrange();
	} else if (e->atom == A(_NET_WM_STRUT) ||
	    e->atom == A(_NET_WM_STRUT_PARTIAL)) {
		if (fetch_strut(c))
			update_workarea();
	} else if (e->atom == A(_MOTIF_WM_HINTS)) {
		int was = c->decorated;

		fetch_motif(c);
		if (was != c->decorated) {
			if (!c->decorated)
				c->shaded = 0;
			apply_geometry(c);
		}
	}
}

static void
net_wm_state(Client *c, long action, Atom prop, int *maxbits)
{
	int *flag = NULL, val;

	if (prop == A(_NET_WM_STATE_MAXIMIZED_VERT) ||
	    prop == A(_NET_WM_STATE_MAXIMIZED_HORZ)) {
		int bit = prop == A(_NET_WM_STATE_MAXIMIZED_VERT) ? MAX_VERT : MAX_HORZ;

		if (action == 1 || (action == 2 && !(c->maxed & bit)))
			*maxbits |= bit;
		else
			*maxbits &= ~bit;
		return;
	}
	if (prop == A(_NET_WM_STATE_FULLSCREEN))
		flag = &c->fullscreen;
	else if (prop == A(_NET_WM_STATE_SHADED))
		flag = &c->shaded;
	else if (prop == A(_NET_WM_STATE_STICKY))
		flag = &c->sticky;
	else if (prop == A(_NET_WM_STATE_HIDDEN))
		flag = &c->hidden;
	else if (prop == A(_NET_WM_STATE_ABOVE))
		flag = &c->above;
	else if (prop == A(_NET_WM_STATE_BELOW))
		flag = &c->below;
	else if (prop == A(_NET_WM_STATE_SKIP_TASKBAR))
		flag = &c->skip_taskbar;
	else if (prop == A(_NET_WM_STATE_SKIP_PAGER))
		flag = &c->skip_pager;
	else if (prop == A(_NET_WM_STATE_MODAL))
		flag = &c->modal;
	else if (prop == A(_NET_WM_STATE_DEMANDS_ATTENTION))
		flag = &c->urgent;
	if (!flag)
		return;
	val = action == 1 ? 1 : action == 0 ? 0 : !*flag;
	if (flag == &c->fullscreen)
		set_fullscreen(c, val);
	else if (flag == &c->shaded)
		set_shaded(c, val);
	else if (flag == &c->sticky)
		set_sticky(c, val);
	else if (flag == &c->hidden)
		set_hidden(c, val);
	else {
		*flag = val;
		update_net_state(c);
		restack();
		draw_frame(c);
	}
}

/* ------------------------------------------------------------------- bus */

/* "wm" on retro-bus: every action of keys.conf by name (close, maximize,
 * workspace 2, snap left, dnd on ...), plus windows, focus ID, close_window
 * ID and actions.  Events: wm.focus, wm.workspace, wm.dnd. */
static void
bus_event(const char *topic, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (rui_bus_fd() < 0)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	rui_bus_emit(topic, buf);
}

static char *
class_of(Window w)
{
	XClassHint ch;
	char *s;

	memset(&ch, 0, sizeof ch);
	if (!XGetClassHint(wm.dpy, w, &ch))
		return rui_strdup("");
	s = rui_strdup(ch.res_class ? ch.res_class : "");
	if (ch.res_name)
		XFree(ch.res_name);
	if (ch.res_class)
		XFree(ch.res_class);
	return s;
}

static Client *
bus_client(const char *arg)
{
	char *end;
	unsigned long w = strtoul(arg, &end, 0);

	return *arg && !*end ? client_from_win((Window)w) : NULL;
}

static void
on_bus(const RuiBusMsg *m, void *ud)
{
	const char *args = m->args;
	Client *c;
	size_t i;

	(void)ud;
	if (strcmp(m->kind, "call"))
		return;
	if (!strcmp(m->name, "windows")) {
		size_t len = 0, cap = 1024;
		char *out = rui_malloc(cap);
		int k;

		out[0] = '\0';
		for (k = 0; k < wm.clients.n; k++) {
			char *cls, *line;

			c = wm.clients.v[k];
			cls = class_of(c->win);
			line = rui_strdupf("%s0x%lx\t%ld\t%s\t%s%s", len ? "\n" : "",
			    (unsigned long)c->win, c->sticky ? -1L : (long)c->desk + 1,
			    cls, c->name ? c->name : "", c == wm.focused ? "\t*" : "");
			if (len + strlen(line) + 1 > cap)
				out = rui_realloc(out, cap = (len + strlen(line)) * 2 + 1);
			strcpy(out + len, line);
			len += strlen(line);
			free(line);
			free(cls);
		}
		rui_bus_reply(m->id, 1, out);
		free(out);
		return;
	}
	if (!strcmp(m->name, "focus") || !strcmp(m->name, "close_window")) {
		if (!(c = bus_client(args))) {
			rui_bus_reply(m->id, 0, "no such window");
			return;
		}
		if (m->name[0] == 'c')
			close_client(c);
		else {
			if (c->hidden)
				set_hidden(c, 0);
			if (!is_visible(c))
				switch_desktop(c->desk);
			if (focusable(c))
				activate(c);
		}
		rui_bus_reply(m->id, 1, "");
		XFlush(wm.dpy);
		return;
	}
	if (!strcmp(m->name, "actions")) {
		char out[1024] = "";

		for (i = 0; i < sizeof action_names / sizeof action_names[0]; i++) {
			if (action_names[i].action == ACT_EXEC)
				continue;
			if (*out)
				rui_strlcpy(out + strlen(out), "\n", sizeof out - strlen(out));
			rui_strlcpy(out + strlen(out), action_names[i].name,
			    sizeof out - strlen(out));
		}
		rui_bus_reply(m->id, 1, out);
		return;
	}
	for (i = 0; i < sizeof action_names / sizeof action_names[0]; i++) {
		if (strcmp(m->name, action_names[i].name) ||
		    action_names[i].action == ACT_EXEC)
			continue;
		do_action(action_names[i].action, atoi(args), *args ? args : NULL,
		    0);
		rui_bus_reply(m->id, 1, "");
		XFlush(wm.dpy);
		return;
	}
	rui_bus_reply(m->id, 0, "no such method");
}

static void
on_client_message(XClientMessageEvent *e)
{
	Client *c = client_from_win(e->window);
	Atom t = e->message_type;

	if (t == A(_RETRO_WM_COMMAND)) {
		char *name = XGetAtomName(wm.dpy, (Atom)e->data.l[0]);
		size_t i;

		if (!name)
			return;
		for (i = 0; i < sizeof action_names / sizeof action_names[0]; i++)
			if (!strcmp(name, action_names[i].name) &&
			    action_names[i].action != ACT_EXEC)
				do_action(action_names[i].action, (int)e->data.l[1], NULL, 0);
		XFree(name);
		return;
	}
	if (t == A(_NET_CURRENT_DESKTOP)) {
		switch_desktop((unsigned long)e->data.l[0]);
		return;
	}
	if (!c)
		return;
	if (t == A(_NET_ACTIVE_WINDOW)) {
		/* do not disturb: programs may not pull themselves to the front
		 * (source 2 is the panel or a pager, that is the user) */
		if (wm.dnd && e->data.l[0] != 2 && c != wm.focused)
			return;
		if (focusable(c))
			activate(c);
	} else if (t == A(_NET_CLOSE_WINDOW)) {
		close_client(c);
	} else if (t == A(_NET_WM_STATE)) {
		int maxbits = c->maxed;

		if (e->data.l[1])
			net_wm_state(c, e->data.l[0], (Atom)e->data.l[1], &maxbits);
		if (e->data.l[2] && client_alive(c))
			net_wm_state(c, e->data.l[0], (Atom)e->data.l[2], &maxbits);
		if (client_alive(c) && maxbits != c->maxed)
			set_maximized(c, maxbits);
	} else if (t == A(_NET_WM_DESKTOP)) {
		set_desktop(c, (unsigned long)e->data.l[0] & 0xffffffffUL);
	} else if (t == A(WM_CHANGE_STATE)) {
		if (e->data.l[0] == IconicState)
			set_hidden(c, 1);
	} else if (t == A(_NET_MOVERESIZE_WINDOW)) {
		long flags = e->data.l[0];

		if (c->fullscreen || c->maxed || c->tiled)
			return;
		if (flags & (1 << 8))
			c->x = (int)e->data.l[1];
		if (flags & (1 << 9))
			c->y = (int)e->data.l[2];
		if ((flags & (1 << 10)) && e->data.l[3] > 0)
			c->w = (int)e->data.l[3];
		if ((flags & (1 << 11)) && e->data.l[4] > 0)
			c->h = (int)e->data.l[4];
		apply_geometry(c);
	} else if (t == A(_NET_WM_MOVERESIZE)) {
		static const int dir_edges[8] = {
			E_TOP | E_LEFT, E_TOP, E_TOP | E_RIGHT, E_RIGHT,
			E_BOTTOM | E_RIGHT, E_BOTTOM, E_BOTTOM | E_LEFT, E_LEFT
		};
		long dir = e->data.l[2];

		if (dir == 11) {
			drag_end();
		} else if (dir == 8) {
			drag_start(c, DRAG_MOVE, 0, (int)e->data.l[0], (int)e->data.l[1],
			    CurrentTime);
		} else if (dir >= 0 && dir < 8) {
			drag_start(c, DRAG_RESIZE, dir_edges[dir], (int)e->data.l[0],
			    (int)e->data.l[1], CurrentTime);
		}
	} else if (t == A(_NET_REQUEST_FRAME_EXTENTS)) {
		update_frame_extents(c);
	}
}

static void
screen_changed(void)
{
	unsigned long v[2];
	int i;

	wm.app->sw = DisplayWidth(wm.dpy, wm.app->screen);
	wm.app->sh = DisplayHeight(wm.dpy, wm.app->screen);
	update_monitors();
	v[0] = (unsigned long)wm.app->sw;
	v[1] = (unsigned long)wm.app->sh;
	rui_set_prop32(wm.app, wm.root, A(_NET_DESKTOP_GEOMETRY), XA_CARDINAL, v, 2);
	wm.workarea.w = -1;                    /* force a re-layout */
	update_workarea();
	for (i = 0; i < wm.clients.n; i++) {
		Client *c = wm.clients.v[i];

		if (c->fullscreen) {
			RuiRect m = monitor_of(c);

			c->x = m.x;
			c->y = m.y;
			c->w = m.w;
			c->h = m.h;
			apply_geometry(c);
		} else if (focusable(c) && !c->tiled) {
			/* its monitor was unplugged or moved: bring the window back
			 * onto the primary one */
			int cx = c->x + frame_w(c) / 2, cy = c->y + frame_h(c) / 2;
			int k, on = 0;

			for (k = 0; k < wm.nmons; k++)
				on |= rui_in_rect(wm.mons[k], cx, cy);
			if (!on) {
				RuiRect a = mon_area(wm.mons[wm.prim]);

				c->x = a.x + 30;
				c->y = a.y + 30;
				if (c->maxed & MAX_HORZ) {
					c->x = a.x;
					c->w = a.w - 2 * bw_of(c);
				}
				if (c->maxed & MAX_VERT) {
					c->y = a.y;
					c->h = a.h - th_of(c) - 2 * bw_of(c);
				}
				apply_geometry(c);
			}
		}
	}
}

static void
handle_event(XEvent *ev)
{
	Client *c;

	if (rui_monitors_event(wm.app, ev)) {
		screen_changed();                  /* layout, mode or primary */
		return;
	}
	switch (ev->type) {
	case MapRequest:
		c = client_from_win(ev->xmaprequest.window);
		if (c) {
			if (focusable(c))
				activate(c);
		} else {
			manage(ev->xmaprequest.window, 0);
		}
		break;
	case UnmapNotify:
		on_unmap(&ev->xunmap);
		break;
	case DestroyNotify:
		c = client_from_win(ev->xdestroywindow.window);
		if (c && c->win == ev->xdestroywindow.window)
			unmanage(c, 1, 0);
		break;
	case ConfigureRequest:
		on_configure_request(&ev->xconfigurerequest);
		break;
	case ConfigureNotify:
		if (ev->xconfigure.window == wm.root &&
		    (ev->xconfigure.width != wm.app->sw ||
		    ev->xconfigure.height != wm.app->sh))
			screen_changed();
		break;
	case PropertyNotify:
		wm.last_time = ev->xproperty.time;
		if (ev->xproperty.window == wm.root &&
		    ev->xproperty.atom == A(_RETRO_RELOAD)) {
			do_action(ACT_RESTART, 0, NULL, 0);     /* settings changed */
			break;
		}
		on_property(&ev->xproperty);
		break;
	case ClientMessage:
		on_client_message(&ev->xclient);
		break;
	case ButtonPress:
		if ((c = client_from_icon(ev->xbutton.window)))
			on_icon_press(c, &ev->xbutton);
		else
			on_button_press(&ev->xbutton);
		break;
	case ButtonRelease:
		on_button_release(&ev->xbutton);
		break;
	case MotionNotify:
		on_motion(&ev->xmotion);
		break;
	case KeyPress:
		on_key_press(&ev->xkey);
		break;
	case KeyRelease:
		on_key_release(&ev->xkey);
		break;
	case EnterNotify:
		wm.last_time = ev->xcrossing.time;
		if (wm.sloppy && ev->xcrossing.mode == NotifyNormal &&
		    wm.drag.mode == DRAG_NONE) {
			c = client_from_win(ev->xcrossing.window);
			if (c && c != wm.focused && focusable(c))
				focus(c);
		}
		break;
	case Expose:
		if (ev->xexpose.count != 0)
			break;
		if (wm.cyc.active && ev->xexpose.window == wm.cyc.win) {
			rui_surf_flush(wm.cyc.s);
			break;
		}
		if ((c = client_from_icon(ev->xexpose.window))) {
			icon_draw(c);
			break;
		}
		c = client_from_win(ev->xexpose.window);
		if (c && ev->xexpose.window == c->frame)
			draw_frame(c);
		break;
	case MappingNotify:
		XRefreshKeyboardMapping(&ev->xmapping);
		if (ev->xmapping.request != MappingPointer)
			grab_keys();
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

/* ------------------------------------------------------- setup / teardown */

static int other_wm;

static int
on_startup_error(Display *dpy, XErrorEvent *e)
{
	(void)dpy;
	if (e->error_code == BadAccess)
		other_wm = 1;
	return 0;
}

static void
setup_ewmh(void)
{
	unsigned long v[A_COUNT], *names;
	char *deskn, buf[32];
	size_t len = 0, cap = 0;
	int i, n = 0;
	unsigned long val[2];

	for (i = A__NET_SUPPORTED; i <= A__NET_WM_ACTION_CLOSE; i++)
		v[n++] = wm.app->atom[i];
	rui_set_prop32(wm.app, wm.root, A(_NET_SUPPORTED), XA_ATOM, v, n);
	val[0] = wm.check;
	rui_set_prop32(wm.app, wm.root, A(_NET_SUPPORTING_WM_CHECK), XA_WINDOW,
	    val, 1);
	rui_set_prop32(wm.app, wm.check, A(_NET_SUPPORTING_WM_CHECK), XA_WINDOW,
	    val, 1);
	XChangeProperty(wm.dpy, wm.check, A(_NET_WM_NAME), A(UTF8_STRING), 8,
	    PropModeReplace, (const unsigned char *)"retro-wm", 8);
	val[0] = (unsigned long)getpid();
	rui_set_prop32(wm.app, wm.check, A(_NET_WM_PID), XA_CARDINAL, val, 1);
	val[0] = (unsigned long)wm.ndesk;
	rui_set_prop32(wm.app, wm.root, A(_NET_NUMBER_OF_DESKTOPS), XA_CARDINAL,
	    val, 1);
	val[0] = wm.cur;
	rui_set_prop32(wm.app, wm.root, A(_NET_CURRENT_DESKTOP), XA_CARDINAL, val,
	    1);
	val[0] = (unsigned long)wm.app->sw;
	val[1] = (unsigned long)wm.app->sh;
	rui_set_prop32(wm.app, wm.root, A(_NET_DESKTOP_GEOMETRY), XA_CARDINAL, val,
	    2);
	names = rui_calloc((size_t)wm.ndesk * 2, sizeof *names);
	rui_set_prop32(wm.app, wm.root, A(_NET_DESKTOP_VIEWPORT), XA_CARDINAL,
	    names, wm.ndesk * 2);
	free(names);
	deskn = NULL;
	for (i = 0; i < wm.ndesk; i++) {
		size_t l;
		const char *nm = rui_settings()->workspace_names;
		int k;

		for (k = 0; k < i && nm; k++)          /* the i-th comma field */
			nm = (nm = strchr(nm, ',')) ? nm + 1 : NULL;
		if (nm && *nm && *nm != ',')
			snprintf(buf, sizeof buf, "%.*s", (int)strcspn(nm, ","), nm);
		else
			snprintf(buf, sizeof buf, "%d", i + 1);
		l = strlen(buf) + 1;

		if (len + l > cap) {
			cap = (len + l) * 2;
			deskn = rui_realloc(deskn, cap);
		}
		memcpy(deskn + len, buf, l);
		len += l;
	}
	XChangeProperty(wm.dpy, wm.root, A(_NET_DESKTOP_NAMES), A(UTF8_STRING), 8,
	    PropModeReplace, (unsigned char *)deskn, (int)len);
	free(deskn);
	set_active_prop();
}

static void
setup(void)
{
	static const unsigned shapes[16] = {
		XC_left_ptr, XC_left_side, XC_right_side, XC_left_ptr,
		XC_top_side, XC_top_left_corner, XC_top_right_corner, XC_top_side,
		XC_bottom_side, XC_bottom_left_corner, XC_bottom_right_corner,
		XC_bottom_side, XC_left_ptr, XC_left_side, XC_right_side, XC_left_ptr
	};
	XSetWindowAttributes swa;
	Window r, p, *kids = NULL;
	unsigned nkids = 0, i;
	unsigned long *v;
	char *path, name[32];
	int k;
	XErrorHandler old;

	wm.dpy = wm.app->dpy;
	wm.root = wm.app->root;
	wm.ndesk = (int)rui_config.workspaces;
	if (wm.ndesk < 1)
		wm.ndesk = 1;
	if (wm.ndesk > 32)
		wm.ndesk = 32;
	init_layouts();
	wm.motif = wm.app->style == RUI_STYLE_CDE;
	wm.icons_on = wm.motif;
	wm.bw = wm.motif ? 6 : 4;
	wm.mousemod = Mod1Mask;
	wm.snap_dist = 10;
	wm.edge_snap = 1;
	wm.move_step = 32;
	wm.focus_new = 1;
	for (k = 0; k < 32; k++)
		wm.mfact[k] = 55;
	for (i = 0; rui_config.keys[i]; i++)
		add_bind(rui_config.keys[i], "config.h");
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, wm_conf, NULL);
	free(path);
	wm.th = wm.app->bold->height + (wm.motif ? 8 : 6);
	if (wm.th < 18)
		wm.th = 18;
	wm.saver_secs = rui_settings()->saver_minutes * 60;
	wm.dpms_secs = rui_settings()->dpms_minutes * 60;
	wm.xss = !strcmp(rui_saver_kind(), "xscreensaver");
	if (!strcmp(rui_saver_kind(), "off"))
		wm.saver_secs = 0;
	/* do not disturb / keep awake survive a restart of the window manager */
	if (rui_get_prop32(wm.app, wm.root, A(_RETRO_DND), &v) > 0)
		wm.dnd = v[0] != 0;
	free(v);
	if (rui_get_prop32(wm.app, wm.root, A(_RETRO_AWAKE), &v) > 0)
		wm.awake = v[0] != 0;
	free(v);
	publish_flags();
	apply_saver();

	/* become the window manager */
	old = XSetErrorHandler(on_startup_error);
	XSelectInput(wm.dpy, wm.root, SubstructureRedirectMask |
	    SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask |
	    ButtonPressMask);
	XSync(wm.dpy, False);
	XSetErrorHandler(old);
	if (other_wm)
		rui_die("another window manager is already running");

	for (i = 0; i < 16; i++)
		wm.curs[i] = XCreateFontCursor(wm.dpy, shapes[i]);
	wm.cur_move = XCreateFontCursor(wm.dpy, XC_fleur);
	XDefineCursor(wm.dpy, wm.root, wm.curs[0]);

	swa.override_redirect = True;
	wm.check = XCreateWindow(wm.dpy, wm.root, -10, -10, 1, 1, 0,
	    CopyFromParent, InputOnly, CopyFromParent, CWOverrideRedirect, &swa);
	snprintf(name, sizeof name, "WM_S%d", wm.app->screen);
	XSetSelectionOwner(wm.dpy, XInternAtom(wm.dpy, name, False), wm.check,
	    CurrentTime);

	/* survive a restart on the same desktop */
	if (rui_get_prop32(wm.app, wm.root, A(_NET_CURRENT_DESKTOP), &v) > 0 &&
	    v[0] < (unsigned long)wm.ndesk)
		wm.cur = v[0];
	free(v);

	update_monitors();
	wm.workarea.x = wm.workarea.y = 0;
	wm.workarea.w = wm.app->sw;
	wm.workarea.h = wm.app->sh;
	setup_ewmh();
	publish_layout();
	update_workarea();
	rui_monitors_watch(wm.app);
	grab_keys();
	wm.app->dispatch = dispatch_cb;

	XGrabServer(wm.dpy);
	if (XQueryTree(wm.dpy, wm.root, &r, &p, &kids, &nkids)) {
		for (i = 0; i < nkids; i++) {
			XWindowAttributes wa;
			int iconic = 0;

			if (kids[i] == wm.check ||
			    !XGetWindowAttributes(wm.dpy, kids[i], &wa) ||
			    wa.override_redirect)
				continue;
			if (wa.map_state != IsViewable) {
				if (rui_get_prop32(wm.app, kids[i], A(WM_STATE), &v) > 0 &&
				    v[0] == IconicState)
					iconic = 1;
				free(v);
				if (!iconic)
					continue;
			}
			manage(kids[i], 1);
		}
		if (kids)
			XFree(kids);
	}
	XUngrabServer(wm.dpy);
	update_client_list();
	focus_top();
}

static void
cleanup(void)
{
	char name[32];
	int i;

	wm.exiting = 1;
	cycle_end(0);
	drag_end();
	/* release bottom-to-top so the stacking order survives */
	while (wm.stack.n > 0)
		unmanage(wm.stack.v[0], 0, 0);
	XUngrabKey(wm.dpy, AnyKey, AnyModifier, wm.root);
	XSetInputFocus(wm.dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(wm.dpy, wm.root, A(_NET_SUPPORTED));
	XDeleteProperty(wm.dpy, wm.root, A(_NET_SUPPORTING_WM_CHECK));
	XDeleteProperty(wm.dpy, wm.root, A(_NET_CLIENT_LIST));
	XDeleteProperty(wm.dpy, wm.root, A(_NET_CLIENT_LIST_STACKING));
	XDeleteProperty(wm.dpy, wm.root, A(_NET_ACTIVE_WINDOW));
	XDeleteProperty(wm.dpy, wm.root, A(_NET_WORKAREA));
	XDeleteProperty(wm.dpy, wm.root, A(_RETRO_LAYOUT));
	if (!wm.restart) {                     /* kept only across a restart */
		XDeleteProperty(wm.dpy, wm.root, A(_NET_CURRENT_DESKTOP));
		XDeleteProperty(wm.dpy, wm.root, A(_RETRO_DND));
		XDeleteProperty(wm.dpy, wm.root, A(_RETRO_AWAKE));
		if (wm.dnd)
			set_dnd(2);                    /* notifications flow again */
	}
	for (i = 0; i < 4; i++)
		if (wm.zone_win[i])
			XDestroyWindow(wm.dpy, wm.zone_win[i]);
	for (i = 0; i < wm.nrules; i++) {
		free(wm.rules[i].cls);
		free(wm.rules[i].name);
		free(wm.rules[i].title);
		free(wm.rules[i].type);
	}
	free(wm.rules);
	free(wm.dnd_cmd[0]);
	free(wm.dnd_cmd[1]);
	snprintf(name, sizeof name, "WM_S%d", wm.app->screen);
	XSetSelectionOwner(wm.dpy, XInternAtom(wm.dpy, name, False), None,
	    CurrentTime);
	XSelectInput(wm.dpy, wm.root, NoEventMask);
	XDestroyWindow(wm.dpy, wm.check);
	for (i = 0; i < 16; i++)
		XFreeCursor(wm.dpy, wm.curs[i]);
	XFreeCursor(wm.dpy, wm.cur_move);
	for (i = 0; i < wm.nbinds; i++)
		free(wm.binds[i].cmd);
	free(wm.binds);
	free(wm.clients.v);
	free(wm.stack.v);
	free(wm.mru.v);
	XSync(wm.dpy, False);
}

int
main(int argc, char **argv)
{
	int xerrors, restart;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-wm " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-wm [-v]\n");
		return 2;
	}
	wm.argv = argv;
	rui_signals_init();
	wm.app = rui_open("retro-wm");
	setup();
	rui_bus_handler(on_bus, NULL);
	if (rui_settings()->bus)
		rui_bus_claim("wm");
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec unix", NULL) == -1)
		rui_die("pledge failed");
#endif
	wm.running = 1;
	while (wm.running && !rui_quit_requested()) {
		XEvent ev;

		while (wm.running && XPending(wm.dpy)) {
			XNextEvent(wm.dpy, &ev);
			handle_event(&ev);
		}
		awake_tick();
		if (wm.running)
			rui_wait(wm.app, wm.awake ? 20000 : -1);
	}
	cleanup();
	restart = wm.restart;
	xerrors = rui_xerror_count();
	rui_close(wm.app);
	if (getenv("RETRO_DEBUG"))
		fprintf(stderr, "retro-wm: exiting, %d X errors\n", xerrors);
	if (restart) {
		execvp(argv[0], argv);
		perror("retro-wm: restart");
		return 1;
	}
	return 0;
}
