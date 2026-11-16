/*
 * retro-display - monitors: which are on, where they are, and each one's
 * resolution, refresh rate and rotation.  Every monitor has its own mode,
 * so a 144 Hz screen next to a 60 Hz one both run at their own rate.
 *
 *   retro-display                  the dialog
 *   retro-display --list           outputs and their modes
 *   retro-display --restore        apply ~/.config/retro-de/monitors.conf
 *                                  (retro-session does this at login)
 *   retro-display --profiles       names of the saved profiles
 *   retro-display --profile NAME   switch to a profile ("docked", "tv" ...)
 *   retro-display --save-profile NAME      the layout in use, under a name
 *   retro-display --delete-profile NAME
 *   retro-display --apply LINE...  apply and save monitors.conf lines, e.g.
 *        retro-display --apply "output HDMI-1 mode 1920x1080 rate 144 pos 0x0 primary" \
 *                              "output eDP-1 mode 1920x1080 rate 60 pos 1920x0"
 *
 * A profile is a whole layout under a name of your own, kept in
 * ~/.config/retro-de/monitors/NAME.conf: make one per situation and switch
 * from the Profiles button, a menu entry or a key
 * (bind = Mod4+p exec retro-display --profile docked).
 *
 * Talks RandR directly; no xrandr(1) needed.  A new layout must be
 * confirmed within 15 seconds or the old one comes back.
 */
#include "randr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define WIN_W 600
#define WIN_H 500
#define CONFIRM_MS 15000
#define IDENT_MS 2500
#define MAX_RES 96

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty;
	DspLayout cur, edit, prev;         /* on screen, being edited, undo  */
	int sel;                           /* selected output                */
	RuiRect pv;                        /* preview area                   */
	RuiRect box[DSP_MAX_OUT];          /* outputs inside the preview     */
	double scale;
	int ox, oy;                        /* preview origin                 */
	int drag, drag_dx, drag_dy;
	RuiList res_list, rate_list;
	char *res_items[MAX_RES], *rate_items[DSP_MAX_MODES];
	int res_w[MAX_RES], res_h[MAX_RES], nres;
	int rate_mode[DSP_MAX_MODES], nrate;
	RuiButton b_on, b_primary, b_rot, b_ident, b_apply, b_revert, b_close;
	RuiButton b_prof;
	RuiInput name;                     /* "save profile as" prompt       */
	int naming;
	char rot_label[40];
	long long confirm_until;           /* 0: nothing to confirm          */
	Window ident[DSP_MAX_OUT];
	RuiSurf *ident_s[DSP_MAX_OUT];
	int nident;
	long long ident_until;
	char msg[200];
} dp;

static void
set_msg(const char *m)
{
	rui_strlcpy(dp.msg, m, sizeof dp.msg);
	dp.dirty = 1;
}

static char *
conf_path(void)
{
	mkdir(rui_config_dir(), 0755);         /* fine if it already exists */
	return rui_config_path("monitors.conf");
}

/* --------------------------------------------------------------- profiles */

static int
profile_name_ok(const char *n)
{
	return *n && strlen(n) <= 48 && !strchr(n, '/') && n[0] != '.';
}

static char *
profile_path(const char *name)
{
	char *dir = rui_config_path("monitors"), *p;

	mkdir(rui_config_dir(), 0755);
	mkdir(dir, 0755);
	p = rui_strdupf("%s/%s.conf", dir, name);
	free(dir);
	return p;
}

/* NULL-terminated, sorted, malloc'd */
static char **
profile_list(void)
{
	char *dir = rui_config_path("monitors"), **v;
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0, cap = 16, i, j;

	v = rui_malloc((size_t)cap * sizeof *v);
	while (d && (de = readdir(d))) {
		size_t len = strlen(de->d_name);

		if (de->d_name[0] == '.' || len <= 5 ||
		    strcmp(de->d_name + len - 5, ".conf"))
			continue;
		if (n + 2 > cap) {
			cap *= 2;
			v = rui_realloc(v, (size_t)cap * sizeof *v);
		}
		v[n] = rui_strdup(de->d_name);
		v[n++][len - 5] = '\0';
	}
	if (d)
		closedir(d);
	free(dir);
	for (i = 1; i < n; i++)                /* few entries: insertion sort */
		for (j = i; j > 0 && strcmp(v[j - 1], v[j]) > 0; j--) {
			char *t = v[j];

			v[j] = v[j - 1];
			v[j - 1] = t;
		}
	v[n] = NULL;
	return v;
}

/* ------------------------------------------------------------------ lists */

static void
free_items(void)
{
	int i;

	for (i = 0; i < dp.nres; i++)
		free(dp.res_items[i]);
	for (i = 0; i < dp.nrate; i++)
		free(dp.rate_items[i]);
	dp.nres = dp.nrate = 0;
}

/* resolution and refresh-rate lists of the selected output */
static void
build_lists(void)
{
	DspOutput *o;
	int i, k, mw = 0, mh = 0;

	free_items();
	dp.res_list.items = dp.res_items;
	dp.rate_list.items = dp.rate_items;
	dp.res_list.sel = dp.rate_list.sel = -1;
	dp.res_list.n = dp.rate_list.n = 0;
	if (dp.sel < 0 || dp.sel >= dp.edit.n)
		return;
	o = &dp.edit.out[dp.sel];
	if (o->mode >= 0 && o->mode < o->nmodes) {
		mw = o->modes[o->mode].w;
		mh = o->modes[o->mode].h;
	} else if (o->preferred >= 0 && o->preferred < o->nmodes) {
		mw = o->modes[o->preferred].w;
		mh = o->modes[o->preferred].h;
	}
	for (i = 0; i < o->nmodes; i++) {
		const DspMode *m = &o->modes[i];

		for (k = 0; k < dp.nres; k++)
			if (dp.res_w[k] == m->w && dp.res_h[k] == m->h)
				break;
		if (k == dp.nres && dp.nres < MAX_RES) {
			int native = o->preferred >= 0 && o->modes[o->preferred].w ==
			    m->w && o->modes[o->preferred].h == m->h;

			dp.res_w[k] = m->w;
			dp.res_h[k] = m->h;
			dp.res_items[dp.nres++] = rui_strdupf("%d x %d%s", m->w, m->h,
			    native ? "  (native)" : "");
		}
		if (m->w == mw && m->h == mh) {
			if (i == o->mode || (o->mode < 0 && i == o->preferred))
				dp.rate_list.sel = dp.nrate;
			dp.rate_mode[dp.nrate] = i;
			/* virtual servers (Xvfb, VNC) have modes without a clock */
			dp.rate_items[dp.nrate++] = m->rate < 1.0 ?
			    rui_strdup("default") : rui_strdupf("%.2f Hz%s", m->rate,
			    m->interlaced ? "  interlaced" : "");
		}
	}
	for (k = 0; k < dp.nres; k++)
		if (dp.res_w[k] == mw && dp.res_h[k] == mh)
			dp.res_list.sel = k;
	dp.res_list.n = dp.nres;
	dp.rate_list.n = dp.nrate;
	dp.res_list.top = dp.rate_list.top = 0;
	if (dp.res_list.sel > 5)
		dp.res_list.top = dp.res_list.sel - 2;
	if (dp.rate_list.sel > 5)
		dp.rate_list.top = dp.rate_list.sel - 2;
}

static void
select_output(int i)
{
	dp.sel = i;
	build_lists();
	dp.dirty = 1;
}

/* re-reads the server's state; the editor follows unless it has changes */
static void
requery(int reset_edit)
{
	DspLayout old = dp.cur;
	int follow = reset_edit || dsp_equal(&old, &dp.edit) ||
	    dp.edit.n != old.n;

	if (dsp_query(dp.app->dpy, &dp.cur) < 0) {
		set_msg("The X server reports no monitors (no RandR).");
		dp.cur.n = 0;
	}
	if (follow)
		dp.edit = dp.cur;
	if (dp.sel >= dp.edit.n)
		dp.sel = dp.edit.n - 1;
	if (dp.sel < 0 && dp.edit.n)
		dp.sel = 0;
	build_lists();
	dp.dirty = 1;
}

/* ---------------------------------------------------------------- preview */

static void
layout_preview(void)
{
	int i, bw, bh, off = 0, noff = 0;
	int aw = dp.pv.w - 40, ah = dp.pv.h - 40;

	for (i = 0; i < dp.edit.n; i++)
		noff += !dp.edit.out[i].enabled;
	if (noff)
		ah -= 50;                          /* a row for the ones off */
	if (!dp.drag) {
		double sx, sy;

		dsp_bounds(&dp.edit, &bw, &bh);
		if (bw < 1)
			bw = 1;
		if (bh < 1)
			bh = 1;
		sx = (double)aw / bw;
		sy = (double)ah / bh;
		dp.scale = sx < sy ? sx : sy;
		dp.ox = dp.pv.x + 20 + (int)((aw - bw * dp.scale) / 2);
		dp.oy = dp.pv.y + 20 + (int)((ah - bh * dp.scale) / 2);
	}
	for (i = 0; i < dp.edit.n; i++) {
		const DspOutput *o = &dp.edit.out[i];
		int w, h;

		if (o->enabled) {
			dsp_size(o, &w, &h);
			dp.box[i].x = dp.ox + (int)(o->x * dp.scale);
			dp.box[i].y = dp.oy + (int)(o->y * dp.scale);
			dp.box[i].w = (int)(w * dp.scale);
			dp.box[i].h = (int)(h * dp.scale);
			if (dp.box[i].w < 24)
				dp.box[i].w = 24;
			if (dp.box[i].h < 18)
				dp.box[i].h = 18;
		} else {
			dp.box[i].x = dp.pv.x + 12 + off * 98;
			dp.box[i].y = dp.pv.y + dp.pv.h - 48;
			dp.box[i].w = 90;
			dp.box[i].h = 38;
			off++;
		}
	}
}

static void
draw_preview(void)
{
	RuiSurf *s = dp.s;
	RuiApp *a = dp.app;
	int i, pass;

	rui_fill(s, dp.pv.x, dp.pv.y, dp.pv.w, dp.pv.h, RUI_C_DESKTOP);
	rui_bevel(s, dp.pv.x, dp.pv.y, dp.pv.w, dp.pv.h, RUI_SUNKEN);
	layout_preview();
	/* the selected monitor is drawn last so it stays on top while dragged */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < dp.edit.n; i++) {
			const DspOutput *o = &dp.edit.out[i];
			RuiRect b = dp.box[i], r;
			char buf[96];
			int sel = i == dp.sel, fg;

			if ((pass == 1) != sel)
				continue;
			if (!o->enabled) {
				rui_fill_dither(s, b.x, b.y, b.w, b.h, RUI_C_FACE,
				    RUI_C_SHADOW);
				fg = RUI_C_TEXT;
			} else {
				rui_fill(s, b.x, b.y, b.w, b.h, sel ? RUI_C_TITLE_ACTIVE :
				    RUI_C_FACE);
				fg = sel ? RUI_C_TITLE_FG : RUI_C_TEXT;
			}
			rui_bevel(s, b.x, b.y, b.w, b.h, RUI_RAISED);
			if (sel)
				rui_bevel_c(s, b.x - 2, b.y - 2, b.w + 4, b.h + 4, 1,
				    RUI_C_DARK, RUI_C_DARK, 1);
			r = b;
			r.x += 4;
			r.w -= 8;
			if (o->enabled && b.h >= 3 * a->font->height + 6) {
				r.h = b.h / 2;
				r.y = b.y + b.h / 2 - a->bold->height - 1;
				r.h = a->bold->height;
				snprintf(buf, sizeof buf, "%d  %s%s", i + 1, o->name,
				    o->primary ? " *" : "");
				rui_text_fit(s, a->bold, r, buf, fg, RUI_CENTER);
				r.y += a->bold->height + 2;
				r.h = a->font->height;
				if (o->mode >= 0 && o->modes[o->mode].rate >= 1.0)
					snprintf(buf, sizeof buf, "%dx%d  %.0f Hz",
					    o->modes[o->mode].w, o->modes[o->mode].h,
					    o->modes[o->mode].rate);
				else if (o->mode >= 0)
					snprintf(buf, sizeof buf, "%dx%d", o->modes[o->mode].w,
					    o->modes[o->mode].h);
				else
					buf[0] = '\0';
				rui_text_fit(s, a->font, r, buf, fg, RUI_CENTER);
			} else {
				snprintf(buf, sizeof buf, o->enabled ? "%d %s" : "%d %s off",
				    i + 1, o->name);
				rui_text_fit(s, a->font, r, buf, fg, RUI_CENTER);
			}
		}
	}
}

/* ------------------------------------------------------------------- draw */

static void
draw(void)
{
	static const char *const rot_names[] = {
		"normal", "left", "upside down", "right"
	};
	RuiSurf *s = dp.s;
	RuiApp *a = dp.app;
	const DspOutput *o = dp.sel >= 0 ? &dp.edit.out[dp.sel] : NULL;
	int confirm = dp.confirm_until != 0, changed;
	RuiRect r;

	rui_fill(s, 0, 0, WIN_W, WIN_H, RUI_C_FACE);
	draw_preview();

	r.x = dp.res_list.r.x;
	r.y = dp.res_list.r.y - a->font->height - 6;
	r.w = dp.res_list.r.w;
	r.h = a->font->height + 4;
	rui_text_fit(s, a->bold, r, "Resolution", RUI_C_TEXT, RUI_LEFT);
	r.x = dp.rate_list.r.x;
	r.w = dp.rate_list.r.w;
	rui_text_fit(s, a->bold, r, "Refresh rate", RUI_C_TEXT, RUI_LEFT);
	r.x = dp.b_on.r.x;
	r.w = dp.b_on.r.w;
	rui_text_fit(s, a->bold, r, o ? o->name : "No monitor", RUI_C_TEXT,
	    RUI_LEFT);
	rui_list_draw(s, &dp.res_list);
	rui_list_draw(s, &dp.rate_list);

	dp.b_on.label = o && o->enabled ? "Switched on" : "Switched off";
	dp.b_on.toggled = o && o->enabled;
	dp.b_primary.toggled = o && o->primary;
	snprintf(dp.rot_label, sizeof dp.rot_label, "Rotation: %s",
	    rot_names[o ? (o->rot / 90) % 4 : 0]);
	dp.b_rot.label = dp.rot_label;
	dp.b_on.disabled = !o || confirm;
	dp.b_primary.disabled = dp.b_rot.disabled = !o || !o->enabled || confirm;
	dp.b_ident.disabled = dp.cur.n == 0;
	dp.b_prof.disabled = dp.cur.n == 0 || confirm;
	rui_button_draw(s, &dp.b_on);
	rui_button_draw(s, &dp.b_primary);
	rui_button_draw(s, &dp.b_rot);
	rui_arrow(s, dp.b_rot.r.x + dp.b_rot.r.w - 12, dp.b_rot.r.y +
	    dp.b_rot.r.h / 2, 4, 1, dp.b_rot.disabled ? RUI_C_DISABLED :
	    RUI_C_TEXT);
	rui_button_draw(s, &dp.b_ident);
	rui_button_draw(s, &dp.b_prof);
	rui_arrow(s, dp.b_prof.r.x + dp.b_prof.r.w - 12, dp.b_prof.r.y +
	    dp.b_prof.r.h / 2, 4, 1, dp.b_prof.disabled ? RUI_C_DISABLED :
	    RUI_C_TEXT);

	changed = !dsp_equal(&dp.cur, &dp.edit);
	dp.b_apply.label = confirm ? "Keep" : "Apply";
	dp.b_apply.disabled = !confirm && !changed;
	dp.b_revert.label = confirm ? "Revert" : "Reset";
	dp.b_revert.disabled = !confirm && !changed;
	rui_button_draw(s, &dp.b_apply);
	rui_button_draw(s, &dp.b_revert);
	rui_button_draw(s, &dp.b_close);

	r.x = 8;
	r.y = WIN_H - a->font->height - 12;
	r.w = WIN_W - 16;
	r.h = a->font->height + 8;
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN_THIN);
	r.x += 6;
	r.w -= 12;
	if (dp.naming) {
		RuiRect l = r;

		l.w = 150;
		rui_text_fit(s, a->bold, l, "Save profile as:", RUI_C_TEXT, RUI_LEFT);
		dp.name.r.x = r.x + 154;
		dp.name.r.y = r.y + 1;
		dp.name.r.w = r.w - 154;
		dp.name.r.h = r.h - 2;
		dp.name.focused = 1;
		rui_input_draw(s, &dp.name);
	} else if (confirm) {
		char buf[160];
		long long left = (dp.confirm_until - rui_now_ms() + 999) / 1000;

		snprintf(buf, sizeof buf, "Keep this layout?  The old one returns "
		    "in %lld second%s.", left < 0 ? 0 : left, left == 1 ? "" : "s");
		rui_text_fit(s, a->bold, r, buf, RUI_C_ALERT, RUI_LEFT);
	} else {
		rui_text_fit(s, a->font, r, dp.msg, RUI_C_TEXT, RUI_LEFT);
	}
	rui_surf_flush(s);
	dp.dirty = 0;
}

/* --------------------------------------------------------------- identify */

static void
ident_close(void)
{
	int i;

	for (i = 0; i < dp.nident; i++) {
		rui_surf_free(dp.ident_s[i]);
		XDestroyWindow(dp.app->dpy, dp.ident[i]);
	}
	dp.nident = 0;
	dp.ident_until = 0;
}

/* a numbered card in the corner of every monitor that is on */
static void
identify(void)
{
	RuiApp *a = dp.app;
	int i, w = 190, h = 2 * a->bold->height + 26;

	ident_close();
	for (i = 0; i < dp.cur.n && dp.nident < DSP_MAX_OUT; i++) {
		const DspOutput *o = &dp.cur.out[i];
		RuiSurf *s;
		RuiRect r;
		char buf[96];

		if (!o->enabled || o->mode < 0)
			continue;
		dp.ident[dp.nident] = rui_window_new(a, a->root, o->x + 24, o->y + 24,
		    w, h, 1, ExposureMask);
		s = dp.ident_s[dp.nident] = rui_surf_new(a, dp.ident[dp.nident], w, h);
		dp.nident++;
		rui_fill(s, 0, 0, w, h, RUI_C_TITLE_ACTIVE);
		rui_bevel(s, 0, 0, w, h, RUI_RAISED);
		r.x = 8;
		r.y = 10;
		r.w = w - 16;
		r.h = a->bold->height;
		snprintf(buf, sizeof buf, "%d   %s", i + 1, o->name);
		rui_text_fit(s, a->bold, r, buf, RUI_C_TITLE_FG, RUI_CENTER);
		r.y += a->bold->height + 6;
		snprintf(buf, sizeof buf, "%dx%d  %.2f Hz%s", o->modes[o->mode].w,
		    o->modes[o->mode].h, o->modes[o->mode].rate,
		    o->primary ? "  primary" : "");
		rui_text_fit(s, a->font, r, buf, RUI_C_TITLE_FG, RUI_CENTER);
		XMapRaised(a->dpy, dp.ident[dp.nident - 1]);
		rui_surf_flush(s);
	}
	dp.ident_until = rui_now_ms() + IDENT_MS;
}

/* ---------------------------------------------------------------- actions */

static void
do_apply(void)
{
	char err[160], buf[200];

	dsp_normalize(&dp.edit);
	dp.prev = dp.cur;
	if (dsp_apply(dp.app->dpy, &dp.edit, err, sizeof err) < 0) {
		/* whatever went through is undone at once */
		dsp_apply(dp.app->dpy, &dp.prev, buf, sizeof buf);
		snprintf(buf, sizeof buf, "Not applied: %s.", err);
		requery(1);
		set_msg(buf);
		return;
	}
	requery(1);
	dp.confirm_until = rui_now_ms() + CONFIRM_MS;
	dp.dirty = 1;
}

static void
do_keep(void)
{
	char *path = conf_path();

	dp.confirm_until = 0;
	if (dsp_save(&dp.cur, path) == 0)
		set_msg("Layout kept; it is restored at every login.");
	else
		set_msg("Layout kept, but monitors.conf could not be written.");
	free(path);
}

static void
do_revert(void)
{
	char err[160];

	dp.confirm_until = 0;
	dsp_apply(dp.app->dpy, &dp.prev, err, sizeof err);
	requery(1);
	set_msg("The previous layout is back.");
}

static void
save_profile(const char *name)
{
	char buf[200], *path;

	if (!profile_name_ok(name)) {
		set_msg("A profile name cannot be empty or contain a slash.");
		return;
	}
	path = profile_path(name);
	if (dsp_save(&dp.cur, path) == 0)
		snprintf(buf, sizeof buf, "Profile \"%.60s\" saved: retro-display "
		    "--profile %.60s", name, name);
	else
		snprintf(buf, sizeof buf, "Could not write %.150s", path);
	free(path);
	set_msg(buf);
}

/* Profiles: every saved one (switch to it), save the current layout under
 * a name, delete one */
static void
profiles_popup(void)
{
	char **names = profile_list(), buf[200];
	RuiMenu *m = rui_menu_new(), *del = rui_menu_new();
	const RuiMenuItem *it;
	Window child;
	int x, y, i, n, id;

	for (n = 0; names[n]; n++) {
		rui_menu_add(m, names[n], NULL, 1000 + n);
		rui_menu_add(del, names[n], NULL, 2000 + n);
	}
	if (!n)
		rui_menu_add(m, "(no profiles yet)", NULL, 0)->disabled = 1;
	rui_menu_add_sep(m);
	rui_menu_add(m, "Save Current As...", NULL, 1);
	if (n)
		rui_menu_add_sub(m, "Delete", del);
	else
		rui_menu_free(del);
	XTranslateCoordinates(dp.app->dpy, dp.win, dp.app->root, dp.b_prof.r.x,
	    dp.b_prof.r.y + dp.b_prof.r.h, &x, &y, &child);
	it = rui_menu_popup(dp.app, m, x, y, 0);
	id = it ? it->id : 0;
	rui_menu_free(m);
	if (id == 1) {
		dp.naming = 1;
		rui_input_set(&dp.name, "");
	} else if (id >= 2000 && id - 2000 < n) {
		char *path = profile_path(names[id - 2000]);

		unlink(path);
		free(path);
		snprintf(buf, sizeof buf, "Profile \"%.80s\" deleted.",
		    names[id - 2000]);
		set_msg(buf);
	} else if (id >= 1000 && id - 1000 < n) {
		char *path = profile_path(names[id - 1000]);
		DspLayout want = dp.cur;

		if (dsp_load(&want, path) <= 0) {
			set_msg("That profile is empty or unreadable.");
		} else if (dsp_equal(&want, &dp.cur)) {
			snprintf(buf, sizeof buf, "Profile \"%.80s\" is the layout in "
			    "use.", names[id - 1000]);
			set_msg(buf);
		} else {
			dp.edit = want;
			do_apply();                    /* with the keep/revert question */
		}
		free(path);
	}
	for (i = 0; names[i]; i++)
		free(names[i]);
	free(names);
	dp.dirty = 1;
}

static void
rotation_popup(void)
{
	static const char *const names[] = {
		"Normal", "Left (90)", "Upside down (180)", "Right (270)"
	};
	DspOutput *o = &dp.edit.out[dp.sel];
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	Window child;
	int x, y, i;

	for (i = 0; i < 4; i++)
		rui_menu_add(m, names[i], NULL, i + 1)->checked = o->rot == i * 90;
	XTranslateCoordinates(dp.app->dpy, dp.win, dp.app->root, dp.b_rot.r.x,
	    dp.b_rot.r.y + dp.b_rot.r.h, &x, &y, &child);
	it = rui_menu_popup(dp.app, m, x, y, 0);
	if (it) {
		o->rot = (it->id - 1) * 90;
		dsp_snap(&dp.edit, dp.sel);        /* its shape changed */
	}
	rui_menu_free(m);
	dp.dirty = 1;
}

static void
toggle_enabled(void)
{
	DspOutput *o = &dp.edit.out[dp.sel];
	int i, on = 0, bw, bh;

	for (i = 0; i < dp.edit.n; i++)
		on += dp.edit.out[i].enabled;
	if (o->enabled && on <= 1) {
		set_msg("The last monitor cannot be switched off.");
		return;
	}
	if (o->enabled) {
		o->enabled = 0;
	} else {
		/* a monitor switched on joins at the right of the others */
		dsp_bounds(&dp.edit, &bw, &bh);
		o->enabled = 1;
		o->mode = o->preferred;
		o->x = bw;
		o->y = 0;
	}
	dsp_normalize(&dp.edit);
	build_lists();
	set_msg(o->enabled ? "Drag the monitors to arrange them." : "");
}

/* ----------------------------------------------------------------- events */

static void
preview_event(XEvent *ev)
{
	int i;

	if (dp.confirm_until)
		return;
	if (ev->type == ButtonPress && ev->xbutton.button == Button1 &&
	    rui_in_rect(dp.pv, ev->xbutton.x, ev->xbutton.y)) {
		/* the selected one is on top, so it is asked first */
		int hit = -1;

		if (dp.sel >= 0 && rui_in_rect(dp.box[dp.sel], ev->xbutton.x,
		    ev->xbutton.y))
			hit = dp.sel;
		for (i = dp.edit.n - 1; i >= 0 && hit < 0; i--)
			if (rui_in_rect(dp.box[i], ev->xbutton.x, ev->xbutton.y))
				hit = i;
		if (hit < 0)
			return;
		if (hit != dp.sel)
			select_output(hit);
		if (dp.edit.out[hit].enabled) {
			dp.drag = 1;
			dp.drag_dx = ev->xbutton.x - dp.box[hit].x;
			dp.drag_dy = ev->xbutton.y - dp.box[hit].y;
		}
		dp.dirty = 1;
	} else if (ev->type == MotionNotify && dp.drag) {
		DspOutput *o = &dp.edit.out[dp.sel];

		o->x = (int)((ev->xmotion.x - dp.drag_dx - dp.ox) / dp.scale);
		o->y = (int)((ev->xmotion.y - dp.drag_dy - dp.oy) / dp.scale);
		dp.dirty = 1;
	} else if (ev->type == ButtonRelease && dp.drag) {
		dp.drag = 0;
		dsp_snap(&dp.edit, dp.sel);
		dp.dirty = 1;
	}
}

static void
handle_event(XEvent *ev)
{
	int r, i;

	if (rui_monitors_event(dp.app, ev)) {
		if (!dp.drag)
			requery(0);                    /* plugged, unplugged, changed */
		return;
	}
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count)
			break;
		if (ev->xexpose.window == dp.win)
			rui_surf_flush(dp.s);
		for (i = 0; i < dp.nident; i++)
			if (ev->xexpose.window == dp.ident[i])
				rui_surf_flush(dp.ident_s[i]);
		break;
	case SelectionNotify:
		if (dp.naming && rui_input_event(dp.app, &dp.name, ev))
			dp.dirty = 1;
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);

		if (dp.naming) {
			int r2 = rui_input_event(dp.app, &dp.name, ev);

			if (r2 & RUI_EV_ACTIVATE) {
				dp.naming = 0;
				save_profile(dp.name.text);
			} else if (r2 & RUI_EV_CANCEL) {
				dp.naming = 0;
			}
			dp.dirty = 1;
			break;
		}
		if (ks == XK_Escape) {
			if (dp.confirm_until)
				do_revert();
			else
				dp.running = 0;
		} else if (ks == XK_Return && dp.confirm_until) {
			do_keep();
		} else if (ks == XK_Tab && dp.edit.n && !dp.confirm_until) {
			select_output((dp.sel + 1) % dp.edit.n);
		}
		break;
	}
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		if (ev->xany.window != dp.win)
			break;
		preview_event(ev);
		if (!dp.confirm_until && dp.sel >= 0) {
			DspOutput *o = &dp.edit.out[dp.sel];

			r = rui_list_event(dp.app, &dp.res_list, ev);
			if ((r & RUI_EV_CHANGED) && dp.res_list.sel >= 0) {
				double rate = o->mode >= 0 ? o->modes[o->mode].rate : 60.0;
				int m = dsp_find_mode(o, dp.res_w[dp.res_list.sel],
				    dp.res_h[dp.res_list.sel], rate);

				if (m >= 0) {
					o->mode = m;
					if (o->enabled)
						dsp_snap(&dp.edit, dp.sel);
					build_lists();
				}
			}
			dp.dirty |= r != 0;
			r = rui_list_event(dp.app, &dp.rate_list, ev);
			if ((r & RUI_EV_CHANGED) && dp.rate_list.sel >= 0)
				o->mode = dp.rate_mode[dp.rate_list.sel];
			dp.dirty |= r != 0;
			if (rui_button_event(&dp.b_on, ev) & RUI_EV_ACTIVATE)
				toggle_enabled();
			if (rui_button_event(&dp.b_primary, ev) & RUI_EV_ACTIVATE) {
				for (i = 0; i < dp.edit.n; i++)
					dp.edit.out[i].primary = i == dp.sel;
			}
			if (rui_button_event(&dp.b_rot, ev) & RUI_EV_ACTIVATE)
				rotation_popup();
		}
		if (rui_button_event(&dp.b_ident, ev) & RUI_EV_ACTIVATE)
			identify();
		if (rui_button_event(&dp.b_prof, ev) & RUI_EV_ACTIVATE)
			profiles_popup();
		if (rui_button_event(&dp.b_apply, ev) & RUI_EV_ACTIVATE) {
			if (dp.confirm_until)
				do_keep();
			else
				do_apply();
		}
		if (rui_button_event(&dp.b_revert, ev) & RUI_EV_ACTIVATE) {
			if (dp.confirm_until) {
				do_revert();
			} else {
				dp.edit = dp.cur;
				build_lists();
				set_msg("");
			}
		}
		if (rui_button_event(&dp.b_close, ev) & RUI_EV_ACTIVATE) {
			if (dp.confirm_until)
				do_revert();
			dp.running = 0;
		}
		if (ev->type != MotionNotify || dp.drag)
			dp.dirty = 1;
		break;
	case ClientMessage:
		if (rui_wm_delete_event(dp.app, ev)) {
			if (dp.confirm_until)
				do_revert();
			dp.running = 0;
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
setup(void)
{
	RuiApp *a = dp.app;
	RuiMonitor mon = rui_monitor_pick(a, "primary");
	XSizeHints sh;
	int fh = a->font->height, bh = fh + 12, y, rx, rw, bw = 96;

	dp.pv.x = 12;
	dp.pv.y = 12;
	dp.pv.w = WIN_W - 24;
	dp.pv.h = 216;
	y = dp.pv.y + dp.pv.h + fh + 14;
	dp.res_list.r.x = 12;
	dp.res_list.r.y = y;
	dp.res_list.r.w = 196;
	dp.res_list.r.h = WIN_H - y - bh - fh - 36;
	dp.rate_list.r = dp.res_list.r;
	dp.rate_list.r.x = 216;
	dp.rate_list.r.w = 158;
	dp.res_list.last_idx = dp.rate_list.last_idx = -1;
	rx = 386;
	rw = WIN_W - 12 - rx;
	dp.b_on.r.x = dp.b_primary.r.x = dp.b_rot.r.x = dp.b_ident.r.x = rx;
	dp.b_on.r.w = dp.b_primary.r.w = dp.b_rot.r.w = dp.b_ident.r.w = rw;
	dp.b_on.r.h = dp.b_primary.r.h = dp.b_rot.r.h = dp.b_ident.r.h = bh;
	dp.b_on.r.y = y;
	dp.b_primary.r.y = y + bh + 6;
	dp.b_rot.r.y = y + 2 * (bh + 6);
	dp.b_ident.r.y = y + 3 * (bh + 6);
	dp.b_prof.r = dp.b_ident.r;
	dp.b_prof.r.y = y + 4 * (bh + 6);
	dp.b_prof.label = "Profiles";
	dp.b_primary.label = "Primary (has the panel)";
	dp.b_ident.label = "Identify";
	dp.b_close.label = "Close";
	y = WIN_H - fh - 20 - bh;
	dp.b_apply.r.y = dp.b_revert.r.y = dp.b_close.r.y = y;
	dp.b_apply.r.w = dp.b_revert.r.w = dp.b_close.r.w = bw;
	dp.b_apply.r.h = dp.b_revert.r.h = dp.b_close.r.h = bh;
	dp.b_close.r.x = WIN_W - 12 - bw;
	dp.b_revert.r.x = dp.b_close.r.x - bw - 6;
	dp.b_apply.r.x = dp.b_revert.r.x - bw - 6;
	dp.b_apply.bold = 1;

	dp.win = rui_window_new(a, a->root, mon.x + (mon.w - WIN_W) / 2,
	    mon.y + (mon.h - WIN_H) / 3, WIN_W, WIN_H, 0, ExposureMask |
	    KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	    StructureNotifyMask);
	dp.s = rui_surf_new(a, dp.win, WIN_W, WIN_H);
	rui_set_title(a, dp.win, "Display", "retro-display");
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = WIN_H;
	XSetWMNormalHints(a->dpy, dp.win, &sh);
	rui_input_init(a, &dp.name, dp.win);
	rui_monitors_watch(a);
	a->dispatch = dispatch_cb;
	dp.sel = -1;
	requery(1);
	if (dp.cur.n)
		set_msg(dp.cur.n > 1 ? "Drag the monitors to arrange them; each "
		    "keeps its own refresh rate." : "One monitor connected.");
	XMapWindow(a->dpy, dp.win);
}

/* --------------------------------------------------------------- the CLI */

static int
cli_list(Display *dpy)
{
	DspLayout l;
	int i, k;

	if (dsp_query(dpy, &l) < 0) {
		fprintf(stderr, "retro-display: no monitors (no RandR?)\n");
		return 1;
	}
	for (i = 0; i < l.n; i++) {
		const DspOutput *o = &l.out[i];

		if (o->enabled && o->mode >= 0)
			printf("%s: %dx%d %.2f Hz at %d,%d rotate %d%s\n", o->name,
			    o->modes[o->mode].w, o->modes[o->mode].h,
			    o->modes[o->mode].rate, o->x, o->y, o->rot,
			    o->primary ? " primary" : "");
		else
			printf("%s: off\n", o->name);
		for (k = 0; k < o->nmodes; k++)
			printf("  %dx%d %.2f%s%s%s\n", o->modes[k].w, o->modes[k].h,
			    o->modes[k].rate, o->modes[k].interlaced ? " interlaced" : "",
			    k == o->preferred ? " native" : "",
			    o->enabled && k == o->mode ? " *" : "");
	}
	return 0;
}

static int
cli_profiles(void)
{
	char **names = profile_list();
	int i;

	for (i = 0; names[i]; i++) {
		puts(names[i]);
		free(names[i]);
	}
	free(names);
	return 0;
}

/* save the layout in use under a name, or delete a profile */
static int
cli_profile_file(Display *dpy, const char *name, int del)
{
	DspLayout cur;
	char *path;
	int rc;

	if (!profile_name_ok(name)) {
		fprintf(stderr, "retro-display: bad profile name '%s'\n", name);
		return 2;
	}
	path = profile_path(name);
	if (del)
		rc = unlink(path) == 0 ? 0 : 1;
	else
		rc = dsp_query(dpy, &cur) == 0 && dsp_save(&cur, path) == 0 ? 0 : 1;
	if (rc)
		fprintf(stderr, "retro-display: cannot %s profile '%s'\n",
		    del ? "delete" : "save", name);
	free(path);
	return rc;
}

/* restore: monitors.conf onto what is plugged in; apply: argv lines;
 * profile: a saved layout, which also becomes the one restored at login */
static int
cli_change(Display *dpy, int argc, char **argv, const char *profile)
{
	DspLayout cur, want;
	char err[160], *path = conf_path();
	int i, rc = 0;

	if (dsp_query(dpy, &cur) < 0) {
		free(path);
		return argc ? 1 : 0;               /* nothing to restore onto */
	}
	want = cur;
	if (profile) {
		char *pp = profile_name_ok(profile) ? profile_path(profile) : NULL;

		if (!pp || dsp_load(&want, pp) <= 0) {
			fprintf(stderr, "retro-display: no profile '%s' (see "
			    "--profiles)\n", profile);
			free(pp);
			free(path);
			return 1;
		}
		free(pp);
		argc = 1;                          /* save it as the current one */
	} else if (argc == 0) {
		if (dsp_load(&want, path) <= 0) {
			free(path);
			return 0;                      /* no saved layout */
		}
	} else {
		for (i = 0; i < argc; i++) {
			if (dsp_parse(&want, argv[i]) != 1) {
				fprintf(stderr, "retro-display: expected \"output NAME mode "
				    "WxH [rate HZ] [pos XxY] [rotate DEG] [primary]\" or "
				    "\"output NAME off\", got '%s'\n", argv[i]);
				free(path);
				return 2;
			}
		}
	}
	if (!dsp_equal(&cur, &want) &&
	    dsp_apply(dpy, &want, err, sizeof err) < 0) {
		fprintf(stderr, "retro-display: %s\n", err);
		dsp_apply(dpy, &cur, err, sizeof err);
		rc = 1;
	} else if (argc && dsp_query(dpy, &cur) == 0) {
		rc = dsp_save(&cur, path) == 0 ? 0 : 1;
	}
	free(path);
	return rc;
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		Display *dpy;
		int rc;

		if (!strcmp(argv[1], "-v")) {
			puts("retro-display " RUI_VERSION);
			return 0;
		}
		if (!strcmp(argv[1], "--profiles"))
			return cli_profiles();
		if (strcmp(argv[1], "--list") && strcmp(argv[1], "--restore") &&
		    !(argc > 2 && !strcmp(argv[1], "--apply")) &&
		    !(argc == 3 && (!strcmp(argv[1], "--profile") ||
		    !strcmp(argv[1], "--save-profile") ||
		    !strcmp(argv[1], "--delete-profile")))) {
			fprintf(stderr, "usage: retro-display [--list | --restore | "
			    "--apply \"output NAME ...\"... | --profiles | --profile "
			    "NAME | --save-profile NAME | --delete-profile NAME]\n");
			return 2;
		}
		if (!(dpy = XOpenDisplay(NULL))) {
			fprintf(stderr, "retro-display: cannot open display\n");
			return 1;
		}
		if (!strcmp(argv[1], "--list"))
			rc = cli_list(dpy);
		else if (!strcmp(argv[1], "--restore"))
			rc = cli_change(dpy, 0, NULL, NULL);
		else if (!strcmp(argv[1], "--profile"))
			rc = cli_change(dpy, 0, NULL, argv[2]);
		else if (!strcmp(argv[1], "--save-profile"))
			rc = cli_profile_file(dpy, argv[2], 0);
		else if (!strcmp(argv[1], "--delete-profile"))
			rc = cli_profile_file(dpy, argv[2], 1);
		else
			rc = cli_change(dpy, argc - 2, argv + 2, NULL);
		XCloseDisplay(dpy);
		return rc;
	}
	rui_signals_init();
	dp.app = rui_open("retro-display");
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath", NULL) == -1)
		rui_die("pledge failed");
#endif
	dp.running = 1;
	dp.dirty = 1;
	while (dp.running && !rui_quit_requested()) {
		XEvent ev;
		long long now;

		while (dp.running && XPending(dp.app->dpy)) {
			XNextEvent(dp.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		now = rui_now_ms();
		if (dp.confirm_until) {
			if (now >= dp.confirm_until)
				do_revert();               /* nobody said "Keep" */
			dp.dirty = 1;
		}
		if (dp.ident_until && now >= dp.ident_until)
			ident_close();
		if (dp.dirty && dp.running)
			draw();
		if (dp.running)
			rui_wait(dp.app, dp.confirm_until || dp.ident_until ? 250 : -1);
	}
	if (dp.confirm_until)
		do_revert();
	ident_close();
	rui_input_destroy(&dp.name);
	free_items();
	rui_surf_free(dp.s);
	XDestroyWindow(dp.app->dpy, dp.win);
	rui_close(dp.app);
	return 0;
}
