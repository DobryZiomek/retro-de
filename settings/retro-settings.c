/*
 * retro-settings - the Style Manager: theme (colour palette + style), font,
 * backdrop, default applications and system integration (audio, Wi-Fi and
 * power modules, D-Bus, XDG, panel).  Changes go to retro.conf; Apply asks
 * every running component to reload.
 *
 *   retro-settings [--page N]           the dialog
 *   retro-settings --apply KEY=VAL...   write and reload (for scripts)
 *   retro-settings --set KEY=VAL...     write only
 *   retro-settings --get KEY            print the value in retro.conf
 *   retro-settings --reload             re-read every .conf file now
 *   retro-settings --list-themes [-v]   -v: kind and description as well
 *   retro-settings --install-theme FILE-OR-URL
 *        a theme module (NAME.so, or NAME.c which is compiled), or a
 *        NAME.theme palette, into ~/.config/retro-de/themes
 */
#include "../lib/retroui.h"
#include "../desk/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define WIN_W 620
#define WIN_H 500
#define MAX_KV 128
#define MAX_FAMILIES 1024

enum { PG_COLOR, PG_FONT, PG_BACKDROP, PG_APPS, PG_SYSTEM, PG_COUNT };
static const char *const page_names[PG_COUNT] = {
	"Color", "Font", "Backdrop", "Applications", "System"
};

static const struct {
	const char *name;
	unsigned char bits[8];
} patterns[] = {
	{ "solid",    { 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ "checker",  { 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa } },
	{ "dots",     { 0x11, 0x00, 0x44, 0x00, 0x11, 0x00, 0x44, 0x00 } },
	{ "dense",    { 0xdd, 0xff, 0x77, 0xff, 0xdd, 0xff, 0x77, 0xff } },
	{ "hlines",   { 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00 } },
	{ "vlines",   { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } },
	{ "diagonal", { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 } },
	{ "bricks",   { 0xff, 0x01, 0x01, 0x01, 0xff, 0x10, 0x10, 0x10 } },
	{ "weave",    { 0x88, 0x54, 0x22, 0x45, 0x88, 0x15, 0x22, 0x51 } },
};
#define NPAT ((int)(sizeof patterns / sizeof patterns[0]))

static const char *const tool_labels[RUI_TOOL_COUNT] = {
	"Terminal", "Web browser", "File manager", "Text editor", "Screen locker",
	"Clock"
};

/* rows of the System page: label, retro.conf key, default, choices */
typedef struct Option {
	const char *label, *key, *def;
	const char *choices[16];
	int nchoices;
	RuiButton b;
} Option;

typedef struct KV {
	char key[48], val[256];
} KV;

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty, page, reexec;
	RuiButton cat[PG_COUNT], ok, apply, close;
	char msg[160];
	KV conf[MAX_KV], pend[MAX_KV];
	int nconf, npend;

	char **themes;
	int nthemes;
	RuiList theme_list;
	RuiButton style_cde, style_classic;
	XftColor pv[RUI_C_THEMED];
	int pv_own[RUI_C_THEMED];          /* allocated by us (to be freed) */
	int pv_ok;

	char *families[MAX_FAMILIES];
	int nfam, fam_busy;
	RuiList fam_list, size_list;
	char *sizes[14];
	RuiFont *preview;

	char *pat_names[NPAT];
	RuiList pat_list;
	RuiInput wallpaper;
	char **wp_paths;                   /* installed and user wallpapers  */
	char **wp_names;                   /* "(none)", then their names     */
	int nwp;
	RuiList wp_list;
	RuiButton wp_mode;
	Image *wp_img;                     /* preview of the chosen one      */
	char wp_loaded[1024];

	RuiInput tool[RUI_TOOL_COUNT];
	char tool_hint[RUI_TOOL_COUNT][160];

	Option opt[24];
	int nopt;
	RuiInput *focus;
} st;

#define A(n) (st.app->atom[A_##n])

/* --------------------------------------------------------- key/value store */

static void
conf_cb(const char *key, const char *val, void *ud)
{
	int i;

	(void)ud;
	for (i = 0; i < st.nconf; i++)
		if (!strcmp(st.conf[i].key, key))
			break;
	if (i == st.nconf) {
		if (st.nconf == MAX_KV)
			return;
		st.nconf++;
	}
	rui_strlcpy(st.conf[i].key, key, sizeof st.conf[i].key);
	rui_strlcpy(st.conf[i].val, val, sizeof st.conf[i].val);
}

static void
conf_load(void)
{
	char *path = rui_config_path("retro.conf");

	st.nconf = 0;
	rui_conf_parse(path, conf_cb, NULL);
	free(path);
}

static const char *
get(const char *key, const char *def)
{
	int i;

	for (i = 0; i < st.npend; i++)
		if (!strcmp(st.pend[i].key, key))
			return st.pend[i].val;
	for (i = 0; i < st.nconf; i++)
		if (!strcmp(st.conf[i].key, key))
			return st.conf[i].val;
	return def;
}

static void
set(const char *key, const char *val)
{
	int i;

	for (i = 0; i < st.npend; i++)
		if (!strcmp(st.pend[i].key, key))
			break;
	if (i == st.npend) {
		if (st.npend == MAX_KV)
			return;
		st.npend++;
	}
	rui_strlcpy(st.pend[i].key, key, sizeof st.pend[i].key);
	rui_strlcpy(st.pend[i].val, val, sizeof st.pend[i].val);
	snprintf(st.msg, sizeof st.msg, "%d change%s not applied yet.", st.npend,
	    st.npend == 1 ? "" : "s");
	st.dirty = 1;
}

/* writes pending changes; returns the number written */
static int
commit(void)
{
	char *path = rui_config_path("retro.conf");
	int i, n = 0;

	for (i = 0; i < st.npend; i++) {
		if (rui_conf_set(path, st.pend[i].key, st.pend[i].val) == 0)
			n++;
		else
			rui_warn("cannot write %s", path);
	}
	free(path);
	st.npend = 0;
	conf_load();
	return n;
}

/* ------------------------------------------------------------ page: Color */

static void
preview_free(void)
{
	int i;

	if (!st.pv_ok)
		return;
	for (i = 0; i < RUI_C_THEMED; i++)
		if (st.pv_own[i])
			XftColorFree(st.app->dpy, st.app->vis, st.app->cmap, &st.pv[i]);
	memset(st.pv_own, 0, sizeof st.pv_own);
	st.pv_ok = 0;
}

static void
preview_load(const char *name)
{
	const RuiTheme *t = rui_theme_find(name);
	int i;

	preview_free();
	if (!t)
		return;
	for (i = 0; i < RUI_C_THEMED; i++) {
		st.pv_own[i] = t->colors[i] && XftColorAllocName(st.app->dpy,
		    st.app->vis, st.app->cmap, t->colors[i], &st.pv[i]);
		if (!st.pv_own[i])
			st.pv[i] = st.app->color[i];
	}
	st.pv_ok = 1;
}

static void
pfill(int x, int y, int w, int h, int c)
{
	rui_fill_pixel(st.s, x, y, w, h, st.pv[c].pixel);
}

static void
pbevel(int x, int y, int w, int h, int raised)
{
	int l = raised ? RUI_C_LIGHT : RUI_C_SHADOW;
	int d = raised ? RUI_C_SHADOW : RUI_C_LIGHT;

	pfill(x, y, w, 1, l);
	pfill(x, y, 1, h, l);
	pfill(x, y + h - 1, w, 1, d);
	pfill(x + w - 1, y, 1, h, d);
}

static void
ptext(int x, int y, const char *t, int c)
{
	RuiFont *f = st.app->bold;

	if (f->xft)
		XftDrawStringUtf8(st.s->xd, &st.pv[c], f->xft, x, y + f->ascent,
		    (const FcChar8 *)t, (int)strlen(t));
}

static void
draw_color_page(RuiRect area)
{
	RuiApp *a = st.app;
	int px = area.x + 230, py = area.y, pw = area.w - 230, ph = 220;
	int th = a->bold->height + 6;
	RuiRect r;

	st.theme_list.r.x = area.x;
	st.theme_list.r.y = area.y;
	st.theme_list.r.w = 214;
	st.theme_list.r.h = area.h;
	rui_list_draw(st.s, &st.theme_list);
	rui_bevel(st.s, px, py, pw, ph, RUI_SUNKEN);
	if (st.pv_ok) {
		/* a miniature desktop with an active and an inactive window */
		pfill(px + 2, py + 2, pw - 4, ph - 4, RUI_C_DESKTOP);
		pfill(px + 20, py + 16, pw - 90, 110, RUI_C_TITLE_INACTIVE);
		pbevel(px + 20, py + 16, pw - 90, 110, 1);
		ptext(px + 30, py + 19, "Inactive", RUI_C_TITLE_FG_INACTIVE);
		pfill(px + 60, py + 60, pw - 80, 140, RUI_C_TITLE_ACTIVE);
		pbevel(px + 60, py + 60, pw - 80, 140, 1);
		ptext(px + 70, py + 63, "Active window", RUI_C_TITLE_FG);
		pfill(px + 66, py + 60 + th, pw - 92, 140 - th - 6, RUI_C_FACE);
		pfill(px + 76, py + 70 + th, pw - 180, 22, RUI_C_FIELD);
		pbevel(px + 76, py + 70 + th, pw - 180, 22, 0);
		pfill(px + 77, py + 71 + th, 60, 20, RUI_C_SEL_BG);
		ptext(px + 80, py + 73 + th, "Select", RUI_C_SEL_FG);
		pfill(px + 76, py + 104 + th, 80, 24, RUI_C_FACE);
		pbevel(px + 76, py + 104 + th, 80, 24, 1);
		ptext(px + 95, py + 107 + th, "OK", RUI_C_TEXT);
	}
	r.x = px;
	r.y = py + ph + 12;
	r.w = pw;
	r.h = a->font->height + 4;
	rui_text_fit(st.s, a->bold, r, "Window style:", RUI_C_TEXT, RUI_LEFT);
	st.style_cde.r.x = px;
	st.style_classic.r.x = px + pw / 2 + 4;
	st.style_cde.r.y = st.style_classic.r.y = r.y + r.h + 4;
	st.style_cde.r.w = st.style_classic.r.w = pw / 2 - 4;
	st.style_cde.r.h = st.style_classic.r.h = a->font->height + 12;
	{
		const char *sv = get("style", "");
		int style = *sv ? rui_style_parse(sv) : -1;

		if (style < 0) {
			const RuiTheme *t = rui_theme_find(get("theme",
			    rui_settings()->theme));

			style = t ? t->style : RUI_STYLE_CDE;
		}
		st.style_cde.toggled = style == RUI_STYLE_CDE;
		st.style_classic.toggled = style == RUI_STYLE_CLASSIC;
	}
	rui_button_draw(st.s, &st.style_cde);
	rui_button_draw(st.s, &st.style_classic);
	r.y = st.style_cde.r.y + st.style_cde.r.h + 6;
	rui_text_fit(st.s, a->font, r, "CDE: Motif frames, minimized windows as "
	    "icons.", RUI_C_DISABLED, RUI_LEFT);
	if (st.theme_list.sel >= 0) {
		const char *name = st.themes[st.theme_list.sel];
		const RuiThemeModule *m = rui_theme_module(name);
		const char *kind = rui_theme_kind(name);
		char buf[200];

		if (m)
			snprintf(buf, sizeof buf, "C module%s%s", m->description ?
			    ": " : "", m->description ? m->description : "");
		else
			snprintf(buf, sizeof buf, "%s theme", kind ? kind : "unknown");
		r.y += r.h + 2;
		rui_text_fit(st.s, a->font, r, buf, RUI_C_DISABLED, RUI_LEFT);
	}
}

/* ------------------------------------------------------------- page: Font */

static int
cmp_str(const void *x, const void *y)
{
	return strcasecmp(*(char *const *)x, *(char *const *)y);
}

static void
font_spec_split(const char *spec, char *fam, size_t n, int *size)
{
	const char *dash = strrchr(spec, '-');

	*size = 9;
	if (dash && dash[1] >= '0' && dash[1] <= '9') {
		snprintf(fam, n, "%.*s", (int)(dash - spec), spec);
		*size = atoi(dash + 1);
	} else {
		rui_strlcpy(fam, spec, n);
		fam[strcspn(fam, ":")] = '\0';
	}
}

static void
font_preview_update(void)
{
	char spec[300];

	if (st.preview)
		rui_font_close(st.app, st.preview);
	st.preview = NULL;
	if (st.fam_list.sel < 0 || st.size_list.sel < 0)
		return;
	snprintf(spec, sizeof spec, "%s-%s", st.families[st.fam_list.sel],
	    st.sizes[st.size_list.sel]);
	st.preview = rui_font_open(st.app, spec);
}

static void
font_select_current(void)
{
	char fam[256];
	int size, i;

	font_spec_split(get("font", rui_settings()->font), fam, sizeof fam, &size);
	st.fam_list.sel = st.size_list.sel = -1;
	for (i = 0; i < st.nfam; i++)
		if (!strcasecmp(st.families[i], fam))
			st.fam_list.sel = i;
	for (i = 0; i < 14; i++)
		if (atoi(st.sizes[i]) == size)
			st.size_list.sel = i;
	st.fam_list.top = st.fam_list.sel > 3 ? st.fam_list.sel - 3 : 0;
	font_preview_update();
}

static void
add_family(const char *f)
{
	int i;

	if (!*f || st.nfam >= MAX_FAMILIES)
		return;
	for (i = 0; i < st.nfam; i++)
		if (!strcasecmp(st.families[i], f))
			return;
	st.families[st.nfam++] = rui_strdup(f);
}

static void
on_fc_list(const char *out, int status, void *ud)
{
	char line[512];
	const char *p = out ? out : "";

	(void)ud;
	(void)status;
	st.fam_busy = 0;
	while (rui_next_line(&p, line, sizeof line)) {
		line[strcspn(line, ",")] = '\0';    /* first name of each family */
		add_family(rui_ltrim(rui_rtrim(line)));
	}
	if (st.nfam > 3)
		qsort(st.families + 3, (size_t)(st.nfam - 3), sizeof st.families[0],
		    cmp_str);
	st.fam_list.items = st.families;
	st.fam_list.n = st.nfam;
	font_select_current();
	st.dirty = 1;
}

static void
fonts_init(void)
{
	static const char *const generic[] = { "sans", "serif", "monospace" };
	int i;

	for (i = 0; i < 3; i++)
		add_family(generic[i]);
	for (i = 0; i < 14; i++)
		st.sizes[i] = rui_strdupf("%d", i + 7);
	st.size_list.items = st.sizes;
	st.size_list.n = 14;
	st.fam_list.items = st.families;
	st.fam_list.n = st.nfam;
	if (rui_have_cmd("fc-list") &&
	    rui_job_start("fc-list : family", on_fc_list, NULL) == 0) {
		st.fam_busy = 1;
	} else {
		static const char *const common[] = {
			"Helvetica", "Times", "Courier", "Lucida", "fixed", NULL
		};

		for (i = 0; common[i]; i++)
			add_family(common[i]);
		st.fam_list.n = st.nfam;
	}
	font_select_current();
}

static void
draw_font_page(RuiRect area)
{
	RuiApp *a = st.app;
	RuiRect r;
	int lh = area.h - 110;

	st.fam_list.r.x = area.x;
	st.fam_list.r.y = area.y;
	st.fam_list.r.w = area.w - 110;
	st.fam_list.r.h = lh;
	st.size_list.r.x = area.x + area.w - 100;
	st.size_list.r.y = area.y;
	st.size_list.r.w = 100;
	st.size_list.r.h = lh;
	rui_list_draw(st.s, &st.fam_list);
	rui_list_draw(st.s, &st.size_list);
	r.x = area.x;
	r.y = area.y + lh + 10;
	r.w = area.w;
	r.h = area.h - lh - 10;
	rui_fill(st.s, r.x, r.y, r.w, r.h, RUI_C_FIELD);
	rui_bevel(st.s, r.x, r.y, r.w, r.h, RUI_SUNKEN);
	r.x += 10;
	r.w -= 20;
	if (st.fam_busy)
		rui_text_fit(st.s, a->font, r, "Reading the font list...",
		    RUI_C_DISABLED, RUI_LEFT);
	else if (st.preview)
		rui_text_fit(st.s, st.preview, r,
		    "The quick brown fox jumps over the lazy dog 0123", RUI_C_TEXT,
		    RUI_LEFT);
}

/* --------------------------------------------------------- page: Backdrop */

static void
wallpapers_init(void)
{
	const char *cur = get("wallpaper", "");
	char *found = rui_wallpaper_find(cur);
	int i;

	st.wp_paths = rui_wallpaper_list();
	for (st.nwp = 0; st.wp_paths[st.nwp]; st.nwp++)
		;
	st.wp_names = rui_calloc((size_t)st.nwp + 2, sizeof *st.wp_names);
	st.wp_names[0] = rui_strdup("(none)");
	st.wp_list.sel = *cur ? -1 : 0;
	for (i = 0; i < st.nwp; i++) {
		const char *base = strrchr(st.wp_paths[i], '/');
		char *name = rui_strdup(base ? base + 1 : st.wp_paths[i]);
		char *dot = strrchr(name, '.');

		if (dot)
			*dot = '\0';
		st.wp_names[i + 1] = name;
		if (found && !strcmp(found, st.wp_paths[i]))
			st.wp_list.sel = i + 1;
	}
	free(found);
	st.wp_list.items = st.wp_names;
	st.wp_list.n = st.nwp + 1;
	st.wp_list.last_idx = -1;
	st.wp_mode.label = get("wallpaper_mode", "auto");
}

/* the wallpaper named in the input field, loaded once per change */
static const Image *
wallpaper_image(void)
{
	const char *want = st.wallpaper.text;

	if (strcmp(want, st.wp_loaded)) {
		char *path = rui_wallpaper_find(want);
		struct stat sb;

		image_free(st.wp_img);
		st.wp_img = NULL;
		rui_strlcpy(st.wp_loaded, want, sizeof st.wp_loaded);
		/* typing a path must not spam warnings for every keystroke */
		if (path && stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
			st.wp_img = image_load(st.app, path);
		free(path);
	}
	return st.wp_img;
}

static void
draw_backdrop_page(RuiRect area)
{
	RuiApp *a = st.app;
	int px = area.x + 190, pw = area.w - 190, ph = 180, sel = st.pat_list.sel;
	int lh = a->font->height + 4, half = (area.h - 2 * lh - 8) / 2;
	const Image *img = wallpaper_image();
	RuiRect r;

	r.x = area.x;
	r.y = area.y;
	r.w = 176;
	r.h = lh;
	rui_text_fit(st.s, a->bold, r, "Pattern", RUI_C_TEXT, RUI_LEFT);
	st.pat_list.r.x = area.x;
	st.pat_list.r.y = area.y + lh;
	st.pat_list.r.w = 176;
	st.pat_list.r.h = half;
	rui_list_draw(st.s, &st.pat_list);
	r.y = st.pat_list.r.y + half + 8;
	rui_text_fit(st.s, a->bold, r, "Wallpaper", RUI_C_TEXT, RUI_LEFT);
	st.wp_list.r.x = area.x;
	st.wp_list.r.y = r.y + lh;
	st.wp_list.r.w = 176;
	st.wp_list.r.h = area.y + area.h - st.wp_list.r.y;
	rui_list_draw(st.s, &st.wp_list);

	rui_bevel(st.s, px, area.y, pw, ph, RUI_SUNKEN);
	if (sel > 0) {
		Pixmap stip = XCreateBitmapFromData(a->dpy, a->root,
		    (const char *)patterns[sel].bits, 8, 8);

		XSetForeground(a->dpy, a->gc, a->color[RUI_C_DARK].pixel);
		XSetBackground(a->dpy, a->gc, a->color[RUI_C_DESKTOP].pixel);
		XSetStipple(a->dpy, a->gc, stip);
		XSetFillStyle(a->dpy, a->gc, FillOpaqueStippled);
		XFillRectangle(a->dpy, st.s->buf, a->gc, px + 2, area.y + 2,
		    (unsigned)(pw - 4), (unsigned)(ph - 4));
		XSetFillStyle(a->dpy, a->gc, FillSolid);
		XSetStipple(a->dpy, a->gc, a->checker);
		XFreePixmap(a->dpy, stip);
	} else {
		rui_fill(st.s, px + 2, area.y + 2, pw - 4, ph - 4, RUI_C_DESKTOP);
	}
	if (img) {
		/* the picture as the desktop will show it */
		const char *ms = get("wallpaper_mode", "auto");
		int mode = !strcmp(ms, "auto") ? (img->w <= 256 && img->h <= 256 ?
		    IMG_TILE : IMG_FILL) : image_mode(ms);

		if (mode == IMG_CENTER)            /* a miniature, not a crop */
			mode = IMG_FILL;
		image_render_at(a, img, st.s->buf, px + 2, area.y + 2, pw - 4,
		    ph - 4, mode);
	}
	r.x = px;
	r.y = area.y + ph + 14;
	r.w = pw;
	r.h = a->font->height + 4;
	rui_text_fit(st.s, a->bold, r, "Wallpaper name or file (XPM, PPM):",
	    RUI_C_TEXT, RUI_LEFT);
	st.wallpaper.r.x = px;
	st.wallpaper.r.y = r.y + r.h + 4;
	st.wallpaper.r.w = pw;
	st.wallpaper.r.h = a->font->height + 10;
	st.wallpaper.focused = st.focus == &st.wallpaper;
	rui_input_draw(st.s, &st.wallpaper);
	r.y = st.wallpaper.r.y + st.wallpaper.r.h + 12;
	r.w = 110;
	r.h = a->font->height + 12;
	rui_text_fit(st.s, a->font, r, "Placement:", RUI_C_TEXT, RUI_LEFT);
	st.wp_mode.r.x = px + 114;
	st.wp_mode.r.y = r.y;
	st.wp_mode.r.w = 150;
	st.wp_mode.r.h = r.h;
	st.wp_mode.label = get("wallpaper_mode", "auto");
	rui_button_draw(st.s, &st.wp_mode);
	rui_arrow(st.s, st.wp_mode.r.x + st.wp_mode.r.w - 12, st.wp_mode.r.y +
	    st.wp_mode.r.h / 2, 4, 1, RUI_C_TEXT);
	r.x = px;
	r.y += r.h + 8;
	r.w = pw;
	r.h = a->font->height + 4;
	rui_text_fit(st.s, a->font, r, "Each monitor gets its own copy.",
	    RUI_C_DISABLED, RUI_LEFT);
	r.y += r.h;
	rui_text_fit(st.s, a->font, r, "Your own: ~/.config/retro-de/wallpapers",
	    RUI_C_DISABLED, RUI_LEFT);
}

static void
wallpaper_mode_popup(void)
{
	static const char *const modes[] = {
		"auto", "fill", "stretch", "center", "tile"
	};
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	Window child;
	int x, y, i;

	for (i = 0; i < 5; i++)
		rui_menu_add(m, modes[i], NULL, i + 1)->checked =
		    !strcmp(modes[i], get("wallpaper_mode", "auto"));
	XTranslateCoordinates(st.app->dpy, st.win, st.app->root, st.wp_mode.r.x,
	    st.wp_mode.r.y + st.wp_mode.r.h, &x, &y, &child);
	it = rui_menu_popup(st.app, m, x, y, 0);
	if (it)
		set("wallpaper_mode", modes[it->id - 1]);
	rui_menu_free(m);
}

/* ------------------------------------------------------ page: Applications */

static void
tool_hints(void)
{
	int i;

	for (i = 0; i < RUI_TOOL_COUNT; i++) {
		const char *v = st.tool[i].text;

		if (!*v || !strcmp(v, "auto")) {
			char *c = rui_tool_command(i);

			snprintf(st.tool_hint[i], sizeof st.tool_hint[i],
			    "auto: %s", c ? c : "nothing installed");
			free(c);
		} else {
			st.tool_hint[i][0] = '\0';
		}
	}
}

static void
draw_apps_page(RuiRect area)
{
	RuiApp *a = st.app;
	int i, rh = a->font->height * 2 + 22;
	RuiRect r;

	for (i = 0; i < RUI_TOOL_COUNT; i++) {
		r.x = area.x;
		r.y = area.y + i * rh;
		r.w = 130;
		r.h = a->font->height + 10;
		rui_text_fit(st.s, a->bold, r, tool_labels[i], RUI_C_TEXT, RUI_LEFT);
		st.tool[i].r.x = area.x + 136;
		st.tool[i].r.y = r.y;
		st.tool[i].r.w = area.w - 136;
		st.tool[i].r.h = r.h;
		st.tool[i].focused = st.focus == &st.tool[i];
		rui_input_draw(st.s, &st.tool[i]);
		r.x = area.x + 140;
		r.y += r.h + 2;
		r.w = area.w - 140;
		r.h = a->font->height + 2;
		rui_text_fit(st.s, a->font, r, st.tool_hint[i], RUI_C_DISABLED,
		    RUI_LEFT);
	}
	r.x = area.x;
	r.y = area.y + RUI_TOOL_COUNT * rh;
	r.w = area.w;
	r.h = a->font->height + 4;
	rui_text_fit(st.s, a->font, r, "\"auto\" uses the XDG default application "
	    "(xdg-mime) or the first one installed.", RUI_C_DISABLED, RUI_LEFT);
}

/* ------------------------------------------------------------ page: System */

static void
opt_add(const char *label, const char *key, const char *def)
{
	Option *o = &st.opt[st.nopt++];

	memset(o, 0, sizeof *o);
	o->label = label;
	o->key = key;
	o->def = def;
}

static void
opt_choice(const char *c)
{
	Option *o = &st.opt[st.nopt - 1];

	if (o->nchoices < 16)
		o->choices[o->nchoices++] = c;
}

static void
options_init(void)
{
	int i;

	opt_add("Audio", "audio_backend", "auto");
	opt_choice("auto");
	for (i = 0; rui_config.audio[i]; i++)
		opt_choice(rui_config.audio[i]->name);
	opt_choice("none");
	opt_add("Wi-Fi", "net_backend", "auto");
	opt_choice("auto");
	for (i = 0; rui_config.net[i]; i++)
		opt_choice(rui_config.net[i]->name);
	opt_choice("none");
	opt_add("Power / init", "power_backend", "auto");
	opt_choice("auto");
	for (i = 0; rui_config.power[i]; i++)
		opt_choice(rui_config.power[i]->name);
	opt_choice("none");
	opt_add("D-Bus session bus", "dbus", rui_config.dbus);
	opt_choice("auto");
	opt_choice("on");
	opt_choice("off");
	opt_add("XDG .desktop menus", "xdg_menus",
	    rui_config.desktop_entries ? "on" : "off");
	opt_choice("on");
	opt_choice("off");
	opt_add("XDG autostart", "xdg_autostart",
	    rui_config.xdg_autostart ? "on" : "off");
	opt_choice("on");
	opt_choice("off");
	opt_add("Bluetooth", "bluetooth_backend", "auto");
	opt_choice("auto");
	for (i = 0; rui_config.bt[i]; i++)
		opt_choice(rui_config.bt[i]->name);
	opt_choice("none");
	opt_add("Panel", "panel_style", "taskbar");
	opt_choice("taskbar");
	opt_choice("frontpanel");
	opt_choice("auto");
	opt_add("Minimized windows", "desktop_icons", "auto");
	opt_choice("auto");
	opt_choice("on");
	opt_choice("off");
	opt_add("Focus", "focus", "click");
	opt_choice("click");
	opt_choice("sloppy");
	opt_add("Confirm logout", "confirm_logout", "on");
	opt_choice("on");
	opt_choice("off");
	/* xscreensaver when installed, else the X server's blanking */
	opt_add("Screen saver", "screensaver", "auto");
	opt_choice("auto");
	opt_choice("xscreensaver");
	opt_choice("blank");
	opt_choice("off");
	opt_add("Blank after (min)", "screensaver_timeout", "10");
	opt_choice("1");
	opt_choice("2");
	opt_choice("5");
	opt_choice("10");
	opt_choice("15");
	opt_choice("30");
	opt_choice("60");
	opt_choice("never");
	opt_add("Monitor off (min)", "monitor_off", "20");
	opt_choice("5");
	opt_choice("10");
	opt_choice("20");
	opt_choice("30");
	opt_choice("60");
	opt_choice("never");
	opt_add("New windows", "placement", "cascade");
	opt_choice("cascade");
	opt_choice("smart");
	opt_choice("center");
	opt_choice("pointer");
	opt_add("Snap at screen edges", "edge_snap", "on");
	opt_choice("on");
	opt_choice("off");
}

static void
draw_system_page(RuiRect area)
{
	RuiApp *a = st.app;
	int i, rh = a->font->height + 14, half = (st.nopt + 1) / 2;
	int cw = (area.w - 12) / 2;
	RuiRect r;

	/* two columns */
	for (i = 0; i < st.nopt; i++) {
		Option *o = &st.opt[i];
		int x = area.x + (i >= half ? cw + 12 : 0);

		r.x = x;
		r.y = area.y + (i % half) * rh;
		r.w = 136;
		r.h = rh - 4;
		rui_text_fit(st.s, a->font, r, o->label, RUI_C_TEXT, RUI_LEFT);
		o->b.r.x = x + 140;
		o->b.r.y = r.y;
		o->b.r.w = cw - 140;
		o->b.r.h = rh - 4;
		o->b.label = get(o->key, o->def);
		rui_button_draw(st.s, &o->b);
		rui_arrow(st.s, o->b.r.x + o->b.r.w - 12, o->b.r.y + o->b.r.h / 2,
		    4, 1, RUI_C_TEXT);
	}
	r.x = area.x;
	r.y = area.y + half * rh + 4;
	r.w = area.w;
	r.h = a->font->height + 4;
	rui_text_fit(st.s, a->font, r, "Keys, window options and per-program "
	    "rules: keys.conf, windows.conf, programs.conf", RUI_C_DISABLED,
	    RUI_LEFT);
	if (!rui_config.desktop_entries) {
		r.y += r.h;
		rui_text_fit(st.s, a->font, r, ".desktop support is not built in "
		    "(DESKTOP_ENTRIES in config.h).", RUI_C_DISABLED, RUI_LEFT);
	}
}

/* ------------------------------------------------------------------ frame */

static RuiRect
content_area(void)
{
	RuiRect r;
	int top = 16 + 60 + 14;

	r.x = 16;
	r.y = top;
	r.w = WIN_W - 32;
	r.h = WIN_H - top - st.app->font->height - 40;
	return r;
}

static void
draw_cat_icon(int page, RuiRect b)
{
	RuiSurf *s = st.s;
	int cx = b.x + b.w / 2, y = b.y + 6, i;

	switch (page) {
	case PG_COLOR:
		rui_fill(s, cx - 13, y, 12, 12, RUI_C_TITLE_ACTIVE);
		rui_fill(s, cx + 1, y, 12, 12, RUI_C_DESKTOP);
		rui_fill(s, cx - 13, y + 14, 12, 12, RUI_C_ALERT);
		rui_fill(s, cx + 1, y + 14, 12, 12, RUI_C_GOOD);
		break;
	case PG_FONT: {
		RuiRect r;

		r.x = b.x;
		r.y = y;
		r.w = b.w;
		r.h = 26;
		rui_text_fit(s, st.app->bold, r, "Aa", RUI_C_TEXT, RUI_CENTER);
		break;
	}
	case PG_BACKDROP:
		rui_fill_dither(s, cx - 14, y, 28, 26, RUI_C_DESKTOP, RUI_C_SHADOW);
		rui_bevel(s, cx - 14, y, 28, 26, RUI_SUNKEN_THIN);
		break;
	case PG_APPS:
		rui_fill(s, cx - 14, y, 28, 20, RUI_C_DARK);
		rui_bevel(s, cx - 14, y, 28, 20, RUI_RAISED_THIN);
		rui_fill(s, cx - 10, y + 5, 6, 2, RUI_C_GOOD);
		rui_fill(s, cx - 4, y + 22, 8, 4, RUI_C_SHADOW);
		break;
	case PG_SYSTEM:
		for (i = 0; i < 3; i++) {
			rui_bevel(s, cx - 14, y + i * 9, 28, 7, RUI_SUNKEN_THIN);
			rui_fill(s, cx - 13 + (i % 2 ? 14 : 0), y + 1 + i * 9, 13, 5,
			    RUI_C_FACE);
			rui_bevel(s, cx - 13 + (i % 2 ? 14 : 0), y + 1 + i * 9, 13, 5,
			    RUI_RAISED_THIN);
		}
		break;
	default:
		break;
	}
}

static void
draw(void)
{
	RuiSurf *s = st.s;
	RuiApp *a = st.app;
	RuiRect area = content_area(), r;
	int i, bw = (WIN_W - 32 - 4 * 8) / PG_COUNT;

	rui_fill(s, 0, 0, WIN_W, WIN_H, RUI_C_FACE);
	for (i = 0; i < PG_COUNT; i++) {
		RuiButton *b = &st.cat[i];

		b->r.x = 16 + i * (bw + 8);
		b->r.y = 16;
		b->r.w = bw;
		b->r.h = 60;
		b->toggled = i == st.page;
		b->label = NULL;
		rui_button_draw(s, b);
		draw_cat_icon(i, b->r);
		r = b->r;
		r.y += 36;
		r.h = 20;
		rui_text_fit(s, i == st.page ? a->bold : a->font, r, page_names[i],
		    RUI_C_TEXT, RUI_CENTER);
	}
	rui_bevel(s, 8, 84, WIN_W - 16, area.h + 14, RUI_GROOVE);
	switch (st.page) {
	case PG_COLOR:
		draw_color_page(area);
		break;
	case PG_FONT:
		draw_font_page(area);
		break;
	case PG_BACKDROP:
		draw_backdrop_page(area);
		break;
	case PG_APPS:
		draw_apps_page(area);
		break;
	default:
		draw_system_page(area);
		break;
	}
	r.x = 16;
	r.y = WIN_H - a->font->height - 26;
	r.w = WIN_W - 3 * 96 - 50;
	r.h = a->font->height + 12;
	rui_text_fit(s, a->font, r, st.msg, RUI_C_TEXT, RUI_LEFT);
	rui_button_draw(s, &st.ok);
	rui_button_draw(s, &st.apply);
	rui_button_draw(s, &st.close);
	rui_surf_flush(s);
	st.dirty = 0;
}

/* ---------------------------------------------------------------- actions */

static int
look_changed(void)
{
	static const char *const keys[] = {
		"theme", "style", "font", "desktop_pattern", NULL
	};
	int i, k;

	for (i = 0; i < st.npend; i++)
		for (k = 0; keys[k]; k++)
			if (!strcmp(st.pend[i].key, keys[k]))
				return 1;
	return 0;
}

static void
apply(int then_close)
{
	int look = look_changed(), n = commit();

	if (n) {
		rui_request_reload(st.app->dpy);
		snprintf(st.msg, sizeof st.msg, "Applied %d change%s.", n,
		    n == 1 ? "" : "s");
		if (look && !then_close) {
			st.reexec = 1;                 /* redraw ourselves too */
			st.running = 0;
		}
	} else {
		snprintf(st.msg, sizeof st.msg, "Nothing to apply.");
	}
	if (then_close)
		st.running = 0;
	st.dirty = 1;
}

static void
option_popup(Option *o)
{
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	int x, y, i;
	Window child;

	for (i = 0; i < o->nchoices; i++)
		rui_menu_add(m, o->choices[i], NULL, i + 1)->checked =
		    !strcmp(o->choices[i], get(o->key, o->def));
	XTranslateCoordinates(st.app->dpy, st.win, st.app->root, o->b.r.x,
	    o->b.r.y + o->b.r.h, &x, &y, &child);
	it = rui_menu_popup(st.app, m, x, y, 0);
	if (it)
		set(o->key, o->choices[it->id - 1]);
	rui_menu_free(m);
}

static void
handle_page_event(XEvent *ev)
{
	RuiRect area = content_area();
	int r, i;

	(void)area;
	switch (st.page) {
	case PG_COLOR:
		r = rui_list_event(st.app, &st.theme_list, ev);
		if ((r & RUI_EV_CHANGED) && st.theme_list.sel >= 0) {
			set("theme", st.themes[st.theme_list.sel]);
			preview_load(st.themes[st.theme_list.sel]);
		}
		if (rui_button_event(&st.style_cde, ev) & RUI_EV_ACTIVATE)
			set("style", "cde");
		if (rui_button_event(&st.style_classic, ev) & RUI_EV_ACTIVATE)
			set("style", "classic");
		st.dirty |= r != 0;
		break;
	case PG_FONT:
		r = rui_list_event(st.app, &st.fam_list, ev) |
		    rui_list_event(st.app, &st.size_list, ev);
		if ((r & RUI_EV_CHANGED) && st.fam_list.sel >= 0 &&
		    st.size_list.sel >= 0) {
			char spec[300];

			snprintf(spec, sizeof spec, "%s-%s",
			    st.families[st.fam_list.sel], st.sizes[st.size_list.sel]);
			set("font", spec);
			font_preview_update();
		}
		st.dirty |= r != 0;
		break;
	case PG_BACKDROP:
		r = rui_list_event(st.app, &st.pat_list, ev);
		if ((r & RUI_EV_CHANGED) && st.pat_list.sel >= 0)
			set("desktop_pattern", patterns[st.pat_list.sel].name);
		i = rui_list_event(st.app, &st.wp_list, ev);
		if ((i & RUI_EV_CHANGED) && st.wp_list.sel >= 0) {
			/* installed wallpapers are stored by name, so they survive
			 * a reinstall under another prefix */
			const char *name = st.wp_list.sel ?
			    st.wp_names[st.wp_list.sel] : "";

			rui_input_set(&st.wallpaper, name);
			set("wallpaper", name);
		}
		r |= i;
		if (rui_button_event(&st.wp_mode, ev) & RUI_EV_ACTIVATE)
			wallpaper_mode_popup();
		if (ev->type == ButtonPress &&
		    rui_in_rect(st.wallpaper.r, ev->xbutton.x, ev->xbutton.y))
			st.focus = &st.wallpaper;
		st.dirty |= r != 0;
		break;
	case PG_APPS:
		for (i = 0; i < RUI_TOOL_COUNT; i++)
			if (ev->type == ButtonPress &&
			    rui_in_rect(st.tool[i].r, ev->xbutton.x, ev->xbutton.y))
				st.focus = &st.tool[i];
		break;
	default:
		for (i = 0; i < st.nopt; i++)
			if (rui_button_event(&st.opt[i].b, ev) & RUI_EV_ACTIVATE)
				option_popup(&st.opt[i]);
		break;
	}
	if (st.focus && (ev->type == ButtonPress || ev->type == SelectionNotify))
		if (rui_input_event(st.app, st.focus, ev))
			st.dirty = 1;
	if (ev->type == ButtonPress || ev->type == ButtonRelease)
		st.dirty = 1;
}

static void
input_changed(RuiInput *in)
{
	int i;

	if (in == &st.wallpaper) {
		int i;

		set("wallpaper", in->text);
		st.wp_list.sel = in->text[0] ? -1 : 0;
		for (i = 0; i < st.nwp; i++)
			if (!strcmp(st.wp_names[i + 1], in->text))
				st.wp_list.sel = i + 1;
		return;
	}
	for (i = 0; i < RUI_TOOL_COUNT; i++)
		if (in == &st.tool[i])
			set(rui_tool_keys[i], in->text[0] ? in->text : "auto");
}

static void
switch_page(int p)
{
	st.page = p;
	st.focus = p == PG_BACKDROP ? &st.wallpaper : p == PG_APPS ?
	    &st.tool[0] : NULL;
	if (p == PG_APPS)
		tool_hints();
	st.dirty = 1;
}

static void
handle_event(XEvent *ev)
{
	int i, r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(st.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (ks == XK_Escape) {
			st.running = 0;
			break;
		}
		if ((ev->xkey.state & ControlMask) && ks >= XK_1 &&
		    ks < XK_1 + PG_COUNT) {
			switch_page((int)(ks - XK_1));
			break;
		}
		if (st.focus) {
			r = rui_input_event(st.app, st.focus, ev);
			if (r & RUI_EV_CHANGED)
				input_changed(st.focus);
			if (r & RUI_EV_ACTIVATE)
				apply(0);
			if (ks == XK_Tab && st.page == PG_APPS) {
				for (i = 0; i < RUI_TOOL_COUNT; i++)
					if (st.focus == &st.tool[i])
						break;
				st.focus = &st.tool[(i + 1) % RUI_TOOL_COUNT];
			}
			st.dirty = 1;
		} else if (st.page == PG_COLOR) {
			r = rui_list_key(st.app, &st.theme_list, ks);
			if ((r & RUI_EV_CHANGED) && st.theme_list.sel >= 0) {
				set("theme", st.themes[st.theme_list.sel]);
				preview_load(st.themes[st.theme_list.sel]);
			}
			st.dirty |= r != 0;
		}
		break;
	}
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		for (i = 0; i < PG_COUNT; i++) {
			if (rui_button_event(&st.cat[i], ev) & RUI_EV_ACTIVATE)
				switch_page(i);
		}
		if (rui_button_event(&st.ok, ev) & RUI_EV_ACTIVATE)
			apply(1);
		if (rui_button_event(&st.apply, ev) & RUI_EV_ACTIVATE)
			apply(0);
		if (rui_button_event(&st.close, ev) & RUI_EV_ACTIVATE)
			st.running = 0;
		handle_page_event(ev);
		break;
	case SelectionNotify:
		if (st.focus && rui_input_event(st.app, st.focus, ev) &
		    RUI_EV_CHANGED) {
			input_changed(st.focus);
			st.dirty = 1;
		}
		break;
	case ClientMessage:
		if (rui_wm_delete_event(st.app, ev))
			st.running = 0;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ setup */

static void
setup(int page)
{
	RuiApp *a = st.app;
	XSizeHints sh;
	int i, fh = a->font->height;
	const char *cur;

	conf_load();
	st.themes = rui_theme_list();
	for (st.nthemes = 0; st.themes[st.nthemes]; st.nthemes++)
		;
	st.theme_list.items = st.themes;
	st.theme_list.n = st.nthemes;
	st.theme_list.sel = st.theme_list.last_idx = -1;
	cur = rui_settings()->theme;
	for (i = 0; i < st.nthemes; i++)
		if (!strcasecmp(st.themes[i], cur))
			st.theme_list.sel = i;
	preview_load(cur);
	st.style_cde.label = "CDE / Motif";
	st.style_classic.label = "Classic";

	st.fam_list.last_idx = st.size_list.last_idx = -1;
	fonts_init();

	for (i = 0; i < NPAT; i++)
		st.pat_names[i] = (char *)patterns[i].name;
	st.pat_list.items = st.pat_names;
	st.pat_list.n = NPAT;
	st.pat_list.sel = 0;
	st.pat_list.last_idx = -1;
	for (i = 0; i < NPAT; i++)
		if (!strcmp(patterns[i].name, rui_settings()->pattern))
			st.pat_list.sel = i;

	options_init();
	st.ok.label = "OK";
	st.apply.label = "Apply";
	st.close.label = "Close";
	st.ok.r.y = st.apply.r.y = st.close.r.y = WIN_H - fh - 26;
	st.ok.r.w = st.apply.r.w = st.close.r.w = 90;
	st.ok.r.h = st.apply.r.h = st.close.r.h = fh + 12;
	st.close.r.x = WIN_W - 16 - 90;
	st.apply.r.x = st.close.r.x - 96;
	st.ok.r.x = st.apply.r.x - 96;
	st.ok.bold = 1;

	st.win = rui_window_new(a, a->root, (a->sw - WIN_W) / 2,
	    (a->sh - WIN_H) / 3, WIN_W, WIN_H, 0, ExposureMask | KeyPressMask |
	    ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	st.s = rui_surf_new(a, st.win, WIN_W, WIN_H);
	rui_set_title(a, st.win, "Style Manager", "retro-settings");
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = WIN_H;
	XSetWMNormalHints(a->dpy, st.win, &sh);
	rui_input_init(a, &st.wallpaper, st.win);
	rui_input_set(&st.wallpaper, get("wallpaper", ""));
	wallpapers_init();
	for (i = 0; i < RUI_TOOL_COUNT; i++) {
		rui_input_init(a, &st.tool[i], st.win);
		rui_input_set(&st.tool[i], rui_settings()->tool[i]);
	}
	snprintf(st.msg, sizeof st.msg, "Theme %s, %s style.", cur,
	    a->style == RUI_STYLE_CDE ? "CDE" : "Classic");
	switch_page(page >= 0 && page < PG_COUNT ? page : PG_COLOR);
	XMapWindow(a->dpy, st.win);
}

static void
cleanup(void)
{
	int i;

	preview_free();
	if (st.preview)
		rui_font_close(st.app, st.preview);
	rui_input_destroy(&st.wallpaper);
	for (i = 0; i < RUI_TOOL_COUNT; i++)
		rui_input_destroy(&st.tool[i]);
	for (i = 0; i < st.nfam; i++)
		free(st.families[i]);
	for (i = 0; i < 14; i++)
		free(st.sizes[i]);
	rui_strv_free(st.themes);
	rui_strv_free(st.wp_paths);
	rui_strv_free(st.wp_names);
	image_free(st.wp_img);
	rui_surf_free(st.s);
	XDestroyWindow(st.app->dpy, st.win);
}

/* --------------------------------------------------------------- the CLI */

#ifndef RETRO_INCDIR
# define RETRO_INCDIR "/usr/local/include/retro-de"
#endif

static int
run(const char *cmd)
{
	int st_ = system(cmd);

	return st_ == 0 ? 0 : -1;
}

/* Installs a theme into ~/.config/retro-de/themes: a compiled module
 * (NAME.so), its source (NAME.c, compiled here) or a NAME.theme palette.
 * `src` may be an http(s)/ftp URL; then it is downloaded first. */
static int
install_theme(const char *src)
{
	static const char *const fetchers[][2] = {
		{ "curl", "curl -fsSL -o" }, { "wget", "wget -q -O" },
		{ "fetch", "fetch -q -o" }, { "ftp", "ftp -V -o" },
	};
	const char *base = strrchr(src, '/'), *dot, *inc, *cc;
	char name[64], *dir, *dest, *tmp = NULL, *cmd, *qs, *qd;
	int url = !strncmp(src, "http://", 7) || !strncmp(src, "https://", 8) ||
	    !strncmp(src, "ftp://", 6), rc = 1;
	size_t i, len;

	base = base ? base + 1 : src;
	dot = strrchr(base, '.');
	len = dot ? (size_t)(dot - base) : 0;
	if (!dot || !len || len >= sizeof name || (strcmp(dot, ".so") &&
	    strcmp(dot, ".c") && strcmp(dot, ".theme"))) {
		fprintf(stderr, "retro-settings: a theme is NAME.so (a compiled "
		    "module), NAME.c (its source) or NAME.theme\n");
		return 2;
	}
	memcpy(name, base, len);
	name[len] = '\0';
	mkdir(rui_config_dir(), 0755);
	dir = rui_config_path("themes");
	mkdir(dir, 0755);
	if (url) {
		tmp = rui_strdupf("%s/.download-%ld%s", dir, (long)getpid(), dot);
		qs = rui_shell_quote(src);
		qd = rui_shell_quote(tmp);
		for (i = 0; i < sizeof fetchers / sizeof fetchers[0]; i++)
			if (rui_have_cmd(fetchers[i][0]))
				break;
		if (i == sizeof fetchers / sizeof fetchers[0]) {
			fprintf(stderr, "retro-settings: no curl, wget, fetch or ftp to "
			    "download with\n");
		} else {
			cmd = rui_strdupf("%s %s %s", fetchers[i][1], qd, qs);
			if (run(cmd) == 0)
				src = tmp;
			else
				fprintf(stderr, "retro-settings: download failed\n");
			free(cmd);
		}
		free(qs);
		free(qd);
		if (src != tmp) {
			unlink(tmp);
			free(tmp);
			free(dir);
			return 1;
		}
	}
	qs = rui_shell_quote(src);
	if (!strcmp(dot, ".c")) {
		dest = rui_strdupf("%s/%s.so", dir, name);
		qd = rui_shell_quote(dest);
		inc = getenv("RETRO_DE_INCLUDE");
		cc = getenv("CC");
		cmd = rui_strdupf("%s -shared -fPIC -O2 -I%s `pkg-config --cflags "
		    "x11 xft 2>/dev/null || echo -I/usr/include/freetype2` -o %s %s",
		    cc && *cc ? cc : "cc", inc && *inc ? inc : RETRO_INCDIR, qd, qs);
	} else {
		dest = rui_strdupf("%s/%s%s", dir, name, dot);
		qd = rui_shell_quote(dest);
		cmd = rui_strdupf("cp %s %s", qs, qd);
	}
	if (run(cmd) != 0) {
		fprintf(stderr, "retro-settings: could not %s %s\n",
		    !strcmp(dot, ".c") ? "compile" : "copy", src);
	} else if (strcmp(dot, ".theme") && !rui_theme_module(name)) {
		/* rui_theme_module() said why on stderr */
		unlink(dest);
		fprintf(stderr, "retro-settings: %s is not a usable theme module; "
		    "not installed\n", base);
	} else {
		const RuiThemeModule *m = rui_theme_module(name);

		printf("installed theme %s%s%s\n  %s\n  use it: retro-settings "
		    "--apply theme=%s\n", name, m && m->description ? " - " : "",
		    m && m->description ? m->description : "", dest, name);
		rc = 0;
	}
	if (tmp) {
		unlink(tmp);
		free(tmp);
	}
	free(cmd);
	free(qs);
	free(qd);
	free(dest);
	free(dir);
	return rc;
}

static int
cli(int argc, char **argv)
{
	char *path = rui_config_path("retro.conf");
	int i, rc = 0, reload = !strcmp(argv[1], "--apply");

	if (!strcmp(argv[1], "--list-themes")) {
		char **t = rui_theme_list();
		int verbose = argc > 2 && !strcmp(argv[2], "-v");

		for (i = 0; t[i]; i++) {
			const RuiThemeModule *m = verbose ? rui_theme_module(t[i]) : NULL;

			if (!verbose)
				puts(t[i]);
			else
				printf("%-14s %-9s %s\n", t[i], rui_theme_kind(t[i]),
				    m && m->description ? m->description : "");
		}
		rui_strv_free(t);
	} else if (!strcmp(argv[1], "--reload")) {
		/* after editing keys.conf, windows.conf, programs.conf ... */
		Display *dpy = XOpenDisplay(NULL);

		if (dpy) {
			rui_request_reload(dpy);
			XCloseDisplay(dpy);
		} else {
			fprintf(stderr, "retro-settings: cannot open display\n");
			rc = 1;
		}
	} else if (!strcmp(argv[1], "--install-theme") && argc == 3) {
		rc = install_theme(argv[2]);
	} else if (!strcmp(argv[1], "--get") && argc == 3) {
		conf_load();
		puts(get(argv[2], ""));
	} else if (!strcmp(argv[1], "--set") || reload) {
		for (i = 2; i < argc; i++) {
			char key[64], *eq = strchr(argv[i], '=');

			if (!eq || eq == argv[i] || (size_t)(eq - argv[i]) >= sizeof key) {
				fprintf(stderr, "retro-settings: expected KEY=VALUE, got "
				    "'%s'\n", argv[i]);
				rc = 2;
				continue;
			}
			snprintf(key, sizeof key, "%.*s", (int)(eq - argv[i]), argv[i]);
			if (rui_conf_set(path, key, eq + 1) < 0) {
				fprintf(stderr, "retro-settings: cannot write %s\n", path);
				rc = 1;
			}
		}
		if (reload && rc == 0) {
			Display *dpy = XOpenDisplay(NULL);

			if (dpy) {
				rui_request_reload(dpy);
				XCloseDisplay(dpy);
			} else {
				fprintf(stderr, "retro-settings: saved; no display to "
				    "reload\n");
			}
		}
	} else {
		fprintf(stderr, "usage: retro-settings [--page N] | --apply KEY=VAL... "
		    "| --set KEY=VAL... | --get KEY | --reload | --list-themes [-v] | "
		    "--install-theme FILE-OR-URL\n");
		rc = 2;
	}
	free(path);
	return rc;
}

int
main(int argc, char **argv)
{
	int page = 0;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		puts("retro-settings " RUI_VERSION);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--page"))
		page = atoi(argv[2]);
	else if (argc > 1)
		return cli(argc, argv);
	rui_signals_init();
	st.app = rui_open("retro-settings");
	setup(page);
#ifdef __OpenBSD__
	/* prot_exec: previewing a theme module loads it */
	if (pledge("stdio rpath wpath cpath proc exec prot_exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	st.running = 1;
	while (st.running && !rui_quit_requested()) {
		XEvent ev;

		while (st.running && XPending(st.app->dpy)) {
			XNextEvent(st.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		if (st.dirty && st.running)
			draw();
		if (st.running)
			rui_wait(st.app, -1);
	}
	cleanup();
	rui_close(st.app);
	if (st.reexec) {
		char pg[8];

		snprintf(pg, sizeof pg, "%d", st.page);
		execlp(argv[0], argv[0], "--page", pg, (char *)NULL);
		perror("retro-settings");
		return 1;
	}
	return 0;
}
