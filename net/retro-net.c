/*
 * retro-net - Wi-Fi network switcher popup.
 *
 * GUI by default; `retro-net -t` prints status and networks to the terminal,
 * `retro-net -t connect SSID [PASSPHRASE]` / `retro-net -t disconnect` act
 * from the command line.  Uses the network modules chosen in config.h.
 */
#include "../lib/retroui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_NETS 64
#define WIN_W 400
#define WIN_H 380

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty, busy;
	const RuiNetModule *backend;
	char iface[32], priv[64], forced[32];
	RuiNetStatus st;
	RuiNetwork nets[MAX_NETS];
	int nnets;
	char *rows[MAX_NETS];
	char rowbuf[MAX_NETS][160];
	RuiList list;
	RuiInput pass;
	RuiButton b_scan, b_conn, b_disc, b_close;
	char msg[256];
} net;

static void start_status(void);
static void start_scan(void);

static void
net_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "net_backend"))
		rui_strlcpy(net.forced, val, sizeof net.forced);
	else if (!strcmp(key, "net_priv"))
		rui_strlcpy(net.priv, val, sizeof net.priv);
}

static void
set_msg(const char *m)
{
	rui_strlcpy(net.msg, m, sizeof net.msg);
	net.dirty = 1;
}

static void
build_rows(void)
{
	int i;

	for (i = 0; i < net.nnets; i++) {
		RuiNetwork *n = &net.nets[i];
		char sig[16] = "";
		char row[160];

		if (n->signal >= 0)
			snprintf(sig, sizeof sig, "  %d%%", n->signal);
		snprintf(row, sizeof row, "%s%s%s%s%s",
		    n->connected ? "* " : "", n->ssid, sig,
		    n->secure ? "  (secured)" : "  (open)",
		    n->connected ? "  - connected" : "");
		rui_strlcpy(net.rowbuf[i], row, sizeof net.rowbuf[i]);
		net.rows[i] = net.rowbuf[i];
	}
	net.list.items = net.rows;
	net.list.n = net.nnets;
	if (net.list.sel >= net.nnets)
		net.list.sel = -1;
	net.dirty = 1;
}

static void
mark_connected(void)
{
	int i;

	for (i = 0; i < net.nnets; i++)
		net.nets[i].connected = net.st.connected &&
		    !strcmp(net.nets[i].ssid, net.st.ssid);
	build_rows();
}

/* ------------------------------------------------------------ async steps */

static void
on_scan(const char *out, int status, void *ud)
{
	char buf[128];

	(void)ud;
	(void)status;
	net.busy = 0;
	net.nnets = rui_net_parse_scan(net.backend, out, net.nets, MAX_NETS);
	mark_connected();
	snprintf(buf, sizeof buf, "%d network%s found on %s.", net.nnets,
	    net.nnets == 1 ? "" : "s", net.iface[0] ? net.iface : "default device");
	set_msg(buf);
}

static void
start_scan(void)
{
	char *cmd;

	if (net.busy || !net.backend)
		return;
	cmd = rui_net_scan_cmd(net.backend, net.iface);
	if (!cmd) {
		set_msg("No wireless device found.");
		return;
	}
	if (rui_job_start(cmd, on_scan, NULL) == 0) {
		net.busy = 1;
		set_msg("Scanning...");
	}
	free(cmd);
}

static void
on_status(const char *out, int status, void *ud)
{
	int then_scan = ud != NULL;

	net.busy = 0;
	if (status == 0)
		rui_net_parse_status(net.backend, out, &net.st);
	mark_connected();
	if (then_scan)
		start_scan();
	net.dirty = 1;
}

static void
start_status_ex(int then_scan)
{
	static int token;
	char *cmd;

	if (net.busy)
		return;
	cmd = rui_net_status_cmd(net.backend, net.iface);
	if (!cmd) {
		set_msg("No wireless device found.");
		return;
	}
	if (rui_job_start(cmd, on_status, then_scan ? &token : NULL) == 0)
		net.busy = 1;
	free(cmd);
}

static void
start_status(void)
{
	start_status_ex(0);
}

static void
on_iface(const char *out, int status, void *ud)
{
	(void)ud;
	net.busy = 0;
	if (status != 0 || !rui_net_parse_iface(net.backend, out, net.iface,
	    sizeof net.iface)) {
		set_msg("No wireless device found.");
		return;
	}
	start_status_ex(1);
}

static void
on_action(const char *out, int status, void *ud)
{
	char buf[256], first[160];

	(void)ud;
	net.busy = 0;
	rui_strlcpy(first, out ? out : "", sizeof first);
	rui_strip_ansi(first);
	first[strcspn(first, "\n")] = '\0';
	if (status == 0)
		snprintf(buf, sizeof buf, "Done.");
	else
		snprintf(buf, sizeof buf, "Failed: %s", first[0] ? first :
		    "command returned an error");
	set_msg(buf);
	start_status();
}

static void
do_connect(void)
{
	char *cmd;
	RuiNetwork *n;

	if (net.busy || net.list.sel < 0 || net.list.sel >= net.nnets)
		return;
	n = &net.nets[net.list.sel];
	if (n->secure && !net.pass.text[0] && !net.backend->pass_optional) {
		set_msg("This network needs a passphrase.");
		return;
	}
	cmd = rui_net_connect_cmd(net.backend, net.iface, n->ssid, net.pass.text,
	    net.priv);
	if (!cmd)
		return;
	if (rui_job_start(cmd, on_action, NULL) == 0) {
		char buf[128];

		net.busy = 1;
		snprintf(buf, sizeof buf, "Connecting to %s...", n->ssid);
		set_msg(buf);
	}
	free(cmd);
}

static void
do_disconnect(void)
{
	char *cmd;

	if (net.busy || !net.backend)
		return;
	cmd = rui_net_disconnect_cmd(net.backend, net.iface, net.priv);
	if (!cmd)
		return;
	if (rui_job_start(cmd, on_action, NULL) == 0) {
		net.busy = 1;
		set_msg("Disconnecting...");
	}
	free(cmd);
}

/* -------------------------------------------------------------------- GUI */

static void
draw(void)
{
	RuiSurf *s = net.s;
	RuiApp *a = net.app;
	RuiRect r;
	char buf[256], ifname[32] = "";

	rui_fill(s, 0, 0, WIN_W, WIN_H, RUI_C_FACE);
	r.x = 12;
	r.y = 8;
	r.w = WIN_W - 24;
	r.h = a->font->height + 4;
	if (!net.backend)
		snprintf(buf, sizeof buf, "No Wi-Fi tool found (iwctl, wpa_cli).");
	else if (net.st.connected)
		snprintf(buf, sizeof buf, "Connected to %s  [%s via %s]", net.st.ssid,
		    net.iface[0] ? net.iface : "wlan", rui_net_backend_name(net.backend));
	else if (rui_net_link_up(ifname, sizeof ifname))
		snprintf(buf, sizeof buf, "Wi-Fi not connected; link up on %s  [%s]",
		    ifname, rui_net_backend_name(net.backend));
	else
		snprintf(buf, sizeof buf, "Not connected  [%s]",
		    rui_net_backend_name(net.backend));
	rui_text_fit(s, a->bold, r, buf, RUI_C_TEXT, RUI_LEFT);

	rui_list_draw(s, &net.list);
	r.x = 12;
	r.y = net.pass.r.y;
	r.w = 90;
	r.h = net.pass.r.h;
	rui_text_fit(s, a->font, r, "Passphrase:", RUI_C_TEXT, RUI_LEFT);
	rui_input_draw(s, &net.pass);
	net.b_scan.disabled = net.busy || !net.backend;
	net.b_conn.disabled = net.busy || net.list.sel < 0;
	net.b_disc.disabled = net.busy || !net.st.connected;
	rui_button_draw(s, &net.b_scan);
	rui_button_draw(s, &net.b_conn);
	rui_button_draw(s, &net.b_disc);
	rui_button_draw(s, &net.b_close);
	r.x = 8;
	r.y = WIN_H - a->font->height - 12;
	r.w = WIN_W - 16;
	r.h = a->font->height + 8;
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	r.x += 6;
	r.w -= 12;
	rui_text_fit(s, a->font, r, net.msg, RUI_C_TEXT, RUI_LEFT);
	rui_surf_flush(s);
	net.dirty = 0;
}

static void
handle_event(XEvent *ev)
{
	int r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(net.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (ks == XK_Up || ks == XK_Down || ks == XK_Prior || ks == XK_Next) {
			if (net.list.sel < 0 && net.list.n > 0)
				net.list.sel = 0;
			else
				rui_list_key(net.app, &net.list, ks);
			net.dirty = 1;
			break;
		}
		r = rui_input_event(net.app, &net.pass, ev);
		if (r & RUI_EV_ACTIVATE)
			do_connect();
		else if (r & RUI_EV_CANCEL)
			net.running = 0;
		net.dirty |= r != 0;
		break;
	}
	case SelectionNotify:
		if (rui_input_event(net.app, &net.pass, ev))
			net.dirty = 1;
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		r = rui_list_event(net.app, &net.list, ev);
		if (r & RUI_EV_ACTIVATE)
			do_connect();
		net.dirty |= r != 0;
		r = rui_button_event(&net.b_scan, ev);
		if (r & RUI_EV_ACTIVATE)
			start_scan();
		net.dirty |= r != 0;
		r = rui_button_event(&net.b_conn, ev);
		if (r & RUI_EV_ACTIVATE)
			do_connect();
		net.dirty |= r != 0;
		r = rui_button_event(&net.b_disc, ev);
		if (r & RUI_EV_ACTIVATE)
			do_disconnect();
		net.dirty |= r != 0;
		r = rui_button_event(&net.b_close, ev);
		if (r & RUI_EV_ACTIVATE)
			net.running = 0;
		net.dirty |= r != 0;
		if (ev->type == ButtonPress && rui_input_event(net.app, &net.pass, ev))
			net.dirty = 1;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(net.app, ev))
			net.running = 0;
		break;
	default:
		break;
	}
}

static void
setup_gui(void)
{
	RuiApp *a = net.app;
	XSizeHints sh;
	int fh = a->font->height, y, bw = (WIN_W - 24 - 3 * 6) / 4, bx = 12;

	net.list.r.x = 12;
	net.list.r.y = 8 + fh + 10;
	net.list.r.w = WIN_W - 24;
	net.list.r.h = WIN_H - net.list.r.y - (fh + 10) - (fh + 12) - (fh + 12) -
	    30;
	net.list.sel = -1;
	net.list.last_idx = -1;
	y = net.list.r.y + net.list.r.h + 8;
	net.pass.r.x = 104;
	net.pass.r.y = y;
	net.pass.r.w = WIN_W - 116;
	net.pass.r.h = fh + 10;
	net.pass.password = 1;
	net.pass.focused = 1;
	y += fh + 10 + 8;
	net.b_scan.label = "Scan";
	net.b_conn.label = "Connect";
	net.b_disc.label = "Disconnect";
	net.b_close.label = "Close";
	net.b_scan.r.x = bx;
	net.b_conn.r.x = bx + (bw + 6);
	net.b_disc.r.x = bx + 2 * (bw + 6);
	net.b_close.r.x = bx + 3 * (bw + 6);
	net.b_scan.r.y = net.b_conn.r.y = net.b_disc.r.y = net.b_close.r.y = y;
	net.b_scan.r.w = net.b_conn.r.w = net.b_disc.r.w = net.b_close.r.w = bw;
	net.b_scan.r.h = net.b_conn.r.h = net.b_disc.r.h = net.b_close.r.h =
	    fh + 12;

	net.win = rui_window_new(a, a->root, (a->sw - WIN_W) / 2,
	    (a->sh - WIN_H) / 2, WIN_W, WIN_H, 0, ExposureMask | KeyPressMask |
	    ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	net.s = rui_surf_new(a, net.win, WIN_W, WIN_H);
	rui_set_title(a, net.win, "Network", "retro-net");
	rui_set_window_type(a, net.win, A__NET_WM_WINDOW_TYPE_DIALOG);
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = WIN_H;
	XSetWMNormalHints(a->dpy, net.win, &sh);
	rui_input_init(a, &net.pass, net.win);
	XMapWindow(a->dpy, net.win);
}

static void
kick_off(void)
{
	char *cmd;

	if (!net.backend) {
		set_msg("Install iwd or wpa_supplicant to manage Wi-Fi here.");
		return;
	}
	cmd = rui_net_iface_cmd(net.backend);
	if (cmd) {
		if (rui_job_start(cmd, on_iface, NULL) == 0) {
			net.busy = 1;
			set_msg("Looking for wireless devices...");
		}
		free(cmd);
	} else {
		start_status_ex(1);
	}
}

/* -------------------------------------------------------------- text mode */

static char *
capture(const char *cmd, int *status)
{
	FILE *p = popen(cmd, "r");
	size_t cap = 4096, len = 0, r;
	char *buf;
	int st;

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
	st = pclose(p);
	*status = st;
	return buf;
}

static int
text_mode(int argc, char **argv)
{
	char *cmd, *out, *path;
	int status = 0, i, rc = 0;

	path = rui_config_path("retro.conf");
	rui_conf_parse(path, net_conf, NULL);
	free(path);
	net.backend = rui_net_detect(net.forced);
	if (!net.backend) {
		char ifname[32] = "";

		puts("backend: none (no iwctl / wpa_cli / ifconfig wlan support)");
		if (rui_net_link_up(ifname, sizeof ifname))
			printf("link:    up on %s\n", ifname);
		else
			puts("link:    down");
		return argc > 2 ? 1 : 0;
	}
	if ((cmd = rui_net_iface_cmd(net.backend))) {
		out = capture(cmd, &status);
		rui_net_parse_iface(net.backend, out, net.iface, sizeof net.iface);
		free(out);
		free(cmd);
	}
	if (argc > 2 && !strcmp(argv[2], "connect") && argc > 3) {
		cmd = rui_net_connect_cmd(net.backend, net.iface, argv[3],
		    argc > 4 ? argv[4] : "", net.priv);
	} else if (argc > 2 && !strcmp(argv[2], "disconnect")) {
		cmd = rui_net_disconnect_cmd(net.backend, net.iface, net.priv);
	} else if (argc > 2) {
		fprintf(stderr, "usage: retro-net [-t [connect SSID [PASS] | "
		    "disconnect]]\n");
		return 2;
	} else {
		cmd = NULL;
	}
	if (cmd) {
		out = capture(cmd, &status);
		rui_strip_ansi(out);
		fputs(out, stdout);
		rc = status == 0 ? 0 : 1;
		free(out);
		free(cmd);
		return rc;
	}
	printf("backend: %s\n", rui_net_backend_name(net.backend));
	printf("device:  %s\n", net.iface[0] ? net.iface : "(default)");
	if ((cmd = rui_net_status_cmd(net.backend, net.iface))) {
		out = capture(cmd, &status);
		rui_net_parse_status(net.backend, out, &net.st);
		free(out);
		free(cmd);
	}
	printf("status:  %s%s\n", net.st.connected ? "connected to " :
	    "not connected", net.st.connected ? net.st.ssid : "");
	if ((cmd = rui_net_scan_cmd(net.backend, net.iface))) {
		out = capture(cmd, &status);
		net.nnets = rui_net_parse_scan(net.backend, out, net.nets, MAX_NETS);
		free(out);
		free(cmd);
	}
	for (i = 0; i < net.nnets; i++)
		printf("  %c %-32s %4d%%  %s\n",
		    net.st.connected && !strcmp(net.nets[i].ssid, net.st.ssid) ?
		    '*' : ' ', net.nets[i].ssid, net.nets[i].signal,
		    net.nets[i].secure ? "secured" : "open");
	return 0;
}

int
main(int argc, char **argv)
{
	char *path;
	long long t_status;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-net " RUI_VERSION);
			return 0;
		}
		if (!strcmp(argv[1], "-t"))
			return text_mode(argc, argv);
		fprintf(stderr, "usage: retro-net [-v] [-t [connect SSID [PASS] | "
		    "disconnect]]\n");
		return 2;
	}
	rui_signals_init();
	net.app = rui_open("retro-net");
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, net_conf, NULL);
	free(path);
	net.backend = rui_net_detect(net.forced);
	setup_gui();
#ifdef __OpenBSD__
	if (unveil("/", "rx") == -1 || unveil(NULL, NULL) == -1)
		rui_die("unveil failed");
	/* route: getifaddrs(3) finds the wired link */
	if (pledge("stdio rpath proc exec route", NULL) == -1)
		rui_die("pledge failed");
#endif
	kick_off();
	net.running = 1;
	net.dirty = 1;
	t_status = rui_now_ms();
	while (net.running && !rui_quit_requested()) {
		XEvent ev;

		while (net.running && XPending(net.app->dpy)) {
			XNextEvent(net.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		if (rui_now_ms() - t_status > 5000) {
			t_status = rui_now_ms();
			if (!net.busy && net.backend &&
			    (net.iface[0] || !net.backend->iface_cmd))
				start_status();
		}
		if (net.dirty && net.running)
			draw();
		if (net.running)
			rui_wait(net.app, 1000);
	}
	rui_input_destroy(&net.pass);
	rui_surf_free(net.s);
	XDestroyWindow(net.app->dpy, net.win);
	rui_close(net.app);
	return 0;
}
