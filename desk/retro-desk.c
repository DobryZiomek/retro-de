/*
 * retro-desk - root window background (solid, dither pattern, PPM/XPM
 * wallpaper painted once per monitor), desktop icon grid (own entries or .desktop applications) and
 * the right-click root menu.
 */
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CELL_W 88
#define CELL_H 78
#define ICON_SZ RUI_ICON_SIZE
#define K_IMAGE RUI_ICON_COUNT         /* a picture instead of a pictogram */

typedef struct Icon {
	char *label, *cmd;
	int kind;
	Image *img;
	RuiRect r;                     /* whole cell */
} Icon;

static const struct {
	const char *name;
	unsigned char bits[8];
} patterns[] = {
	{ "checker",  { 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa } },
	{ "dots",     { 0x11, 0x00, 0x44, 0x00, 0x11, 0x00, 0x44, 0x00 } },
	{ "dense",    { 0xdd, 0xff, 0x77, 0xff, 0xdd, 0xff, 0x77, 0xff } },
	{ "hlines",   { 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00 } },
	{ "vlines",   { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } },
	{ "diagonal", { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 } },
	{ "bricks",   { 0xff, 0x01, 0x01, 0x01, 0xff, 0x10, 0x10, 0x10 } },
	{ "weave",    { 0x88, 0x54, 0x22, 0x45, 0x88, 0x15, 0x22, 0x51 } },
};

static const char default_icons[] =
	"icon \"Programs\" programs retro-programs\n"
	"icon \"Terminal\" terminal @terminal\n"
	"icon \"Files\" folder retro-files\n"
	"icon \"Calculator\" calculator retro-calc\n"
	"icon \"Run...\" app retro-run\n"
	"icon \"Network\" network retro-net\n";

static struct {
	RuiApp *app;
	Display *dpy;
	Window win;
	RuiSurf *s;
	Pixmap bg;
	int w, h;
	int running, dirty;
	Icon *icons;
	int nicons, sel;
	int single_click;
	long long last_click;
	int last_idx;
	char wallpaper[1024];
	int wp_mode, pattern;
	char pattern_color[64];
	unsigned long pattern_pixel;
	int reload;
	RuiRect area;
} desk;

#define A(n) (desk.app->atom[A_##n])

/* ------------------------------------------------------------- background */

static void
build_background(void)
{
	RuiApp *a = desk.app;
	Image *img = NULL;
	unsigned long id;

	if (desk.bg)
		XFreePixmap(a->dpy, desk.bg);
	desk.bg = XCreatePixmap(a->dpy, a->root, (unsigned)desk.w,
	    (unsigned)desk.h, (unsigned)a->depth);
	XSetForeground(a->dpy, a->gc, a->color[RUI_C_DESKTOP].pixel);
	XFillRectangle(a->dpy, desk.bg, a->gc, 0, 0, (unsigned)desk.w,
	    (unsigned)desk.h);
	if (desk.pattern >= 0) {
		XColor xc, exact;
		unsigned long fg = desk.pattern_pixel;
		Pixmap stip = XCreateBitmapFromData(a->dpy, a->root,
		    (const char *)patterns[desk.pattern].bits, 8, 8);

		if (desk.pattern_color[0] && XAllocNamedColor(a->dpy, a->cmap,
		    desk.pattern_color, &xc, &exact))
			fg = xc.pixel;
		XSetForeground(a->dpy, a->gc, fg);
		XSetBackground(a->dpy, a->gc, a->color[RUI_C_DESKTOP].pixel);
		XSetStipple(a->dpy, a->gc, stip);
		XSetFillStyle(a->dpy, a->gc, FillOpaqueStippled);
		XFillRectangle(a->dpy, desk.bg, a->gc, 0, 0, (unsigned)desk.w,
		    (unsigned)desk.h);
		XSetFillStyle(a->dpy, a->gc, FillSolid);
		XSetStipple(a->dpy, a->gc, a->checker);
		XFreePixmap(a->dpy, stip);
	}
	if (desk.wallpaper[0]) {
		/* a path, or the name of an installed wallpaper ("Clouds") */
		char *path = rui_wallpaper_find(desk.wallpaper);

		if (!path)
			rui_warn("no wallpaper '%s'", desk.wallpaper);
		else if ((img = image_load(a, path))) {
			RuiMonitor m[RUI_MAX_MONITORS];
			int n = rui_monitors(a, m, RUI_MAX_MONITORS), i;
			int mode = desk.wp_mode;

			if (mode < 0)                  /* auto: small images are tiles */
				mode = img->w <= 256 && img->h <= 256 ? IMG_TILE : IMG_FILL;
			/* every monitor gets its own copy, whatever its size */
			for (i = 0; i < n; i++)
				image_render_at(a, img, desk.bg, m[i].x, m[i].y, m[i].w,
				    m[i].h, mode);
			image_free(img);
		}
		free(path);
	}
	/* also paint the real root so it looks right before/without us */
	XSetWindowBackgroundPixmap(a->dpy, a->root, desk.bg);
	XClearWindow(a->dpy, a->root);
	id = desk.bg;
	rui_set_prop32(a, a->root, A(_XROOTPMAP_ID), XA_PIXMAP, &id, 1);
}

/* ------------------------------------------------------------------ icons */

static int
kind_from_name(const char *s)
{
	return rui_icon_kind(s);
}

static Icon *
icon_new(void)
{
	Icon *ic;

	desk.icons = rui_realloc(desk.icons, (size_t)(desk.nicons + 1) *
	    sizeof *desk.icons);
	ic = &desk.icons[desk.nicons++];
	memset(ic, 0, sizeof *ic);
	return ic;
}

static Image *
load_icon_image(const char *path)
{
	return image_fit(image_load(desk.app, path), ICON_SZ);
}

/* "app ID [Label]": label, command and picture from a .desktop entry */
static void
add_app_icon(const char *s, const char *origin, int lineno)
{
	char id[256], label[128], *path;
	size_t o = 0;
	RuiDesktopEntry *e;
	Icon *ic;

	s += strspn(s, " \t");
	while (*s && *s != ' ' && *s != '\t' && o + 1 < sizeof id)
		id[o++] = *s++;
	id[o] = '\0';
	s += strspn(s, " \t");
	rui_strlcpy(label, *s == '"' ? s + 1 : s, sizeof label);
	o = strlen(label);
	if (o && label[o - 1] == '"')
		label[o - 1] = '\0';
	if (!id[0]) {
		rui_warn("%s:%d: expected: app <desktop-file-id> [label]", origin,
		    lineno);
		return;
	}
	if (!rui_xdg_menus()) {
		rui_warn("%s:%d: .desktop support is disabled in config.h", origin,
		    lineno);
		return;
	}
	if (!(e = rui_app_find(id))) {
		rui_warn("%s:%d: no application '%s'", origin, lineno, id);
		return;
	}
	ic = icon_new();
	ic->label = rui_strdup(label[0] ? label : e->name);
	ic->cmd = rui_desktop_command(e);
	ic->kind = rui_icon_guess(ic->cmd);
	if ((path = rui_icon_lookup(e->icon))) {
		if ((ic->img = load_icon_image(path)))
			ic->kind = K_IMAGE;
		free(path);
	}
	rui_desktop_free(e);
}

static void
parse_icons(const char *text, const char *origin)
{
	const char *p = text;
	int lineno = 0;

	while (*p) {
		char line[1024], label[128], kind[512];
		size_t len = strcspn(p, "\n"), c = len < sizeof line - 1 ? len :
		    sizeof line - 1, o = 0;
		char *s, *e;
		Icon *ic;

		memcpy(line, p, c);
		line[c] = '\0';
		p += len;
		if (*p == '\n')
			p++;
		lineno++;
		e = line + strlen(line);
		while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
			*--e = '\0';
		s = line + strspn(line, " \t");
		if (!*s || *s == '#')
			continue;
		if (!strncmp(s, "app", 3) && (s[3] == ' ' || s[3] == '\t')) {
			add_app_icon(s + 3, origin, lineno);
			continue;
		}
		if (strncmp(s, "icon", 4) || (s[4] != ' ' && s[4] != '\t')) {
			rui_warn("%s:%d: expected: icon \"Label\" <kind|image> command"
			    " or app <id>", origin, lineno);
			continue;
		}
		s += 4;
		s += strspn(s, " \t");
		if (*s == '"') {
			s++;
			while (*s && *s != '"') {
				if (*s == '\\' && s[1])
					s++;
				if (o + 1 < sizeof label)
					label[o++] = *s;
				s++;
			}
			if (*s == '"')
				s++;
		} else {
			while (*s && *s != ' ' && *s != '\t') {
				if (o + 1 < sizeof label)
					label[o++] = *s;
				s++;
			}
		}
		label[o] = '\0';
		s += strspn(s, " \t");
		o = 0;
		while (*s && *s != ' ' && *s != '\t') {
			if (o + 1 < sizeof kind)
				kind[o++] = *s;
			s++;
		}
		kind[o] = '\0';
		s += strspn(s, " \t");
		if (!label[0] || !kind[0] || !*s) {
			rui_warn("%s:%d: incomplete icon line", origin, lineno);
			continue;
		}
		ic = icon_new();
		ic->label = rui_strdup(label);
		ic->cmd = rui_strdup(s);
		ic->kind = kind_from_name(kind);
		if (ic->kind < 0) {
			ic->img = load_icon_image(kind);
			ic->kind = ic->img ? K_IMAGE : rui_icon_guess(ic->cmd);
		}
	}
}

static void
load_icons(void)
{
	char *path = rui_config_path("desktop.conf");
	FILE *f = fopen(path, "r");

	if (f) {
		char buf[16384];
		size_t n = fread(buf, 1, sizeof buf - 1, f);

		buf[n] = '\0';
		fclose(f);
		parse_icons(buf, path);
	} else {
		parse_icons(default_icons, "(built-in icons)");
	}
	free(path);
}

static void
free_icons(void)
{
	int i;

	for (i = 0; i < desk.nicons; i++) {
		free(desk.icons[i].label);
		free(desk.icons[i].cmd);
		image_free(desk.icons[i].img);
	}
	free(desk.icons);
	desk.icons = NULL;
	desk.nicons = 0;
}

static void
layout_icons(void)
{
	unsigned long *v;
	int n, i, col = 0, row = 0, rows;

	desk.area.x = desk.area.y = 0;
	desk.area.w = desk.w;
	desk.area.h = desk.h;
	n = rui_get_prop32(desk.app, desk.app->root, A(_NET_WORKAREA), &v);
	if (n >= 4 && v[2] > 0 && v[3] > 0) {
		desk.area.x = (int)v[0];
		desk.area.y = (int)v[1];
		desk.area.w = (int)v[2];
		desk.area.h = (int)v[3];
	} else {
		/* no window manager yet: the primary monitor */
		RuiMonitor m = rui_monitor_pick(desk.app, "primary");

		desk.area.x = m.x;
		desk.area.y = m.y;
		desk.area.w = m.w;
		desk.area.h = m.h;
	}
	free(v);
	rows = (desk.area.h - 12) / CELL_H;
	if (rows < 1)
		rows = 1;
	for (i = 0; i < desk.nicons; i++) {
		desk.icons[i].r.x = desk.area.x + 8 + col * CELL_W;
		desk.icons[i].r.y = desk.area.y + 8 + row * CELL_H;
		desk.icons[i].r.w = CELL_W;
		desk.icons[i].r.h = CELL_H;
		if (++row >= rows) {
			row = 0;
			col++;
		}
	}
	desk.dirty = 1;
}

static void
draw(void)
{
	RuiSurf *s = desk.s;
	RuiApp *a = desk.app;
	int i;

	XCopyArea(a->dpy, desk.bg, s->buf, a->gc, 0, 0, (unsigned)desk.w,
	    (unsigned)desk.h, 0, 0);
	for (i = 0; i < desk.nicons; i++) {
		Icon *ic = &desk.icons[i];
		int ix = ic->r.x + (CELL_W - ICON_SZ) / 2, iy = ic->r.y + 4, tw;
		RuiRect t;

		if (ic->kind == K_IMAGE && ic->img)
			image_draw(a, ic->img, s->buf, ic->r.x + (CELL_W - ic->img->w) / 2,
			    iy + (ICON_SZ - ic->img->h) / 2);
		else
			rui_icon_draw(s, ic->kind, ix, iy);
		tw = rui_text_width(a, a->font, ic->label, -1) + 6;
		if (tw > CELL_W - 4)
			tw = CELL_W - 4;
		t.x = ic->r.x + (CELL_W - tw) / 2;
		t.y = iy + ICON_SZ + 4;
		t.w = tw;
		t.h = a->font->height + 2;
		if (i == desk.sel) {
			rui_fill(s, t.x, t.y, t.w, t.h, RUI_C_SEL_BG);
			rui_text_fit(s, a->font, t, ic->label, RUI_C_SEL_FG, RUI_CENTER);
		} else {
			RuiRect sh = t;

			sh.x++;
			sh.y++;
			rui_text_fit(s, a->font, sh, ic->label, RUI_C_DARK, RUI_CENTER);
			rui_text_fit(s, a->font, t, ic->label, RUI_C_DESKTOP_FG,
			    RUI_CENTER);
		}
	}
	rui_surf_flush(s);
	desk.dirty = 0;
}

/* ----------------------------------------------------------------- events */

static void
root_menu(int x, int y)
{
	char *path = rui_config_path("menu.conf");
	RuiMenu *m = rui_menu_load(path);
	const RuiMenuItem *it = rui_menu_popup(desk.app, m, x, y, 0);
	char *cmd = it && it->cmd ? rui_strdup(it->cmd) : NULL;

	free(path);
	rui_menu_free(m);
	if (cmd)
		rui_run_command(desk.app, cmd);
	free(cmd);
}

static void
switch_desk_rel(int d)
{
	unsigned long *v;
	long cur = 0, n = 1;

	if (rui_get_prop32(desk.app, desk.app->root, A(_NET_CURRENT_DESKTOP),
	    &v) > 0)
		cur = (long)v[0];
	free(v);
	if (rui_get_prop32(desk.app, desk.app->root, A(_NET_NUMBER_OF_DESKTOPS),
	    &v) > 0 && v[0] > 0)
		n = (long)v[0];
	free(v);
	rui_send_root_msg(desk.app, desk.app->root, A(_NET_CURRENT_DESKTOP),
	    (cur + d + n) % n, CurrentTime, 0, 0, 0);
}

static void
on_click(XButtonEvent *e)
{
	int i, hit = -1;
	long long now = rui_now_ms();

	for (i = 0; i < desk.nicons; i++)
		if (rui_in_rect(desk.icons[i].r, e->x, e->y))
			hit = i;
	switch (e->button) {
	case Button1:
		if (hit != desk.sel) {
			desk.sel = hit;
			desk.dirty = 1;
		}
		if (hit >= 0 && (desk.single_click || (hit == desk.last_idx &&
		    now - desk.last_click < 400))) {
			rui_run_command(desk.app, desk.icons[hit].cmd);
			desk.last_idx = -1;
		} else {
			desk.last_idx = hit;
			desk.last_click = now;
		}
		break;
	case Button3:
		root_menu(e->x_root, e->y_root);
		break;
	case Button4:
		switch_desk_rel(-1);
		break;
	case Button5:
		switch_desk_rel(1);
		break;
	default:
		break;
	}
}

static void
resize(void)
{
	RuiApp *a = desk.app;

	a->sw = DisplayWidth(a->dpy, a->screen);
	a->sh = DisplayHeight(a->dpy, a->screen);
	desk.w = a->sw;
	desk.h = a->sh;
	XMoveResizeWindow(a->dpy, desk.win, 0, 0, (unsigned)desk.w,
	    (unsigned)desk.h);
	rui_surf_resize(desk.s, desk.w, desk.h);
	build_background();
	layout_icons();
}

static void
handle_event(XEvent *ev)
{
	if (rui_monitors_event(desk.app, ev)) {
		resize();                          /* repaint for the new layout */
		return;
	}
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.window == desk.win && ev->xexpose.count == 0)
			rui_surf_flush(desk.s);
		break;
	case ButtonPress:
		if (ev->xbutton.window == desk.win)
			on_click(&ev->xbutton);
		break;
	case PropertyNotify:
		if (ev->xproperty.window == desk.app->root &&
		    ev->xproperty.atom == A(_NET_WORKAREA))
			layout_icons();
		else if (ev->xproperty.window == desk.app->root &&
		    ev->xproperty.atom == A(_RETRO_RELOAD)) {
			desk.reload = 1;                   /* re-exec with new settings */
			desk.running = 0;
		}
		break;
	case ConfigureNotify:
		if (ev->xconfigure.window == desk.app->root &&
		    (ev->xconfigure.width != desk.w ||
		    ev->xconfigure.height != desk.h))
			resize();
		break;
	case ClientMessage:
		if (rui_wm_delete_event(desk.app, ev))
			desk.running = 0;
		break;
	case DestroyNotify:
		if (ev->xdestroywindow.window == desk.win)
			desk.running = 0;
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

static int
pattern_index(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
		if (!strcmp(name, patterns[i].name))
			return (int)i;
	if (strcmp(name, "solid") && strcmp(name, "none"))
		rui_warn("unknown desktop pattern '%s'", name);
	return -1;
}

static void
desk_conf(const char *key, const char *val, void *ud)
{

	(void)ud;
	if (!strcmp(key, "wallpaper")) {
		rui_strlcpy(desk.wallpaper, val, sizeof desk.wallpaper);
	} else if (!strcmp(key, "wallpaper_mode")) {
		desk.wp_mode = !strcmp(val, "auto") ? -1 : image_mode(val);
	} else if (!strcmp(key, "desktop_pattern")) {
		desk.pattern = pattern_index(val);
	} else if (!strcmp(key, "desktop_pattern_color")) {
		rui_strlcpy(desk.pattern_color, val, sizeof desk.pattern_color);
	} else if (!strcmp(key, "icon_single_click")) {
		desk.single_click = rui_conf_bool(val);
	}
}

static void
setup(void)
{
	RuiApp *a = desk.app;
	XWMHints wmh;
	unsigned long v[2];
	char *path;

	desk.dpy = a->dpy;
	desk.w = a->sw;
	desk.h = a->sh;
	desk.sel = desk.last_idx = -1;
	desk.pattern = pattern_index(rui_settings()->pattern);  /* theme default */
	{
		/* pattern ink: a darker shade of the backdrop colour */
		XColor c = { 0, 0, 0, 0, DoRed | DoGreen | DoBlue, 0 };
		XRenderColor d = a->color[RUI_C_DESKTOP].color;

		c.red = (unsigned short)(d.red * 0.8);
		c.green = (unsigned short)(d.green * 0.8);
		c.blue = (unsigned short)(d.blue * 0.8);
		desk.pattern_pixel = XAllocColor(a->dpy, a->cmap, &c) ? c.pixel :
		    a->color[RUI_C_DARK].pixel;
	}
	desk.wp_mode = -1;                     /* auto */
	/* a theme module may bring its own wallpaper; retro.conf overrides */
	if (a->theme_mod && a->theme_mod->wallpaper)
		rui_strlcpy(desk.wallpaper, a->theme_mod->wallpaper,
		    sizeof desk.wallpaper);
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, desk_conf, NULL);
	free(path);

	desk.win = rui_window_new(a, a->root, 0, 0, desk.w, desk.h, 0,
	    ExposureMask | ButtonPressMask | StructureNotifyMask);
	desk.s = rui_surf_new(a, desk.win, desk.w, desk.h);
	rui_set_title(a, desk.win, "retro-desk", "retro-desk");
	rui_set_window_type(a, desk.win, A__NET_WM_WINDOW_TYPE_DESKTOP);
	v[0] = 0xffffffffUL;
	rui_set_prop32(a, desk.win, A(_NET_WM_DESKTOP), XA_CARDINAL, v, 1);
	v[0] = A(_NET_WM_STATE_STICKY);
	v[1] = A(_NET_WM_STATE_SKIP_TASKBAR);
	rui_set_prop32(a, desk.win, A(_NET_WM_STATE), XA_ATOM, v, 2);
	memset(&wmh, 0, sizeof wmh);
	wmh.flags = InputHint;
	wmh.input = False;
	XSetWMHints(a->dpy, desk.win, &wmh);
	{
		XSizeHints sh;

		memset(&sh, 0, sizeof sh);
		sh.flags = USPosition | PPosition;
		XSetWMNormalHints(a->dpy, desk.win, &sh);
	}
	XSelectInput(a->dpy, a->root, PropertyChangeMask | StructureNotifyMask);
	rui_monitors_watch(a);
	a->dispatch = dispatch_cb;
	build_background();
	load_icons();
	layout_icons();
	XLowerWindow(a->dpy, desk.win);
	XMapWindow(a->dpy, desk.win);
}

int
main(int argc, char **argv)
{
	int xerrors;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-desk " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-desk [-v]\n");
		return 2;
	}
	rui_signals_init();
	desk.app = rui_open("retro-desk");
	setup();
#ifdef __OpenBSD__
	/* wpath cpath: the programs folder is created on first use */
	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	desk.running = 1;
	while (desk.running && !rui_quit_requested()) {
		XEvent ev;

		while (desk.running && XPending(desk.dpy)) {
			XNextEvent(desk.dpy, &ev);
			handle_event(&ev);
		}
		if (desk.dirty)
			draw();
		if (desk.running)
			rui_wait(desk.app, -1);
	}
	/* leave a plain root behind */
	XSetWindowBackground(desk.dpy, desk.app->root,
	    desk.app->color[RUI_C_DESKTOP].pixel);
	XClearWindow(desk.dpy, desk.app->root);
	XDeleteProperty(desk.dpy, desk.app->root, A(_XROOTPMAP_ID));
	free_icons();
	rui_surf_free(desk.s);
	XDestroyWindow(desk.dpy, desk.win);
	if (desk.bg)
		XFreePixmap(desk.dpy, desk.bg);
	xerrors = rui_xerror_count();
	rui_close(desk.app);
	if (getenv("RETRO_DEBUG"))
		fprintf(stderr, "retro-desk: exiting, %d X errors\n", xerrors);
	if (desk.reload && !rui_quit_requested()) {
		execvp(argv[0], argv);
		perror("retro-desk: reload");
		return 1;
	}
	return 0;
}
