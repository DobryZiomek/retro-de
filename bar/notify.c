/*
 * retro-bar on retro-bus: the notification system ("notify") and starting
 * programs for others ("launcher").
 *
 *   notify show SUMMARY [BODY [SECONDS [ICON [URGENCY [ACTIONS [APP
 *               [REPLACES]]]]]]]            -> the notification's number
 *          SECONDS   empty: notification_time; 0: until clicked
 *          ICON      pictogram name, image path or icon NAME (PNG/SVG/XPM)
 *          URGENCY   low | normal | critical (critical stays, and shows
 *                    even with "do not disturb")
 *          ACTIONS   key:Label|key2:Label2  - buttons; "default" is the
 *                    click on the popup itself
 *          REPLACES  the number of one to update in place
 *   notify close ID       notify history       notify clear
 *   notify count          notify center        (opens the list)
 *   launcher run COMMAND          (@terminal, @browser ... understood)
 *   launcher open FILE-OR-URL     browser, file manager or editor
 *
 * Events: notify.clicked ID, notify.action ID KEY, notify.closed ID
 * REASON (expired, dismissed, closed).  Every notification also goes into
 * a history of the last HIST_MAX, shown by the "notes" applet.
 */
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define MAX_NOTES 6
#define NOTE_W 340
#define BODY_LINES 6
#define PAD 8
#define MAX_ACTIONS 4
#define HIST_MAX 50
#define CENTER_W 400
#define CENTER_ROWS 10

enum { URG_LOW, URG_NORMAL, URG_CRITICAL };

typedef struct Action {
	char key[48], label[64];
	RuiRect r;
} Action;

typedef struct Note {
	unsigned id;
	char *summary, *body, *app;
	int urgency;
	int kind;                          /* pictogram, when no image       */
	Image *img;
	Action act[MAX_ACTIONS];
	int nact, has_default;
	Window win;
	RuiSurf *s;
	int w, h;
	long long expires;                 /* 0: until clicked               */
} Note;

typedef struct Past {
	unsigned id;
	time_t when;
	int urgency;
	char *app, *summary, *body;
} Past;

static Note notes[MAX_NOTES];
static int nnotes;
static unsigned next_id = 1;
static int active;
static Past hist[HIST_MAX];
static int nhist, unread;
static Window center;
static RuiSurf *center_s;
static RuiButton center_clear;

/* ----------------------------------------------------------------- layout */

/* breaks text into lines at most maxw wide; returns the count */
static int
wrap(const char *body, int maxw, char lines[][256], int max)
{
	RuiApp *a = bar.app;
	const char *p = body;
	int n = 0;

	while (*p && n < max) {
		const char *end = p, *brk = NULL;
		size_t len;

		while (*end && *end != '\n') {
			const char *nx = end + 1;

			while ((*nx & 0xc0) == 0x80)
				nx++;                          /* whole UTF-8 characters */
			if (rui_text_width(a, a->font, p, (int)(nx - p)) > maxw)
				break;
			if (*end == ' ')
				brk = end;
			end = nx;
		}
		if (*end && *end != '\n' && brk)
			end = brk;                         /* at the last space */
		if (end == p)
			end = p + 1;                       /* one huge character */
		len = (size_t)(end - p) < 255 ? (size_t)(end - p) : 255;
		memcpy(lines[n], p, len);
		lines[n][len] = '\0';
		n++;
		p = end;
		if (*p == ' ' || *p == '\n')
			p++;
	}
	if (*p && n == max) {                      /* more than fits */
		size_t l = strlen(lines[n - 1]);

		rui_strlcpy(lines[n - 1] + (l > 250 ? 250 : l), "...", 6);
	}
	return n;
}

static int
title_h(void)
{
	return bar.app->bold->height + 6;
}

static int
button_h(void)
{
	return bar.app->font->height + 10;
}

static void
note_draw(Note *nt)
{
	RuiApp *a = bar.app;
	RuiSurf *s = nt->s;
	char lines[BODY_LINES][256], title[300];
	int n, i, tx = PAD + RUI_ICON_SIZE + PAD, th = title_h();
	RuiRect r;

	rui_fill(s, 0, 0, nt->w, nt->h, RUI_C_FACE);
	rui_bevel(s, 0, 0, nt->w, nt->h, RUI_RAISED);
	rui_fill(s, 3, 3, nt->w - 6, th, nt->urgency == URG_CRITICAL ?
	    RUI_C_ALERT : nt->urgency == URG_LOW ? RUI_C_TITLE_INACTIVE :
	    RUI_C_TITLE_ACTIVE);
	r.x = 8;
	r.y = 3 + (th - a->bold->height) / 2;
	r.w = nt->w - 16;
	r.h = a->bold->height;
	if (nt->app && *nt->app)
		snprintf(title, sizeof title, "%s - %s", nt->app, nt->summary);
	else
		snprintf(title, sizeof title, "%s", nt->summary);
	rui_text_fit(s, a->bold, r, title, nt->urgency == URG_LOW ?
	    RUI_C_TITLE_FG_INACTIVE : RUI_C_TITLE_FG, RUI_LEFT);
	if (nt->img)
		image_draw(a, nt->img, s->buf, PAD + (RUI_ICON_SIZE - nt->img->w) / 2,
		    th + PAD + 3 + (RUI_ICON_SIZE - nt->img->h) / 2);
	else
		rui_icon_draw(s, nt->kind, PAD, th + PAD + 3);
	n = wrap(nt->body, nt->w - tx - PAD, lines, BODY_LINES);
	for (i = 0; i < n; i++)
		rui_text(s, a->font, tx, th + PAD + 3 + i * a->font->height,
		    lines[i], -1, RUI_C_TEXT);
	for (i = 0; i < nt->nact; i++) {
		RuiButton b;

		memset(&b, 0, sizeof b);
		b.r = nt->act[i].r;
		b.label = nt->act[i].label;
		rui_button_draw(s, &b);
	}
	rui_surf_flush(s);
}

static void
note_layout(Note *nt)
{
	RuiApp *a = bar.app;
	char lines[BODY_LINES][256];
	int n = wrap(nt->body, nt->w - (PAD + RUI_ICON_SIZE + PAD) - PAD, lines,
	    BODY_LINES), body = n * a->font->height, i, x;

	if (body < RUI_ICON_SIZE)
		body = RUI_ICON_SIZE;
	nt->h = 3 + title_h() + PAD + body + PAD + 3;
	if (!nt->nact)
		return;
	/* buttons right-aligned under the text */
	x = nt->w - PAD - 3;
	for (i = nt->nact - 1; i >= 0; i--) {
		int bw = rui_text_width(a, a->font, nt->act[i].label, -1) + 20;

		if (bw < 64)
			bw = 64;
		x -= bw;
		nt->act[i].r.x = x;
		nt->act[i].r.y = nt->h - 3;
		nt->act[i].r.w = bw;
		nt->act[i].r.h = button_h();
		x -= 6;
	}
	nt->h += button_h() + PAD;
}

/* stacked in the corner beside the panel's clock, newest nearest */
static void
restack(void)
{
	int i, y = bar.top ? bar.y + bar.h + 6 : bar.y - 6;

	for (i = nnotes - 1; i >= 0; i--) {
		Note *nt = &notes[i];
		int x = bar.x + bar.w - nt->w - 6;

		if (!bar.top)
			y -= nt->h;
		XMoveResizeWindow(bar.dpy, nt->win, x < 0 ? 0 : x, y,
		    (unsigned)nt->w, (unsigned)nt->h);
		y += bar.top ? nt->h + 4 : -4;
	}
}

/* --------------------------------------------------------------- history */

static void
center_draw(void);

static void
hist_add(const Note *nt)
{
	Past *p;
	int i;

	for (i = 0; i < nhist; i++)              /* an update replaces it */
		if (hist[i].id == nt->id)
			break;
	if (i == nhist) {
		if (nhist == HIST_MAX) {
			free(hist[0].app);
			free(hist[0].summary);
			free(hist[0].body);
			memmove(&hist[0], &hist[1], (HIST_MAX - 1) * sizeof hist[0]);
			nhist--;
		}
		i = nhist++;
		unread++;
	} else {
		free(hist[i].app);
		free(hist[i].summary);
		free(hist[i].body);
	}
	p = &hist[i];
	p->id = nt->id;
	p->when = time(NULL);
	p->urgency = nt->urgency;
	p->app = rui_strdup(nt->app ? nt->app : "");
	p->summary = rui_strdup(nt->summary);
	p->body = rui_strdup(nt->body);
	bar.dirty = 1;
	if (center)
		center_draw();
}

static void
hist_clear(void)
{
	int i;

	for (i = 0; i < nhist; i++) {
		free(hist[i].app);
		free(hist[i].summary);
		free(hist[i].body);
	}
	nhist = unread = 0;
	bar.dirty = 1;
}

int
notify_unread(void)
{
	return unread;
}

static int
center_row_h(void)
{
	return bar.app->bold->height + bar.app->font->height + 8;
}

static void
center_draw(void)
{
	RuiApp *a = bar.app;
	RuiSurf *s = center_s;
	int w = s->w, h = s->h, rh = center_row_h(), i, y = 3 + title_h() + 4;
	RuiRect r;

	rui_fill(s, 0, 0, w, h, RUI_C_FACE);
	rui_bevel(s, 0, 0, w, h, RUI_RAISED);
	rui_fill(s, 3, 3, w - 6, title_h(), RUI_C_TITLE_ACTIVE);
	r.x = 8;
	r.y = 3 + (title_h() - a->bold->height) / 2;
	r.w = w - 16;
	r.h = a->bold->height;
	rui_text_fit(s, a->bold, r, "Notifications", RUI_C_TITLE_FG, RUI_LEFT);
	rui_fill(s, 6, y, w - 12, CENTER_ROWS * rh + 4, RUI_C_FIELD);
	rui_bevel(s, 6, y, w - 12, CENTER_ROWS * rh + 4, RUI_SUNKEN_THIN);
	if (!nhist) {
		r.x = 6;
		r.y = y + CENTER_ROWS * rh / 2 - a->font->height / 2;
		r.w = w - 12;
		r.h = a->font->height;
		rui_text_fit(s, a->font, r, "No notifications.", RUI_C_DISABLED,
		    RUI_CENTER);
	}
	for (i = 0; i < nhist && i < CENTER_ROWS; i++) {
		const Past *p = &hist[nhist - 1 - i];
		char head[400], when[16], *nl;
		struct tm tm;

		localtime_r(&p->when, &tm);
		strftime(when, sizeof when, "%H:%M", &tm);
		snprintf(head, sizeof head, "%s  %s%s%s", when, p->app,
		    *p->app ? ": " : "", p->summary);
		r.x = 12;
		r.y = y + 4 + i * rh;
		r.w = w - 24;
		r.h = a->bold->height;
		rui_text_fit(s, a->bold, r, head, p->urgency == URG_CRITICAL ?
		    RUI_C_ALERT : RUI_C_TEXT, RUI_LEFT);
		rui_strlcpy(head, p->body, sizeof head);
		if ((nl = strchr(head, '\n')))
			*nl = '\0';
		r.y += a->bold->height;
		r.h = a->font->height;
		r.x += 10;
		r.w -= 10;
		rui_text_fit(s, a->font, r, head, RUI_C_DISABLED, RUI_LEFT);
		if (i + 1 < nhist && i + 1 < CENTER_ROWS)
			rui_fill(s, 10, y + 4 + (i + 1) * rh - 3, w - 20, 1,
			    RUI_C_SHADOW);
	}
	rui_button_draw(s, &center_clear);
	rui_surf_flush(s);
}

static void
center_close(void)
{
	if (!center)
		return;
	rui_surf_free(center_s);
	XDestroyWindow(bar.dpy, center);
	center = None;
	center_s = NULL;
}

void
notify_center_toggle(int anchor_x)
{
	int w = CENTER_W, h, x, y;

	if (center) {
		center_close();
		return;
	}
	h = 3 + title_h() + 4 + CENTER_ROWS * center_row_h() + 4 + 8 +
	    button_h() + 8;
	x = anchor_x - w / 2;
	if (x + w > bar.x + bar.w)
		x = bar.x + bar.w - w - 2;
	if (x < bar.x)
		x = bar.x;
	y = bar.top ? bar.y + bar.h + 2 : bar.y - h - 2;
	center_clear.label = "Clear all";
	center_clear.r.w = 90;
	center_clear.r.h = button_h();
	center_clear.r.x = w - 8 - center_clear.r.w;
	center_clear.r.y = h - 8 - button_h();
	center = rui_window_new(bar.app, bar.app->root, x, y, w, h, 1,
	    ExposureMask | ButtonPressMask | ButtonReleaseMask);
	center_s = rui_surf_new(bar.app, center, w, h);
	XMapRaised(bar.dpy, center);
	unread = 0;
	bar.dirty = 1;
	center_draw();
}

/* ---------------------------------------------------------------- notes */

static void
note_free(Note *nt)
{
	if (nt->s)
		rui_surf_free(nt->s);
	if (nt->win)
		XDestroyWindow(bar.dpy, nt->win);
	image_free(nt->img);
	free(nt->summary);
	free(nt->body);
	free(nt->app);
}

static void
note_close(int i, const char *reason)
{
	char buf[64];

	snprintf(buf, sizeof buf, "%u\t%s", notes[i].id, reason);
	note_free(&notes[i]);
	memmove(&notes[i], &notes[i + 1], (size_t)(nnotes - i - 1) *
	    sizeof notes[0]);
	nnotes--;
	restack();
	rui_bus_emit("notify.closed", buf);
}

static Image *
note_image(const char *spec)
{
	char *path = NULL;
	Image *img;

	if (!spec || !*spec)
		return NULL;
	if (spec[0] == '/')
		path = rui_strdup(spec);
	else if (!strncmp(spec, "file://", 7))
		path = rui_strdup(spec + 7);
	else if (spec[0] == '~' && spec[1] == '/' && getenv("HOME"))
		path = rui_strdupf("%s%s", getenv("HOME"), spec + 1);
	else if (!(path = rui_icon_find(spec)))
		path = rui_icon_lookup(spec);
	if (!path)
		return NULL;
	img = image_load_size(bar.app, path, RUI_ICON_SIZE);
	free(path);
	return img;
}

/* "key:Label|key2:Label2" */
static void
parse_actions(Note *nt, const char *spec)
{
	char buf[512], *tok, *save = NULL;

	nt->nact = 0;
	nt->has_default = 0;
	if (!spec || !*spec)
		return;
	rui_strlcpy(buf, spec, sizeof buf);
	for (tok = strtok_r(buf, "|", &save); tok;
	    tok = strtok_r(NULL, "|", &save)) {
		char *colon = strchr(tok, ':');
		const char *label = colon ? colon + 1 : tok;

		if (colon)
			*colon = '\0';
		if (!strcmp(tok, "default")) {
			nt->has_default = 1;
			continue;
		}
		if (nt->nact == MAX_ACTIONS || !*tok)
			continue;
		rui_strlcpy(nt->act[nt->nact].key, tok, sizeof nt->act[0].key);
		rui_strlcpy(nt->act[nt->nact].label, *label ? label : tok,
		    sizeof nt->act[0].label);
		nt->nact++;
	}
}

static int
urgency_of(const char *s)
{
	if (!s || !*s)
		return URG_NORMAL;
	if (!strcmp(s, "low") || !strcmp(s, "0"))
		return URG_LOW;
	if (!strcmp(s, "critical") || !strcmp(s, "2"))
		return URG_CRITICAL;
	return URG_NORMAL;
}

/* f: summary body seconds icon urgency actions app replaces */
static unsigned
note_show(char **f)
{
	const RuiSettings *set = rui_settings();
	unsigned replaces = (unsigned)strtoul(f[7], NULL, 10);
	int urgency = urgency_of(f[4]), t, i, fresh = 1;
	Note *nt = NULL, tmp;

	for (i = 0; replaces && i < nnotes; i++)
		if (notes[i].id == replaces) {
			nt = &notes[i];
			fresh = 0;
		}
	memset(&tmp, 0, sizeof tmp);
	tmp.id = replaces ? replaces : next_id++;
	if (replaces >= next_id)
		next_id = replaces + 1;
	tmp.summary = rui_strdup(f[0]);
	tmp.body = rui_strdup(f[1]);
	tmp.app = rui_strdup(f[6]);
	tmp.urgency = urgency;
	hist_add(&tmp);
	if (center)
		unread = 0;
	if (bar.dnd && urgency != URG_CRITICAL) {   /* do not disturb */
		free(tmp.summary);
		free(tmp.body);
		free(tmp.app);
		return tmp.id;
	}
	if (fresh) {
		if (nnotes == MAX_NOTES)
			note_close(0, "expired");      /* the oldest makes room */
		nt = &notes[nnotes++];
		memset(nt, 0, sizeof *nt);
	} else {
		free(nt->summary);
		free(nt->body);
		free(nt->app);
		image_free(nt->img);
		nt->img = NULL;
	}
	nt->id = tmp.id;
	nt->summary = tmp.summary;
	nt->body = tmp.body;
	nt->app = tmp.app;
	nt->urgency = urgency;
	nt->kind = *f[3] ? rui_icon_kind(f[3]) : -1;
	if (nt->kind < 0) {
		nt->img = note_image(f[3]);
		nt->kind = RUI_ICON_INFO;
	}
	parse_actions(nt, f[5]);
	t = *f[2] ? atoi(f[2]) : urgency == URG_CRITICAL ? 0 : set->notify_seconds;
	nt->expires = t > 0 ? rui_now_ms() + t * 1000LL : 0;
	nt->w = NOTE_W;
	note_layout(nt);
	if (fresh) {
		nt->win = rui_window_new(bar.app, bar.app->root, 0, 0, nt->w, nt->h,
		    1, ExposureMask | ButtonPressMask);
		nt->s = rui_surf_new(bar.app, nt->win, nt->w, nt->h);
	} else {
		rui_surf_resize(nt->s, nt->w, nt->h);
	}
	restack();
	XMapRaised(bar.dpy, nt->win);
	note_draw(nt);
	return nt->id;
}

/* -------------------------------------------------------------- the bus */

static void
launcher(const RuiBusMsg *m)
{
	char *cmd = NULL;

	if (!*m->args) {
		rui_bus_reply(m->id, 0, "what?");
		return;
	}
	if (!strcmp(m->name, "run")) {
		cmd = rui_strdup(m->args);
	} else if (!strcmp(m->name, "open")) {
		const char *target = m->args;
		char *q;
		struct stat st;

		if (!strncmp(target, "file://", 7))
			target += 7;
		q = rui_shell_quote(target);
		if (strstr(target, "://") || !strncmp(target, "mailto:", 7))
			cmd = rui_strdupf("@browser %s", q);
		else if (stat(target, &st) == 0 && S_ISDIR(st.st_mode))
			cmd = rui_strdupf("@filemanager %s", q);
		else
			cmd = rui_strdupf("@editor %s", q);
		free(q);
	} else {
		rui_bus_reply(m->id, 0, "no such method");
		return;
	}
	rui_run_command(bar.app, cmd);
	free(cmd);
	rui_bus_reply(m->id, 1, "");
}

static void
on_bus(const RuiBusMsg *m, void *ud)
{
	char *f[8] = { "", "", "", "", "", "", "", "" }, buf[32];
	int i;

	(void)ud;
	if (strcmp(m->kind, "call"))
		return;
	if (!strcmp(m->service, "launcher")) {
		launcher(m);
		return;
	}
	if (!strcmp(m->name, "show")) {
		rui_bus_fields(m->args, f, 8);
		if (!*f[0]) {
			rui_bus_reply(m->id, 0, "no summary");
			return;
		}
		snprintf(buf, sizeof buf, "%u", note_show(f));
		rui_bus_reply(m->id, 1, buf);
	} else if (!strcmp(m->name, "close")) {
		unsigned id = (unsigned)strtoul(m->args, NULL, 10);

		for (i = 0; i < nnotes; i++) {
			if (notes[i].id == id) {
				note_close(i, "closed");
				break;
			}
		}
		rui_bus_reply(m->id, 1, "");
	} else if (!strcmp(m->name, "history")) {
		size_t len = 0, cap = 256;
		char *out = rui_malloc(cap);

		out[0] = '\0';
		for (i = 0; i < nhist; i++) {
			char *line = rui_strdupf("%s%u\t%ld\t%s\t%s\t%s", len ? "\n" : "",
			    hist[i].id, (long)hist[i].when, hist[i].app, hist[i].summary,
			    hist[i].body);
			size_t n = strlen(line);

			if (len + n + 1 > cap)
				out = rui_realloc(out, cap = (len + n + 1) * 2);
			memcpy(out + len, line, n + 1);
			len += n;
			free(line);
		}
		rui_bus_reply(m->id, 1, out);
		free(out);
	} else if (!strcmp(m->name, "clear")) {
		hist_clear();
		if (center)
			center_draw();
		rui_bus_reply(m->id, 1, "");
	} else if (!strcmp(m->name, "count")) {
		snprintf(buf, sizeof buf, "%d\t%d", nhist, unread);
		rui_bus_reply(m->id, 1, buf);
	} else if (!strcmp(m->name, "center")) {
		notify_center_toggle(bar.x + bar.w - CENTER_W / 2);
		rui_bus_reply(m->id, 1, "");
	} else {
		rui_bus_reply(m->id, 0, "no such method");
	}
}

void
notify_init(void)
{
	const RuiSettings *set = rui_settings();

	if (!set->bus)
		return;
	active = 1;
	rui_bus_handler(on_bus, NULL);
	if (set->notify)
		rui_bus_claim("notify");
	rui_bus_claim("launcher");
}

int
notify_event(XEvent *ev)
{
	Window w = ev->xany.window;
	char buf[96];
	int i, k;

	if (center && w == center) {
		if (ev->type == Expose && ev->xexpose.count == 0)
			rui_surf_flush(center_s);
		else if (ev->type == ButtonPress || ev->type == ButtonRelease) {
			int r = rui_button_event(&center_clear, ev);

			if (r & RUI_EV_ACTIVATE) {
				hist_clear();
				center_draw();
			} else if (r) {
				center_draw();
			} else if (ev->type == ButtonPress) {
				center_close();                /* a click elsewhere */
			}
		}
		return 1;
	}
	for (i = 0; i < nnotes; i++) {
		Note *nt = &notes[i];

		if (nt->win != w)
			continue;
		if (ev->type == Expose && ev->xexpose.count == 0) {
			rui_surf_flush(nt->s);
		} else if (ev->type == ButtonPress) {
			for (k = 0; k < nt->nact; k++) {
				if (rui_in_rect(nt->act[k].r, ev->xbutton.x, ev->xbutton.y)) {
					snprintf(buf, sizeof buf, "%u\t%s", nt->id,
					    nt->act[k].key);
					rui_bus_emit("notify.action", buf);
					note_close(i, "dismissed");
					return 1;
				}
			}
			/* the first button "clicks" it, the others just dismiss */
			if (ev->xbutton.button == Button1) {
				snprintf(buf, sizeof buf, "%u", nt->id);
				rui_bus_emit("notify.clicked", buf);
				if (nt->has_default) {
					snprintf(buf, sizeof buf, "%u\tdefault", nt->id);
					rui_bus_emit("notify.action", buf);
				}
			}
			note_close(i, "dismissed");
		}
		return 1;
	}
	return 0;
}

void
notify_tick(long long now)
{
	int i;

	if (!active)
		return;
	for (i = 0; i < nnotes; i++) {
		if (notes[i].expires && now >= notes[i].expires) {
			note_close(i, "expired");
			i--;
		}
	}
}

void
notify_shutdown(void)
{
	while (nnotes > 0)
		note_free(&notes[--nnotes]);
	center_close();
	hist_clear();
	active = 0;
}
