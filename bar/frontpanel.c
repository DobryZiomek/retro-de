/*
 * retro-bar front panel: the CDE-style panel used with the CDE style.
 *
 *  [handle][controls...][Lock|One Two |Exit][controls...][status][handle]
 *                       [Lyt |Three Four|busy]
 *
 * Every control has a tab above it that opens its subpanel (a menu).  The
 * status area holds the tray and the applets of the classic taskbar.
 */
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	FPC_CLOCK, FPC_CALENDAR, FPC_FILES, FPC_EDITOR, FPC_TERMINAL, FPC_BROWSER,
	FPC_STYLE, FPC_APPS, FPC_NETWORK, FPC_COUNT
};
static const char *const fc_names[FPC_COUNT] = {
	"clock", "calendar", "files", "editor", "terminal", "browser", "style",
	"apps", "network"
};

/* hotspot ids published in _RETRO_FP_HOTSPOTS (for scripts and tests) */
enum { HS_WORKSPACES = 1, HS_EXIT_TAB, HS_EXIT, HS_LOCK, HS_LAYOUT,
       HS_CONTROL = 100, HS_TAB = 200 };

#define MAX_CONTROLS 12
#define HANDLE_W 12
#define TAB_H 12
#define CTRL_W 58
#define COL_W 42
#define STATUS_W 236

typedef struct Control {
	int kind;
	RuiRect tab, r;
} Control;

static struct {
	Control c[MAX_CONTROLS];
	int n, nleft;
	RuiRect lock, layout, exit_tab, exit, light, ws, sw, status;
	int ws_cols, ws_w, row_h, inner_h, width;
	long long busy_until;
	char left[256], right[256];
	unsigned long hs_sum;
} fp;

/* sin(6 degrees * i) * 1000, for the clock hands */
static const short sin60[60] = {
	0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
	1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208,
	105, 0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866,
	-914, -951, -978, -995, -1000, -995, -978, -951, -914, -866, -809, -743,
	-669, -588, -500, -407, -309, -208, -105
};

/* ------------------------------------------------------------------ setup */

static void
fp_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "fp_left"))
		rui_strlcpy(fp.left, val, sizeof fp.left);
	else if (!strcmp(key, "fp_right"))
		rui_strlcpy(fp.right, val, sizeof fp.right);
}

static void
add_controls(const char *spec)
{
	char buf[256], *tok;
	int k;

	rui_strlcpy(buf, spec, sizeof buf);
	for (tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
		for (k = 0; k < FPC_COUNT; k++)
			if (!strcmp(tok, fc_names[k]))
				break;
		if (k == FPC_COUNT) {
			rui_warn("unknown front panel control '%s'", tok);
			continue;
		}
		if (fp.n < MAX_CONTROLS)
			fp.c[fp.n++].kind = k;
	}
}

void
fp_setup(void)
{
	char *path = rui_config_path("retro.conf");

	rui_strlcpy(fp.left, rui_config.fp_left, sizeof fp.left);
	rui_strlcpy(fp.right, rui_config.fp_right, sizeof fp.right);
	rui_conf_parse(path, fp_conf, NULL);
	free(path);
	add_controls(fp.left);
	fp.nleft = fp.n;
	add_controls(fp.right);
	fp.inner_h = 2 * (bar.app->font->height + 12);
	if (fp.inner_h < 60)
		fp.inner_h = 60;
	fp.row_h = (fp.inner_h - 4) / 2;
}

int
fp_height(void)
{
	return fp.inner_h + 8;
}

int
fp_busy(void)
{
	return fp.busy_until && rui_now_ms() < fp.busy_until + 500;
}

/* ----------------------------------------------------------------- layout */

static void
place_control(Control *c, int *x)
{
	c->tab.x = c->r.x = *x;
	c->tab.y = 4;
	c->tab.w = c->r.w = CTRL_W;
	c->tab.h = TAB_H;
	c->r.y = 4 + TAB_H;
	c->r.h = fp.inner_h - TAB_H;
	*x += CTRL_W + 2;
}

static void
status_layout(void)
{
	RuiRect st = fp.status;
	int rowh = (st.h - 8) / 2, x = st.x + 4, y = st.y + 4, row = 0, i;
	int sz = bar.app->font->height + 8;
	char buf[160];

	if (sz > rowh)
		sz = rowh;
	bar.r_tray.x = x;
	bar.r_tray.y = y;
	bar.r_tray.h = rowh;
	bar.r_tray.w = bar.nicons ? bar.nicons * (sz + 2) + 2 : 0;
	if (bar.nicons)
		x += bar.r_tray.w + 2;
	for (i = 0; i < bar.nap; i++) {
		Applet *ap = &bar.ap[i];
		int w = 0;

		ap->r.w = 0;
		if (ap->kind == AP_CLOCK)
			continue;                      /* the clock is a control here */
		if (ap->kind == AP_CPU) {
			w = bar.have_cpu ? CPU_HIST + 4 : 0;
		} else if (ap->kind == AP_MEM) {
			w = bar.have_mem ? 14 : 0;
		} else {
			ap_text(ap, buf, sizeof buf);
			if (buf[0]) {
				w = rui_text_width(bar.app, bar.app->font, buf, -1) + 12;
				if (w > st.w - 8)
					w = st.w - 8;
			}
		}
		if (!w)
			continue;
		if (x + w > st.x + st.w - 4) {
			if (++row > 1)
				break;                     /* out of room */
			x = st.x + 4;
			y += rowh + 2;
		}
		ap->r.x = x;
		ap->r.y = y;
		ap->r.w = w;
		ap->r.h = rowh;
		x += w + 2;
	}
}

static void
publish_hotspots(void)
{
	unsigned long v[5 * (MAX_CONTROLS * 2 + 6)], sum = 0;
	int n = 0, i;

#define HS(id, R) do { v[n++] = (unsigned long)(id); \
	v[n++] = (unsigned long)(R).x; v[n++] = (unsigned long)(R).y; \
	v[n++] = (unsigned long)(R).w; v[n++] = (unsigned long)(R).h; } while (0)
	HS(HS_WORKSPACES, fp.ws);
	HS(HS_EXIT_TAB, fp.exit_tab);
	HS(HS_EXIT, fp.exit);
	HS(HS_LOCK, fp.lock);
	HS(HS_LAYOUT, fp.layout);
	for (i = 0; i < fp.n; i++) {
		HS(HS_CONTROL + fp.c[i].kind, fp.c[i].r);
		HS(HS_TAB + fp.c[i].kind, fp.c[i].tab);
	}
#undef HS
	for (i = 0; i < n; i++)
		sum = sum * 31 + v[i];
	if (sum == fp.hs_sum)
		return;
	fp.hs_sum = sum;
	rui_set_prop32(bar.app, bar.win, A(_RETRO_FP_HOTSPOTS), XA_CARDINAL, v, n);
}

static void
fp_layout(void)
{
	RuiApp *a = bar.app;
	int x = 3 + HANDLE_W + 4, i, nd = bar.ndesk > 0 ? (int)bar.ndesk : 1;

	fp.ws_cols = (nd + 1) / 2;
	fp.ws_w = 60;
	for (i = 0; i < nd && i < 32; i++) {
		int w = rui_text_width(a, a->bold, bar.desk_names[i], -1) + 18;

		if (w > fp.ws_w)
			fp.ws_w = w;
	}
	for (i = 0; i < fp.nleft; i++)
		place_control(&fp.c[i], &x);
	x += 4;
	/* the switch: lock + layout column, workspace grid, exit column */
	fp.sw.x = x;
	fp.sw.y = 4;
	fp.sw.h = fp.inner_h;
	fp.lock.x = fp.layout.x = x + 3;
	fp.lock.y = 6;
	fp.lock.w = fp.layout.w = COL_W;
	fp.lock.h = fp.layout.h = fp.row_h - 2;
	fp.layout.y = 6 + fp.row_h;
	fp.ws.x = fp.lock.x + COL_W + 3;
	fp.ws.y = 6;
	fp.ws.w = fp.ws_cols * fp.ws_w;
	fp.ws.h = 2 * fp.row_h;
	fp.exit_tab.x = fp.exit.x = fp.ws.x + fp.ws.w + 3;
	fp.exit_tab.y = 6;
	fp.exit_tab.w = fp.exit.w = COL_W;
	fp.exit_tab.h = TAB_H;
	fp.exit.y = 6 + TAB_H + 1;
	fp.exit.h = 2 * fp.row_h - TAB_H - 10;
	fp.light.x = fp.exit.x + 6;
	fp.light.y = fp.exit.y + fp.exit.h + 3;
	fp.light.w = COL_W - 12;
	fp.light.h = 5;
	fp.sw.w = fp.exit.x + COL_W + 3 - fp.sw.x;
	x = fp.sw.x + fp.sw.w + 4;
	for (i = fp.nleft; i < fp.n; i++)
		place_control(&fp.c[i], &x);
	x += 2;
	fp.status.x = x;
	fp.status.y = 4;
	fp.status.w = STATUS_W;
	fp.status.h = fp.inner_h;
	x += STATUS_W + 4 + HANDLE_W + 3;
	fp.width = x;
	status_layout();
	tray_layout();
}

int
fp_width(void)
{
	fp_layout();
	return fp.width;
}

/* ------------------------------------------------------------- pictograms */

static void
arc(RuiSurf *s, int x, int y, int w, int h, int color, int fill)
{
	RuiApp *a = s->app;

	XSetForeground(a->dpy, a->gc, a->color[color].pixel);
	if (fill)
		XFillArc(a->dpy, s->buf, a->gc, x, y, (unsigned)w, (unsigned)h, 0,
		    360 * 64);
	else
		XDrawArc(a->dpy, s->buf, a->gc, x, y, (unsigned)w, (unsigned)h, 0,
		    360 * 64);
}

static void
draw_clock(RuiSurf *s, int cx, int cy)
{
	time_t now = time(NULL);
	struct tm tm;
	int r = 16, i, h, m;

	localtime_r(&now, &tm);
	arc(s, cx - r - 1, cy - r + 1, 2 * r, 2 * r, RUI_C_SHADOW, 1);
	arc(s, cx - r, cy - r, 2 * r, 2 * r, RUI_C_FIELD, 1);
	arc(s, cx - r, cy - r, 2 * r, 2 * r, RUI_C_DARK, 0);
	for (i = 0; i < 60; i += 5) {
		int px = cx + sin60[i] * (r - 3) / 1000;
		int py = cy - sin60[(i + 15) % 60] * (r - 3) / 1000;

		rui_fill(s, px, py, i % 15 ? 1 : 2, i % 15 ? 1 : 2, RUI_C_DARK);
	}
	m = tm.tm_min;
	h = (tm.tm_hour % 12) * 5 + tm.tm_min / 12;
	rui_line(s, cx, cy, cx + sin60[h] * (r - 8) / 1000,
	    cy - sin60[(h + 15) % 60] * (r - 8) / 1000, RUI_C_DARK);
	rui_line(s, cx + 1, cy, cx + 1 + sin60[h] * (r - 8) / 1000,
	    cy - sin60[(h + 15) % 60] * (r - 8) / 1000, RUI_C_DARK);
	rui_line(s, cx, cy, cx + sin60[m] * (r - 4) / 1000,
	    cy - sin60[(m + 15) % 60] * (r - 4) / 1000, RUI_C_DARK);
	rui_fill(s, cx - 1, cy - 1, 3, 3, RUI_C_ALERT);
}

static void
draw_calendar(RuiSurf *s, RuiRect r)
{
	RuiApp *a = s->app;
	time_t now = time(NULL);
	struct tm tm;
	char buf[16];
	int w = 40, h = r.h - 8, x = r.x + (r.w - w) / 2, y = r.y + 4;
	int band = a->font->height + 2;
	RuiRect t;

	localtime_r(&now, &tm);
	rui_fill(s, x + 2, y + 2, w, h, RUI_C_SHADOW);
	rui_fill(s, x, y, w, h, RUI_C_FIELD);
	rui_fill(s, x, y, w, band, RUI_C_TITLE_ACTIVE);
	strftime(buf, sizeof buf, "%b", &tm);
	t.x = x;
	t.y = y;
	t.w = w;
	t.h = band;
	rui_text_fit(s, a->font, t, buf, RUI_C_TITLE_FG, RUI_CENTER);
	snprintf(buf, sizeof buf, "%d", tm.tm_mday);
	t.y = y + band;
	t.h = h - band;
	rui_text_fit(s, a->bold, t, buf, RUI_C_TEXT, RUI_CENTER);
	rui_bevel(s, x, y, w, h, RUI_SUNKEN_THIN);
}

static void
draw_monitor(RuiSurf *s, int x, int y, int w, int h)
{
	rui_fill(s, x, y, w, h, RUI_C_FACE);
	rui_bevel(s, x, y, w, h, RUI_RAISED);
	rui_fill(s, x + 4, y + 4, w - 8, h - 8, RUI_C_DARK);
	rui_fill(s, x + w / 2 - 4, y + h, 8, 3, RUI_C_SHADOW);
	rui_fill(s, x + w / 2 - 9, y + h + 3, 18, 3, RUI_C_FACE);
	rui_bevel(s, x + w / 2 - 9, y + h + 3, 18, 3, RUI_RAISED_THIN);
}

static void
draw_pictogram(RuiSurf *s, int kind, RuiRect r)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i, j;

	switch (kind) {
	case FPC_CLOCK:
		draw_clock(s, cx, cy);
		break;
	case FPC_CALENDAR:
		draw_calendar(s, r);
		break;
	case FPC_FILES:                         /* filing cabinet */
		rui_fill(s, cx - 13, cy - 16, 26, 32, RUI_C_FACE);
		rui_bevel(s, cx - 13, cy - 16, 26, 32, RUI_RAISED);
		for (i = 0; i < 2; i++) {
			rui_bevel(s, cx - 10, cy - 13 + i * 14, 20, 12, RUI_RAISED_THIN);
			rui_fill(s, cx - 4, cy - 9 + i * 14, 8, 3, RUI_C_DARK);
		}
		break;
	case FPC_EDITOR:                        /* page and pencil */
		rui_fill(s, cx - 12, cy - 15, 22, 30, RUI_C_FIELD);
		rui_bevel(s, cx - 12, cy - 15, 22, 30, RUI_SUNKEN_THIN);
		for (i = 0; i < 6; i++)
			rui_fill(s, cx - 9, cy - 10 + i * 4, 15, 1, RUI_C_SHADOW);
		for (i = 0; i < 18; i++) {
			rui_fill(s, cx + 12 - i, cy - 12 + i + 6, 3, 1, RUI_C_ALERT);
			rui_fill(s, cx + 13 - i, cy - 12 + i + 7, 2, 1, RUI_C_DARK);
		}
		break;
	case FPC_TERMINAL:
		draw_monitor(s, cx - 16, cy - 15, 32, 24);
		rui_fill(s, cx - 10, cy - 9, 2, 2, RUI_C_GOOD);
		rui_fill(s, cx - 8, cy - 7, 2, 2, RUI_C_GOOD);
		rui_fill(s, cx - 10, cy - 5, 2, 2, RUI_C_GOOD);
		rui_fill(s, cx - 5, cy - 4, 8, 2, RUI_C_GOOD);
		break;
	case FPC_BROWSER:                       /* globe */
		arc(s, cx - 15, cy - 15, 30, 30, RUI_C_SEL_BG, 1);
		arc(s, cx - 15, cy - 15, 30, 30, RUI_C_DARK, 0);
		arc(s, cx - 7, cy - 15, 14, 30, RUI_C_LIGHT, 0);
		rui_fill(s, cx, cy - 15, 1, 30, RUI_C_LIGHT);
		rui_fill(s, cx - 15, cy, 30, 1, RUI_C_LIGHT);
		rui_fill(s, cx - 13, cy - 7, 26, 1, RUI_C_LIGHT);
		rui_fill(s, cx - 13, cy + 7, 26, 1, RUI_C_LIGHT);
		break;
	case FPC_STYLE:                         /* palette swatches */
		rui_fill(s, cx - 15, cy - 15, 30, 30, RUI_C_FACE);
		rui_bevel(s, cx - 15, cy - 15, 30, 30, RUI_SUNKEN);
		rui_fill(s, cx - 11, cy - 11, 10, 10, RUI_C_TITLE_ACTIVE);
		rui_fill(s, cx + 1, cy - 11, 10, 10, RUI_C_DESKTOP);
		rui_fill(s, cx - 11, cy + 1, 10, 10, RUI_C_ALERT);
		rui_fill(s, cx + 1, cy + 1, 10, 10, RUI_C_GOOD);
		break;
	case FPC_APPS:                          /* application grid */
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++) {
				rui_fill(s, cx - 15 + i * 11, cy - 15 + j * 11, 9, 9,
				    (i + j) % 2 ? RUI_C_FIELD : RUI_C_TITLE_ACTIVE);
				rui_bevel(s, cx - 15 + i * 11, cy - 15 + j * 11, 9, 9,
				    RUI_RAISED_THIN);
			}
		break;
	case FPC_NETWORK:
		draw_monitor(s, cx - 16, cy - 15, 18, 13);
		draw_monitor(s, cx - 2, cy - 2, 18, 13);
		rui_fill(s, cx - 8, cy + 4, 2, 8, RUI_C_DARK);
		rui_fill(s, cx - 8, cy + 11, 7, 2, RUI_C_DARK);
		break;
	default:
		break;
	}
}

static void
draw_padlock(RuiSurf *s, RuiRect r)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2 + 3;

	XSetLineAttributes(s->app->dpy, s->app->gc, 3, LineSolid, CapButt,
	    JoinMiter);
	arc(s, cx - 6, cy - 14, 12, 14, RUI_C_SHADOW, 0);
	XSetLineAttributes(s->app->dpy, s->app->gc, 0, LineSolid, CapButt,
	    JoinMiter);
	rui_fill(s, cx - 9, cy - 6, 18, 13, RUI_C_ALERT);
	rui_bevel(s, cx - 9, cy - 6, 18, 13, RUI_RAISED_THIN);
	rui_fill(s, cx - 1, cy - 2, 2, 5, RUI_C_DARK);
}

static void
draw_handle(RuiSurf *s, int x)
{
	int y;

	rui_fill(s, x, 4, HANDLE_W, fp.inner_h, RUI_C_FACE);
	rui_bevel(s, x, 4, HANDLE_W, fp.inner_h, RUI_RAISED);
	for (y = 10; y < fp.inner_h - 4; y += 5) {
		rui_fill(s, x + 4, y, 2, 2, RUI_C_SHADOW);
		rui_fill(s, x + 4, y, 1, 1, RUI_C_LIGHT);
		rui_fill(s, x + 7, y + 2, 2, 2, RUI_C_SHADOW);
		rui_fill(s, x + 7, y + 2, 1, 1, RUI_C_LIGHT);
	}
}

static void
draw_tab(RuiSurf *s, RuiRect t)
{
	rui_fill(s, t.x, t.y, t.w, t.h, RUI_C_FACE);
	rui_bevel(s, t.x, t.y, t.w, t.h, RUI_RAISED_THIN);
	rui_arrow(s, t.x + t.w / 2, t.y + t.h / 2, 4, 0, RUI_C_TEXT);
}

void
fp_draw(void)
{
	RuiSurf *s = bar.s;
	RuiApp *a = bar.app;
	int i, nd = (int)bar.ndesk, blink;
	RuiRect r;

	fp_layout();
	if (fp.width != bar.w)
		rui_surf_resize(s, fp.width, bar.h);
	rui_fill(s, 0, 0, s->w, bar.h, RUI_C_FACE);
	rui_bevel(s, 0, 0, s->w, bar.h, RUI_RAISED);
	draw_handle(s, 3);
	draw_handle(s, fp.width - 3 - HANDLE_W);

	for (i = 0; i < fp.n; i++) {
		Control *c = &fp.c[i];

		draw_tab(s, c->tab);
		rui_fill(s, c->r.x, c->r.y, c->r.w, c->r.h, RUI_C_FACE);
		rui_bevel(s, c->r.x, c->r.y, c->r.w, c->r.h, RUI_RAISED);
		draw_pictogram(s, c->kind, c->r);
	}

	/* workspace switch */
	rui_fill(s, fp.sw.x, fp.sw.y, fp.sw.w, fp.sw.h, RUI_C_FACE);
	rui_bevel(s, fp.sw.x, fp.sw.y, fp.sw.w, fp.sw.h, RUI_SUNKEN);
	rui_fill(s, fp.lock.x, fp.lock.y, fp.lock.w, fp.lock.h, RUI_C_FACE);
	rui_bevel(s, fp.lock.x, fp.lock.y, fp.lock.w, fp.lock.h, RUI_RAISED);
	draw_padlock(s, fp.lock);
	rui_fill(s, fp.layout.x, fp.layout.y, fp.layout.w, fp.layout.h,
	    RUI_C_FACE);
	rui_bevel(s, fp.layout.x, fp.layout.y, fp.layout.w, fp.layout.h,
	    RUI_RAISED);
	rui_text_fit(s, a->bold, fp.layout, bar.layout_sym[0] ? bar.layout_sym :
	    "--", RUI_C_TEXT, RUI_CENTER);
	for (i = 0; i < nd && i < 32; i++) {
		int cur = (unsigned long)i == bar.cur;

		r.x = fp.ws.x + (i % fp.ws_cols) * fp.ws_w;
		r.y = fp.ws.y + (i / fp.ws_cols) * fp.row_h;
		r.w = fp.ws_w - 2;
		r.h = fp.row_h - 2;
		rui_fill(s, r.x, r.y, r.w, r.h, cur ? RUI_C_SEL_BG : RUI_C_FACE);
		rui_bevel(s, r.x, r.y, r.w, r.h, cur ? RUI_PRESSED : RUI_RAISED);
		if (cur) {
			r.x++;
			r.y++;
		}
		rui_text_fit(s, cur ? a->bold : a->font, r, bar.desk_names[i],
		    cur ? RUI_C_SEL_FG : RUI_C_TEXT, RUI_CENTER);
	}
	draw_tab(s, fp.exit_tab);
	rui_fill(s, fp.exit.x, fp.exit.y, fp.exit.w, fp.exit.h, RUI_C_FACE);
	rui_bevel(s, fp.exit.x, fp.exit.y, fp.exit.w, fp.exit.h, RUI_RAISED);
	rui_text_fit(s, a->bold, fp.exit, "EXIT", RUI_C_TEXT, RUI_CENTER);
	blink = fp_busy() && (rui_now_ms() / 500) % 2 == 0;
	rui_fill(s, fp.light.x, fp.light.y, fp.light.w, fp.light.h,
	    blink ? RUI_C_GOOD : RUI_C_SHADOW);
	rui_bevel(s, fp.light.x - 1, fp.light.y - 1, fp.light.w + 2,
	    fp.light.h + 2, RUI_SUNKEN_THIN);

	/* status area: tray and applets */
	rui_fill(s, fp.status.x, fp.status.y, fp.status.w, fp.status.h,
	    RUI_C_FACE);
	rui_bevel(s, fp.status.x, fp.status.y, fp.status.w, fp.status.h,
	    RUI_SUNKEN);
	if (bar.r_tray.w)
		rui_bevel(s, bar.r_tray.x, bar.r_tray.y, bar.r_tray.w, bar.r_tray.h,
		    RUI_SUNKEN_THIN);
	for (i = 0; i < bar.nap; i++)
		if (bar.ap[i].r.w)
			draw_applet(s, &bar.ap[i]);
	rui_surf_flush(s);
	publish_hotspots();
	if (fp.width != bar.w) {
		bar.w = fp.width;
		bar.x = (bar.app->sw - bar.w) / 2;
		XMoveResizeWindow(bar.dpy, bar.win, bar.x, bar.y, (unsigned)bar.w,
		    (unsigned)bar.h);
	}
}

/* ------------------------------------------------------------- subpanels */

static void
launch(const char *cmd)
{
	fp.busy_until = rui_now_ms() + 2500;
	bar.dirty = 1;
	rui_run_command(bar.app, cmd);
}

static RuiMenu *
power_menu(void)
{
	static const char *const labels[RUI_POWER_ACTIONS] = {
		"Shut Down", "Restart", "Suspend", "Hibernate"
	};
	static const int order[RUI_POWER_ACTIONS] = {
		RUI_SUSPEND, RUI_HIBERNATE, RUI_REBOOT, RUI_POWEROFF
	};
	const RuiSettings *set = rui_settings();
	const RuiPowerModule *m = rui_power_detect(set->power);
	RuiMenu *menu = rui_menu_new();
	int i;

	rui_menu_add(menu, "Lock Screen", "@lock", 0);
	rui_menu_add_sep(menu);
	for (i = 0; i < RUI_POWER_ACTIONS; i++) {
		int act = order[i];
		char *cmd = rui_power_command(m, act, set->power_priv), at[32];

		snprintf(at, sizeof at, "@%s", rui_power_action_names[act]);
		rui_menu_add(menu, labels[act], at, 0)->disabled = cmd == NULL;
		free(cmd);
	}
	rui_menu_add_sep(menu);
	rui_menu_add(menu, "Log Out", "@logout", 0);
	return menu;
}

static RuiMenu *
subpanel(int kind)
{
	RuiMenu *m = rui_menu_new();
	char **themes, *q, *cmd;
	int i;

	switch (kind) {
	case FPC_CLOCK:
	case FPC_CALENDAR:
		rui_menu_add(m, "Calendar", NULL, 1);
		rui_menu_add(m, "Clock", "@clock", 0);
		break;
	case FPC_FILES:
		rui_menu_add(m, "File Manager", "@filemanager", 0);
		rui_menu_add(m, "Home Folder", "@filemanager \"$HOME\"", 0);
		rui_menu_add(m, "Text Editor", "@editor", 0);
		break;
	case FPC_EDITOR:
		rui_menu_add(m, "Text Editor", "@editor", 0);
		rui_menu_add(m, "Terminal", "@terminal", 0);
		break;
	case FPC_TERMINAL:
		rui_menu_add(m, "Terminal", "@terminal", 0);
		rui_menu_add(m, "Run...", "retro-run", 0);
		break;
	case FPC_BROWSER:
		rui_menu_add(m, "Web Browser", "@browser", 0);
		break;
	case FPC_STYLE:
		rui_menu_add(m, "Style Manager", "retro-settings", 0);
		rui_menu_add(m, "Display", "retro-display", 0);
		rui_menu_add_sep(m);
		themes = rui_theme_list();
		for (i = 0; themes[i]; i++) {
			q = rui_shell_quote(themes[i]);
			cmd = rui_strdupf("retro-settings --apply theme=%s", q);
			rui_menu_add(m, themes[i], cmd, 0)->checked =
			    !strcmp(themes[i], rui_settings()->theme);
			free(cmd);
			free(q);
		}
		rui_strv_free(themes);
		break;
	case FPC_NETWORK:
		rui_menu_add(m, "Network", "retro-net", 0);
		rui_menu_add(m, "Bluetooth", "retro-bt", 0);
		break;
	default:
		break;
	}
	return m;
}

static void
popup_menu(RuiMenu *m, int x)
{
	const RuiMenuItem *it = rui_menu_popup(bar.app, m, bar.x + x,
	    bar.top ? bar.y + bar.h : bar.y, bar.top ? 0 : RUI_MENU_UPWARD);
	char *cmd = it && it->cmd ? rui_strdup(it->cmd) : NULL;
	int id = it ? it->id : 0;

	if (cmd)
		launch(cmd);
	else if (id == 1) {
		bar.cal_anchor = bar.x + x;
		cal_toggle();
	}
	free(cmd);
}

static void
control_menu(Control *c)
{
	RuiMenu *m;

	if (c->kind == FPC_APPS) {
		menu_refresh();
		popup_menu(bar.menu, c->tab.x);
		return;
	}
	m = subpanel(c->kind);
	popup_menu(m, c->tab.x);
	rui_menu_free(m);
}

static void
control_click(Control *c)
{
	switch (c->kind) {
	case FPC_CLOCK:
	case FPC_CALENDAR:
		bar.cal_anchor = bar.x + c->r.x;
		cal_toggle();
		break;
	case FPC_FILES:
		launch("@filemanager");
		break;
	case FPC_EDITOR:
		launch("@editor");
		break;
	case FPC_TERMINAL:
		launch("@terminal");
		break;
	case FPC_BROWSER:
		launch("@browser");
		break;
	case FPC_STYLE:
		launch("retro-settings");
		break;
	case FPC_APPS:
		control_menu(c);
		break;
	case FPC_NETWORK:
		launch("retro-net");
		break;
	default:
		break;
	}
}

void
fp_click(XButtonEvent *e)
{
	int i;

	if (e->button == Button4 || e->button == Button5) {
		switch_desk_rel(e->button == Button4 ? -1 : 1);
		return;
	}
	if (e->button == Button1) {
		Applet *v = applet_of(AP_VOL);
		int on_cal = 0;

		for (i = 0; i < fp.n; i++)
			if ((fp.c[i].kind == FPC_CLOCK || fp.c[i].kind == FPC_CALENDAR) &&
			    rui_in_rect(fp.c[i].r, e->x, e->y))
				on_cal = 1;
		if (!on_cal)
			cal_close();
		if (!v || !rui_in_rect(v->r, e->x, e->y))
			vol_close();
	}
	for (i = 0; i < fp.n; i++) {
		Control *c = &fp.c[i];

		if (rui_in_rect(c->tab, e->x, e->y) ||
		    (e->button == Button3 && rui_in_rect(c->r, e->x, e->y))) {
			control_menu(c);
			return;
		}
		if (e->button == Button1 && rui_in_rect(c->r, e->x, e->y)) {
			control_click(c);
			return;
		}
	}
	if (rui_in_rect(fp.ws, e->x, e->y) && e->button == Button1) {
		int col = (e->x - fp.ws.x) / fp.ws_w, row = (e->y - fp.ws.y) /
		    fp.row_h, d = row * fp.ws_cols + col;

		if (d >= 0 && d < (int)bar.ndesk)
			rui_send_root_msg(bar.app, bar.app->root,
			    A(_NET_CURRENT_DESKTOP), d, CurrentTime, 0, 0, 0);
		return;
	}
	if (rui_in_rect(fp.exit_tab, e->x, e->y) ||
	    (e->button == Button3 && rui_in_rect(fp.exit, e->x, e->y))) {
		RuiMenu *m = power_menu();

		popup_menu(m, fp.exit_tab.x);
		rui_menu_free(m);
		return;
	}
	if (e->button != Button1)
		return;
	if (rui_in_rect(fp.exit, e->x, e->y)) {
		rui_run_command(bar.app, "@logout");
	} else if (rui_in_rect(fp.lock, e->x, e->y)) {
		launch("@lock");
	} else if (rui_in_rect(fp.layout, e->x, e->y)) {
		rui_wm_command(bar.app, "next_layout");
	} else {
		for (i = 0; i < bar.nap; i++)
			if (bar.ap[i].r.w && rui_in_rect(bar.ap[i].r, e->x, e->y))
				applet_click(&bar.ap[i], e);
	}
}
