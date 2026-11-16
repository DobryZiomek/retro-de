/*
 * retro-display model and RandR glue: several monitors, each with its own
 * resolution, refresh rate, rotation and position.
 */
#include "randr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/extensions/Xrandr.h>

/* ------------------------------------------------------------------ model */

void
dsp_size(const DspOutput *o, int *w, int *h)
{
	int mw = 0, mh = 0;

	if (o->mode >= 0 && o->mode < o->nmodes) {
		mw = o->modes[o->mode].w;
		mh = o->modes[o->mode].h;
	}
	if (o->rot == 90 || o->rot == 270) {
		*w = mh;
		*h = mw;
	} else {
		*w = mw;
		*h = mh;
	}
}

int
dsp_find_mode(const DspOutput *o, int w, int h, double rate)
{
	int i, best = -1;
	double bd = 0;

	for (i = 0; i < o->nmodes; i++) {
		double d;

		if (o->modes[i].w != w || o->modes[i].h != h)
			continue;
		d = fabs(o->modes[i].rate - rate);
		if (best < 0 || d < bd) {
			best = i;
			bd = d;
		}
	}
	return best;
}

void
dsp_bounds(const DspLayout *l, int *w, int *h)
{
	int i, ow, oh;

	*w = *h = 0;
	for (i = 0; i < l->n; i++) {
		if (!l->out[i].enabled)
			continue;
		dsp_size(&l->out[i], &ow, &oh);
		if (l->out[i].x + ow > *w)
			*w = l->out[i].x + ow;
		if (l->out[i].y + oh > *h)
			*h = l->out[i].y + oh;
	}
}

void
dsp_normalize(DspLayout *l)
{
	int i, minx = 0, miny = 0, first = 1, on = 0, prim = -1;

	for (i = 0; i < l->n; i++) {
		DspOutput *o = &l->out[i];

		if (o->enabled && o->nmodes == 0)
			o->enabled = 0;
		if (o->enabled && (o->mode < 0 || o->mode >= o->nmodes))
			o->mode = o->preferred >= 0 && o->preferred < o->nmodes ?
			    o->preferred : 0;
		if (o->rot != 90 && o->rot != 180 && o->rot != 270)
			o->rot = 0;
		on += o->enabled;
	}
	if (!on) {                             /* never switch everything off */
		for (i = 0; i < l->n; i++) {
			DspOutput *o = &l->out[i];

			if (o->nmodes == 0)
				continue;
			o->enabled = 1;
			o->mode = o->preferred >= 0 && o->preferred < o->nmodes ?
			    o->preferred : 0;
			o->x = o->y = 0;
			break;
		}
	}
	for (i = 0; i < l->n; i++) {
		DspOutput *o = &l->out[i];

		if (!o->enabled) {
			o->primary = 0;
			continue;
		}
		if (first || o->x < minx)
			minx = o->x;
		if (first || o->y < miny)
			miny = o->y;
		first = 0;
		if (o->primary && prim < 0)
			prim = i;
		else
			o->primary = 0;
	}
	for (i = 0; i < l->n; i++) {
		DspOutput *o = &l->out[i];

		if (!o->enabled)
			continue;
		o->x -= minx;
		o->y -= miny;
		if (prim < 0) {
			o->primary = 1;
			prim = i;
		}
	}
}

static int
overlaps(const DspLayout *l, int idx, int x, int y)
{
	int i, w, h, ow, oh;

	dsp_size(&l->out[idx], &w, &h);
	for (i = 0; i < l->n; i++) {
		const DspOutput *o = &l->out[i];

		if (i == idx || !o->enabled)
			continue;
		dsp_size(o, &ow, &oh);
		if (x < o->x + ow && x + w > o->x && y < o->y + oh && y + h > o->y)
			return 1;
	}
	return 0;
}

void
dsp_snap(DspLayout *l, int idx)
{
	DspOutput *m;
	int i, k, w, h, best = -1, bx = 0, by = 0;
	long bd = 0;

	if (idx < 0 || idx >= l->n || !l->out[idx].enabled)
		return;
	m = &l->out[idx];
	dsp_size(m, &w, &h);
	for (i = 0; i < l->n; i++) {
		const DspOutput *o = &l->out[i];
		int ow, oh, cx[4], cy[4];

		if (i == idx || !o->enabled)
			continue;
		dsp_size(o, &ow, &oh);
		/* right of, left of, below, above - sliding along the shared
		 * edge, but never past the corners */
		cx[0] = o->x + ow;
		cx[1] = o->x - w;
		cy[0] = cy[1] = m->y < o->y - h + 1 ? o->y - h + 1 :
		    m->y > o->y + oh - 1 ? o->y + oh - 1 : m->y;
		cy[2] = o->y + oh;
		cy[3] = o->y - h;
		cx[2] = cx[3] = m->x < o->x - w + 1 ? o->x - w + 1 :
		    m->x > o->x + ow - 1 ? o->x + ow - 1 : m->x;
		for (k = 0; k < 4; k++) {
			long dx, dy, d;

			/* align the edges when they are nearly aligned already */
			if (k < 2 && labs((long)cy[k] - o->y) < oh / 16)
				cy[k] = o->y;
			if (k >= 2 && labs((long)cx[k] - o->x) < ow / 16)
				cx[k] = o->x;
			dy = cy[k] - m->y;
			dx = cx[k] - m->x;
			d = dx * dx + dy * dy;
			if (overlaps(l, idx, cx[k], cy[k]))
				continue;
			if (best < 0 || d < bd) {
				best = k;
				bd = d;
				bx = cx[k];
				by = cy[k];
			}
		}
	}
	if (best >= 0) {
		m->x = bx;
		m->y = by;
	}
	dsp_normalize(l);
}

int
dsp_equal(const DspLayout *a, const DspLayout *b)
{
	int i;

	if (a->n != b->n)
		return 0;
	for (i = 0; i < a->n; i++) {
		const DspOutput *x = &a->out[i], *y = &b->out[i];

		if (strcmp(x->name, y->name) || x->enabled != y->enabled)
			return 0;
		if (!x->enabled)
			continue;
		if (x->x != y->x || x->y != y->y || x->rot != y->rot ||
		    x->mode != y->mode || x->primary != y->primary)
			return 0;
	}
	return 1;
}

/* ---------------------------------------------------------- monitors.conf */

char *
dsp_format(const DspLayout *l)
{
	char *text = rui_strdup("# retro-de monitor layout (written by "
	    "retro-display, restored at login)\n");
	int i;

	for (i = 0; i < l->n; i++) {
		const DspOutput *o = &l->out[i];
		char *line, *nt;

		if (o->enabled && o->mode >= 0 && o->mode < o->nmodes)
			line = rui_strdupf("output %s mode %dx%d rate %.3f pos %dx%d "
			    "rotate %d%s\n", o->name, o->modes[o->mode].w,
			    o->modes[o->mode].h, o->modes[o->mode].rate, o->x, o->y,
			    o->rot, o->primary ? " primary" : "");
		else
			line = rui_strdupf("output %s off\n", o->name);
		nt = rui_strdupf("%s%s", text, line);
		free(text);
		free(line);
		text = nt;
	}
	return text;
}

int
dsp_parse(DspLayout *l, const char *text)
{
	char line[512];
	const char *p = text;
	int named = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char name[32], *s = rui_ltrim(rui_rtrim(line)), *tok;
		DspOutput *o = NULL;
		int i, w = 0, h = 0, on = 1;
		double rate = 0;

		if (strncmp(s, "output ", 7) || sscanf(s + 7, "%31s", name) != 1)
			continue;
		named++;
		for (i = 0; i < l->n; i++)
			if (!strcmp(l->out[i].name, name))
				o = &l->out[i];
		if (!o)
			continue;                      /* not plugged in right now */
		s = rui_ltrim(s + 7) + strlen(name);
		o->primary = 0;
		for (tok = strtok(s, " \t"); tok; tok = strtok(NULL, " \t")) {
			char *arg;

			if (!strcmp(tok, "off")) {
				on = 0;
			} else if (!strcmp(tok, "primary")) {
				o->primary = 1;
			} else if (!(arg = strtok(NULL, " \t"))) {
				break;
			} else if (!strcmp(tok, "mode")) {
				sscanf(arg, "%dx%d", &w, &h);
			} else if (!strcmp(tok, "rate")) {
				rate = atof(arg);
			} else if (!strcmp(tok, "pos")) {
				sscanf(arg, "%dx%d", &o->x, &o->y);
			} else if (!strcmp(tok, "rotate")) {
				o->rot = atoi(arg);
			}
		}
		o->enabled = on;
		if (on) {
			int m = dsp_find_mode(o, w, h, rate);

			/* the saved mode is gone (another monitor on this port):
			 * its native mode is the safe choice */
			o->mode = m >= 0 ? m : o->preferred;
		}
	}
	dsp_normalize(l);
	return named;
}

int
dsp_save(const DspLayout *l, const char *path)
{
	char *text = dsp_format(l);
	FILE *f = fopen(path, "w");
	int rc = -1;

	if (f) {
		rc = fputs(text, f) < 0 ? -1 : 0;
		if (fclose(f) != 0)
			rc = -1;
	}
	free(text);
	return rc;
}

int
dsp_load(DspLayout *l, const char *path)
{
	FILE *f = fopen(path, "r");
	char buf[8192];
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = '\0';
	fclose(f);
	return dsp_parse(l, buf);
}

/* --------------------------------------------------------------- X server */

static double
mode_rate(const XRRModeInfo *m)
{
	double vt = m->vTotal;

	if (!m->hTotal || !m->vTotal)
		return 0;
	if (m->modeFlags & RR_DoubleScan)
		vt *= 2;
	if (m->modeFlags & RR_Interlace)
		vt /= 2;
	return (double)m->dotClock / ((double)m->hTotal * vt);
}

static int
cmp_mode(const void *a, const void *b)
{
	const DspMode *x = a, *y = b;
	long ax = (long)x->w * x->h, ay = (long)y->w * y->h;

	if (ax != ay)
		return ax < ay ? 1 : -1;
	if (x->w != y->w)
		return y->w - x->w;
	if (x->rate != y->rate)
		return x->rate < y->rate ? 1 : -1;
	return x->interlaced - y->interlaced;
}

static int
rot_degrees(Rotation r)
{
	return (r & RR_Rotate_90) ? 90 : (r & RR_Rotate_180) ? 180 :
	    (r & RR_Rotate_270) ? 270 : 0;
}

static Rotation
rot_mask(int deg)
{
	return deg == 90 ? RR_Rotate_90 : deg == 180 ? RR_Rotate_180 :
	    deg == 270 ? RR_Rotate_270 : RR_Rotate_0;
}

int
dsp_query(Display *dpy, DspLayout *l)
{
	Window root = DefaultRootWindow(dpy);
	XRRScreenResources *res;
	RROutput prim;
	int evb, erb, i, k, j;

	memset(l, 0, sizeof *l);
	if (!XRRQueryExtension(dpy, &evb, &erb))
		return -1;
	if (!(res = XRRGetScreenResourcesCurrent(dpy, root)))
		return -1;
	prim = XRRGetOutputPrimary(dpy, root);
	for (i = 0; i < res->noutput && l->n < DSP_MAX_OUT; i++) {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		DspOutput *o;
		unsigned long cur = 0, pref = 0;

		if (!oi)
			continue;
		if (oi->connection != RR_Connected || oi->nmode == 0) {
			XRRFreeOutputInfo(oi);
			continue;
		}
		o = &l->out[l->n++];
		rui_strlcpy(o->name, oi->name, sizeof o->name);
		o->xid = res->outputs[i];
		o->primary = res->outputs[i] == prim;
		o->mode = o->preferred = -1;
		if (oi->crtc) {
			XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);

			if (ci) {
				o->enabled = ci->mode != None;
				o->x = ci->x;
				o->y = ci->y;
				o->rot = rot_degrees(ci->rotation);
				cur = ci->mode;
				XRRFreeCrtcInfo(ci);
			}
		}
		if (oi->npreferred > 0)
			pref = oi->modes[0];
		for (k = 0; k < oi->nmode && o->nmodes < DSP_MAX_MODES; k++) {
			for (j = 0; j < res->nmode; j++) {
				const XRRModeInfo *mi = &res->modes[j];
				DspMode *m;

				if (mi->id != oi->modes[k])
					continue;
				m = &o->modes[o->nmodes++];
				m->id = mi->id;
				m->w = (int)mi->width;
				m->h = (int)mi->height;
				m->rate = mode_rate(mi);
				m->interlaced = (mi->modeFlags & RR_Interlace) != 0;
				break;
			}
		}
		qsort(o->modes, (size_t)o->nmodes, sizeof o->modes[0], cmp_mode);
		for (k = 0; k < o->nmodes; k++) {
			if (o->modes[k].id == cur)
				o->mode = k;
			if (o->modes[k].id == pref)
				o->preferred = k;
		}
		if (o->preferred < 0)
			o->preferred = 0;
		if (o->mode < 0)
			o->enabled = 0;
		XRRFreeOutputInfo(oi);
	}
	XRRFreeScreenResources(res);
	return l->n ? 0 : -1;
}

/* a CRTC this output may use and nothing else has taken */
static RRCrtc
pick_crtc(Display *dpy, XRRScreenResources *res, RROutput out,
    const RRCrtc *taken, int ntaken)
{
	XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, out);
	RRCrtc found = None;
	int pass, i, k;

	if (!oi)
		return None;
	/* first choice: the one it is on already */
	for (pass = 0; pass < 2 && !found; pass++) {
		for (i = 0; i < oi->ncrtc && !found; i++) {
			int used = 0;

			if (pass == 0 && oi->crtcs[i] != oi->crtc)
				continue;
			for (k = 0; k < ntaken; k++)
				used |= taken[k] == oi->crtcs[i];
			if (!used)
				found = oi->crtcs[i];
		}
	}
	XRRFreeOutputInfo(oi);
	return found;
}

int
dsp_apply(Display *dpy, const DspLayout *l, char *err, size_t n)
{
	Window root = DefaultRootWindow(dpy);
	int scr = DefaultScreen(dpy);
	XRRScreenResources *res;
	RRCrtc taken[DSP_MAX_OUT], crtc[DSP_MAX_OUT];
	int ntaken = 0, i, k, sw, sh, mmw, mmh, rc = 0;
	double dpi = 96.0;

	err[0] = '\0';
	dsp_bounds(l, &sw, &sh);
	if (sw <= 0 || sh <= 0) {
		snprintf(err, n, "no monitor would stay on");
		return -1;
	}
	if (!(res = XRRGetScreenResources(dpy, root))) {
		snprintf(err, n, "the X server has no RandR");
		return -1;
	}
	{
		int minw, minh, maxw, maxh;

		if (XRRGetScreenSizeRange(dpy, root, &minw, &minh, &maxw, &maxh) &&
		    (sw > maxw || sh > maxh)) {
			snprintf(err, n, "%dx%d is larger than the graphics card "
			    "allows (%dx%d)", sw, sh, maxw, maxh);
			XRRFreeScreenResources(res);
			return -1;
		}
	}
	if (DisplayHeightMM(dpy, scr) > 0)
		dpi = 25.4 * DisplayHeight(dpy, scr) / DisplayHeightMM(dpy, scr);
	if (dpi < 40 || dpi > 400)
		dpi = 96.0;
	mmw = (int)(sw * 25.4 / dpi);
	mmh = (int)(sh * 25.4 / dpi);

	for (i = 0; i < l->n; i++) {
		crtc[i] = None;
		if (!l->out[i].enabled)
			continue;
		crtc[i] = pick_crtc(dpy, res, l->out[i].xid, taken, ntaken);
		if (!crtc[i]) {
			snprintf(err, n, "the graphics card cannot drive %s as well",
			    l->out[i].name);
			XRRFreeScreenResources(res);
			return -1;
		}
		taken[ntaken++] = crtc[i];
	}

	XGrabServer(dpy);
	/* 1. switch off every CRTC that will be unused, or that would stick
	 *    out of the new screen while it is being resized */
	for (k = 0; k < res->ncrtc; k++) {
		XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[k]);
		int keep = 0;

		if (!ci)
			continue;
		for (i = 0; i < ntaken; i++)
			keep |= taken[i] == res->crtcs[k];
		if (ci->mode != None && (!keep || ci->x + (int)ci->width > sw ||
		    ci->y + (int)ci->height > sh))
			XRRSetCrtcConfig(dpy, res, res->crtcs[k], CurrentTime, 0, 0,
			    None, RR_Rotate_0, NULL, 0);
		XRRFreeCrtcInfo(ci);
	}
	/* 2. the screen takes the size of the new layout */
	XRRSetScreenSize(dpy, root, sw, sh, mmw, mmh);
	/* 3. every monitor gets its own mode - and so its own refresh rate */
	for (i = 0; i < l->n; i++) {
		const DspOutput *o = &l->out[i];
		RROutput out = o->xid;

		if (!o->enabled || o->mode < 0 || o->mode >= o->nmodes)
			continue;
		if (XRRSetCrtcConfig(dpy, res, crtc[i], CurrentTime, o->x, o->y,
		    o->modes[o->mode].id, rot_mask(o->rot), &out, 1) !=
		    RRSetConfigSuccess) {
			snprintf(err, n, "%s rejected %dx%d at %.2f Hz", o->name,
			    o->modes[o->mode].w, o->modes[o->mode].h,
			    o->modes[o->mode].rate);
			rc = -1;
		}
		if (o->primary)
			XRRSetOutputPrimary(dpy, root, out);
	}
	XUngrabServer(dpy);
	XSync(dpy, False);
	XRRFreeScreenResources(res);
	return rc;
}
