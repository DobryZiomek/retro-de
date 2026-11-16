/*
 * retro-about - "About retro-de": the version of the desktop and a short
 * description of this computer (system, host, processor, memory, screens,
 * X server) and of the desktop's setup (theme, modules in use).
 *
 * Everything comes from POSIX calls (uname, gethostname, getpwuid,
 * sysconf), the kernel's own interfaces (/proc on Linux, sysctl on the
 * BSDs) and Xlib.  No D-Bus, no XDG, no helper programs.
 *
 *   retro-about        the window
 *   retro-about -t     the same as text on standard output
 */
#include "../lib/retroui.h"

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__) || defined(__APPLE__)
# include <sys/param.h>
# include <sys/sysctl.h>
# define ABOUT_BSD 1
#endif

#define WIN_W 460
#define MAX_ROWS 32

typedef struct Row {
	char key[32];
	char val[256];
	int heading;
} Row;

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	RuiFont *big;
	int h, running, dirty;
	Row rows[MAX_ROWS];
	int nrows;
	RuiButton ok;
	RuiRect field;
	int row_h, key_w;
} ab;

/* ----------------------------------------------------------- the facts */

static void
add(const char *key, const char *fmt, ...)
{
	Row *r;
	va_list ap;

	if (ab.nrows >= MAX_ROWS)
		return;
	r = &ab.rows[ab.nrows++];
	memset(r, 0, sizeof *r);
	rui_strlcpy(r->key, key, sizeof r->key);
	va_start(ap, fmt);
	vsnprintf(r->val, sizeof r->val, fmt, ap);
	va_end(ap);
}

static void
heading(const char *title)
{
	if (ab.nrows >= MAX_ROWS)
		return;
	memset(&ab.rows[ab.nrows], 0, sizeof ab.rows[0]);
	rui_strlcpy(ab.rows[ab.nrows].key, title, sizeof ab.rows[0].key);
	ab.rows[ab.nrows++].heading = 1;
}

/* the processor's name, where the kernel tells it without a helper */
static int
cpu_model(char *buf, size_t n)
{
#if defined(__linux__)
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[512];
	int found = 0;

	if (!f)
		return 0;
	while (!found && fgets(line, sizeof line, f)) {
		/* x86: "model name", ARM/RISC-V: "Hardware", "uarch", "cpu model" */
		char *c = strchr(line, ':');

		if (!c || (strncmp(line, "model name", 10) &&
		    strncmp(line, "cpu model", 9) && strncmp(line, "Hardware", 8) &&
		    strncmp(line, "uarch", 5) && strncmp(line, "cpu\t", 4)))
			continue;
		c = rui_ltrim(rui_rtrim(c + 1));
		if (*c) {
			rui_strlcpy(buf, c, n);
			found = 1;
		}
	}
	fclose(f);
	return found;
#elif defined(ABOUT_BSD) && defined(HW_MODEL)
	int mib[2] = { CTL_HW, HW_MODEL };
	size_t len = n;

	if (sysctl(mib, 2, buf, &len, NULL, 0) < 0 || !buf[0])
		return 0;
	buf[n - 1] = '\0';
	return 1;
#else
	(void)buf;
	(void)n;
	return 0;
#endif
}

static long
cpu_count(void)
{
#ifdef _SC_NPROCESSORS_ONLN
	return sysconf(_SC_NPROCESSORS_ONLN);
#else
	return -1;
#endif
}

/* a NULL-terminated module list as "a, b, c" */
#define MODLIST(list, buf) do { \
	size_t o_ = 0; int i_; \
	(buf)[0] = '\0'; \
	for (i_ = 0; (list) && (list)[i_]; i_++) \
		o_ += (size_t)snprintf((buf) + o_, o_ < sizeof(buf) ? \
		    sizeof(buf) - o_ : 0, "%s%s", i_ ? ", " : "", (list)[i_]->name); \
	if (!(buf)[0]) rui_strlcpy((buf), "none", sizeof(buf)); \
} while (0)

static void
collect(Display *dpy)
{
	const RuiSettings *set = rui_settings();
	const RuiPowerModule *pw;
	struct utsname u;
	struct passwd *pwd;
	char buf[256], host[256];
	unsigned long total_mb = 0;
	double used = 0;
	long ncpu;

	ab.nrows = 0;
	heading("This computer");
	if (gethostname(host, sizeof host) == 0) {
		host[sizeof host - 1] = '\0';
		add("Computer name", "%s", host);
	}
	pwd = getpwuid(getuid());
	if (pwd && pwd->pw_name)
		add("User", "%s%s%s%s", pwd->pw_name, pwd->pw_gecos &&
		    pwd->pw_gecos[0] && pwd->pw_gecos[0] != ',' ? " (" : "",
		    pwd->pw_gecos && pwd->pw_gecos[0] != ',' ? pwd->pw_gecos : "",
		    pwd->pw_gecos && pwd->pw_gecos[0] && pwd->pw_gecos[0] != ',' ?
		    ")" : "");
	if (uname(&u) == 0) {
		add("System", "%s %s", u.sysname, u.release);
		add("Machine", "%s", u.machine);
	}
	ncpu = cpu_count();
	if (cpu_model(buf, sizeof buf))
		add("Processor", ncpu > 1 ? "%s (x%ld)" : "%s", buf, ncpu);
	else if (ncpu > 0)
		add("Processor", "%ld online", ncpu);
	if (rui_sys_mem(&used, &total_mb) && total_mb)
		add("Memory", "%lu MB, %d%% in use", total_mb, (int)(used * 100 +
		    0.5));
	if (dpy) {
		heading("Display");
		{
			int v = VendorRelease(dpy);

			/* X.Org packs 1.21.1 as 12101000 */
			if (strstr(ServerVendor(dpy), "X.Org") && v >= 10000000)
				add("X server", "%s %d.%d.%d", ServerVendor(dpy),
				    v / 10000000, v / 100000 % 100, v / 1000 % 100);
			else
				add("X server", "%s %d", ServerVendor(dpy), v);
		}
		if (ab.app) {
			RuiMonitor m[RUI_MAX_MONITORS];
			int n = rui_monitors(ab.app, m, RUI_MAX_MONITORS), i;
			size_t o = 0;

			buf[0] = '\0';
			for (i = 0; i < n && o < sizeof buf; i++)
				o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s%s%dx%d",
				    i ? ", " : "", m[i].name, m[i].name[0] ? " " : "",
				    m[i].w, m[i].h);
			add(n == 1 ? "Monitor" : "Monitors", "%s", buf);
		} else {
			int sc = DefaultScreen(dpy);

			add("Screen", "%dx%d, %d bits", DisplayWidth(dpy, sc),
			    DisplayHeight(dpy, sc), DefaultDepth(dpy, sc));
		}
	}
	heading("Desktop");
	add("Version", "retro-de %s", RUI_VERSION);
	add("Theme", "%s (%s windows)", set->theme, set->style == RUI_STYLE_CDE ?
	    "CDE" : "Classic");
	add("Font", "%s", set->font);
	MODLIST(rui_config.audio, buf);
	add("Audio modules", "%s", buf);
	MODLIST(rui_config.net, buf);
	add("Wi-Fi modules", "%s", buf);
	MODLIST(rui_config.bt, buf);
	add("Bluetooth", "%s", buf);
	pw = rui_power_detect(set->power);
	add("Power", "%s", pw ? pw->name : "none");
	add("Settings", "%s", rui_config_dir());
}

/* ---------------------------------------------------------------- window */

static void
draw(void)
{
	RuiSurf *s = ab.s;
	RuiApp *a = ab.app;
	RuiRect r;
	int i, y;

	rui_fill(s, 0, 0, WIN_W, ab.h, RUI_C_FACE);
	/* the emblem */
	rui_fill(s, 14, 14, 52, 52, RUI_C_FIELD);
	rui_bevel(s, 14, 14, 52, 52, RUI_SUNKEN);
	rui_icon_draw(s, RUI_ICON_COMPUTER, 24, 24);
	r.x = 80;
	r.y = 14;
	r.w = WIN_W - 94;
	r.h = ab.big->height + 2;
	rui_text_fit(s, ab.big, r, "retro-de " RUI_VERSION, RUI_C_TEXT, RUI_LEFT);
	r.y += r.h + 2;
	r.h = a->font->height + 1;
	rui_text_fit(s, a->font, r, "A small CDE-style desktop for X11",
	    RUI_C_TEXT, RUI_LEFT);
	r.y += r.h;
	rui_text_fit(s, a->font, r, "C11 and POSIX: no D-Bus, no XDG required",
	    RUI_C_DISABLED, RUI_LEFT);

	rui_fill(s, ab.field.x, ab.field.y, ab.field.w, ab.field.h, RUI_C_FIELD);
	rui_bevel(s, ab.field.x, ab.field.y, ab.field.w, ab.field.h, RUI_SUNKEN);
	y = ab.field.y + 6;
	for (i = 0; i < ab.nrows; i++) {
		Row *row = &ab.rows[i];

		r.x = ab.field.x + 10;
		r.y = y;
		r.h = ab.row_h;
		if (row->heading) {
			if (i) {
				y += ab.row_h / 2;
				r.y = y;
			}
			r.w = ab.field.w - 20;
			rui_text_fit(s, a->bold, r, row->key, RUI_C_TITLE_ACTIVE,
			    RUI_LEFT);
			rui_fill(s, r.x, r.y + ab.row_h - 1, r.w, 1, RUI_C_SHADOW);
		} else {
			r.x += 8;
			r.w = ab.key_w;
			rui_text_fit(s, a->font, r, row->key, RUI_C_DISABLED, RUI_LEFT);
			r.x += ab.key_w + 8;
			r.w = ab.field.x + ab.field.w - 10 - r.x;
			rui_text_fit(s, a->font, r, row->val, RUI_C_TEXT, RUI_LEFT);
		}
		y += ab.row_h;
	}
	rui_button_draw(s, &ab.ok);
	rui_surf_flush(s);
	ab.dirty = 0;
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(ab.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter ||
		    ks == XK_space || ((ev->xkey.state & ControlMask) &&
		    (ks == XK_q || ks == XK_w)))
			ab.running = 0;
		break;
	}
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		r = rui_button_event(&ab.ok, ev);
		if (r & RUI_EV_ACTIVATE)
			ab.running = 0;
		ab.dirty |= r != 0;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(ab.app, ev))
			ab.running = 0;
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
setup(void)
{
	RuiApp *a = ab.app;
	const RuiSettings *set = rui_settings();
	XSizeHints sh;
	char spec[300], fam[256], *dash;
	int i, w, heads = 0, top;

	/* the title is the same family at 16 points, like the calculator's */
	rui_strlcpy(fam, set->font, sizeof fam);
	dash = strrchr(fam, '-');
	if (a->font->xft && dash && dash[1] >= '0' && dash[1] <= '9') {
		*dash = '\0';
		snprintf(spec, sizeof spec, "%s-16:bold", fam);
		ab.big = rui_font_open(a, spec);
	} else {
		ab.big = rui_font_open(a, set->font_bold[0] ? set->font_bold :
		    set->font);
	}
	collect(a->dpy);
	ab.row_h = a->font->height + 3;
	for (i = 0; i < ab.nrows; i++) {
		if (ab.rows[i].heading) {
			heads += i > 0;
			continue;
		}
		w = rui_text_width(a, a->font, ab.rows[i].key, -1);
		if (w > ab.key_w)
			ab.key_w = w;
	}
	top = 14 + ab.big->height + 2 * a->font->height + 12;
	if (top < 80)
		top = 80;
	ab.field.x = 14;
	ab.field.y = top;
	ab.field.w = WIN_W - 28;
	ab.field.h = ab.nrows * ab.row_h + heads * (ab.row_h / 2) + 12;
	ab.ok.label = "OK";
	ab.ok.bold = 1;
	ab.ok.r.w = 90;
	ab.ok.r.h = a->font->height + 12;
	ab.ok.r.x = WIN_W - 14 - ab.ok.r.w;
	ab.ok.r.y = ab.field.y + ab.field.h + 10;
	ab.h = ab.ok.r.y + ab.ok.r.h + 12;

	ab.win = rui_window_new(a, a->root, 100, 100, WIN_W, ab.h, 0,
	    ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
	    ButtonMotionMask | StructureNotifyMask);
	ab.s = rui_surf_new(a, ab.win, WIN_W, ab.h);
	rui_set_title(a, ab.win, "About retro-de", "retro-about");
	rui_set_window_type(a, ab.win, A__NET_WM_WINDOW_TYPE_DIALOG);
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = ab.h;
	XSetWMNormalHints(a->dpy, ab.win, &sh);
	a->dispatch = dispatch_cb;
	XMapWindow(a->dpy, ab.win);
}

/* -t: plain text, with or without an X display */
static int
print_text(void)
{
	Display *dpy = getenv("DISPLAY") ? XOpenDisplay(NULL) : NULL;
	int i;

	collect(dpy);
	printf("retro-de %s - a small CDE-style desktop for X11\n", RUI_VERSION);
	for (i = 0; i < ab.nrows; i++) {
		if (ab.rows[i].heading)
			printf("\n%s\n", ab.rows[i].key);
		else
			printf("  %-14s %s\n", ab.rows[i].key, ab.rows[i].val);
	}
	if (dpy)
		XCloseDisplay(dpy);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-about " RUI_VERSION);
			return 0;
		}
		if (!strcmp(argv[1], "-t") && argc == 2)
			return print_text();
		fprintf(stderr, "usage: retro-about [-v | -t]\n");
		return 2;
	}
	rui_signals_init();
	ab.app = rui_open("retro-about");
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		rui_die("pledge failed");
#endif
	ab.running = 1;
	ab.dirty = 1;
	while (ab.running && !rui_quit_requested()) {
		XEvent ev;

		while (ab.running && XPending(ab.app->dpy)) {
			XNextEvent(ab.app->dpy, &ev);
			handle_event(&ev);
		}
		if (ab.dirty && ab.running)
			draw();
		if (ab.running)
			rui_wait(ab.app, -1);
	}
	rui_font_close(ab.app, ab.big);
	rui_surf_free(ab.s);
	XDestroyWindow(ab.app->dpy, ab.win);
	rui_close(ab.app);
	return 0;
}
