/*
 * drive - end-to-end session test driver.
 *
 * Runs against a live retro-session on $DISPLAY, spawns test clients and
 * checks the window manager, panel and desktop through ICCCM/EWMH state and
 * synthetic input (XTest).  Exit status is the number of failed checks.
 *
 *   drive <path-to-testclient> <marker-dir> [screenshot-dir]
 */
#if defined(__linux__) || defined(__GLIBC__)
# define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static Display *dpy;
static Window root;
static int failures, checks;
static const char *client_bin, *marker_dir, *shot_dir;

#define AT(n) XInternAtom(dpy, n, False)
#define CHECK(cond, ...) do { \
	checks++; \
	if (cond) { printf("  ok    "); } else { printf("  FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
} while (0)
/* poll `expr` for up to `ms` milliseconds */
#define WAIT(expr, ms) do { \
	int waited_ = 0; \
	for (;;) { \
		XSync(dpy, False); \
		if ((expr) || waited_ >= (ms)) break; \
		msleep(20); waited_ += 20; \
	} \
} while (0)

static void
msleep(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static int
on_xerror(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	return 0;                              /* windows vanish while polling */
}

static int
prop32(Window w, const char *name, unsigned long *out, int max)
{
	Atom type;
	int fmt, n = 0;
	unsigned long nitems = 0, after, i;
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, w, AT(name), 0, 4096, False, AnyPropertyType,
	    &type, &fmt, &nitems, &after, &data) != Success)
		return 0;
	if (data && fmt == 32) {
		unsigned long *v = (unsigned long *)(void *)data;

		for (i = 0; i < nitems && n < max; i++)
			out[n++] = v[i] & 0xffffffffUL;
	}
	if (data)
		XFree(data);
	return n;
}

static unsigned long
prop1(Window w, const char *name, unsigned long def)
{
	unsigned long v;

	return prop32(w, name, &v, 1) == 1 ? v : def;
}

static int
prop_has(Window w, const char *name, unsigned long val)
{
	unsigned long v[256];
	int n = prop32(w, name, v, 256), i;

	for (i = 0; i < n; i++)
		if (v[i] == val)
			return 1;
	return 0;
}

static int
has_state(Window w, const char *state)
{
	return prop_has(w, "_NET_WM_STATE", AT(state));
}

static int
text_prop(Window w, const char *name, char *buf, size_t n)
{
	Atom type;
	int fmt;
	unsigned long nitems, after;
	unsigned char *data = NULL;

	buf[0] = '\0';
	if (XGetWindowProperty(dpy, w, AT(name), 0, 256, False,
	    AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success ||
	    !data)
		return 0;
	snprintf(buf, n, "%s", (char *)data);
	XFree(data);
	return 1;
}

static int
title_of(Window w, char *buf, size_t n)
{
	return text_prop(w, "_NET_WM_NAME", buf, n);
}

static Window
parent_of(Window w)
{
	Window r, p = None, *kids = NULL;
	unsigned n;

	if (!XQueryTree(dpy, w, &r, &p, &kids, &n))
		return None;
	if (kids)
		XFree(kids);
	return p;
}

typedef struct Geom { int x, y, w, h; } Geom;

static Geom
geom(Window w)
{
	Geom g = { 0, 0, 0, 0 };
	Window r;
	unsigned gw, gh, bw, depth;

	if (XGetGeometry(dpy, w, &r, &g.x, &g.y, &gw, &gh, &bw, &depth)) {
		g.w = (int)gw;
		g.h = (int)gh;
	}
	return g;
}

static int
viewable(Window w)
{
	XWindowAttributes wa;

	return XGetWindowAttributes(dpy, w, &wa) && wa.map_state == IsViewable;
}

static Window
find_client(const char *title, const char *type)
{
	unsigned long list[256];
	int n = prop32(root, "_NET_CLIENT_LIST", list, 256), i;

	for (i = 0; i < n; i++) {
		char buf[256];

		if (type && !prop_has(list[i], "_NET_WM_WINDOW_TYPE", AT(type)))
			continue;
		if (title && (!title_of(list[i], buf, sizeof buf) || strcmp(buf, title)))
			continue;
		return list[i];
	}
	return None;
}

static int
managed(Window w)
{
	return prop_has(root, "_NET_CLIENT_LIST", w);
}

static Window
active(void)
{
	return prop1(root, "_NET_ACTIVE_WINDOW", None);
}

static Window
input_focus(void)
{
	Window f = None;
	int revert;

	XGetInputFocus(dpy, &f, &revert);
	return f;
}

static void
root_msg(Window w, const char *type, long d0, long d1, long d2, long d3)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = AT(type);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	ev.xclient.data.l[3] = d3;
	XSendEvent(dpy, root, False, SubstructureNotifyMask |
	    SubstructureRedirectMask, &ev);
	XFlush(dpy);
}

static void
wm_command(const char *verb)
{
	root_msg(root, "_RETRO_WM_COMMAND", (long)AT(verb), 0, 0, 0);
}

/* ---------------------------------------------------------- fake input */

static void
pointer_to(int x, int y)
{
	XTestFakeMotionEvent(dpy, DefaultScreen(dpy), x, y, CurrentTime);
	XSync(dpy, False);
	msleep(30);
}

static void
button(int b, int press)
{
	XTestFakeButtonEvent(dpy, (unsigned)b, press ? True : False, CurrentTime);
	XSync(dpy, False);
	msleep(30);
}

static void
click(int x, int y, int b)
{
	pointer_to(x, y);
	button(b, 1);
	button(b, 0);
}

static void
key(KeySym ks, int press)
{
	KeyCode kc = XKeysymToKeycode(dpy, ks);

	if (!kc) {
		printf("  (no keycode for keysym 0x%lx)\n", (unsigned long)ks);
		return;
	}
	XTestFakeKeyEvent(dpy, kc, press ? True : False, CurrentTime);
	XSync(dpy, False);
	msleep(30);
}

static void
tap(KeySym ks)
{
	key(ks, 1);
	key(ks, 0);
}

static void
combo(KeySym mod, KeySym ks)
{
	key(mod, 1);
	tap(ks);
	key(mod, 0);
}

static void
type_text(const char *s)
{
	for (; *s; s++) {
		char name[2] = { *s, '\0' };
		KeySym ks = *s == ' ' ? XK_space : *s == '-' ? XK_minus :
		    XStringToKeysym(name);

		if (ks != NoSymbol)
			tap(ks);
	}
}

/* number of viewable override-redirect top-levels (frames, menus, popups) */
static int
popup_count(void)
{
	Window r, p, *kids = NULL;
	unsigned n, i;
	int count = 0;

	if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
		return 0;
	for (i = 0; i < n; i++) {
		XWindowAttributes wa;

		if (XGetWindowAttributes(dpy, kids[i], &wa) && wa.override_redirect &&
		    wa.map_state == IsViewable && wa.class == InputOutput)
			count++;
	}
	if (kids)
		XFree(kids);
	return count;
}

static void
shot(const char *name)
{
	char cmd[1024];

	if (!shot_dir)
		return;
	XSync(dpy, False);
	msleep(150);
	snprintf(cmd, sizeof cmd, "import -window root '%s/%s.png' 2>/dev/null",
	    shot_dir, name);
	if (system(cmd) != 0)
		printf("  (screenshot %s not taken)\n", name);
}

/* ------------------------------------------------------------- clients */

static pid_t kids[16];
static int nkids;

/* never leave test clients behind, whatever happened */
static void
kill_kids(void)
{
	int i;

	for (i = 0; i < nkids; i++)
		if (kids[i] > 0 && waitpid(kids[i], NULL, WNOHANG) == 0) {
			kill(kids[i], SIGKILL);
			waitpid(kids[i], NULL, 0);
		}
}

typedef struct Proc {
	pid_t pid;
	Window win;
} Proc;

static Proc
spawn_client(const char *title, const char *geometry, int tray)
{
	Proc p = { 0, None };
	int fd[2];
	char line[128];
	FILE *f;

	if (pipe(fd) < 0)
		return p;
	p.pid = fork();
	if (p.pid == 0) {
		dup2(fd[1], 1);
		close(fd[0]);
		close(fd[1]);
		if (tray)
			execl(client_bin, client_bin, "-title", title, "-tray",
			    (char *)NULL);
		else
			execl(client_bin, client_bin, "-title", title, "-geometry",
			    geometry, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	if (nkids < 16)
		kids[nkids++] = p.pid;
	f = fdopen(fd[0], "r");
	if (f && fgets(line, sizeof line, f))
		sscanf(line, "window %lx", &p.win);
	if (f)
		fclose(f);
	return p;
}

static int
marker_exists(const char *name)
{
	char path[1024];
	struct stat st;

	snprintf(path, sizeof path, "%s/%s", marker_dir, name);
	return stat(path, &st) == 0;
}

/* ------------------------------------------------------------- CDE tests */

/* a front panel hotspot (see bar/frontpanel.c) in root coordinates */
static int
hotspot(Window panel, unsigned long id, Geom *g)
{
	unsigned long v[512];
	int n = prop32(panel, "_RETRO_FP_HOTSPOTS", v, 512), i;
	Window child;

	for (i = 0; i + 4 < n; i += 5) {
		if (v[i] != id)
			continue;
		XTranslateCoordinates(dpy, panel, root, (int)v[i + 1], (int)v[i + 2],
		    &g->x, &g->y, &child);
		g->w = (int)v[i + 3];
		g->h = (int)v[i + 4];
		return 1;
	}
	return 0;
}

static void
click_center(Geom g, int b)
{
	click(g.x + g.w / 2, g.y + g.h / 2, b);
}

/* viewable override-redirect top-level with the given WM_CLASS name */
static Window
find_or_window(const char *res_name)
{
	Window r, p, *kids = NULL, found = None;
	unsigned n, i;

	if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
		return None;
	for (i = 0; i < n && found == None; i++) {
		XWindowAttributes wa;
		XClassHint ch;

		if (!XGetWindowAttributes(dpy, kids[i], &wa) || !wa.override_redirect ||
		    wa.map_state != IsViewable)
			continue;
		memset(&ch, 0, sizeof ch);
		if (XGetClassHint(dpy, kids[i], &ch)) {
			if (ch.res_name && !strcmp(ch.res_name, res_name))
				found = kids[i];
			if (ch.res_name)
				XFree(ch.res_name);
			if (ch.res_class)
				XFree(ch.res_class);
		}
	}
	if (kids)
		XFree(kids);
	return found;
}

static pid_t
spawn_prog(const char *prog)
{
	pid_t pid = fork();

	if (pid == 0) {
		execlp(prog, prog, (char *)NULL);
		_exit(127);
	}
	return pid;
}

static int
cde_tests(int sw, int sh)
{
	unsigned long wa[4] = { 0, 0, 0, 0 }, ext[4] = { 0, 0, 0, 0 }, mark = 1;
	Window check, bar, panel_frame, frame_a, icon, conf;
	Geom g, ws, gp;
	Proc a, b;
	pid_t sm;
	char buf[256];
	int l, th, status, pc;

	printf("[c1] CDE session\n");
	WAIT(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) != None, 5000);
	check = prop1(root, "_NET_SUPPORTING_WM_CHECK", None);
	CHECK(check != None, "window manager is running");
	text_prop(root, "_NET_DESKTOP_NAMES", buf, sizeof buf);
	CHECK(!strcmp(buf, "One"), "CDE workspace names (first: %s)", buf);
	WAIT((bar = find_client(NULL, "_NET_WM_WINDOW_TYPE_DOCK")) != None &&
	    hotspot(bar, 1, &ws), 5000);
	CHECK(bar != None && hotspot(bar, 1, &ws), "front panel with hotspots");
	panel_frame = parent_of(bar);
	gp = geom(panel_frame);
	CHECK(gp.w < sw && abs(gp.x + gp.w / 2 - sw / 2) <= 1 &&
	    gp.y + gp.h == sh, "front panel is centred at the bottom (%dx%d)",
	    gp.w, gp.h);
	WAIT(prop32(root, "_NET_WORKAREA", wa, 4) == 4 && (int)wa[3] < sh, 3000);
	CHECK((int)wa[3] == sh - gp.h, "work area stops above the front panel");
	msleep(600);
	shot("c01-cde-desktop");

	printf("[c2] Motif frames\n");
	a = spawn_client("Alpha", "320x200+200+120", 0);
	b = spawn_client("Beta", "300x180+620+300", 0);
	WAIT(managed(a.win) && managed(b.win) && active() == b.win, 3000);
	frame_a = parent_of(a.win);
	prop32(a.win, "_NET_FRAME_EXTENTS", ext, 4);
	l = (int)ext[0];
	th = (int)ext[2] - l;
	CHECK(l >= 5 && ext[1] == ext[0] && ext[3] == ext[0] && th > 12,
	    "Motif frame: %d px resize border, %d px title", l, th);
	g = geom(frame_a);
	click(g.x + g.w / 2, g.y + l + th / 2, 1);
	WAIT(active() == a.win, 2000);
	CHECK(active() == a.win, "clicking the title box focuses the window");
	msleep(400);
	shot("c02-motif-frames");

	printf("[c3] maximize button\n");
	click(g.x + g.w - l - th / 2, g.y + l + th / 2, 1);
	WAIT(has_state(a.win, "_NET_WM_STATE_MAXIMIZED_HORZ"), 2000);
	CHECK(has_state(a.win, "_NET_WM_STATE_MAXIMIZED_HORZ") &&
	    geom(frame_a).w == (int)wa[2], "maximize button maximizes");
	msleep(500);
	g = geom(frame_a);
	click(g.x + g.w - l - th / 2, g.y + l + th / 2, 1);
	WAIT(!has_state(a.win, "_NET_WM_STATE_MAXIMIZED_HORZ"), 2000);
	g = geom(frame_a);
	CHECK(g.x == 200 && g.w == 320 + 2 * l, "pressing it again restores");

	printf("[c4] iconify to the desktop\n");
	msleep(500);
	click(g.x + g.w - l - th - th / 2, g.y + l + th / 2, 1);
	WAIT(has_state(a.win, "_NET_WM_STATE_HIDDEN") &&
	    (icon = find_or_window("retro-icon")) != None, 2000);
	icon = find_or_window("retro-icon");
	CHECK(has_state(a.win, "_NET_WM_STATE_HIDDEN") && !viewable(a.win) &&
	    icon != None, "minimize button turns the window into a desktop icon");
	if (icon != None) {
		char *nm = NULL;

		XFetchName(dpy, icon, &nm);
		CHECK(nm && !strcmp(nm, "Alpha"), "the icon is labelled '%s'",
		    nm ? nm : "");
		if (nm)
			XFree(nm);
		shot("c04-icon");
		g = geom(icon);
		click(g.x + g.w / 2, g.y + 10, 1);
		click(g.x + g.w / 2, g.y + 10, 1);
		WAIT(viewable(a.win) && active() == a.win, 2000);
	}
	CHECK(viewable(a.win) && active() == a.win &&
	    find_or_window("retro-icon") == None,
	    "double-clicking the icon restores the window");

	printf("[c5] workspace switch on the front panel\n");
	click(ws.x + ws.w * 3 / 4, ws.y + ws.h / 4, 1);
	WAIT(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1, 2000);
	CHECK(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1 && !viewable(a.win),
	    "button 'Two' switches to workspace 2");
	shot("c05-workspace-two");
	click(ws.x + ws.w / 4, ws.y + ws.h / 4, 1);
	WAIT(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 0 && viewable(a.win), 2000);
	CHECK(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 0, "button 'One' returns");

	printf("[c6] controls and subpanels\n");
	pc = popup_count();
	if (hotspot(bar, 200 + 6, &g)) {
		click_center(g, 1);
		WAIT(popup_count() == pc + 1, 2000);
	}
	CHECK(popup_count() == pc + 1, "Style Manager subpanel opens");
	shot("c06-subpanel");
	tap(XK_Escape);
	WAIT(popup_count() == pc, 2000);
	if (hotspot(bar, 2, &g)) {
		click_center(g, 1);
		WAIT(popup_count() == pc + 1, 2000);
	}
	CHECK(popup_count() == pc + 1, "Exit subpanel (lock/power) opens");
	shot("c06b-power-subpanel");
	tap(XK_Down);
	tap(XK_Return);
	WAIT(marker_exists("lock-marker"), 3000);
	CHECK(marker_exists("lock-marker"), "Lock Screen runs the screen locker");
	if (hotspot(bar, 100 + 2, &g))
		click_center(g, 1);
	WAIT(marker_exists("files-marker"), 3000);
	CHECK(marker_exists("files-marker"),
	    "Files control runs the default file manager");

	printf("[c7] EXIT asks first\n");
	msleep(300);
	if (hotspot(bar, 3, &g))
		click_center(g, 1);
	WAIT((conf = find_or_window("retro-confirm")) != None, 3000);
	CHECK(conf != None, "logout confirmation dialog appears");
	shot("c07-confirm");
	tap(XK_Escape);
	WAIT(find_or_window("retro-confirm") == None, 2000);
	msleep(300);
	CHECK(find_or_window("retro-confirm") == None &&
	    prop1(root, "_NET_SUPPORTING_WM_CHECK", None) == check,
	    "Cancel keeps the session running");

	printf("[c8] Motif menu button\n");
	msleep(400);
	g = geom(frame_a);
	click(g.x + l + th / 2, g.y + l + th / 2, 1);
	msleep(150);
	click(g.x + l + th / 2, g.y + l + th / 2, 1);
	status = -1;
	WAIT(waitpid(a.pid, &status, WNOHANG) == a.pid, 3000);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "double-clicking the window menu button closes the window");

	printf("[c9] Style Manager\n");
	sm = spawn_prog("retro-settings");
	WAIT(find_client("Style Manager", NULL) != None, 4000);
	CHECK(find_client("Style Manager", NULL) != None,
	    "Style Manager opens as a managed window");
	msleep(800);
	shot("c09-style-manager");
	root_msg(find_client("Style Manager", NULL), "_NET_CLOSE_WINDOW",
	    CurrentTime, 2, 0, 0);
	status = -1;
	WAIT(waitpid(sm, &status, WNOHANG) == sm, 3000);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "Style Manager closes cleanly");

	printf("[c10] switching theme and style live\n");
	XChangeProperty(dpy, check, AT("_DRIVE_MARK"), XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *)&mark, 1);
	XSync(dpy, False);
	if (system("retro-settings --apply theme=Classic style=classic") != 0)
		printf("  (retro-settings --apply failed)\n");
	WAIT((check = prop1(root, "_NET_SUPPORTING_WM_CHECK", None)) != None &&
	    prop1(check, "_DRIVE_MARK", 0) == 0 &&
	    (bar = find_client(NULL, "_NET_WM_WINDOW_TYPE_DOCK")) != None &&
	    geom(parent_of(bar)).w == sw && managed(b.win), 8000);
	CHECK(check != None && prop1(check, "_DRIVE_MARK", 0) == 0,
	    "window manager reloaded");
	CHECK(bar != None && geom(parent_of(bar)).w == sw,
	    "panel reloaded as the Classic taskbar");
	WAIT(prop32(b.win, "_NET_FRAME_EXTENTS", ext, 4) == 4 && ext[0] == 4,
	    3000);
	CHECK(ext[0] == 4 && managed(b.win), "windows now have Classic frames");
	msleep(800);
	shot("c10-reloaded-classic");

	printf("[c11] shutdown\n");
	kill(b.pid, SIGTERM);
	waitpid(b.pid, NULL, 0);
	msleep(300);
	wm_command("quit");
	WAIT(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) == None, 5000);
	CHECK(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) == None,
	    "quit ends the session");
	kill_kids();
	printf("%d checks, %d failed\n", checks, failures);
	XCloseDisplay(dpy);
	return failures;
}

/* ------------------------------------------------------------------ tests */

int
main(int argc, char **argv)
{
	unsigned long wa[4] = { 0, 0, 0, 0 }, ext[4] = { 0, 0, 0, 0 }, v[64];
	unsigned long mark;
	Window check, bar, deskw, frame_a, frame_b, runw;
	Proc a, b, c, tray;
	Geom g, g2, gbar, ga, gb;
	char buf[256];
	int ev, er, maj, min, sw, sh, n, pc, status, bs, x;
	Proc d;

	if (argc < 3) {
		fprintf(stderr, "usage: drive testclient marker-dir [shot-dir]\n");
		return 2;
	}
	client_bin = argv[1];
	marker_dir = argv[2];
	shot_dir = argc > 3 ? argv[3] : NULL;
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "drive: cannot open display\n");
		return 2;
	}
	XSetErrorHandler(on_xerror);
	root = DefaultRootWindow(dpy);
	sw = DisplayWidth(dpy, DefaultScreen(dpy));
	sh = DisplayHeight(dpy, DefaultScreen(dpy));
	if (!XTestQueryExtension(dpy, &ev, &er, &maj, &min)) {
		fprintf(stderr, "drive: XTest extension missing\n");
		return 2;
	}
	if (argc > 4 && !strcmp(argv[4], "cde"))
		return cde_tests(sw, sh);

	printf("[1] session components\n");
	WAIT(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) != None, 5000);
	check = prop1(root, "_NET_SUPPORTING_WM_CHECK", None);
	CHECK(check != None, "EWMH supporting WM check window is set");
	title_of(check, buf, sizeof buf);
	CHECK(!strcmp(buf, "retro-wm"), "window manager identifies as retro-wm");
	CHECK(prop1(root, "_NET_NUMBER_OF_DESKTOPS", 0) == 4, "4 virtual desktops");
	WAIT((bar = find_client(NULL, "_NET_WM_WINDOW_TYPE_DOCK")) != None, 5000);
	CHECK(bar != None, "panel is managed as a dock");
	WAIT((deskw = find_client(NULL, "_NET_WM_WINDOW_TYPE_DESKTOP")) != None,
	    5000);
	CHECK(deskw != None, "desktop window is managed");
	WAIT(prop32(root, "_NET_WORKAREA", wa, 4) == 4 && (int)wa[3] < sh, 3000);
	gbar = geom(parent_of(bar));
	CHECK((int)wa[3] == sh - gbar.h && gbar.h > 10,
	    "work area excludes the panel strut (%lux%lu, panel %d px)", wa[2],
	    wa[3], gbar.h);
	CHECK(gbar.y + gbar.h == sh && gbar.w == sw, "panel docked at the bottom");
	CHECK(viewable(bar) && viewable(deskw), "panel and desktop are mapped");
	WAIT(marker_exists("block-marker"), 5000);
	CHECK(marker_exists("block-marker"), "panel block from retro.conf runs");
	text_prop(root, "_RETRO_LAYOUT", buf, sizeof buf);
	CHECK(!strcmp(buf, "><>"), "default layout from config.h published: %s",
	    buf);
	msleep(700);                           /* let the applets draw once */
	shot("01-empty-desktop");

	printf("[2] re-parenting and focus\n");
	a = spawn_client("Alpha", "320x200+100+80", 0);
	WAIT(managed(a.win), 3000);
	CHECK(managed(a.win), "client Alpha appears in _NET_CLIENT_LIST");
	frame_a = parent_of(a.win);
	CHECK(frame_a != root && frame_a != None && parent_of(frame_a) == root,
	    "Alpha is re-parented into a frame window");
	CHECK(prop32(a.win, "_NET_FRAME_EXTENTS", ext, 4) == 4 && ext[2] > ext[0] &&
	    ext[0] > 0, "frame extents l=%lu r=%lu t=%lu b=%lu", ext[0], ext[1],
	    ext[2], ext[3]);
	g = geom(frame_a);
	CHECK(g.x == 100 && g.y == 80 && g.w == 320 + (int)(ext[0] + ext[1]) &&
	    g.h == 200 + (int)(ext[2] + ext[3]),
	    "frame geometry honours USPosition (%d,%d %dx%d)", g.x, g.y, g.w, g.h);
	CHECK(prop1(a.win, "WM_STATE", 99) == NormalState, "WM_STATE is Normal");
	WAIT(active() == a.win && input_focus() == a.win, 2000);
	CHECK(active() == a.win, "_NET_ACTIVE_WINDOW is Alpha");
	CHECK(input_focus() == a.win, "X input focus is on Alpha");

	b = spawn_client("Beta", "300x180+360+240", 0);
	WAIT(managed(b.win) && active() == b.win, 3000);
	frame_b = parent_of(b.win);
	CHECK(active() == b.win && input_focus() == b.win,
	    "new client Beta takes focus");
	msleep(500);
	click(g.x + 80, g.y + (int)ext[0] + 6, 1);
	WAIT(active() == a.win, 2000);
	CHECK(active() == a.win && input_focus() == a.win,
	    "clicking Alpha's title bar focuses it");
	n = prop32(root, "_NET_CLIENT_LIST_STACKING", v, 64);
	CHECK(n > 0 && (v[n - 1] == a.win || v[n - 2] == a.win),
	    "Alpha was raised in the stacking order");
	shot("02-two-windows");

	printf("[3] Alt+Tab switcher\n");
	pc = popup_count();
	key(XK_Alt_L, 1);
	tap(XK_Tab);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "switcher popup is shown while Alt is held");
	shot("03-alt-tab");
	key(XK_Alt_L, 0);
	WAIT(active() == b.win, 2000);
	CHECK(active() == b.win, "Alt+Tab switched to Beta");
	CHECK(popup_count() == pc, "switcher popup is gone");

	printf("[4] move, maximize, fullscreen, minimize\n");
	msleep(500);
	g = geom(frame_a);
	pointer_to(g.x + 80, g.y + (int)ext[0] + 6);
	button(1, 1);
	pointer_to(g.x + 110, g.y + (int)ext[0] + 26);
	pointer_to(g.x + 140, g.y + (int)ext[0] + 46);
	button(1, 0);
	WAIT(geom(frame_a).x == g.x + 60, 2000);
	g2 = geom(frame_a);
	CHECK(g2.x == g.x + 60 && g2.y == g.y + 40,
	    "title-bar drag moved the frame by (60,40): now (%d,%d)", g2.x, g2.y);
	msleep(500);
	click(g2.x + 80, g2.y + (int)ext[0] + 6, 1);
	click(g2.x + 80, g2.y + (int)ext[0] + 6, 1);
	WAIT(has_state(a.win, "_NET_WM_STATE_MAXIMIZED_VERT"), 2000);
	g = geom(frame_a);
	CHECK(has_state(a.win, "_NET_WM_STATE_MAXIMIZED_VERT") &&
	    has_state(a.win, "_NET_WM_STATE_MAXIMIZED_HORZ") &&
	    g.x == (int)wa[0] && g.y == (int)wa[1] && g.w == (int)wa[2] &&
	    g.h == (int)wa[3], "double-click maximizes to the work area");
	shot("04-maximized");
	root_msg(a.win, "_NET_WM_STATE", 0,
	    (long)AT("_NET_WM_STATE_MAXIMIZED_VERT"),
	    (long)AT("_NET_WM_STATE_MAXIMIZED_HORZ"), 2);
	WAIT(geom(frame_a).w == g2.w, 2000);
	g = geom(frame_a);
	CHECK(g.x == g2.x && g.y == g2.y && g.w == g2.w && g.h == g2.h,
	    "un-maximize restores the previous geometry");
	root_msg(a.win, "_NET_WM_STATE", 1,
	    (long)AT("_NET_WM_STATE_FULLSCREEN"), 0, 2);
	WAIT(geom(parent_of(a.win)).w == sw, 2000);
	g = geom(parent_of(a.win));
	CHECK(g.x == 0 && g.y == 0 && g.w == sw && g.h == sh &&
	    geom(a.win).w == sw, "fullscreen covers the screen, no decoration");
	root_msg(a.win, "_NET_WM_STATE", 0,
	    (long)AT("_NET_WM_STATE_FULLSCREEN"), 0, 2);
	WAIT(geom(frame_a).w == g2.w, 2000);
	CHECK(geom(frame_a).w == g2.w && geom(frame_a).x == g2.x,
	    "leaving fullscreen restores the frame");

	printf("[4b] snapping, keyboard moves, program rules, do not disturb\n");
	root_msg(a.win, "_NET_ACTIVE_WINDOW", 2, CurrentTime, 0, 0);
	WAIT(active() == a.win, 2000);
	combo(XK_Super_L, XK_Left);
	WAIT(geom(frame_a).w == (int)wa[2] / 2, 2000);
	g = geom(frame_a);
	CHECK(g.x == (int)wa[0] && g.y == (int)wa[1] && g.w == (int)wa[2] / 2 &&
	    g.h == (int)wa[3], "Super+Left snaps to the left half (%d,%d %dx%d)",
	    g.x, g.y, g.w, g.h);
	combo(XK_Super_L, XK_Right);
	WAIT(geom(frame_a).x == (int)wa[0] + (int)wa[2] / 2, 2000);
	g = geom(frame_a);
	CHECK(g.x == (int)wa[0] + (int)wa[2] / 2 && g.w == (int)wa[2] -
	    (int)wa[2] / 2 && g.h == (int)wa[3], "Super+Right snaps to the right half");
	shot("04b-snapped");
	combo(XK_Super_L, XK_Down);
	WAIT(geom(frame_a).w == g2.w, 2000);
	g = geom(frame_a);
	CHECK(g.w == g2.w && g.h == g2.h && g.x == (int)wa[0] + ((int)wa[2] -
	    g2.w) / 2, "Super+Down centres it at its own size again");
	key(XK_Alt_L, 1);
	combo(XK_Super_L, XK_Right);
	key(XK_Alt_L, 0);
	WAIT(geom(frame_a).x == g.x + 32, 2000);
	CHECK(geom(frame_a).x == g.x + 32 && geom(frame_a).y == g.y,
	    "Super+Alt+Right moves it one step");
	key(XK_Control_L, 1);
	combo(XK_Super_L, XK_Right);
	key(XK_Control_L, 0);
	WAIT(geom(frame_a).w == g2.w + 32, 2000);
	CHECK(geom(frame_a).w == g2.w + 32, "Super+Ctrl+Right makes it wider");
	key(XK_Control_L, 1);
	combo(XK_Super_L, XK_Left);
	key(XK_Control_L, 0);
	WAIT(geom(frame_a).w == g2.w, 2000);
	/* drag the title against the left edge of the screen */
	msleep(500);
	g = geom(frame_a);
	pointer_to(g.x + 80, g.y + (int)ext[0] + 6);
	button(1, 1);
	pointer_to(g.x + 40, g.y + (int)ext[0] + 30);
	pointer_to(40, 300);
	pointer_to(0, 300);
	msleep(150);
	button(1, 0);
	WAIT(geom(frame_a).w == (int)wa[2] / 2 && geom(frame_a).x == (int)wa[0],
	    2000);
	g = geom(frame_a);
	CHECK(g.x == (int)wa[0] && g.w == (int)wa[2] / 2 && g.h == (int)wa[3],
	    "dragging to the screen edge snaps to that half");
	/* pulling it off the edge gives the old size back */
	msleep(500);                           /* not a double click */
	pointer_to(g.x + 80, g.y + (int)ext[0] + 6);
	button(1, 1);
	pointer_to(g.x + 120, g.y + (int)ext[0] + 40);
	pointer_to(g.x + 200, g.y + (int)ext[0] + 106);
	button(1, 0);
	WAIT(geom(frame_a).w == g2.w, 2000);
	CHECK(geom(frame_a).w == g2.w && geom(frame_a).h == g2.h,
	    "dragging a snapped window away restores its size");
	root_msg(a.win, "_NET_MOVERESIZE_WINDOW", (1 << 8) | (1 << 9), g2.x, g2.y,
	    0);
	WAIT(geom(frame_a).x == g2.x && geom(frame_a).y == g2.y, 2000);
	{
		/* programs.conf: rule = title=Ruled => workspace=3 geometry=... */
		Proc r = spawn_client("Ruled", "120x90", 0);
		Geom rg;

		WAIT(managed(r.win), 3000);
		WAIT(prop1(r.win, "_NET_WM_DESKTOP", 99) == 2, 2000);
		rg = geom(r.win);
		CHECK(managed(r.win) && prop1(r.win, "_NET_WM_DESKTOP", 99) == 2 &&
		    rg.w == 260 && rg.h == 170 &&
		    has_state(r.win, "_NET_WM_STATE_ABOVE") && !viewable(r.win),
		    "a rule in programs.conf places a new window (workspace 3, "
		    "%dx%d, above)", rg.w, rg.h);
		CHECK(active() == a.win, "the ruled window did not take the focus");
		root_msg(r.win, "_NET_CLOSE_WINDOW", CurrentTime, 2, 0, 0);
		WAIT(!managed(r.win), 3000);
	}
	combo(XK_Super_L, XK_z);
	WAIT(marker_exists("keys-marker"), 3000);
	CHECK(marker_exists("keys-marker"), "a binding from keys.conf works");
	wm_command("dnd");
	WAIT(prop1(root, "_RETRO_DND", 0) == 1, 2000);
	CHECK(prop1(root, "_RETRO_DND", 0) == 1, "do not disturb switches on");
	root_msg(b.win, "_NET_ACTIVE_WINDOW", 1, CurrentTime, 0, 0);
	msleep(300);
	CHECK(active() == a.win,
	    "do not disturb: a program cannot pull itself to the front");
	wm_command("dnd");
	WAIT(prop1(root, "_RETRO_DND", 9) == 0, 2000);
	wm_command("awake");
	WAIT(prop1(root, "_RETRO_AWAKE", 0) == 1, 2000);
	CHECK(prop1(root, "_RETRO_DND", 9) == 0 &&
	    prop1(root, "_RETRO_AWAKE", 0) == 1,
	    "do not disturb off again; keep-the-screen-on switches on");
	shot("04c-awake");
	wm_command("awake");
	WAIT(prop1(root, "_RETRO_AWAKE", 9) == 0, 2000);
	root_msg(a.win, "_NET_WM_STATE", 1, (long)AT("_NET_WM_STATE_SHADED"), 0, 2);
	WAIT(geom(frame_a).h < 60, 2000);
	CHECK(geom(frame_a).h == (int)(ext[2] + ext[3]) &&
	    has_state(a.win, "_NET_WM_STATE_SHADED"), "shade rolls up to the title");
	root_msg(a.win, "_NET_WM_STATE", 0, (long)AT("_NET_WM_STATE_SHADED"), 0, 2);
	WAIT(geom(frame_a).h == g2.h, 2000);
	root_msg(a.win, "WM_CHANGE_STATE", IconicState, 0, 0, 0);
	WAIT(has_state(a.win, "_NET_WM_STATE_HIDDEN"), 2000);
	CHECK(has_state(a.win, "_NET_WM_STATE_HIDDEN") &&
	    prop1(a.win, "WM_STATE", 99) == IconicState && !viewable(a.win),
	    "minimize hides the window (Iconic, _NET_WM_STATE_HIDDEN)");
	CHECK(active() != a.win, "focus moved away from the minimized window");
	shot("05-minimized");
	root_msg(a.win, "_NET_ACTIVE_WINDOW", 2, CurrentTime, 0, 0);
	WAIT(active() == a.win && viewable(a.win), 2000);
	CHECK(active() == a.win && viewable(a.win) &&
	    !has_state(a.win, "_NET_WM_STATE_HIDDEN"),
	    "_NET_ACTIVE_WINDOW restores and focuses it");
	/* the same again with the mouse: title button, then the task bar */
	msleep(500);
	g = geom(frame_a);
	bs = (int)(ext[2] - ext[0]) - 5;
	click(g.x + g.w - (int)ext[0] - 4 - 3 * bs + bs / 2,
	    g.y + (int)ext[0] + 2 + bs / 2, 1);
	WAIT(has_state(a.win, "_NET_WM_STATE_HIDDEN"), 2000);
	CHECK(has_state(a.win, "_NET_WM_STATE_HIDDEN"),
	    "clicking the minimize title button hides the window");
	/* task buttons start after Start, the pager and the layout symbol */
	for (x = gbar.x + 260; x < gbar.x + 800; x += 30) {
		click(x, gbar.y + gbar.h / 2, 1);
		WAIT(active() == a.win && viewable(a.win), 300);
		if (prop1(root, "_NET_CURRENT_DESKTOP", 0) != 0)
			root_msg(root, "_NET_CURRENT_DESKTOP", 0, CurrentTime, 0, 0);
		if (active() == a.win && viewable(a.win))
			break;
	}
	CHECK(active() == a.win && viewable(a.win),
	    "clicking its task bar button restores it (x=%d)", x);
	text_prop(root, "_RETRO_LAYOUT", buf, sizeof buf);
	CHECK(!strcmp(buf, "><>"), "still in the floating layout");
	pc = popup_count();
	click(g.x + 80, g.y + (int)ext[0] + 6, 3);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "right-click on the title opens the window menu");
	shot("05b-window-menu");
	tap(XK_Escape);
	WAIT(popup_count() == pc, 2000);
	msleep(500);

	printf("[5] workspaces\n");
	root_msg(root, "_NET_CURRENT_DESKTOP", 1, CurrentTime, 0, 0);
	WAIT(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1 && !viewable(a.win), 2000);
	CHECK(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1,
	    "_NET_CURRENT_DESKTOP switches to workspace 2");
	CHECK(!viewable(a.win) && !viewable(b.win),
	    "windows of workspace 1 are unmapped");
	CHECK(viewable(bar) && viewable(deskw), "panel and desktop stay (sticky)");
	CHECK(active() == None, "no active window on the empty workspace");
	c = spawn_client("Gamma", "280x160+500+120", 0);
	WAIT(managed(c.win) && active() == c.win, 3000);
	CHECK(prop1(c.win, "_NET_WM_DESKTOP", 9) == 1,
	    "new client Gamma is placed on workspace 2");
	shot("06-workspace-2");
	combo(XK_Super_L, XK_1);
	WAIT(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 0 && viewable(a.win), 2000);
	CHECK(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 0 && viewable(a.win) &&
	    !viewable(c.win), "Super+1 key binding returns to workspace 1");
	WAIT(active() == a.win, 2000);
	CHECK(active() == a.win, "focus returns to the last used window");
	root_msg(b.win, "_NET_WM_DESKTOP", 1, 2, 0, 0);
	WAIT(!viewable(b.win), 2000);
	CHECK(!viewable(b.win) && prop1(b.win, "_NET_WM_DESKTOP", 9) == 1,
	    "_NET_WM_DESKTOP moves Beta to workspace 2");
	root_msg(b.win, "_NET_WM_DESKTOP", 0, 2, 0, 0);
	WAIT(viewable(b.win), 2000);
	root_msg(b.win, "_NET_WM_DESKTOP", (long)0xffffffffUL, 2, 0, 0);
	WAIT(has_state(b.win, "_NET_WM_STATE_STICKY"), 2000);
	CHECK(has_state(b.win, "_NET_WM_STATE_STICKY"), "Beta can be made sticky");
	root_msg(b.win, "_NET_WM_DESKTOP", 0, 2, 0, 0);
	WAIT(!has_state(b.win, "_NET_WM_STATE_STICKY"), 2000);

	combo(XK_Super_L, XK_x);
	WAIT(marker_exists("bind-marker"), 3000);
	CHECK(marker_exists("bind-marker"),
	    "key binding from retro.conf (Mod4+x exec ...) works");
	combo(XK_Super_L, XK_y);
	WAIT(marker_exists("app-marker"), 3000);
	CHECK(marker_exists("app-marker"),
	    "exec @app <id> launches a .desktop application");

	printf("[6] tiling layout\n");
	ga = geom(frame_a);
	gb = geom(frame_b);
	combo(XK_Super_L, XK_t);
	WAIT(geom(frame_a).h == (int)wa[3], 2000);
	g = geom(frame_a);
	g2 = geom(frame_b);
	CHECK(g.x == (int)wa[0] && g.y == (int)wa[1] && g.h == (int)wa[3] &&
	    g2.x == g.x + g.w && g.w + g2.w == (int)wa[2] && g2.h == (int)wa[3],
	    "Super+t tiles master %dx%d + stack %dx%d", g.w, g.h, g2.w, g2.h);
	text_prop(root, "_RETRO_LAYOUT", buf, sizeof buf);
	CHECK(!strcmp(buf, "[]="), "tile layout symbol: %s", buf);
	shot("07-tiled");
	combo(XK_Super_L, XK_t);
	WAIT(geom(frame_a).h == ga.h, 2000);
	g = geom(frame_a);
	g2 = geom(frame_b);
	CHECK(g.x == ga.x && g.w == ga.w && g.h == ga.h && g2.x == gb.x &&
	    g2.w == gb.w, "leaving tiling restores floating geometry");
	combo(XK_Super_L, XK_m);
	WAIT(geom(frame_a).w == (int)wa[2] && geom(frame_b).w == (int)wa[2], 2000);
	g = geom(frame_a);
	g2 = geom(frame_b);
	text_prop(root, "_RETRO_LAYOUT", buf, sizeof buf);
	CHECK(g.x == (int)wa[0] && g.y == (int)wa[1] && g.w == (int)wa[2] &&
	    g.h == (int)wa[3] && g2.w == (int)wa[2] && g2.h == (int)wa[3] &&
	    !strcmp(buf, "[M]"), "Super+m monocle fills the work area (%s)", buf);
	shot("07b-monocle");
	combo(XK_Super_L, XK_m);
	WAIT(geom(frame_a).h == ga.h, 2000);
	CHECK(geom(frame_a).w == ga.w && geom(frame_b).w == gb.w,
	    "Super+m again returns to floating");

	printf("[7] start menu\n");
	pc = popup_count();
	click(gbar.x + 20, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "start menu pops up");
	tap(XK_Down);
	tap(XK_Right);                         /* opens the first submenu */
	WAIT(popup_count() == pc + 2, 2000);
	CHECK(popup_count() == pc + 2, "submenu parsed from menu.conf cascades");
	shot("08-start-menu");
	tap(XK_Escape);
	tap(XK_Escape);
	WAIT(popup_count() == pc, 2000);
	CHECK(popup_count() == pc, "Escape closes the menu");
	click(gbar.x + 20, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc + 1, 2000);
	tap(XK_Down);
	tap(XK_Down);                          /* second entry: marker command */
	tap(XK_Return);
	WAIT(marker_exists("menu-marker"), 3000);
	CHECK(marker_exists("menu-marker"), "menu item launches its command");
	click(gbar.x + 20, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc + 1, 2000);
	tap(XK_Up);                            /* wraps to "Applications" */
	tap(XK_Right);
	WAIT(popup_count() == pc + 2, 2000);
	tap(XK_Right);                         /* first category */
	WAIT(popup_count() == pc + 3, 2000);
	CHECK(popup_count() == pc + 3,
	    "Applications > category submenus built from .desktop files");
	shot("08a-applications");
	tap(XK_Return);
	WAIT(marker_exists("menu-app-marker"), 3000);
	CHECK(marker_exists("menu-app-marker"),
	    ".desktop entry launched from the Start menu");
	WAIT(popup_count() == pc, 2000);
	/* right-click on the desktop opens the root menu */
	click(sw - 200, 40, 3);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "right-click root menu pops up");
	tap(XK_Escape);
	WAIT(popup_count() == pc, 2000);

	printf("[7b] panel applets and desktop icons\n");
	click(gbar.x + gbar.w - 15, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "clicking the clock opens the calendar");
	shot("08b-calendar");
	click(gbar.x + gbar.w - 15, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc, 2000);
	CHECK(popup_count() == pc, "clicking it again closes the calendar");
	click(gbar.x + gbar.w - 80, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc + 1, 2000);
	CHECK(popup_count() == pc + 1, "clicking the volume applet opens the slider");
	shot("08c-volume");
	click(gbar.x + gbar.w - 80, gbar.y + gbar.h / 2, 1);
	WAIT(popup_count() == pc, 2000);
	click(52, 40, 1);
	msleep(600);
	CHECK(!marker_exists("icon-marker"), "single click only selects an icon");
	click(52, 40, 1);
	click(52, 40, 1);
	WAIT(marker_exists("icon-marker"), 3000);
	CHECK(marker_exists("icon-marker"), "double-click launches the desktop icon");

	printf("[8] system tray (XEMBED)\n");
	tray = spawn_client("TrayIcon", "", 1);
	WAIT(parent_of(tray.win) == bar && viewable(tray.win), 3000);
	CHECK(parent_of(tray.win) == bar, "tray icon is embedded in the panel");
	g = geom(tray.win);
	CHECK(viewable(tray.win) && g.w == g.h && g.w >= 12 && g.w <= gbar.h,
	    "tray icon is mapped and sized %dx%d", g.w, g.h);
	shot("09-tray");

	printf("[9] run dialog\n");
	combo(XK_Alt_L, XK_F2);
	WAIT((runw = find_client("Run", NULL)) != None && active() == runw, 3000);
	CHECK(runw != None && active() == runw, "Alt+F2 opens the focused Run dialog");
	type_text("zzretro");
	tap(XK_Tab);                           /* completes zzretromarker */
	msleep(200);
	shot("10-run-dialog");
	tap(XK_Return);
	WAIT(marker_exists("run-marker"), 3000);
	CHECK(marker_exists("run-marker"),
	    "typed command with PATH completion was executed");
	WAIT(find_client("Run", NULL) == None, 2000);
	CHECK(find_client("Run", NULL) == None, "Run dialog closed itself");

	printf("[10] closing clients\n");
	d = spawn_client("Delta", "200x120+700+300", 0);
	WAIT(managed(d.win), 3000);
	root_msg(d.win, "_NET_CLOSE_WINDOW", CurrentTime, 2, 0, 0);
	status = -1;
	WAIT(waitpid(d.pid, &status, WNOHANG) == d.pid, 3000);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "_NET_CLOSE_WINDOW delivered WM_DELETE_WINDOW; client exited cleanly");
	msleep(500);
	g = geom(frame_a);
	click(g.x + g.w - (int)ext[0] - 2 - bs / 2, g.y + (int)ext[0] + 2 + bs / 2, 1);
	status = -1;
	WAIT(waitpid(a.pid, &status, WNOHANG) == a.pid, 3000);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "clicking the close title button closes Alpha cleanly");
	WAIT(!managed(a.win), 2000);
	CHECK(!managed(a.win), "closed client left _NET_CLIENT_LIST");
	XKillClient(dpy, b.win);
	WAIT(!managed(b.win), 2000);
	waitpid(b.pid, NULL, 0);
	CHECK(!managed(b.win), "killed client is unmanaged without fuss");

	printf("[11] window manager restart\n");
	root_msg(root, "_NET_CURRENT_DESKTOP", 1, CurrentTime, 0, 0);
	WAIT(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1, 2000);
	/* tag the old check window: the new WM's window will not carry it */
	mark = 1;
	XChangeProperty(dpy, check, AT("_DRIVE_MARK"), XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *)&mark, 1);
	XSync(dpy, False);
	wm_command("restart");
	WAIT((check = prop1(root, "_NET_SUPPORTING_WM_CHECK", None)) != None &&
	    prop1(check, "_DRIVE_MARK", 0) == 0 && managed(c.win) && managed(bar),
	    5000);
	title_of(check, buf, sizeof buf);
	CHECK(check != None && prop1(check, "_DRIVE_MARK", 0) == 0 &&
	    !strcmp(buf, "retro-wm"), "window manager restarted in place");
	CHECK(managed(c.win) && managed(bar) && parent_of(c.win) != root,
	    "existing clients were re-adopted and re-framed");
	CHECK(prop1(root, "_NET_CURRENT_DESKTOP", 9) == 1 && viewable(c.win),
	    "current workspace survived the restart");
	CHECK(parent_of(tray.win) == bar, "tray icon still embedded");
	shot("11-after-restart");

	printf("[12] shutdown\n");
	kill(tray.pid, SIGTERM);
	waitpid(tray.pid, NULL, 0);
	msleep(200);
	wm_command("quit");
	status = -1;
	WAIT(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) == None, 5000);
	CHECK(prop1(root, "_NET_SUPPORTING_WM_CHECK", None) == None,
	    "quit command shuts the window manager down");
	WAIT(parent_of(c.win) == root, 2000);
	CHECK(parent_of(c.win) == root && viewable(c.win),
	    "clients were handed back to the root window, still mapped");
	kill(c.pid, SIGTERM);
	waitpid(c.pid, NULL, 0);

	kill_kids();
	printf("%d checks, %d failed\n", checks, failures);
	XCloseDisplay(dpy);
	return failures;
}
