/*
 * testclient - minimal well-behaved X11 client used by the session tests
 * (stands in for xterm).  Plain Xlib, no toolkit.
 *
 *   testclient [-title T] [-geometry WxH+X+Y] [-tray] [-transient WID]
 *              [-icon] [-class NAME]
 *
 * -icon sets a 24x24 _NET_WM_ICON (a red square with a green centre);
 * -class sets WM_CLASS to NAME/NAME.
 *
 * Prints "window 0x<id>" once the window exists and exits with status 0
 * when the window manager asks it to close (WM_DELETE_WINDOW).
 */
#if defined(__linux__) || defined(__GLIBC__)
# define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

static void
msleep(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int
main(int argc, char **argv)
{
	const char *title = "testclient";
	const char *cls = NULL;
	int x = 0, y = 0, tray = 0, i, positioned = 0, icon = 0;
	unsigned w = 320, h = 200;
	Window transient = None, win;
	Display *dpy;
	Atom wm_protocols, wm_delete, net_name, utf8;
	XSetWindowAttributes swa;
	XSizeHints sh;
	GC gc;
	int scr;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-title") && i + 1 < argc) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "-geometry") && i + 1 < argc) {
			int m = XParseGeometry(argv[++i], &x, &y, &w, &h);

			positioned = (m & XValue) && (m & YValue);
		} else if (!strcmp(argv[i], "-tray")) {
			tray = 1;
		} else if (!strcmp(argv[i], "-transient") && i + 1 < argc) {
			transient = (Window)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-icon")) {
			icon = 1;
		} else if (!strcmp(argv[i], "-class") && i + 1 < argc) {
			cls = argv[++i];
		} else {
			fprintf(stderr, "usage: testclient [-title T] [-geometry G] "
			    "[-tray] [-transient WID] [-icon] [-class NAME]\n");
			return 2;
		}
	}
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "testclient: cannot open display\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	swa.background_pixel = tray ? 0xd04040 : 0xe0e0d0;
	swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
	    ButtonPressMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, w, h, 0,
	    CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask,
	    &swa);
	gc = XCreateGC(dpy, win, 0, NULL);
	wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	XStoreName(dpy, win, title);
	XChangeProperty(dpy, win, net_name, utf8, 8, PropModeReplace,
	    (const unsigned char *)title, (int)strlen(title));
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	memset(&sh, 0, sizeof sh);
	sh.flags = PSize | PMinSize;
	sh.min_width = 80;
	sh.min_height = 40;
	if (positioned)
		sh.flags |= USPosition;
	XSetWMNormalHints(dpy, win, &sh);
	if (transient != None)
		XSetTransientForHint(dpy, win, transient);
	if (cls) {
		XClassHint ch;

		ch.res_name = (char *)cls;
		ch.res_class = (char *)cls;
		XSetClassHint(dpy, win, &ch);
	}
	if (icon) {
		static unsigned long v[2 + 24 * 24];
		int k;

		v[0] = v[1] = 24;
		for (k = 0; k < 24 * 24; k++) {
			int px = k % 24, py = k / 24;

			v[2 + k] = px >= 8 && px < 16 && py >= 8 && py < 16 ?
			    0xff00c000UL : 0xffd02020UL;
		}
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False),
		    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)v,
		    2 + 24 * 24);
	}

	if (tray) {
		char name[40];
		Atom sel, opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
		Window owner = None;
		XEvent ev;

		snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", scr);
		sel = XInternAtom(dpy, name, False);
		for (i = 0; i < 100 && owner == None; i++) {
			owner = XGetSelectionOwner(dpy, sel);
			if (owner == None)
				msleep(50);
		}
		if (owner == None) {
			fprintf(stderr, "testclient: no system tray\n");
			return 1;
		}
		memset(&ev, 0, sizeof ev);
		ev.xclient.type = ClientMessage;
		ev.xclient.window = owner;
		ev.xclient.message_type = opcode;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = CurrentTime;
		ev.xclient.data.l[1] = 0;          /* SYSTEM_TRAY_REQUEST_DOCK */
		ev.xclient.data.l[2] = (long)win;
		XSendEvent(dpy, owner, False, NoEventMask, &ev);
	} else {
		XMapWindow(dpy, win);
	}
	printf("window 0x%lx\n", (unsigned long)win);
	fflush(stdout);

	for (;;) {
		XEvent ev;

		XNextEvent(dpy, &ev);
		if (ev.type == Expose && ev.xexpose.count == 0) {
			XWindowAttributes wa;

			XGetWindowAttributes(dpy, win, &wa);
			XClearWindow(dpy, win);
			XSetForeground(dpy, gc, 0x202020);
			XDrawRectangle(dpy, win, gc, 4, 4, (unsigned)wa.width - 9,
			    (unsigned)wa.height - 9);
			XDrawString(dpy, win, gc, 12, 24, title, (int)strlen(title));
		} else if (ev.type == ClientMessage &&
		    ev.xclient.message_type == wm_protocols &&
		    (Atom)ev.xclient.data.l[0] == wm_delete) {
			break;
		} else if (ev.type == DestroyNotify && ev.xdestroywindow.window == win) {
			XCloseDisplay(dpy);
			return 3;
		}
	}
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
