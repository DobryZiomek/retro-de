/*
 * retro-bar - panel: start menu (menu.conf + .desktop applications), pager,
 * layout symbol, task buttons, XEMBED system tray and an ordered row of
 * applets.  Applets are built in (cpu mem battery net bluetooth volume
 * clock) or
 * "blocks": dwmblocks-style shell commands from config.h / retro.conf.
 * Battery, network and volume go through the modules chosen in config.h.
 * With the CDE style the same data drives the front panel (frontpanel.c).
 */
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

Bar bar;

const char *const applet_names[] = {
	"cpu", "mem", "battery", "net", "volume", "clock", "bluetooth", "dnd",
	"awake", "notes"
};

static void handle_event(XEvent *ev);
static int reload_requested;
/* retro.conf panel_style: 0 taskbar (the default), 1 front panel, -1 "auto"
 * (the front panel with the CDE style, the taskbar with Classic) */
static int panel_style;

/* ------------------------------------------------------------------ tasks */

static void
task_read(Task *t)
{
	unsigned long *v;
	int n, i;

	free(t->title);
	t->title = rui_window_title(bar.app, t->win);
	n = rui_get_prop32(bar.app, t->win, A(_NET_WM_DESKTOP), &v);
	t->desk = n > 0 ? v[0] : bar.cur;
	free(v);
	t->hidden = t->skip = t->urgent = 0;
	n = rui_get_prop32(bar.app, t->win, A(_NET_WM_STATE), &v);
	for (i = 0; i < n; i++) {
		if (v[i] == A(_NET_WM_STATE_HIDDEN))
			t->hidden = 1;
		else if (v[i] == A(_NET_WM_STATE_SKIP_TASKBAR))
			t->skip = 1;
		else if (v[i] == A(_NET_WM_STATE_DEMANDS_ATTENTION))
			t->urgent = 1;
	}
	free(v);
	n = rui_get_prop32(bar.app, t->win, A(_NET_WM_WINDOW_TYPE), &v);
	for (i = 0; i < n; i++)
		if (v[i] == A(_NET_WM_WINDOW_TYPE_DOCK) ||
		    v[i] == A(_NET_WM_WINDOW_TYPE_DESKTOP))
			t->skip = 1;
	free(v);
}

/* the window's own icon (_NET_WM_ICON), blended onto the button face;
 * else the one learned for its class; also learns it for the Programs
 * window and the desktop */
static void
task_icon(Task *t)
{
	unsigned char *rgba;
	XColor face;

	image_free(t->icon);
	t->icon = NULL;
	if ((rgba = rui_netwm_icon(bar.app, t->win, TASK_ICON))) {
		Image *img = rui_calloc(1, sizeof *img);
		int i, n = TASK_ICON * TASK_ICON;

		face.pixel = bar.app->color[RUI_C_FACE].pixel;
		face.red = bar.app->color[RUI_C_FACE].color.red;
		face.green = bar.app->color[RUI_C_FACE].color.green;
		face.blue = bar.app->color[RUI_C_FACE].color.blue;
		img->w = img->h = TASK_ICON;
		img->rgb = rui_malloc((size_t)n * 3);
		img->alpha = rui_malloc((size_t)n);
		for (i = 0; i < n; i++) {
			const unsigned char *p = rgba + i * 4;
			unsigned a = p[3];

			img->rgb[i * 3] = (unsigned char)((p[0] * a +
			    (face.red >> 8) * (255 - a)) / 255);
			img->rgb[i * 3 + 1] = (unsigned char)((p[1] * a +
			    (face.green >> 8) * (255 - a)) / 255);
			img->rgb[i * 3 + 2] = (unsigned char)((p[2] * a +
			    (face.blue >> 8) * (255 - a)) / 255);
			img->alpha[i] = a >= 40 ? 255 : 0;
		}
		free(rgba);
		t->icon = img;
		rui_icon_learn(bar.app, t->win);
	} else {
		XClassHint ch;
		char *path = NULL;

		memset(&ch, 0, sizeof ch);
		if (XGetClassHint(bar.dpy, t->win, &ch)) {
			path = rui_icon_find(ch.res_name);
			if (!path)
				path = rui_icon_find(ch.res_class);
			if (ch.res_name)
				XFree(ch.res_name);
			if (ch.res_class)
				XFree(ch.res_class);
		}
		if (path)
			t->icon = image_load_size(bar.app, path, TASK_ICON);
		free(path);
	}
}

static Task *
task_find(Window w)
{
	int i;

	for (i = 0; i < bar.ntasks; i++)
		if (bar.tasks[i].win == w)
			return &bar.tasks[i];
	return NULL;
}

static void
tasks_refresh(void)
{
	unsigned long *list;
	int n = rui_get_prop32(bar.app, bar.app->root, A(_NET_CLIENT_LIST), &list);
	Task *nt = rui_calloc((size_t)n + 1, sizeof *nt);
	int i, k = 0;

	for (i = 0; i < n; i++) {
		Task *old;

		if (list[i] == bar.win)
			continue;
		old = task_find(list[i]);
		if (old) {
			nt[k] = *old;
			old->title = NULL;
			old->icon = NULL;
			old->win = None;
		} else {
			nt[k].win = list[i];
			XSelectInput(bar.dpy, list[i], PropertyChangeMask);
			task_read(&nt[k]);
			task_icon(&nt[k]);
		}
		k++;
	}
	for (i = 0; i < bar.ntasks; i++) {
		free(bar.tasks[i].title);
		image_free(bar.tasks[i].icon);
	}
	free(bar.tasks);
	free(list);
	bar.tasks = nt;
	bar.ntasks = k;
	bar.dirty = 1;
}

static void
layout_refresh(void)
{
	char *sym = rui_get_text_prop(bar.app, bar.app->root, A(_RETRO_LAYOUT));

	rui_strlcpy(bar.layout_sym, sym ? sym : "", sizeof bar.layout_sym);
	free(sym);
	bar.dirty = 1;
}

/* _NET_DESKTOP_NAMES: NUL-separated UTF-8 strings */
static void
desk_names_refresh(void)
{
	Atom type;
	int fmt, i;
	unsigned long n = 0, after, off = 0;
	unsigned char *data = NULL;

	for (i = 0; i < 32; i++)
		snprintf(bar.desk_names[i], sizeof bar.desk_names[i], "%d", i + 1);
	if (XGetWindowProperty(bar.dpy, bar.app->root, A(_NET_DESKTOP_NAMES), 0,
	    1024, False, AnyPropertyType, &type, &fmt, &n, &after, &data) !=
	    Success || !data)
		return;
	for (i = 0; i < 32 && fmt == 8 && off < n; i++) {
		const char *nm = (const char *)data + off;
		size_t len = strnlen(nm, n - off);

		if (len)
			snprintf(bar.desk_names[i], sizeof bar.desk_names[i], "%.*s",
			    (int)len, nm);
		off += len + 1;
	}
	XFree(data);
}

static void
root_props_refresh(void)
{
	unsigned long *v;

	if (rui_get_prop32(bar.app, bar.app->root, A(_NET_CURRENT_DESKTOP), &v) > 0)
		bar.cur = v[0];
	free(v);
	if (rui_get_prop32(bar.app, bar.app->root, A(_NET_NUMBER_OF_DESKTOPS),
	    &v) > 0 && v[0] > 0 && v[0] <= 32)
		bar.ndesk = v[0];
	free(v);
	bar.active = None;
	if (rui_get_prop32(bar.app, bar.app->root, A(_NET_ACTIVE_WINDOW), &v) > 0)
		bar.active = v[0];
	free(v);
	bar.dnd = bar.awake = 0;
	if (rui_get_prop32(bar.app, bar.app->root, A(_RETRO_DND), &v) > 0)
		bar.dnd = v[0] != 0;
	free(v);
	if (rui_get_prop32(bar.app, bar.app->root, A(_RETRO_AWAKE), &v) > 0)
		bar.awake = v[0] != 0;
	free(v);
	desk_names_refresh();
	bar.dirty = 1;
}

/* ------------------------------------------------------------ system tray */

static int
tray_size(void)
{
	return bar.fp ? bar.app->font->height + 8 : bar.h - 8;
}

void
tray_layout(void)
{
	int i, sz = tray_size(), y = bar.r_tray.y + (bar.r_tray.h - sz) / 2;

	for (i = 0; i < bar.nicons; i++)
		XMoveResizeWindow(bar.dpy, bar.icons[i], bar.r_tray.x + 2 +
		    i * (sz + 2), y, (unsigned)sz, (unsigned)sz);
}

static void
tray_init(void)
{
	char name[40];
	unsigned long orient = 0;
	XEvent ev;

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", bar.app->screen);
	bar.tray_atom = XInternAtom(bar.dpy, name, False);
	if (XGetSelectionOwner(bar.dpy, bar.tray_atom) != None) {
		rui_warn("another system tray is running; tray disabled");
		return;
	}
	XSetSelectionOwner(bar.dpy, bar.tray_atom, bar.win, CurrentTime);
	if (XGetSelectionOwner(bar.dpy, bar.tray_atom) != bar.win)
		return;
	bar.tray_ok = 1;
	rui_set_prop32(bar.app, bar.win, A(_NET_SYSTEM_TRAY_ORIENTATION),
	    XA_CARDINAL, &orient, 1);
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = bar.app->root;
	ev.xclient.message_type = A(MANAGER);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = (long)bar.tray_atom;
	ev.xclient.data.l[2] = (long)bar.win;
	XSendEvent(bar.dpy, bar.app->root, False, StructureNotifyMask, &ev);
}

static void
tray_add(Window w)
{
	XEvent ev;
	int i, sz = tray_size();

	if (!bar.tray_ok || w == None || bar.nicons >= MAX_ICONS)
		return;
	for (i = 0; i < bar.nicons; i++)
		if (bar.icons[i] == w)
			return;
	XSelectInput(bar.dpy, w, StructureNotifyMask);
	XAddToSaveSet(bar.dpy, w);
	XReparentWindow(bar.dpy, w, bar.win, bar.r_tray.x + 2 +
	    bar.nicons * (sz + 2), bar.r_tray.y);
	XResizeWindow(bar.dpy, w, (unsigned)sz, (unsigned)sz);
	XMapRaised(bar.dpy, w);
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = A(_XEMBED);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = 0;              /* XEMBED_EMBEDDED_NOTIFY */
	ev.xclient.data.l[3] = (long)bar.win;
	ev.xclient.data.l[4] = 0;              /* protocol version */
	XSendEvent(bar.dpy, w, False, NoEventMask, &ev);
	bar.icons[bar.nicons++] = w;
	bar.dirty = 1;
}

static int
tray_remove(Window w)
{
	int i;

	for (i = 0; i < bar.nicons; i++) {
		if (bar.icons[i] == w) {
			memmove(&bar.icons[i], &bar.icons[i + 1],
			    (size_t)(bar.nicons - i - 1) * sizeof bar.icons[0]);
			bar.nicons--;
			bar.dirty = 1;
			return 1;
		}
	}
	return 0;
}

static void
tray_shutdown(void)
{
	int i;

	for (i = 0; i < bar.nicons; i++) {
		XSelectInput(bar.dpy, bar.icons[i], NoEventMask);
		XUnmapWindow(bar.dpy, bar.icons[i]);
		XReparentWindow(bar.dpy, bar.icons[i], bar.app->root, 0, 0);
		XRemoveFromSaveSet(bar.dpy, bar.icons[i]);
	}
	bar.nicons = 0;
	if (bar.tray_ok)
		XSetSelectionOwner(bar.dpy, bar.tray_atom, None, CurrentTime);
}

/* ---------------------------------------------------------------- blocks */

static Block *
block_find(const char *name)
{
	int i;

	for (i = 0; i < bar.nblocks; i++)
		if (!strcmp(bar.blocks[i].name, name))
			return &bar.blocks[i];
	return NULL;
}

static Block *
block_add(const char *name, int interval, const char *cmd)
{
	Block *b = block_find(name);

	if (!b) {
		if (bar.nblocks == MAX_BLOCKS) {
			rui_warn("too many blocks, '%s' ignored", name);
			return NULL;
		}
		b = &bar.blocks[bar.nblocks++];
		memset(b, 0, sizeof *b);
		rui_strlcpy(b->name, name, sizeof b->name);
	}
	free(b->cmd);
	b->cmd = rui_strdup(cmd);
	b->interval = interval > 0 ? interval : 1;
	return b;
}

static void
on_block(const char *out, int status, void *ud)
{
	Block *b = ud;
	char line[sizeof b->text];

	(void)status;
	b->busy = 0;
	rui_strlcpy(line, out ? out : "", sizeof line);
	line[strcspn(line, "\n")] = '\0';
	rui_strip_ansi(line);
	if (strcmp(line, b->text)) {
		rui_strlcpy(b->text, line, sizeof b->text);
		bar.dirty = 1;
	}
}

/* when the next custom block wants to run */
static long long
blocks_due(void)
{
	long long due = 0x7fffffffffffLL;
	int i;

	for (i = 0; i < bar.nblocks; i++) {
		Block *b = &bar.blocks[i];

		if (b->used && !b->busy && b->last + (long long)b->interval * 1000 <
		    due)
			due = b->last + (long long)b->interval * 1000;
	}
	return due;
}

static void
poll_blocks(long long now)
{
	int i;

	for (i = 0; i < bar.nblocks; i++) {
		Block *b = &bar.blocks[i];

		if (!b->used || b->busy ||
		    (b->last && now - b->last < (long long)b->interval * 1000))
			continue;
		b->last = now;
		if (rui_job_start(b->cmd, on_block, b) == 0)
			b->busy = 1;
	}
}

/* builds the ordered applet row from the "applets" setting */
static void
applets_build(void)
{
	char buf[sizeof bar.applets_spec], *tok;
	int i;

	bar.nap = 0;
	for (i = 0; i < bar.nblocks; i++)
		bar.blocks[i].used = 0;
	rui_strlcpy(buf, bar.applets_spec, sizeof buf);
	for (tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
		Applet *a;
		Block *b = NULL;
		int kind = -1;

		for (i = 0; i < NBUILTIN; i++)
			if (!strcmp(tok, applet_names[i]))
				kind = i;
		if (kind < 0 && (b = block_find(tok)))
			kind = AP_BLOCK;
		if (kind < 0) {
			rui_warn("unknown applet '%s' (not built in, no such block)",
			    tok);
			continue;
		}
		if (bar.nap == MAX_APPLETS)
			break;
		a = &bar.ap[bar.nap++];
		memset(a, 0, sizeof *a);
		a->kind = kind;
		if (b) {
			a->block = (int)(b - bar.blocks);
			b->used = 1;
		}
	}
}

Applet *
applet_of(int kind)
{
	int i;

	for (i = 0; i < bar.nap; i++)
		if (bar.ap[i].kind == kind && bar.ap[i].r.w > 0)
			return &bar.ap[i];
	return NULL;
}

int
has_applet(int kind)
{
	int i;

	for (i = 0; i < bar.nap; i++)
		if (bar.ap[i].kind == kind)
			return 1;
	return 0;
}

/* --------------------------------------------------------------- applets */

static void
on_battery(const char *out, int status, void *ud)
{
	(void)ud;
	bar.bat_busy = 0;
	bar.have_bat = status == 0 && bar.bat_mod &&
	    bar.bat_mod->parse(out, &bar.bat_pct, &bar.bat_chg);
	bar.dirty = 1;
}

/* the first battery module that finds a battery wins */
static void
poll_battery(void)
{
	const RuiBatteryModule *const *mp;

	for (mp = rui_config.battery; *mp; mp++) {
		const RuiBatteryModule *m = *mp;

		if (m->read) {
			if (m->read(&bar.bat_pct, &bar.bat_chg) == 1) {
				bar.bat_mod = m;
				bar.have_bat = 1;
				bar.dirty = 1;
				return;
			}
		} else if (m->cmd && m->parse && rui_have_cmd(m->program)) {
			bar.bat_mod = m;
			if (!bar.bat_busy && rui_job_start(m->cmd, on_battery, NULL) == 0)
				bar.bat_busy = 1;
			return;
		}
	}
	if (bar.have_bat)
		bar.dirty = 1;
	bar.have_bat = 0;
	bar.bat_mod = NULL;
}

static void
net_set_text(const RuiNetStatus *st)
{
	char ifname[32] = "";
	char old[sizeof bar.net_text];

	rui_strlcpy(old, bar.net_text, sizeof old);
	if (st && st->connected && st->ssid[0]) {
		snprintf(bar.net_text, sizeof bar.net_text, "%s", st->ssid);
		bar.net_up = 1;
	} else if (rui_net_link_up(ifname, sizeof ifname)) {
		snprintf(bar.net_text, sizeof bar.net_text, "%s", ifname);
		bar.net_up = 1;
	} else {
		snprintf(bar.net_text, sizeof bar.net_text, "offline");
		bar.net_up = 0;
	}
	if (strcmp(old, bar.net_text))
		bar.dirty = 1;
}

static void
on_net_status(const char *out, int status, void *ud)
{
	RuiNetStatus st;

	(void)ud;
	bar.net_busy = 0;
	memset(&st, 0, sizeof st);
	if (status == 0 && rui_net_parse_status(bar.net_mod, out, &st))
		net_set_text(&st);
	else
		net_set_text(NULL);
}

static void poll_net(void);

static void
on_net_iface(const char *out, int status, void *ud)
{
	(void)ud;
	bar.net_busy = 0;
	if (status != 0 || !rui_net_parse_iface(bar.net_mod, out, bar.net_iface,
	    sizeof bar.net_iface))
		net_set_text(NULL);
	else
		poll_net();                    /* found: its status right away */
}

static void
poll_net(void)
{
	char *cmd;

	if (bar.net_busy)
		return;
	if (!bar.net_mod) {
		net_set_text(NULL);
		return;
	}
	if (!bar.net_iface[0] && (cmd = rui_net_iface_cmd(bar.net_mod))) {
		if (rui_job_start(cmd, on_net_iface, NULL) == 0)
			bar.net_busy = 1;
		free(cmd);
		return;
	}
	if (!(cmd = rui_net_status_cmd(bar.net_mod, bar.net_iface))) {
		net_set_text(NULL);
		return;
	}
	if (rui_job_start(cmd, on_net_status, NULL) == 0)
		bar.net_busy = 1;
	free(cmd);
}

static void
on_bt_status(const char *out, int status, void *ud)
{
	RuiBtStatus old = bar.bt;

	(void)ud;
	(void)status;
	bar.bt_busy = 0;
	rui_bt_parse_status(bar.bt_mod, out, &bar.bt);
	if (memcmp(&old, &bar.bt, sizeof old))
		bar.dirty = 1;
}

static int bt_force;

/* after the user switched Bluetooth: ask again soon, whatever sysfs says */
static void
bt_recheck(void)
{
	bt_force = 1;
}

static void
poll_bt(void)
{
	bt_force = 0;
	if (!bar.bt_mod || bar.bt_busy)
		return;
	if (rui_job_start(bar.bt_mod->status_cmd, on_bt_status, NULL) == 0)
		bar.bt_busy = 1;
}

static void
poll_cpu_mem(void)
{
	double busy = 0;

	bar.have_cpu = rui_sys_cpu(&busy);
	if (bar.have_cpu) {
		memmove(&bar.cpu_hist[0], &bar.cpu_hist[1],
		    (CPU_HIST - 1) * sizeof bar.cpu_hist[0]);
		bar.cpu_hist[CPU_HIST - 1] = busy;
	}
	bar.have_mem = rui_sys_mem(&bar.mem, &bar.mem_total);
	bar.dirty = 1;
}

static void
poll_clock(void)
{
	char buf[sizeof bar.clock];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	if (!strftime(buf, sizeof buf, bar.clock_fmt, &tm))
		rui_strlcpy(buf, "--:--", sizeof buf);
	if (strcmp(buf, bar.clock)) {
		rui_strlcpy(bar.clock, buf, sizeof bar.clock);
		bar.dirty = 1;
	}
}

void
ap_text(const Applet *a, char *buf, size_t n)
{
	buf[0] = '\0';
	switch (a->kind) {
	case AP_BAT:
		if (bar.have_bat)
			snprintf(buf, n, "Bat %d%%%s", bar.bat_pct, bar.bat_chg ? "+" : "");
		break;
	case AP_NET:
		snprintf(buf, n, "Net: %s", bar.net_text);
		break;
	case AP_VOL:
		if (!bar.audio.mod || !bar.audio.valid)
			snprintf(buf, n, "Vol --");
		else if (bar.audio.muted)
			snprintf(buf, n, "Vol mute");
		else
			snprintf(buf, n, "Vol %d%%", bar.audio.volume);
		break;
	case AP_CLOCK:
		snprintf(buf, n, "%s", bar.clock);
		break;
	case AP_BT:                            /* hidden without a controller */
		if (!bar.bt.present)
			break;
		if (!bar.bt.powered)
			snprintf(buf, n, "BT off");
		else if (bar.bt.nconnected > 1)
			snprintf(buf, n, "BT: %d devices", bar.bt.nconnected);
		else if (bar.bt.nconnected == 1)
			snprintf(buf, n, "BT: %s", bar.bt.device);
		else
			snprintf(buf, n, "BT on");
		break;
	case AP_DND:                           /* do not disturb */
		snprintf(buf, n, "DND");
		break;
	case AP_AWAKE:                         /* keep the screen on */
		snprintf(buf, n, "Awake");
		break;
	case AP_NOTES:                         /* the notification list */
		if (notify_unread())
			snprintf(buf, n, "Msg %d", notify_unread());
		else
			snprintf(buf, n, "Msg");
		break;
	case AP_BLOCK:
		snprintf(buf, n, "%s", bar.blocks[a->block].text);
		break;
	default:
		break;
	}
}

/* ----------------------------------------------------------------- layout */

static int
task_visible(const Task *t)
{
	return !t->skip && (t->desk == bar.cur || t->desk == ALL_DESKS);
}

static void
layout(void)
{
	RuiApp *a = bar.app;
	int x, right, i, n = 0, tw, pw = bar.h - 6, isz = bar.h - 8, full = 0;
	char buf[160];

	bar.r_start.x = 2;
	bar.r_start.y = 2;
	bar.r_start.w = rui_text_width(a, a->bold, bar.start_label, -1) + 34;
	bar.r_start.h = bar.h - 4;
	x = bar.r_start.x + bar.r_start.w + 6;
	bar.r_pager.x = x;
	bar.r_pager.y = 3;
	bar.r_pager.w = (int)bar.ndesk * pw;
	bar.r_pager.h = bar.h - 6;
	x += bar.r_pager.w + 4;
	bar.r_layout.w = 0;
	if (bar.layout_sym[0]) {
		bar.r_layout.x = x;
		bar.r_layout.y = 3;
		bar.r_layout.w = rui_text_width(a, a->bold, bar.layout_sym, -1) + 12;
		bar.r_layout.h = bar.h - 6;
		x += bar.r_layout.w + 4;
	}
	x += 2;

	right = bar.w - 3;
	for (i = bar.nap - 1; i >= 0; i--) {
		Applet *ap = &bar.ap[i];
		int w = 0;

		ap->r.w = 0;
		if (full)
			continue;                  /* no room left: hidden */
		if (ap->kind == AP_CPU) {
			w = bar.have_cpu ? CPU_HIST + 4 : 0;
		} else if (ap->kind == AP_MEM) {
			w = bar.have_mem ? 14 : 0;
		} else {
			ap_text(ap, buf, sizeof buf);
			if (buf[0]) {
				w = rui_text_width(a, ap->kind == AP_DND ||
				    ap->kind == AP_AWAKE ? a->bold : a->font, buf, -1);
				if (w > 200)
					w = 200;
				w += 12;
			}
		}
		if (w == 0)
			continue;
		if (right - w < x) {
			/* a narrow screen: keep the applets on the right (the clock)
			 * and drop this one and those left of it rather than draw
			 * them over the Start button */
			full = 1;
			continue;
		}
		right -= w;
		ap->r.x = right;
		ap->r.y = 3;
		ap->r.w = w;
		ap->r.h = bar.h - 6;
		right -= 2;
	}
	bar.r_tray.w = bar.nicons ? bar.nicons * (isz + 2) + 2 : 0;
	bar.r_tray.x = right - bar.r_tray.w;
	bar.r_tray.y = 3;
	bar.r_tray.h = bar.h - 6;
	right = bar.r_tray.x - 4;

	bar.r_tasks.x = x;
	bar.r_tasks.y = 3;
	bar.r_tasks.w = right - x;
	bar.r_tasks.h = bar.h - 6;
	for (i = 0; i < bar.ntasks; i++) {
		bar.tasks[i].shown = task_visible(&bar.tasks[i]);
		if (bar.tasks[i].shown)
			n++;
	}
	tw = n ? bar.r_tasks.w / n : 0;
	if (tw > 170)
		tw = 170;
	if (tw < 24)
		tw = 24;
	for (i = 0, n = 0; i < bar.ntasks; i++) {
		Task *t = &bar.tasks[i];

		if (!t->shown)
			continue;
		t->r.x = x + n * tw;
		t->r.y = 3;
		t->r.w = tw - 2;
		t->r.h = bar.h - 6;
		if (t->r.x + t->r.w > right)
			t->shown = 0;              /* out of room */
		n++;
	}
	tray_layout();
}

/* ---------------------------------------------------------------- drawing */

void
draw_applet(RuiSurf *s, const Applet *ap)
{
	RuiRect r = ap->r;
	char buf[160];

	if (ap->kind == AP_DND || ap->kind == AP_AWAKE) {
		/* a switch: pressed in and bold while it is on */
		int on = ap->kind == AP_DND ? bar.dnd : bar.awake;

		ap_text(ap, buf, sizeof buf);
		if (on)
			rui_fill_dither(s, r.x, r.y, r.w, r.h, RUI_C_FACE, RUI_C_LIGHT);
		rui_bevel(s, r.x, r.y, r.w, r.h, on ? RUI_PRESSED : RUI_RAISED_THIN);
		r.x += 4 + on;
		r.y += on;
		r.w -= 8;
		rui_text_fit(s, on ? bar.app->bold : bar.app->font, r, buf,
		    on ? RUI_C_TEXT : RUI_C_DISABLED, RUI_CENTER);
		return;
	}
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	if (ap->kind == AP_CPU) {
		int k, gh = r.h - 4;

		rui_fill(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RUI_C_DARK);
		for (k = 0; k < CPU_HIST; k++) {
			int bh = (int)(bar.cpu_hist[k] * gh + 0.5);

			if (bh > gh)
				bh = gh;
			if (bh == 0 && bar.cpu_hist[k] > 0.002)
				bh = 1;
			if (bh > 0)
				rui_fill(s, r.x + 2 + k, r.y + 2 + gh - bh, 1, bh,
				    bar.cpu_hist[k] > 0.85 ? RUI_C_ALERT : RUI_C_GOOD);
		}
	} else if (ap->kind == AP_MEM) {
		int gh = r.h - 4, bh = (int)(bar.mem * gh + 0.5);

		rui_fill(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RUI_C_DARK);
		if (bh > 0)
			rui_fill(s, r.x + 3, r.y + 2 + gh - bh, r.w - 6, bh,
			    bar.mem > 0.9 ? RUI_C_ALERT : RUI_C_SEL_BG);
	} else {
		int col = RUI_C_TEXT;

		ap_text(ap, buf, sizeof buf);
		if (ap->kind == AP_BAT && !bar.bat_chg && bar.bat_pct <= 15)
			col = RUI_C_ALERT;
		if (ap->kind == AP_NET && !bar.net_up)
			col = RUI_C_DISABLED;
		if (ap->kind == AP_BT && !bar.bt.powered)
			col = RUI_C_DISABLED;
		r.x += 6;
		r.w -= 12;
		rui_text_fit(s, bar.app->font, r, buf, col, RUI_CENTER);
	}
}

static void
draw(void)
{
	RuiSurf *s = bar.s;
	RuiApp *a = bar.app;
	RuiButton b;
	RuiRect r;
	char buf[128];
	int i, pw = bar.h - 6;

	if (bar.fp) {
		fp_draw();
		bar.dirty = 0;
		return;
	}
	layout();
	rui_fill(s, 0, 0, bar.w, bar.h, RUI_C_FACE);
	if (bar.top)
		rui_fill(s, 0, bar.h - 1, bar.w, 1, RUI_C_SHADOW);
	else
		rui_fill(s, 0, 0, bar.w, 1, RUI_C_LIGHT);

	/* start button with its little four-colour logo */
	memset(&b, 0, sizeof b);
	b.r = bar.r_start;
	b.pressed = b.armed = bar.start_down;
	rui_button_draw(s, &b);
	{
		int o = bar.start_down ? 1 : 0;
		int lx = b.r.x + 6 + o, ly = b.r.y + (b.r.h - 13) / 2 + o;

		rui_fill(s, lx, ly, 6, 6, RUI_C_ALERT);
		rui_fill(s, lx + 7, ly, 6, 6, RUI_C_GOOD);
		rui_fill(s, lx, ly + 7, 6, 6, RUI_C_SEL_BG);
		rui_fill(s, lx + 7, ly + 7, 6, 6, RUI_C_SHADOW);
		r = b.r;
		r.x += 24 + o;
		r.y += o;
		r.w -= 28;
		rui_text_fit(s, a->bold, r, bar.start_label, RUI_C_TEXT, RUI_LEFT);
	}

	/* pager */
	for (i = 0; i < (int)bar.ndesk; i++) {
		int cur = (unsigned long)i == bar.cur, used = 0, k;

		for (k = 0; k < bar.ntasks; k++)
			if (bar.tasks[k].desk == (unsigned long)i && !bar.tasks[k].skip)
				used = 1;
		r.x = bar.r_pager.x + i * pw;
		r.y = bar.r_pager.y;
		r.w = pw;
		r.h = bar.r_pager.h;
		if (cur)
			rui_fill_dither(s, r.x, r.y, r.w, r.h, RUI_C_FACE, RUI_C_LIGHT);
		rui_bevel(s, r.x, r.y, r.w, r.h, cur ? RUI_PRESSED : RUI_RAISED);
		snprintf(buf, sizeof buf, "%d", i + 1);
		if (cur) {
			r.x++;
			r.y++;
		}
		rui_text_fit(s, used ? a->bold : a->font, r, buf,
		    used || cur ? RUI_C_TEXT : RUI_C_DISABLED, RUI_CENTER);
	}

	/* layout symbol */
	if (bar.r_layout.w) {
		r = bar.r_layout;
		rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
		rui_text_fit(s, a->bold, r, bar.layout_sym, RUI_C_TEXT, RUI_CENTER);
	}

	/* task buttons */
	for (i = 0; i < bar.ntasks; i++) {
		Task *t = &bar.tasks[i];
		int act = t->win == bar.active && !t->hidden;

		if (!t->shown)
			continue;
		if (act)
			rui_fill_dither(s, t->r.x, t->r.y, t->r.w, t->r.h, RUI_C_FACE,
			    RUI_C_LIGHT);
		rui_bevel(s, t->r.x, t->r.y, t->r.w, t->r.h, act ? RUI_PRESSED :
		    RUI_RAISED);
		r = t->r;
		r.x += 6 + (act ? 1 : 0);
		r.y += act ? 1 : 0;
		r.w -= 12;
		if (t->icon && r.w > TASK_ICON + 24) {
			image_draw(a, t->icon, s->buf, r.x - 2,
			    r.y + (r.h - t->icon->h) / 2);
			r.x += TASK_ICON + 2;
			r.w -= TASK_ICON + 2;
		}
		if (t->hidden)
			snprintf(buf, sizeof buf, "[%.120s]", t->title);
		else
			snprintf(buf, sizeof buf, "%.124s", t->title);
		rui_text_fit(s, act ? a->bold : a->font, r, buf,
		    t->urgent && !bar.dnd ? RUI_C_ALERT : t->hidden ? RUI_C_DISABLED :
		    RUI_C_TEXT,
		    RUI_LEFT);
	}

	if (bar.r_tray.w)
		rui_bevel(s, bar.r_tray.x, bar.r_tray.y, bar.r_tray.w, bar.r_tray.h,
		    RUI_SUNKEN_THIN);
	for (i = 0; i < bar.nap; i++)
		if (bar.ap[i].r.w)
			draw_applet(s, &bar.ap[i]);
	rui_surf_flush(s);
	bar.dirty = 0;
}

/* ------------------------------------------------------------- calendar */

#define CAL_ROWS 8          /* header, weekday names, six weeks */

static int
cal_cell_w(void)
{
	return rui_text_width(bar.app, bar.app->font, "00", 2) + 12;
}

static int
cal_cell_h(void)
{
	return bar.app->font->height + 6;
}

static void
cal_draw(void)
{
	RuiSurf *s = bar.cal_s;
	RuiApp *a = bar.app;
	struct tm tm, today, last;
	time_t now = time(NULL), t;
	char buf[64];
	int cw = cal_cell_w(), ch = cal_cell_h(), first, days, d, i;
	RuiRect r;

	localtime_r(&now, &today);
	memset(&tm, 0, sizeof tm);
	tm.tm_year = bar.cal_year;
	tm.tm_mon = bar.cal_mon;
	tm.tm_mday = 1;
	tm.tm_hour = 12;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	localtime_r(&t, &tm);
	first = (tm.tm_wday + 6) % 7;          /* Monday first */
	memset(&last, 0, sizeof last);
	last.tm_year = bar.cal_year;
	last.tm_mon = bar.cal_mon + 1;
	last.tm_mday = 0;
	last.tm_hour = 12;
	last.tm_isdst = -1;
	mktime(&last);
	days = last.tm_mday;

	rui_fill(s, 0, 0, s->w, s->h, RUI_C_FACE);
	rui_bevel(s, 0, 0, s->w, s->h, RUI_RAISED);
	rui_fill(s, 4, 4, s->w - 8, ch, RUI_C_TITLE_ACTIVE);
	strftime(buf, sizeof buf, "%B %Y", &tm);
	r.x = 4 + ch;
	r.y = 4;
	r.w = s->w - 8 - 2 * ch;
	r.h = ch;
	rui_text_fit(s, a->bold, r, buf, RUI_C_TITLE_FG, RUI_CENTER);
	rui_fill(s, 5, 5, ch - 2, ch - 2, RUI_C_FACE);
	rui_bevel(s, 5, 5, ch - 2, ch - 2, RUI_RAISED);
	rui_arrow(s, 5 + (ch - 2) / 2, 5 + (ch - 2) / 2, 4, 2, RUI_C_TEXT);
	rui_fill(s, s->w - 3 - ch, 5, ch - 2, ch - 2, RUI_C_FACE);
	rui_bevel(s, s->w - 3 - ch, 5, ch - 2, ch - 2, RUI_RAISED);
	rui_arrow(s, s->w - 3 - ch + (ch - 2) / 2, 5 + (ch - 2) / 2, 4, 3,
	    RUI_C_TEXT);
	for (i = 0; i < 7; i++) {
		struct tm wd;

		memset(&wd, 0, sizeof wd);
		wd.tm_wday = (i + 1) % 7;
		strftime(buf, sizeof buf, "%a", &wd);
		buf[2] = '\0';
		r.x = 6 + i * cw;
		r.y = 6 + ch;
		r.w = cw;
		r.h = ch;
		rui_text_fit(s, a->font, r, buf, RUI_C_SHADOW, RUI_CENTER);
	}
	rui_fill(s, 6, 6 + 2 * ch - 1, 7 * cw, 1, RUI_C_SHADOW);
	for (d = 1; d <= days; d++) {
		int cell = first + d - 1;
		int is_today = d == today.tm_mday && bar.cal_mon == today.tm_mon &&
		    bar.cal_year == today.tm_year;

		r.x = 6 + (cell % 7) * cw;
		r.y = 6 + (2 + cell / 7) * ch;
		r.w = cw;
		r.h = ch;
		if (is_today)
			rui_fill(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RUI_C_SEL_BG);
		snprintf(buf, sizeof buf, "%d", d);
		rui_text_fit(s, a->font, r, buf, is_today ? RUI_C_SEL_FG :
		    cell % 7 >= 5 ? RUI_C_ALERT : RUI_C_TEXT, RUI_CENTER);
	}
	rui_surf_flush(s);
}

void
cal_close(void)
{
	if (!bar.cal_win)
		return;
	rui_surf_free(bar.cal_s);
	XDestroyWindow(bar.dpy, bar.cal_win);
	bar.cal_win = None;
	bar.cal_s = NULL;
}

void
cal_toggle(void)
{
	time_t now = time(NULL);
	struct tm tm;
	int w, h, x, y;

	if (bar.cal_win) {
		cal_close();
		return;
	}
	localtime_r(&now, &tm);
	bar.cal_year = tm.tm_year;
	bar.cal_mon = tm.tm_mon;
	w = 7 * cal_cell_w() + 12;
	h = CAL_ROWS * cal_cell_h() + 12;
	x = bar.cal_anchor >= 0 ? bar.cal_anchor : bar.x + bar.w - w - 2;
	if (x + w > bar.app->sw)
		x = bar.app->sw - w;
	y = bar.top ? bar.y + bar.h + 2 : bar.y - h - 2;
	bar.cal_win = rui_window_new(bar.app, bar.app->root, x, y, w, h, 1,
	    ExposureMask | ButtonPressMask);
	bar.cal_s = rui_surf_new(bar.app, bar.cal_win, w, h);
	XMapRaised(bar.dpy, bar.cal_win);
	cal_draw();
}

static void
cal_click(XButtonEvent *e)
{
	int ch = cal_cell_h(), step = 0;

	if (e->button == Button4)
		step = -1;
	else if (e->button == Button5)
		step = 1;
	else if (e->button == Button1 && e->y < 4 + ch) {
		if (e->x < 4 + ch)
			step = -1;
		else if (e->x >= bar.cal_s->w - 4 - ch)
			step = 1;
	}
	if (!step)
		return;
	bar.cal_mon += step;
	if (bar.cal_mon < 0) {
		bar.cal_mon = 11;
		bar.cal_year--;
	} else if (bar.cal_mon > 11) {
		bar.cal_mon = 0;
		bar.cal_year++;
	}
	cal_draw();
}

/* ----------------------------------------------------------- volume popup */

static void
vol_draw(void)
{
	RuiSurf *s = bar.vol_s;
	RuiRect r;
	char buf[32];

	if (!s)
		return;
	rui_fill(s, 0, 0, s->w, s->h, RUI_C_FACE);
	rui_bevel(s, 0, 0, s->w, s->h, RUI_RAISED);
	if (!bar.vol_slider.drag)
		bar.vol_slider.value = bar.audio.volume / 100.0;
	if (bar.vol_slider.value > 1.0)
		bar.vol_slider.value = 1.0;
	rui_slider_draw(s, &bar.vol_slider);
	bar.vol_mute.toggled = bar.audio.muted;
	rui_button_draw(s, &bar.vol_mute);
	r.x = 4;
	r.y = 5;
	r.w = s->w - 8;
	r.h = bar.app->font->height;
	if (!bar.audio.mod)
		snprintf(buf, sizeof buf, "n/a");
	else
		snprintf(buf, sizeof buf, "%d%%", bar.audio.volume);
	rui_text_fit(s, bar.app->font, r, buf, RUI_C_TEXT, RUI_CENTER);
	rui_surf_flush(s);
}

void
vol_close(void)
{
	if (!bar.vol_win)
		return;
	rui_surf_free(bar.vol_s);
	XDestroyWindow(bar.dpy, bar.vol_win);
	bar.vol_win = None;
	bar.vol_s = NULL;
}

void
vol_toggle(const Applet *ap)
{
	int w = 64, h = 170, x, y, fh = bar.app->font->height;

	if (bar.vol_win) {
		vol_close();
		return;
	}
	x = bar.x + ap->r.x + ap->r.w / 2 - w / 2;
	if (x + w > bar.x + bar.w)
		x = bar.x + bar.w - w;
	y = bar.top ? bar.y + bar.h + 2 : bar.y - h - 2;
	bar.vol_win = rui_window_new(bar.app, bar.app->root, x, y, w, h, 1,
	    ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);
	bar.vol_s = rui_surf_new(bar.app, bar.vol_win, w, h);
	memset(&bar.vol_slider, 0, sizeof bar.vol_slider);
	bar.vol_slider.vertical = 1;
	bar.vol_slider.r.x = w / 2 - 14;
	bar.vol_slider.r.y = fh + 12;
	bar.vol_slider.r.w = 28;
	bar.vol_slider.r.h = h - fh - 12 - fh - 22;
	memset(&bar.vol_mute, 0, sizeof bar.vol_mute);
	bar.vol_mute.label = "Mute";
	bar.vol_mute.r.x = 6;
	bar.vol_mute.r.y = h - fh - 14;
	bar.vol_mute.r.w = w - 12;
	bar.vol_mute.r.h = fh + 8;
	XMapRaised(bar.dpy, bar.vol_win);
	rui_audio_refresh(&bar.audio);
	vol_draw();
}

static void
vol_event(XEvent *ev)
{
	int r = rui_slider_event(&bar.vol_slider, ev);

	if (r & RUI_EV_CHANGED)
		rui_audio_set(&bar.audio, (int)(bar.vol_slider.value * 100.0 + 0.5));
	r |= rui_button_event(&bar.vol_mute, ev);
	if (r & RUI_EV_ACTIVATE)
		rui_audio_toggle_mute(&bar.audio);
	if (r)
		vol_draw();
}

static void
audio_changed(void *ud)
{
	(void)ud;
	bar.dirty = 1;
	vol_draw();
}

/* ----------------------------------------------------------------- input */

/* reload menu.conf / .desktop applications when either changed */
void
menu_refresh(void)
{
	long long stamp = rui_menu_stamp(bar.menu_path);

	if (bar.menu && stamp == bar.menu_stamp)
		return;
	rui_menu_free(bar.menu);
	bar.menu = rui_menu_load(bar.menu_path);
	bar.menu_stamp = stamp;
}

static void
start_menu(void)
{
	const RuiMenuItem *it;
	char *cmd = NULL;

	cal_close();
	vol_close();
	menu_refresh();
	bar.start_down = 1;
	draw();
	it = rui_menu_popup(bar.app, bar.menu, bar.x + bar.r_start.x,
	    bar.top ? bar.y + bar.h : bar.y, bar.top ? 0 : RUI_MENU_UPWARD);
	if (it && it->cmd)
		cmd = rui_strdup(it->cmd);
	bar.start_down = 0;
	bar.dirty = 1;
	if (cmd)
		rui_run_command(bar.app, cmd);
	free(cmd);
}

static void
task_menu(Task *t, int xr, int yr)
{
	Window w = t->win;
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	int id;

	rui_menu_add(m, t->hidden ? "Restore" : "Minimize", NULL, 1);
	rui_menu_add(m, "Maximize", NULL, 2);
	rui_menu_add(m, "Sticky", NULL, 3)->checked = t->desk == ALL_DESKS;
	rui_menu_add_sep(m);
	rui_menu_add(m, "Close", NULL, 4);
	it = rui_menu_popup(bar.app, m, xr, bar.top ? bar.y + bar.h : yr,
	    bar.top ? 0 : RUI_MENU_UPWARD);
	id = it ? it->id : 0;
	rui_menu_free(m);
	t = task_find(w);                       /* list may have changed */
	if (!t)
		return;
	switch (id) {
	case 1:
		if (t->hidden)
			rui_send_root_msg(bar.app, w, A(_NET_ACTIVE_WINDOW), 2,
			    CurrentTime, 0, 0, 0);
		else
			rui_send_root_msg(bar.app, w, A(WM_CHANGE_STATE), IconicState, 0,
			    0, 0, 0);
		break;
	case 2:
		rui_send_root_msg(bar.app, w, A(_NET_WM_STATE), 2,
		    (long)A(_NET_WM_STATE_MAXIMIZED_VERT),
		    (long)A(_NET_WM_STATE_MAXIMIZED_HORZ), 2, 0);
		break;
	case 3:
		rui_send_root_msg(bar.app, w, A(_NET_WM_DESKTOP),
		    t->desk == ALL_DESKS ? (long)bar.cur : (long)ALL_DESKS, 2, 0, 0,
		    0);
		break;
	case 4:
		rui_send_root_msg(bar.app, w, A(_NET_CLOSE_WINDOW), CurrentTime, 2, 0,
		    0, 0);
		break;
	default:
		break;
	}
}

void
switch_desk_rel(int d)
{
	long n = (long)bar.ndesk, next = ((long)bar.cur + d + n) % n;

	rui_send_root_msg(bar.app, bar.app->root, A(_NET_CURRENT_DESKTOP), next,
	    CurrentTime, 0, 0, 0);
}

void
applet_click(Applet *ap, XButtonEvent *e)
{
	switch (ap->kind) {
	case AP_CLOCK:
		if (e->button == Button1)
			cal_toggle();
		else if (e->button == Button2 || e->button == Button3)
			rui_run_command(bar.app, "@clock");    /* xclock by default */
		break;
	case AP_DND:
		if (e->button == Button1)
			rui_wm_command(bar.app, "dnd");
		break;
	case AP_AWAKE:
		if (e->button == Button1)
			rui_wm_command(bar.app, "awake");
		break;
	case AP_NOTES:
		notify_center_toggle(e->x_root);
		break;
	case AP_BT:
		if (e->button == Button1) {
			rui_spawn("retro-bt");
		} else if ((e->button == Button2 || e->button == Button3) &&
		    bar.bt_mod && bar.bt.present) {
			rui_spawn(bar.bt_mod->power_cmd[!bar.bt.powered]);
			bar.bt.powered = !bar.bt.powered;      /* confirmed by a poll */
			bt_recheck();
			bar.dirty = 1;
		}
		break;
	case AP_VOL:
		if (e->button == Button1)
			vol_toggle(ap);
		else if (e->button == Button2 || e->button == Button3)
			rui_audio_toggle_mute(&bar.audio);
		else if (e->button == Button4)
			rui_audio_set(&bar.audio, bar.audio.volume + 5);
		else if (e->button == Button5)
			rui_audio_set(&bar.audio, bar.audio.volume - 5);
		break;
	case AP_NET:
		if (e->button == Button1)
			rui_spawn("retro-net");
		break;
	case AP_CPU:
	case AP_MEM:
		if (e->button == Button1)
			rui_run_command(bar.app, "@terminal -e top");
		break;
	case AP_BLOCK: {
		Block *b = &bar.blocks[ap->block];

		if (e->button == Button1 && b->click)
			rui_run_command(bar.app, b->click);
		else if (e->button == Button3)
			b->last = 0;                       /* refresh now */
		break;
	}
	default:
		break;
	}
}

static void
bar_click(XButtonEvent *e)
{
	int i, pw = bar.h - 6;

	if (bar.fp) {
		fp_click(e);
		return;
	}
	if (rui_in_rect(bar.r_start, e->x, e->y)) {
		if (e->button == Button1)
			start_menu();
		return;
	}
	if (e->button == Button1) {
		Applet *c = applet_of(AP_CLOCK), *v = applet_of(AP_VOL);

		if (!c || !rui_in_rect(c->r, e->x, e->y))
			cal_close();
		if (!v || !rui_in_rect(v->r, e->x, e->y))
			vol_close();
	}
	if (rui_in_rect(bar.r_pager, e->x, e->y)) {
		if (e->button == Button1)
			rui_send_root_msg(bar.app, bar.app->root, A(_NET_CURRENT_DESKTOP),
			    (e->x - bar.r_pager.x) / pw, CurrentTime, 0, 0, 0);
		else if (e->button == Button4)
			switch_desk_rel(-1);
		else if (e->button == Button5)
			switch_desk_rel(1);
		return;
	}
	if (bar.r_layout.w && rui_in_rect(bar.r_layout, e->x, e->y)) {
		if (e->button == Button1)
			rui_wm_command(bar.app, "next_layout");
		return;
	}
	for (i = 0; i < bar.ntasks; i++) {
		Task *t = &bar.tasks[i];

		if (!t->shown || !rui_in_rect(t->r, e->x, e->y))
			continue;
		if (e->button == Button1) {
			if (t->win == bar.active && !t->hidden)
				rui_send_root_msg(bar.app, t->win, A(WM_CHANGE_STATE),
				    IconicState, 0, 0, 0, 0);
			else
				rui_send_root_msg(bar.app, t->win, A(_NET_ACTIVE_WINDOW), 2,
				    (long)e->time, 0, 0, 0);
		} else if (e->button == Button2) {
			rui_send_root_msg(bar.app, t->win, A(_NET_CLOSE_WINDOW),
			    (long)e->time, 2, 0, 0, 0);
		} else if (e->button == Button3) {
			task_menu(t, e->x_root, e->y_root);
		}
		return;
	}
	for (i = 0; i < bar.nap; i++) {
		if (bar.ap[i].r.w && rui_in_rect(bar.ap[i].r, e->x, e->y)) {
			applet_click(&bar.ap[i], e);
			return;
		}
	}
	if (e->button == Button4)
		switch_desk_rel(-1);
	else if (e->button == Button5)
		switch_desk_rel(1);
}

/* --------------------------------------------------------------- geometry */

static void
place(void)
{
	RuiApp *a = bar.app;
	RuiMonitor mon;
	XSizeHints sh;
	unsigned long strut[12];
	int mx, my, mw, mh;

	a->sw = DisplayWidth(a->dpy, a->screen);
	a->sh = DisplayHeight(a->dpy, a->screen);
	mon = rui_monitor_pick(a, bar.monitor);    /* the primary by default */
	mx = mon.x;
	my = mon.y;
	mw = mon.w;
	mh = mon.h;
	if (bar.fp) {
		bar.w = fp_width();
		if (bar.w > mw)
			bar.w = mw;
		bar.x = mx + (mw - bar.w) / 2;
	} else {
		bar.x = mx;
		bar.w = mw;
	}
	bar.y = bar.top ? my : my + mh - bar.h;
	XMoveResizeWindow(a->dpy, bar.win, bar.x, bar.y, (unsigned)bar.w,
	    (unsigned)bar.h);
	rui_surf_resize(bar.s, bar.w, bar.h);
	memset(&sh, 0, sizeof sh);
	sh.flags = USPosition | PPosition | PSize | PMinSize | PMaxSize;
	sh.x = bar.x;
	sh.y = bar.y;
	sh.width = sh.min_width = sh.max_width = bar.w;
	sh.height = sh.min_height = sh.max_height = bar.h;
	XSetWMNormalHints(a->dpy, bar.win, &sh);
	memset(strut, 0, sizeof strut);
	/* struts count from the edges of the whole screen, which a monitor
	 * in a mixed layout need not touch; the window manager clips them to
	 * the monitor the panel is on */
	if (bar.top) {
		strut[2] = (unsigned long)(bar.y + bar.h);
		strut[8] = (unsigned long)bar.x;
		strut[9] = (unsigned long)(bar.x + bar.w - 1);
	} else {
		strut[3] = (unsigned long)(a->sh - bar.y);
		strut[10] = (unsigned long)bar.x;
		strut[11] = (unsigned long)(bar.x + bar.w - 1);
	}
	rui_set_prop32(a, bar.win, A(_NET_WM_STRUT), XA_CARDINAL, strut, 4);
	rui_set_prop32(a, bar.win, A(_NET_WM_STRUT_PARTIAL), XA_CARDINAL, strut,
	    12);
	bar.dirty = 1;
}

/* ----------------------------------------------------------------- events */

static void
handle_event(XEvent *ev)
{
	Task *t;

	if (notify_event(ev))
		return;
	if (rui_monitors_event(bar.app, ev)) {
		cal_close();                       /* monitors were rearranged */
		vol_close();
		place();
		return;
	}
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count)
			break;
		if (ev->xexpose.window == bar.win)
			rui_surf_flush(bar.s);
		else if (bar.cal_win && ev->xexpose.window == bar.cal_win)
			rui_surf_flush(bar.cal_s);
		else if (bar.vol_win && ev->xexpose.window == bar.vol_win)
			rui_surf_flush(bar.vol_s);
		break;
	case ButtonPress:
		if (ev->xbutton.window == bar.win)
			bar_click(&ev->xbutton);
		else if (bar.cal_win && ev->xbutton.window == bar.cal_win)
			cal_click(&ev->xbutton);
		else if (bar.vol_win && ev->xbutton.window == bar.vol_win)
			vol_event(ev);
		break;
	case ButtonRelease:
	case MotionNotify:
		if (bar.vol_win && ev->xany.window == bar.vol_win)
			vol_event(ev);
		break;
	case PropertyNotify:
		if (ev->xproperty.window == bar.app->root) {
			Atom a = ev->xproperty.atom;

			if (a == A(_NET_CLIENT_LIST))
				tasks_refresh();
			else if (a == A(_NET_CURRENT_DESKTOP) ||
			    a == A(_NET_NUMBER_OF_DESKTOPS) ||
			    a == A(_NET_ACTIVE_WINDOW) || a == A(_RETRO_DND) ||
			    a == A(_RETRO_AWAKE))
				root_props_refresh();
			else if (a == A(_RETRO_LAYOUT))
				layout_refresh();
			else if (a == A(_NET_DESKTOP_NAMES)) {
				desk_names_refresh();
				bar.dirty = 1;
			} else if (a == A(_RETRO_RELOAD)) {
				reload_requested = 1;          /* re-exec with new settings */
				bar.running = 0;
			}
		} else if ((t = task_find(ev->xproperty.window))) {
			Atom a = ev->xproperty.atom;

			if (a == A(_NET_WM_NAME) || a == XA_WM_NAME ||
			    a == A(_NET_WM_STATE) || a == A(_NET_WM_DESKTOP) ||
			    a == A(_NET_WM_WINDOW_TYPE)) {
				task_read(t);
				bar.dirty = 1;
			} else if (a == A(_NET_WM_ICON)) {
				task_icon(t);
				bar.dirty = 1;
			}
		}
		break;
	case ClientMessage:
		if (ev->xclient.message_type == A(_NET_SYSTEM_TRAY_OPCODE) &&
		    ev->xclient.data.l[1] == 0)
			tray_add((Window)ev->xclient.data.l[2]);
		else if (rui_wm_delete_event(bar.app, ev))
			bar.running = 0;
		break;
	case DestroyNotify:
		if (ev->xdestroywindow.window == bar.win)
			bar.running = 0;
		else
			tray_remove(ev->xdestroywindow.window);
		break;
	case ReparentNotify:
		if (ev->xreparent.parent != bar.win)
			tray_remove(ev->xreparent.window);
		break;
	case SelectionClear:
		if (ev->xselectionclear.selection == bar.tray_atom)
			bar.tray_ok = 0;
		break;
	case ConfigureNotify:
		if (ev->xconfigure.window == bar.app->root &&
		    (ev->xconfigure.width != bar.app->sw ||
		    ev->xconfigure.height != bar.app->sh)) {
			cal_close();
			vol_close();
			place();
		}
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

/* ------------------------------------------------------------------ setup */

static void
bar_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "panel_position")) {
		bar.top = !strcmp(val, "top");
	} else if (!strcmp(key, "panel_height")) {
		bar.h = atoi(val);
	} else if (!strcmp(key, "clock_format")) {
		rui_strlcpy(bar.clock_fmt, val, sizeof bar.clock_fmt);
	} else if (!strcmp(key, "start_label")) {
		rui_strlcpy(bar.start_label, val, sizeof bar.start_label);
	} else if (!strcmp(key, "audio_backend")) {
		rui_strlcpy(bar.audio_forced, val, sizeof bar.audio_forced);
	} else if (!strcmp(key, "net_backend")) {
		rui_strlcpy(bar.net_forced, val, sizeof bar.net_forced);
	} else if (!strcmp(key, "bluetooth_backend")) {
		rui_strlcpy(bar.bt_forced, val, sizeof bar.bt_forced);
	} else if (!strcmp(key, "panel_monitor")) {
		rui_strlcpy(bar.monitor, val, sizeof bar.monitor);
	} else if (!strcmp(key, "panel_style")) {
		panel_style = !strcmp(val, "auto") ? -1 :
		    !strcmp(val, "frontpanel") || !strcmp(val, "cde");
	} else if (!strcmp(key, "applets")) {
		rui_strlcpy(bar.applets_spec, val, sizeof bar.applets_spec);
	} else if (!strcmp(key, "block")) {
		/* block = NAME SECONDS COMMAND... */
		char name[32];
		int interval, off = 0;

		if (sscanf(val, "%31s %d %n", name, &interval, &off) < 2 || !off ||
		    !val[off])
			rui_warn("retro.conf: expected block = NAME SECONDS COMMAND");
		else
			block_add(name, interval, val + off);
	} else if (!strcmp(key, "block_click")) {
		char name[32];
		int off = 0;
		Block *b;

		if (sscanf(val, "%31s %n", name, &off) < 1 || !off || !val[off] ||
		    !(b = block_find(name))) {
			rui_warn("retro.conf: block_click needs a defined block and "
			    "a command");
		} else {
			free(b->click);
			b->click = rui_strdup(val + off);
		}
	}
}

static void
setup(void)
{
	RuiApp *a = bar.app;
	XWMHints wmh;
	unsigned long v[2];
	const RuiBlock *cb;
	char *path;

	bar.dpy = a->dpy;
	bar.ndesk = 1;
	rui_strlcpy(bar.applets_spec, rui_config.applets, sizeof bar.applets_spec);
	rui_strlcpy(bar.clock_fmt, "%H:%M", sizeof bar.clock_fmt);
	rui_strlcpy(bar.start_label, "Start", sizeof bar.start_label);
	rui_strlcpy(bar.net_text, "...", sizeof bar.net_text);
	for (cb = rui_config.blocks; cb->name; cb++) {
		Block *b = block_add(cb->name, cb->interval, cb->cmd);

		if (b && cb->click)
			b->click = rui_strdup(cb->click);
	}
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, bar_conf, NULL);
	free(path);
	applets_build();
	bar.cal_anchor = -1;
	bar.fp = panel_style >= 0 ? panel_style : a->style == RUI_STYLE_CDE;
	if (bar.fp) {
		fp_setup();
		bar.h = fp_height();
	} else {
		/* the taskbar keeps its Windows-95 look (black-edged bevels,
		 * flat menu highlight) whatever the window style is */
		a->style = RUI_STYLE_CLASSIC;
		if (bar.h < a->font->height + 10)
			bar.h = a->font->height + 12;
		if (bar.h > 64)
			bar.h = 64;
	}

	bar.menu_path = rui_config_path("menu.conf");
	menu_refresh();

	bar.win = rui_window_new(a, a->root, 0, a->sh - bar.h, a->sw, bar.h, 0,
	    ExposureMask | ButtonPressMask | StructureNotifyMask);
	bar.s = rui_surf_new(a, bar.win, a->sw, bar.h);
	rui_set_title(a, bar.win, "retro-bar", "retro-bar");
	rui_set_window_type(a, bar.win, A__NET_WM_WINDOW_TYPE_DOCK);
	v[0] = ALL_DESKS;
	rui_set_prop32(a, bar.win, A(_NET_WM_DESKTOP), XA_CARDINAL, v, 1);
	v[0] = A(_NET_WM_STATE_STICKY);
	v[1] = A(_NET_WM_STATE_SKIP_TASKBAR);
	rui_set_prop32(a, bar.win, A(_NET_WM_STATE), XA_ATOM, v, 2);
	memset(&wmh, 0, sizeof wmh);
	wmh.flags = InputHint;
	wmh.input = False;
	XSetWMHints(a->dpy, bar.win, &wmh);
	place();

	XSelectInput(a->dpy, a->root, PropertyChangeMask | StructureNotifyMask);
	rui_monitors_watch(a);
	a->dispatch = dispatch_cb;
	root_props_refresh();
	layout_refresh();
	tasks_refresh();
	XMapWindow(a->dpy, bar.win);
	tray_init();

	/* only the modules of the applets in use are started */
	if (has_applet(AP_NET))
		bar.net_mod = rui_net_detect(bar.net_forced);
	if (has_applet(AP_VOL))
		rui_audio_init(&bar.audio, bar.audio_forced, audio_changed, NULL);
	if (has_applet(AP_BT)) {
		bar.bt_mod = rui_bt_detect(bar.bt_forced);
		poll_bt();
	}
	poll_clock();
	poll_cpu_mem();
	if (has_applet(AP_BAT))
		poll_battery();
	if (has_applet(AP_NET))
		poll_net();
}

/* ------------------------------------------------------------- the loop */

/* The panel sleeps until something is due instead of waking twice a
 * second, asks the sound system only when it reports a change, and runs
 * the network and Bluetooth tools only when the links changed (or once a
 * minute, to be sure). */
static int audio_watch;                    /* rui_watch_start id, 0 none */
static long long audio_watch_retry, audio_kick;

static void
on_audio_event(const char *line, int status, void *ud)
{
	(void)status;
	(void)ud;
	if (!line) {                           /* the monitor went away */
		audio_watch = 0;
		audio_watch_retry = rui_now_ms() + 30000;
		return;
	}
	/* pactl subscribe also reports clients coming and going (our own
	 * pactl calls among them): only sinks and the server matter */
	if (strstr(line, "Event '") && !strstr(line, "on sink #") &&
	    !strstr(line, "on server"))
		return;
	if (!audio_kick)
		audio_kick = rui_now_ms() + 120;   /* events come in bursts */
}

/* when the clock text can next change */
static long long
clock_due(long long now)
{
	static const char *const secs = "STrXcs";
	const char *p;

	for (p = bar.clock_fmt; (p = strchr(p, '%')) && p[1]; p += 2)
		if (strchr(secs, p[1]))
			return now - now % 1000 + 1000;
	return now - now % 60000 + 60000 + 20;
}

static void
next_due(long long *due, long long when)
{
	if (when < *due)
		*due = when;
}

int
main(int argc, char **argv)
{
	long long t_clock = 0, t_cpu = 0, t_bat = 0, t_net = 0, t_audio = 0;
	long long t_bt = 0, t_net_full = 0, t_bt_full = 0;
	unsigned long net_fp = 0, bt_fp = 0;
	int i, xerrors;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-bar " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-bar [-v]\n");
		return 2;
	}
	rui_signals_init();
	bar.app = rui_open("retro-bar");
	setup();
	notify_init();
#ifdef __OpenBSD__
	/* wpath cpath: the programs folder is created on first use;
	 * route: getifaddrs(3) for the network applet's link state */
	if (pledge("stdio rpath wpath cpath proc exec vminfo route unix", NULL)
	    == -1)
		rui_die("pledge failed");
#endif
	bar.running = 1;
	while (bar.running && !rui_quit_requested()) {
		XEvent ev;
		long long now, due;
		int vol = has_applet(AP_VOL);

		while (bar.running && XPending(bar.dpy)) {
			XNextEvent(bar.dpy, &ev);
			handle_event(&ev);
		}
		now = rui_now_ms();
		if (now >= t_clock) {
			poll_clock();
			t_clock = clock_due(now);
		}
		if ((has_applet(AP_CPU) || has_applet(AP_MEM)) && now - t_cpu >= 2000) {
			t_cpu = now;
			poll_cpu_mem();
		}
		if (has_applet(AP_BAT) && now - t_bat >= 30000) {
			t_bat = now;
			poll_battery();
		}
		if (has_applet(AP_NET) && now - t_net >= 5000) {
			unsigned long fp = rui_net_fingerprint();

			t_net = now;
			/* (a missing Wi-Fi interface is looked for again when the
			 * links change, or after a minute) */
			if (!fp || fp != net_fp || now - t_net_full >= 60000 ||
			    (bar.net_busy == 0 && t_net_full == 0)) {
				net_fp = fp;
				t_net_full = now;
				poll_net();
			}
		}
		/* sound: follow the mixer's events when it has them */
		if (vol && bar.audio.mod && bar.audio.mod->watch_cmd &&
		    !audio_watch && now >= audio_watch_retry) {
			audio_watch = rui_watch_start(bar.audio.mod->watch_cmd,
			    on_audio_event, NULL);
			if (audio_watch < 0) {
				audio_watch = 0;
				audio_watch_retry = now + 30000;
			}
		}
		if (vol && audio_kick && now >= audio_kick) {
			audio_kick = 0;
			t_audio = now;
			rui_audio_refresh(&bar.audio);
		}
		if (vol && now - t_audio >= (audio_watch ? 60000 : 3000)) {
			t_audio = now;
			rui_audio_refresh(&bar.audio);
		}
		if (bar.bt_mod && (now - t_bt >= 8000 ||
		    (bt_force && now - t_bt >= 1500))) {
			unsigned long fp = rui_bt_fingerprint();

			t_bt = now;
			if (!fp || fp != bt_fp || bt_force || now - t_bt_full >= 60000) {
				bt_fp = fp;
				t_bt_full = now;
				poll_bt();
			}
		}
		poll_blocks(now);
		if (bar.fp && fp_busy())
			bar.dirty = 1;                     /* blink the busy light */
		notify_tick(now);
		if (bar.dirty)
			draw();
		if (!bar.running)
			break;
		/* sleep until the next thing is due */
		due = t_clock;
		if (has_applet(AP_CPU) || has_applet(AP_MEM))
			next_due(&due, t_cpu + 2000);
		if (has_applet(AP_BAT))
			next_due(&due, t_bat + 30000);
		if (has_applet(AP_NET))
			next_due(&due, t_net + 5000);
		if (vol)
			next_due(&due, audio_kick ? audio_kick : t_audio +
			    (audio_watch ? 60000 : 3000));
		if (vol && !audio_watch && bar.audio.mod && bar.audio.mod->watch_cmd)
			next_due(&due, audio_watch_retry);
		if (bar.bt_mod)
			next_due(&due, t_bt + (bt_force ? 1500 : 8000));
		next_due(&due, blocks_due());
		next_due(&due, notify_due());
		if (bar.fp && fp_busy())
			next_due(&due, now + 250);
		rui_wait(bar.app, due <= now ? 0 : (int)(due - now > 3600000 ?
		    3600000 : due - now));
	}
	cal_close();
	vol_close();
	tray_shutdown();
	notify_shutdown();
	for (i = 0; i < bar.ntasks; i++) {
		free(bar.tasks[i].title);
		image_free(bar.tasks[i].icon);
	}
	free(bar.tasks);
	for (i = 0; i < bar.nblocks; i++) {
		free(bar.blocks[i].cmd);
		free(bar.blocks[i].click);
	}
	rui_menu_free(bar.menu);
	free(bar.menu_path);
	rui_surf_free(bar.s);
	XDestroyWindow(bar.dpy, bar.win);
	xerrors = rui_xerror_count();
	rui_close(bar.app);
	if (getenv("RETRO_DEBUG"))
		fprintf(stderr, "retro-bar: exiting, %d X errors\n", xerrors);
	if (reload_requested && !rui_quit_requested()) {
		execvp(argv[0], argv);
		perror("retro-bar: reload");
		return 1;
	}
	return 0;
}
