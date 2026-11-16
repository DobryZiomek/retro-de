/*
 * retro-bt - Bluetooth devices: switch the adapter on and off, scan, pair,
 * connect, disconnect and forget.
 *
 * GUI by default; `retro-bt -t` prints the adapter and the known devices,
 * `retro-bt -t on|off|scan`, `retro-bt -t connect|disconnect|forget ADDR`
 * act from the command line.  Uses the Bluetooth modules chosen in
 * config.h (BlueZ through bluetoothctl).
 */
#include "../lib/retroui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_DEVS 64
#define WIN_W 430
#define WIN_H 390

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty, busy;
	const RuiBtModule *mod;
	char forced[32];
	RuiBtStatus st;
	RuiBtDevice devs[MAX_DEVS];
	int ndevs;
	char *rows[MAX_DEVS];
	char rowbuf[MAX_DEVS][160];
	char sel_addr[24];                 /* keeps the selection over refreshes */
	RuiList list;
	RuiButton b_power, b_scan, b_conn, b_disc, b_forget, b_close;
	char msg[256];
} bt;

static void start_list(int scan);

static void
bt_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "bluetooth_backend"))
		rui_strlcpy(bt.forced, val, sizeof bt.forced);
}

static void
set_msg(const char *m)
{
	rui_strlcpy(bt.msg, m, sizeof bt.msg);
	bt.dirty = 1;
}

static RuiBtDevice *
selected(void)
{
	return bt.list.sel >= 0 && bt.list.sel < bt.ndevs ?
	    &bt.devs[bt.list.sel] : NULL;
}

static void
build_rows(void)
{
	int i;

	bt.list.sel = -1;
	for (i = 0; i < bt.ndevs; i++) {
		const RuiBtDevice *d = &bt.devs[i];

		snprintf(bt.rowbuf[i], sizeof bt.rowbuf[i], "%s%s%s",
		    d->connected ? "* " : "", d->name,
		    d->connected ? "  - connected" : d->paired ? "  (paired)" :
		    "  (new)");
		bt.rows[i] = bt.rowbuf[i];
		if (!strcmp(d->addr, bt.sel_addr))
			bt.list.sel = i;
	}
	bt.list.items = bt.rows;
	bt.list.n = bt.ndevs;
	bt.dirty = 1;
}

/* ------------------------------------------------------------ async steps */

static void
on_list(const char *out, int status, void *ud)
{
	char buf[128];
	int scanned = ud != NULL;

	(void)status;
	bt.busy = 0;
	bt.ndevs = rui_bt_parse_devices(bt.mod, out, bt.devs, MAX_DEVS);
	build_rows();
	if (scanned) {
		snprintf(buf, sizeof buf, "%d device%s in range or known.", bt.ndevs,
		    bt.ndevs == 1 ? "" : "s");
		set_msg(buf);
	}
}

static void
start_list(int scan)
{
	static int token;

	if (bt.busy || !bt.mod)
		return;
	if (scan && !bt.st.powered) {
		set_msg("Bluetooth is switched off.");
		return;
	}
	if (rui_job_start(scan ? bt.mod->scan_cmd : bt.mod->list_cmd, on_list,
	    scan ? &token : NULL) == 0) {
		bt.busy = 1;
		if (scan)
			set_msg("Scanning for devices (about 8 seconds)...");
	}
}

static void
on_status(const char *out, int status, void *ud)
{
	int then_list = ud != NULL;

	(void)status;
	bt.busy = 0;
	rui_bt_parse_status(bt.mod, out, &bt.st);
	if (!bt.st.present)
		set_msg("No Bluetooth adapter found (is bluetoothd running?).");
	bt.dirty = 1;
	if (then_list && bt.st.present)
		start_list(0);
}

static void
start_status(int then_list)
{
	static int token;

	if (bt.busy || !bt.mod)
		return;
	if (rui_job_start(bt.mod->status_cmd, on_status, then_list ? &token :
	    NULL) == 0)
		bt.busy = 1;
}

static void
on_action(const char *out, int status, void *ud)
{
	char buf[256], last[160] = "", line[160];
	const char *p = out ? out : "";

	(void)ud;
	bt.busy = 0;
	/* bluetoothctl's verdict is on its last line */
	while (rui_next_line(&p, line, sizeof line)) {
		rui_strip_ansi(line);
		if (*rui_ltrim(line))
			rui_strlcpy(last, rui_ltrim(line), sizeof last);
	}
	if (status == 0)
		snprintf(buf, sizeof buf, "Done.");
	else
		snprintf(buf, sizeof buf, "Failed: %s", last[0] ? last :
		    "the command returned an error");
	set_msg(buf);
	start_status(1);
}

static void
run_action(char *cmd, const char *what)
{
	if (!cmd)
		return;
	if (!bt.busy && rui_job_start(cmd, on_action, NULL) == 0) {
		bt.busy = 1;
		set_msg(what);
	}
	free(cmd);
}

static void
do_power(void)
{
	if (!bt.mod || !bt.st.present)
		return;
	run_action(rui_strdup(bt.mod->power_cmd[!bt.st.powered]),
	    bt.st.powered ? "Switching Bluetooth off..." :
	    "Switching Bluetooth on...");
}

static void
do_connect(void)
{
	RuiBtDevice *d = selected();
	char buf[160];

	if (!d || !bt.mod)
		return;
	snprintf(buf, sizeof buf, d->paired ? "Connecting to %s..." :
	    "Pairing with %s (confirm on the device if it asks)...", d->name);
	run_action(bt.mod->connect_cmd(d->addr, d->paired), buf);
}

static void
do_disconnect(void)
{
	RuiBtDevice *d = selected();

	if (d && bt.mod)
		run_action(bt.mod->disconnect_cmd(d->addr), "Disconnecting...");
}

static void
do_forget(void)
{
	RuiBtDevice *d = selected();
	char text[160];

	if (!d || !bt.mod)
		return;
	snprintf(text, sizeof text, "Forget %.100s?", d->name);
	if (d->paired && !rui_confirm(bt.app, "Bluetooth", text, "Forget"))
		return;
	run_action(bt.mod->forget_cmd(d->addr), "Forgetting the device...");
}

/* -------------------------------------------------------------------- GUI */

static void
draw(void)
{
	RuiSurf *s = bt.s;
	RuiApp *a = bt.app;
	RuiBtDevice *d = selected();
	RuiRect r;
	char buf[256];

	rui_fill(s, 0, 0, WIN_W, WIN_H, RUI_C_FACE);
	r.x = 12;
	r.y = bt.b_power.r.y;
	r.w = bt.b_power.r.x - 20;
	r.h = bt.b_power.r.h;
	if (!bt.mod)
		snprintf(buf, sizeof buf, "No Bluetooth tool found (bluetoothctl).");
	else if (!bt.st.present)
		snprintf(buf, sizeof buf, "No Bluetooth adapter  [%s]", bt.mod->name);
	else if (!bt.st.powered)
		snprintf(buf, sizeof buf, "Bluetooth is off  [%s]", bt.mod->name);
	else if (bt.st.nconnected)
		snprintf(buf, sizeof buf, "Connected to %s%s  [%s]", bt.st.device,
		    bt.st.nconnected > 1 ? " and more" : "", bt.mod->name);
	else
		snprintf(buf, sizeof buf, "Bluetooth is on  [%s]", bt.mod->name);
	rui_text_fit(s, a->bold, r, buf, RUI_C_TEXT, RUI_LEFT);
	bt.b_power.label = bt.st.powered ? "Turn Off" : "Turn On";
	bt.b_power.disabled = bt.busy || !bt.st.present;
	rui_button_draw(s, &bt.b_power);

	rui_list_draw(s, &bt.list);
	bt.b_scan.disabled = bt.busy || !bt.st.powered;
	bt.b_conn.disabled = bt.busy || !d || d->connected || !bt.st.powered;
	bt.b_conn.label = d && !d->paired ? "Pair" : "Connect";
	bt.b_disc.disabled = bt.busy || !d || !d->connected;
	bt.b_forget.disabled = bt.busy || !d;
	rui_button_draw(s, &bt.b_scan);
	rui_button_draw(s, &bt.b_conn);
	rui_button_draw(s, &bt.b_disc);
	rui_button_draw(s, &bt.b_forget);
	rui_button_draw(s, &bt.b_close);
	r.x = 8;
	r.y = WIN_H - a->font->height - 12;
	r.w = WIN_W - 16;
	r.h = a->font->height + 8;
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	r.x += 6;
	r.w -= 12;
	rui_text_fit(s, a->font, r, bt.msg, RUI_C_TEXT, RUI_LEFT);
	rui_surf_flush(s);
	bt.dirty = 0;
}

static void
remember_selection(void)
{
	RuiBtDevice *d = selected();

	rui_strlcpy(bt.sel_addr, d ? d->addr : "", sizeof bt.sel_addr);
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(bt.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (ks == XK_Escape) {
			bt.running = 0;
		} else if (ks == XK_Return) {
			do_connect();
		} else if (ks == XK_Up || ks == XK_Down || ks == XK_Prior ||
		    ks == XK_Next) {
			if (bt.list.sel < 0 && bt.list.n > 0)
				bt.list.sel = 0;
			else
				rui_list_key(bt.app, &bt.list, ks);
			remember_selection();
			bt.dirty = 1;
		}
		break;
	}
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		r = rui_list_event(bt.app, &bt.list, ev);
		if (r)
			remember_selection();
		if (r & RUI_EV_ACTIVATE)
			do_connect();
		bt.dirty |= r != 0;
		if (rui_button_event(&bt.b_power, ev) & RUI_EV_ACTIVATE)
			do_power();
		if (rui_button_event(&bt.b_scan, ev) & RUI_EV_ACTIVATE)
			start_list(1);
		if (rui_button_event(&bt.b_conn, ev) & RUI_EV_ACTIVATE)
			do_connect();
		if (rui_button_event(&bt.b_disc, ev) & RUI_EV_ACTIVATE)
			do_disconnect();
		if (rui_button_event(&bt.b_forget, ev) & RUI_EV_ACTIVATE)
			do_forget();
		if (rui_button_event(&bt.b_close, ev) & RUI_EV_ACTIVATE)
			bt.running = 0;
		if (ev->type != MotionNotify)
			bt.dirty = 1;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(bt.app, ev))
			bt.running = 0;
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
setup_gui(void)
{
	RuiApp *a = bt.app;
	RuiMonitor mon = rui_monitor_pick(a, "primary");
	RuiButton *row[5];
	XSizeHints sh;
	int fh = a->font->height, bh = fh + 12, i, bw = (WIN_W - 24 - 4 * 6) / 5;

	bt.b_power.r.w = 96;
	bt.b_power.r.h = bh;
	bt.b_power.r.x = WIN_W - 12 - bt.b_power.r.w;
	bt.b_power.r.y = 8;
	bt.list.r.x = 12;
	bt.list.r.y = 8 + bh + 8;
	bt.list.r.w = WIN_W - 24;
	bt.list.r.h = WIN_H - bt.list.r.y - bh - fh - 34;
	bt.list.sel = bt.list.last_idx = -1;
	row[0] = &bt.b_scan;
	row[1] = &bt.b_conn;
	row[2] = &bt.b_disc;
	row[3] = &bt.b_forget;
	row[4] = &bt.b_close;
	bt.b_scan.label = "Scan";
	bt.b_conn.label = "Connect";
	bt.b_disc.label = "Disconnect";
	bt.b_forget.label = "Forget";
	bt.b_close.label = "Close";
	for (i = 0; i < 5; i++) {
		row[i]->r.x = 12 + i * (bw + 6);
		row[i]->r.y = bt.list.r.y + bt.list.r.h + 8;
		row[i]->r.w = bw;
		row[i]->r.h = bh;
	}
	bt.win = rui_window_new(a, a->root, mon.x + (mon.w - WIN_W) / 2,
	    mon.y + (mon.h - WIN_H) / 2, WIN_W, WIN_H, 0, ExposureMask |
	    KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	bt.s = rui_surf_new(a, bt.win, WIN_W, WIN_H);
	rui_set_title(a, bt.win, "Bluetooth", "retro-bt");
	rui_set_window_type(a, bt.win, A__NET_WM_WINDOW_TYPE_DIALOG);
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = WIN_H;
	XSetWMNormalHints(a->dpy, bt.win, &sh);
	a->dispatch = dispatch_cb;         /* for the "Forget?" question */
	XMapWindow(a->dpy, bt.win);
}

/* -------------------------------------------------------------- text mode */

static char *
capture(const char *cmd, int *status)
{
	FILE *p = popen(cmd, "r");
	size_t cap = 4096, len = 0, r;
	char *buf;

	if (!p) {
		*status = -1;
		return rui_strdup("");
	}
	buf = rui_malloc(cap);
	while ((r = fread(buf + len, 1, cap - len - 1, p)) > 0) {
		len += r;
		if (len + 1 >= cap) {
			cap *= 2;
			buf = rui_realloc(buf, cap);
		}
	}
	buf[len] = '\0';
	*status = pclose(p);
	return buf;
}

static int
text_mode(int argc, char **argv)
{
	const char *verb = argc > 2 ? argv[2] : "";
	char *path = rui_config_path("retro.conf"), *out, *cmd = NULL;
	int status = 0, i;

	rui_conf_parse(path, bt_conf, NULL);
	free(path);
	if (!(bt.mod = rui_bt_detect(bt.forced))) {
		puts("backend: none (bluetoothctl is not installed)");
		return argc > 2 ? 1 : 0;
	}
	if (!strcmp(verb, "on") || !strcmp(verb, "off"))
		cmd = rui_strdup(bt.mod->power_cmd[!strcmp(verb, "on")]);
	else if (!strcmp(verb, "connect") && argc > 3)
		cmd = bt.mod->connect_cmd(argv[3], 1);
	else if (!strcmp(verb, "pair") && argc > 3)
		cmd = bt.mod->connect_cmd(argv[3], 0);
	else if (!strcmp(verb, "disconnect") && argc > 3)
		cmd = bt.mod->disconnect_cmd(argv[3]);
	else if (!strcmp(verb, "forget") && argc > 3)
		cmd = bt.mod->forget_cmd(argv[3]);
	else if (*verb && strcmp(verb, "scan")) {
		fprintf(stderr, "usage: retro-bt [-t [on | off | scan | pair ADDR | "
		    "connect ADDR | disconnect ADDR | forget ADDR]]\n");
		return 2;
	}
	if (cmd) {
		out = capture(cmd, &status);
		rui_strip_ansi(out);
		fputs(out, stdout);
		free(out);
		free(cmd);
		return status == 0 ? 0 : 1;
	}
	out = capture(bt.mod->status_cmd, &status);
	rui_bt_parse_status(bt.mod, out, &bt.st);
	free(out);
	printf("backend: %s\n", bt.mod->name);
	printf("adapter: %s\n", !bt.st.present ? "none" : bt.st.powered ? "on" :
	    "off");
	if (!bt.st.present)
		return 0;
	out = capture(*verb ? bt.mod->scan_cmd : bt.mod->list_cmd, &status);
	bt.ndevs = rui_bt_parse_devices(bt.mod, out, bt.devs, MAX_DEVS);
	free(out);
	for (i = 0; i < bt.ndevs; i++)
		printf("  %c %s  %-28s %s\n", bt.devs[i].connected ? '*' : ' ',
		    bt.devs[i].addr, bt.devs[i].name, bt.devs[i].connected ?
		    "connected" : bt.devs[i].paired ? "paired" : "new");
	return 0;
}

int
main(int argc, char **argv)
{
	char *path;
	long long t_status;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-bt " RUI_VERSION);
			return 0;
		}
		if (!strcmp(argv[1], "-t"))
			return text_mode(argc, argv);
		fprintf(stderr, "usage: retro-bt [-v] [-t [on | off | scan | pair "
		    "ADDR | connect ADDR | disconnect ADDR | forget ADDR]]\n");
		return 2;
	}
	rui_signals_init();
	bt.app = rui_open("retro-bt");
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, bt_conf, NULL);
	free(path);
	bt.mod = rui_bt_detect(bt.forced);
	setup_gui();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == -1)
		rui_die("pledge failed");
#endif
	if (bt.mod)
		start_status(1);
	else
		set_msg("Install BlueZ (bluetoothctl) to manage Bluetooth here.");
	bt.running = 1;
	bt.dirty = 1;
	t_status = rui_now_ms();
	while (bt.running && !rui_quit_requested()) {
		XEvent ev;

		while (bt.running && XPending(bt.app->dpy)) {
			XNextEvent(bt.app->dpy, &ev);
			handle_event(&ev);
		}
		if (rui_now_ms() - t_status > 6000) {
			t_status = rui_now_ms();
			start_status(1);               /* follows what devices do */
		}
		if (bt.dirty && bt.running)
			draw();
		if (bt.running)
			rui_wait(bt.app, 1000);
	}
	rui_surf_free(bt.s);
	XDestroyWindow(bt.app->dpy, bt.win);
	rui_close(bt.app);
	return 0;
}
