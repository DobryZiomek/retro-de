/*
 * retro-wallpaper - choose the desktop's wallpaper, previewed on an old CRT
 * monitor: the picture is shown as the screen would show it (fill, stretch,
 * centre or tiles, at the real screen's proportions), with scan lines, a
 * curved glass glare and a darker rim - and it "degausses" when applied.
 *
 *   retro-wallpaper [-v]
 *
 * Writes `wallpaper` and `wallpaper_mode` into retro.conf and asks the
 * desktop to reload.  Browse... opens retro-files as a chooser; PNG, SVG,
 * XPM and PPM pictures all work.
 */
#include "../lib/retroui.h"
#include "../desk/image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 800
#define WIN_H 540
#define CASE_X 18
#define CASE_Y 18
#define CASE_W 450
#define CASE_H 376
#define PANEL_X 492

enum { M_AUTO, M_FILL, M_STRETCH, M_CENTER, M_TILE, M_COUNT };
static const char *const mode_names[M_COUNT] = {
	"auto", "fill", "stretch", "center", "tile"
};
static const char *const mode_labels[M_COUNT] = {
	"Position: Auto", "Position: Fill", "Position: Stretch",
	"Position: Center", "Position: Tile"
};

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty;
	char **paths;                      /* installed + user wallpapers   */
	char **names;                      /* "(none)", then their names    */
	int n;                             /* names                         */
	RuiList list;
	RuiButton mode_b, browse, apply, close;
	int mode;
	Image *img;                        /* the chosen picture            */
	char loaded[1024];
	Image *screen;                     /* composed CRT picture          */
	int screen_dirty;
	RuiRect r_screen;
	int real_w, real_h;                /* the primary monitor           */
	long long degauss;                 /* when Apply was pressed        */
	int browsing;
	char msg[160];
	unsigned long beige, beige_lt, beige_dk, beige_dd, glass, led_on,
	    led_amber, label;
} wp;

/* --------------------------------------------------------------- colours */

static unsigned long
pixel(const char *spec)
{
	XColor xc, exact;

	if (XAllocNamedColor(wp.app->dpy, wp.app->cmap, spec, &xc, &exact))
		return xc.pixel;
	return wp.app->color[RUI_C_FACE].pixel;
}

static void
theme_rgb(int c, unsigned char *o)
{
	o[0] = (unsigned char)(wp.app->color[c].color.red >> 8);
	o[1] = (unsigned char)(wp.app->color[c].color.green >> 8);
	o[2] = (unsigned char)(wp.app->color[c].color.blue >> 8);
}

/* ------------------------------------------------------------ the list */

static const char *
current_path(void)
{
	int sel = wp.list.sel;

	return sel > 0 && sel <= wp.n - 1 ? wp.paths[sel - 1] : NULL;
}

static void
list_load(const char *want)
{
	char *found = want && *want ? rui_wallpaper_find(want) : NULL;
	int i, n;

	rui_strv_free(wp.paths);
	if (wp.names) {
		for (i = 0; i < wp.n; i++)
			free(wp.names[i]);
		free(wp.names);
	}
	wp.paths = rui_wallpaper_list();
	for (n = 0; wp.paths[n]; n++)
		;
	/* a picture from elsewhere joins the list */
	if (found && !strstr(found, "/wallpapers/")) {
		int k;

		for (k = 0; k < n && strcmp(wp.paths[k], found); k++)
			;
		if (k == n) {
			wp.paths = rui_realloc(wp.paths, (size_t)(n + 2) *
			    sizeof *wp.paths);
			wp.paths[n++] = rui_strdup(found);
			wp.paths[n] = NULL;
		}
	}
	wp.names = rui_calloc((size_t)n + 2, sizeof *wp.names);
	wp.names[0] = rui_strdup("(none)");
	wp.list.sel = 0;
	for (i = 0; i < n; i++) {
		const char *base = strrchr(wp.paths[i], '/');
		char *name = rui_strdup(base ? base + 1 : wp.paths[i]), *dot;

		if ((dot = strrchr(name, '.')) && dot != name &&
		    strstr(wp.paths[i], "/wallpapers/"))
			*dot = '\0';
		wp.names[i + 1] = name;
		if (found && !strcmp(found, wp.paths[i]))
			wp.list.sel = i + 1;
	}
	wp.n = n + 1;
	wp.list.items = wp.names;
	wp.list.n = wp.n;
	wp.list.last_idx = -1;
	wp.list.top = wp.list.sel > 8 ? wp.list.sel - 4 : 0;
	free(found);
	wp.screen_dirty = 1;
}

/* ----------------------------------------------------- the CRT picture */

static void
load_image(void)
{
	const char *p = current_path();

	if (!p) {
		image_free(wp.img);
		wp.img = NULL;
		wp.loaded[0] = '\0';
		return;
	}
	if (!strcmp(p, wp.loaded))
		return;
	image_free(wp.img);
	/* big enough for the little screen, small enough to be quick */
	wp.img = image_load_size(wp.app, p, 900);
	rui_strlcpy(wp.loaded, p, sizeof wp.loaded);
}

/* bilinear sample of img at (fx, fy) in its pixels */
static void
sample(const Image *img, double fx, double fy, unsigned char *o)
{
	int x0, y0, x1, y1, k;
	double ax, ay;

	if (fx < 0)
		fx = 0;
	if (fy < 0)
		fy = 0;
	x0 = (int)fx;
	y0 = (int)fy;
	if (x0 > img->w - 1)
		x0 = img->w - 1;
	if (y0 > img->h - 1)
		y0 = img->h - 1;
	x1 = x0 + 1 < img->w ? x0 + 1 : x0;
	y1 = y0 + 1 < img->h ? y0 + 1 : y0;
	ax = fx - x0;
	ay = fy - y0;
	if (ax > 1)
		ax = 1;
	if (ay > 1)
		ay = 1;
	for (k = 0; k < 3; k++) {
		double a = img->rgb[((size_t)y0 * img->w + x0) * 3 + k],
		    b = img->rgb[((size_t)y0 * img->w + x1) * 3 + k],
		    c = img->rgb[((size_t)y1 * img->w + x0) * 3 + k],
		    d = img->rgb[((size_t)y1 * img->w + x1) * 3 + k];

		o[k] = (unsigned char)((a * (1 - ax) + b * ax) * (1 - ay) +
		    (c * (1 - ax) + d * ax) * ay + 0.5);
	}
}

static void
compose(long long now)
{
	int W = wp.r_screen.w, H = wp.r_screen.h, x, y, mode = wp.mode;
	double scale = (double)W / wp.real_w, t = 0, amp = 0;
	unsigned char desk[3], face[3], light[3], dark[3], title[3], *px;
	Image *img = wp.img;
	int bar_h;

	if (!wp.screen) {
		wp.screen = rui_calloc(1, sizeof *wp.screen);
		wp.screen->w = W;
		wp.screen->h = H;
		wp.screen->rgb = rui_malloc((size_t)W * H * 3);
	}
	px = wp.screen->rgb;
	theme_rgb(RUI_C_DESKTOP, desk);
	theme_rgb(RUI_C_FACE, face);
	theme_rgb(RUI_C_LIGHT, light);
	theme_rgb(RUI_C_SHADOW, dark);
	theme_rgb(RUI_C_TITLE_ACTIVE, title);
	if (img && mode == M_AUTO)
		mode = img->w <= 256 && img->h <= 256 ? M_TILE : M_FILL;
	if (wp.degauss) {
		t = (now - wp.degauss) / 1000.0;
		amp = t < 0.9 ? 9 * exp(-t * 4.5) : 0;
	}
	for (y = 0; y < H; y++) {
		/* the degauss wobble shifts each line sideways */
		double shift = amp * sin(y / 5.0 + t * 40);

		for (x = 0; x < W; x++) {
			unsigned char *o = px + ((size_t)y * W + x) * 3;
			double sx = x - shift;

			memcpy(o, desk, 3);
			if (!img)
				continue;
			if (mode == M_FILL || mode == M_STRETCH) {
				double sw = img->w, sh = img->h, x0 = 0, y0 = 0;

				if (mode == M_FILL) {
					if ((double)img->w * H > (double)img->h * W) {
						sw = (double)img->h * W / H;
						x0 = (img->w - sw) / 2;
					} else {
						sh = (double)img->w * H / W;
						y0 = (img->h - sh) / 2;
					}
				}
				if (sx >= 0 && sx < W)
					sample(img, x0 + (sx + 0.5) * sw / W - 0.5,
					    y0 + (y + 0.5) * sh / H - 0.5, o);
			} else {
				/* centre and tiles: the picture at the screen's real scale
				 * (the image may have been shrunk on loading: assume it is
				 * shown 1:1 on the real screen) */
				double iw = img->w * scale, ih = img->h * scale, ix, iy;

				if (iw < 1)
					iw = 1;
				if (ih < 1)
					ih = 1;
				if (mode == M_CENTER) {
					ix = sx - (W - iw) / 2;
					iy = y - (H - ih) / 2;
					if (ix < 0 || iy < 0 || ix >= iw || iy >= ih)
						continue;
				} else {
					ix = fmod(sx < 0 ? sx + iw * 64 : sx, iw);
					iy = fmod(y, ih);
				}
				sample(img, ix / scale, iy / scale, o);
			}
		}
	}
	/* a little taskbar at the bottom, as on the real desktop */
	bar_h = H / 14 < 6 ? 6 : H / 14;
	for (y = H - bar_h; y < H; y++)
		for (x = 0; x < W; x++) {
			unsigned char *o = px + ((size_t)y * W + x) * 3;

			memcpy(o, y == H - bar_h ? light : face, 3);
			if (x >= 3 && x < 3 + bar_h * 3 && y > H - bar_h + 1 && y < H - 1)
				memcpy(o, x == 3 || y == H - bar_h + 2 ? light :
				    x == 2 + bar_h * 3 || y == H - 2 ? dark : face, 3);
			if (x >= 6 && x < 6 + bar_h / 2 && y > H - bar_h + 3 && y < H - 3)
				memcpy(o, title, 3);
		}
	/* the tube: scan lines, a darker rim, the glare of curved glass and
	 * rounded corners */
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			unsigned char *o = px + ((size_t)y * W + x) * 3;
			double nx = (x + 0.5) / W * 2 - 1, ny = (y + 0.5) / H * 2 - 1;
			double vig = 1 - 0.28 * (nx * nx * 0.8 + ny * ny), f, glare;
			double gx = nx + 0.55, gy = ny + 0.65, cr = W * 0.06;
			int k, cx, cy;

			f = vig * (y % 2 ? 0.80 : 1.0);
			if (amp > 0)                       /* colour fringes */
				f *= 1 + 0.15 * sin(x / 3.0 + t * 30) * amp / 9;
			glare = exp(-(gx * gx * 2.2 + gy * gy * 6)) * 0.22;
			for (k = 0; k < 3; k++) {
				double v = o[k] * f + 255 * glare;

				o[k] = (unsigned char)(v > 255 ? 255 : v < 0 ? 0 : v);
			}
			/* corners of the tube */
			cx = x < cr ? (int)cr - x : x >= W - cr ? x - (W - 1 - (int)cr) : 0;
			cy = y < cr ? (int)cr - y : y >= H - cr ? y - (H - 1 - (int)cr) : 0;
			if (cx > 0 && cy > 0 && cx * cx + cy * cy > cr * cr) {
				o[0] = 0x14;
				o[1] = 0x16;
				o[2] = 0x14;
			}
		}
	}
	wp.screen_dirty = 0;
}

/* --------------------------------------------------------------- drawing */

static void
rect(int x, int y, int w, int h, unsigned long px)
{
	rui_fill_pixel(wp.s, x, y, w, h, px);
}

/* a raised beige panel with soft two-pixel edges */
static void
beige_box(int x, int y, int w, int h, int raised)
{
	rect(x, y, w, h, wp.beige);
	rect(x, y, w, 2, raised ? wp.beige_lt : wp.beige_dk);
	rect(x, y, 2, h, raised ? wp.beige_lt : wp.beige_dk);
	rect(x, y + h - 2, w, 2, raised ? wp.beige_dk : wp.beige_lt);
	rect(x + w - 2, y, 2, h, raised ? wp.beige_dk : wp.beige_lt);
	rect(x + w - 1, y, 1, h, raised ? wp.beige_dd : wp.beige_lt);
	rect(x, y + h - 1, w, 1, raised ? wp.beige_dd : wp.beige_lt);
}

static void
draw_monitor(long long now)
{
	RuiApp *a = wp.app;
	RuiSurf *s = wp.s;
	int cx = CASE_X, cy = CASE_Y, i, led_y, base_y;
	RuiRect r;

	/* the stand: a neck and a foot */
	base_y = cy + CASE_H;
	for (i = 0; i < 26; i++) {
		int half = 70 + i * 2;

		rect(cx + CASE_W / 2 - half, base_y + i, half * 2, 1,
		    i < 2 ? wp.beige_dk : wp.beige);
		rect(cx + CASE_W / 2 - half, base_y + i, 2, 1, wp.beige_lt);
		rect(cx + CASE_W / 2 + half - 2, base_y + i, 2, 1, wp.beige_dk);
	}
	beige_box(cx + CASE_W / 2 - 150, base_y + 26, 300, 22, 1);
	rect(cx + CASE_W / 2 - 140, base_y + 48, 280, 3, wp.beige_dd);

	/* the case */
	beige_box(cx, cy, CASE_W, CASE_H, 1);
	beige_box(cx + 6, cy + 6, CASE_W - 12, CASE_H - 12, 1);
	/* the bezel around the tube, sunk in */
	r = wp.r_screen;
	beige_box(r.x - 16, r.y - 16, r.w + 32, r.h + 32, 0);
	rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12, wp.glass);
	if (wp.screen_dirty || (wp.degauss && now - wp.degauss < 1000)) {
		load_image();
		compose(now);
	}
	image_draw(a, wp.screen, s->buf, r.x, r.y);

	/* the chin: a badge, the power light, two knobs and a switch */
	led_y = r.y + r.h + 30;
	r.x = cx + 28;
	r.y = led_y - 2;
	r.w = 120;
	r.h = a->bold->height;
	rect(r.x - 4, r.y - 3, rui_text_width(a, a->bold, "RETRO 1440", -1) + 8,
	    r.h + 6, wp.beige_dk);
	rect(r.x - 3, r.y - 2, rui_text_width(a, a->bold, "RETRO 1440", -1) + 6,
	    r.h + 4, wp.label);
	rui_text_fit(s, a->bold, r, "RETRO 1440", RUI_C_TITLE_FG, RUI_LEFT);
	for (i = 0; i < 2; i++) {
		int kx = cx + CASE_W - 170 + i * 34, ky = led_y + 1;

		rect(kx, ky, 18, 18, wp.beige_dd);
		rect(kx + 1, ky + 1, 16, 16, wp.beige_dk);
		rect(kx + 3, ky + 3, 12, 12, wp.beige);
		rect(kx + 3, ky + 3, 12, 2, wp.beige_lt);
		rect(kx + 8, ky + 4, 2, 6, wp.beige_dd);
	}
	{
		int bx = cx + CASE_W - 92, by = led_y;
		int busy = wp.degauss && now - wp.degauss < 900;

		beige_box(bx, by, 40, 20, 1);
		rect(bx + 50, by + 6, 10, 8, wp.beige_dd);
		rect(bx + 51, by + 7, 8, 6, busy && (now / 120) % 2 ? wp.led_amber :
		    wp.led_on);
		rect(bx + 52, by + 7, 3, 2, pixel("#d8ffd8"));
	}
	/* vents on the top edge */
	for (i = 0; i < 14; i++)
		rect(cx + 120 + i * 15, cy + 10, 9, 2, wp.beige_dk);
}

static void
draw(void)
{
	RuiApp *a = wp.app;
	RuiSurf *s = wp.s;
	long long now = rui_now_ms();
	RuiRect r;

	rui_fill(s, 0, 0, WIN_W, WIN_H, RUI_C_FACE);
	draw_monitor(now);
	r.x = PANEL_X;
	r.y = 18;
	r.w = WIN_W - PANEL_X - 18;
	r.h = a->bold->height;
	rui_text_fit(s, a->bold, r, "Wallpaper", RUI_C_TEXT, RUI_LEFT);
	rui_list_draw(s, &wp.list);
	rui_button_draw(s, &wp.mode_b);
	rui_button_draw(s, &wp.browse);
	rui_button_draw(s, &wp.apply);
	rui_button_draw(s, &wp.close);
	r.x = 18;
	r.y = WIN_H - 18 - a->font->height;
	r.w = PANEL_X - 36;
	r.h = a->font->height;
	rui_text_fit(s, a->font, r, wp.msg, RUI_C_TEXT, RUI_LEFT);
	rui_surf_flush(s);
	wp.dirty = 0;
}

/* --------------------------------------------------------------- actions */

static void
apply(void)
{
	char *conf = rui_config_path("retro.conf");
	const char *p = current_path();
	char value[1024] = "none";
	int sel = wp.list.sel;

	if (p) {
		/* installed ones by name, others by path */
		if (strstr(p, "/wallpapers/"))
			rui_strlcpy(value, wp.names[sel], sizeof value);
		else
			rui_strlcpy(value, p, sizeof value);
	}
	if (rui_conf_set(conf, "wallpaper", p ? value : "") < 0 ||
	    rui_conf_set(conf, "wallpaper_mode", mode_names[wp.mode]) < 0) {
		snprintf(wp.msg, sizeof wp.msg, "Cannot write %s", conf);
	} else {
		rui_request_reload(wp.app->dpy);
		snprintf(wp.msg, sizeof wp.msg, "Applied: %s, %s.", wp.names[sel],
		    mode_names[wp.mode]);
		wp.degauss = rui_now_ms();
	}
	free(conf);
	wp.dirty = 1;
}

static void
on_chosen(const char *out, int status, void *ud)
{
	char path[1024];

	(void)ud;
	wp.browsing = 0;
	if (status != 0 || !out || out[0] != '/') {
		wp.dirty = 1;
		return;
	}
	rui_strlcpy(path, out, sizeof path);
	path[strcspn(path, "\n")] = '\0';
	list_load(path);
	snprintf(wp.msg, sizeof wp.msg, "Chosen: %.140s", path);
	wp.dirty = 1;
}

static void
browse(void)
{
	char *pics = rui_strdupf("%s/Pictures", getenv("HOME") ? getenv("HOME") :
	    "/"), *q = rui_shell_quote(pics), *cmd;

	if (wp.browsing)
		return;
	cmd = rui_strdupf("retro-files --choose --title 'Choose a wallpaper' %s",
	    q);
	if (rui_job_start(cmd, on_chosen, NULL) == 0)
		wp.browsing = 1;
	free(cmd);
	free(q);
	free(pics);
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(wp.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (ks == XK_Escape)
			wp.running = 0;
		else if (ks == XK_Return || ks == XK_KP_Enter)
			apply();
		else if (rui_list_key(wp.app, &wp.list, ks)) {
			wp.screen_dirty = 1;
			wp.dirty = 1;
		}
		break;
	}
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		r = rui_list_event(wp.app, &wp.list, ev);
		if (r & (RUI_EV_CHANGED | RUI_EV_ACTIVATE)) {
			wp.screen_dirty = 1;
			wp.msg[0] = '\0';
		}
		if (r & RUI_EV_ACTIVATE)
			apply();
		wp.dirty |= r != 0;
		r = rui_button_event(&wp.mode_b, ev);
		if (r & RUI_EV_ACTIVATE) {
			wp.mode = (wp.mode + 1) % M_COUNT;
			wp.mode_b.label = mode_labels[wp.mode];
			wp.screen_dirty = 1;
		}
		wp.dirty |= r != 0;
		r = rui_button_event(&wp.browse, ev);
		if (r & RUI_EV_ACTIVATE)
			browse();
		wp.dirty |= r != 0;
		r = rui_button_event(&wp.apply, ev);
		if (r & RUI_EV_ACTIVATE)
			apply();
		wp.dirty |= r != 0;
		r = rui_button_event(&wp.close, ev);
		if (r & RUI_EV_ACTIVATE)
			wp.running = 0;
		wp.dirty |= r != 0;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(wp.app, ev))
			wp.running = 0;
		break;
	default:
		break;
	}
}

static void
read_conf(const char *key, const char *val, void *ud)
{
	char *want = ud;
	int i;

	if (!strcmp(key, "wallpaper"))
		rui_strlcpy(want, val, 1024);
	else if (!strcmp(key, "wallpaper_mode"))
		for (i = 0; i < M_COUNT; i++)
			if (!strcmp(val, mode_names[i]))
				wp.mode = i;
}

static void
setup(void)
{
	RuiApp *a = wp.app;
	RuiMonitor mon = rui_monitor_pick(a, "primary");
	XSizeHints sh;
	char want[1024] = "", *conf;
	int fh = a->font->height, bh = fh + 12, maxw, maxh, y;

	wp.real_w = mon.w > 0 ? mon.w : a->sw;
	wp.real_h = mon.h > 0 ? mon.h : a->sh;
	/* the tube has the real screen's proportions */
	maxw = CASE_W - 2 * 46;
	maxh = CASE_H - 46 - 86;
	wp.r_screen.w = maxw;
	wp.r_screen.h = (int)((double)maxw * wp.real_h / wp.real_w);
	if (wp.r_screen.h > maxh) {
		wp.r_screen.h = maxh;
		wp.r_screen.w = (int)((double)maxh * wp.real_w / wp.real_h);
	}
	wp.r_screen.x = CASE_X + (CASE_W - wp.r_screen.w) / 2;
	wp.r_screen.y = CASE_Y + 40 + (maxh - wp.r_screen.h) / 2;

	wp.beige = pixel("#d9d0b9");
	wp.beige_lt = pixel("#f1ebdc");
	wp.beige_dk = pixel("#a89f88");
	wp.beige_dd = pixel("#6c6554");
	wp.glass = pixel("#141614");
	wp.led_on = pixel("#30d040");
	wp.led_amber = pixel("#f0a020");
	wp.label = a->color[RUI_C_TITLE_ACTIVE].pixel;

	conf = rui_config_path("retro.conf");
	rui_conf_parse(conf, read_conf, want);
	free(conf);
	list_load(want);
	wp.mode_b.label = mode_labels[wp.mode];

	y = 18 + a->bold->height + 8;
	wp.list.r.x = PANEL_X;
	wp.list.r.y = y;
	wp.list.r.w = WIN_W - PANEL_X - 18;
	wp.list.r.h = WIN_H - y - 18 - 3 * (bh + 8) - 8;
	y += wp.list.r.h + 10;
	wp.mode_b.r.x = PANEL_X;
	wp.mode_b.r.y = y;
	wp.mode_b.r.w = wp.list.r.w;
	wp.mode_b.r.h = bh;
	y += bh + 8;
	wp.browse.label = "Browse...";
	wp.browse.r = wp.mode_b.r;
	wp.browse.r.y = y;
	y += bh + 8;
	wp.apply.label = "Apply";
	wp.apply.bold = 1;
	wp.apply.r.x = PANEL_X;
	wp.apply.r.y = y;
	wp.apply.r.w = (wp.list.r.w - 8) / 2;
	wp.apply.r.h = bh;
	wp.close.label = "Close";
	wp.close.r = wp.apply.r;
	wp.close.r.x = PANEL_X + wp.apply.r.w + 8;

	wp.win = rui_window_new(a, a->root, mon.x + (mon.w - WIN_W) / 2,
	    mon.y + (mon.h - WIN_H) / 3, WIN_W, WIN_H, 0, ExposureMask |
	    KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	wp.s = rui_surf_new(a, wp.win, WIN_W, WIN_H);
	rui_set_title(a, wp.win, "Wallpaper", "retro-wallpaper");
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = WIN_H;
	XSetWMNormalHints(a->dpy, wp.win, &sh);
	snprintf(wp.msg, sizeof wp.msg, "Screen %dx%d. Double-click a picture "
	    "to apply it.", wp.real_w, wp.real_h);
	wp.screen_dirty = 1;
	XMapWindow(a->dpy, wp.win);
}

int
main(int argc, char **argv)
{
	int i;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-wallpaper " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-wallpaper [-v]\n");
		return 2;
	}
	rui_signals_init();
	wp.app = rui_open("retro-wallpaper");
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	wp.running = 1;
	wp.dirty = 1;
	while (wp.running && !rui_quit_requested()) {
		XEvent ev;
		int animating;

		while (wp.running && XPending(wp.app->dpy)) {
			XNextEvent(wp.app->dpy, &ev);
			handle_event(&ev);
		}
		animating = wp.degauss && rui_now_ms() - wp.degauss < 1100;
		if ((wp.dirty || animating) && wp.running)
			draw();
		if (!animating && wp.degauss) {
			wp.degauss = 0;
			wp.screen_dirty = 1;
			wp.dirty = 1;
		}
		if (wp.running)
			rui_wait(wp.app, animating || wp.dirty ? 33 : -1);
	}
	image_free(wp.img);
	image_free(wp.screen);
	rui_strv_free(wp.paths);
	for (i = 0; i < wp.n; i++)
		free(wp.names[i]);
	free(wp.names);
	rui_surf_free(wp.s);
	XDestroyWindow(wp.app->dpy, wp.win);
	rui_close(wp.app);
	return 0;
}
