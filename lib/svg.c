/*
 * libretroui: a small SVG rasteriser for icons and wallpapers, with no
 * librsvg, cairo or XML library.
 *
 * Supported: <svg> width/height/viewBox, <g>, <path> (all commands, arcs
 * included), <rect> (rx/ry), <circle>, <ellipse>, <line>, <polyline>,
 * <polygon>, <use> of an element by id, transforms, fill / stroke /
 * opacity / fill-opacity / stroke-opacity / stroke-width / fill-rule as
 * attributes, in style="" and in simple <style> class rules,
 * currentColor, linear and radial gradients (with xlink:href stop
 * inheritance and gradientTransform).  Not supported: text, filters,
 * masks, clip paths, patterns, dashes - they are skipped, so an icon may
 * lose a detail but still draws.
 *
 * Shapes are flattened to polygons and filled with 4x vertical
 * supersampling and exact horizontal coverage; strokes become a polygon
 * per segment plus round joins.
 */
#include "retroui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

#define SUB 4                           /* vertical samples per pixel     */
#define MAX_DEPTH 32
#define MAX_GRAD 256
#define MAX_STOPS 32
#define MAX_CLASSES 128

typedef struct Mat { double a, b, c, d, e, f; } Mat;

typedef struct Paint {
	int type;                           /* 0 none, 1 colour, 2 gradient   */
	unsigned rgb;
	int grad;                           /* index into grads              */
} Paint;

typedef struct Style {
	Paint fill, stroke;
	double opacity, fill_op, stroke_op, stroke_w;
	int evenodd;
	unsigned color;                     /* currentColor                   */
	int display;
} Style;

typedef struct Stop { double off; unsigned rgb; double op; } Stop;

typedef struct Grad {
	char id[64], href[64];
	int radial, user_space, have_coords;
	double x1, y1, x2, y2, cx, cy, r, fx, fy;
	Mat xf;
	Stop stops[MAX_STOPS];
	int nstops;
} Grad;

typedef struct Edge { double x0, y0, x1, y1; int dir; } Edge;

typedef struct Svg {
	const char *src;
	size_t len;
	unsigned char *px;                  /* RGBA output                    */
	int w, h;
	Grad grads[MAX_GRAD];
	int ngrads;
	struct { char name[48]; char decl[256]; } cls[MAX_CLASSES];
	int ncls;
	/* the polygon being built (device space) */
	double *pts;
	int npts, cappts;
	int *starts;                        /* index where each subpath starts */
	int nstarts, capstarts;
	int use_depth;
} Svg;

/* ------------------------------------------------------------- matrices */

static Mat
mat_mul(Mat m, Mat n)                   /* m then n: n * m */
{
	Mat r;

	r.a = n.a * m.a + n.c * m.b;
	r.b = n.b * m.a + n.d * m.b;
	r.c = n.a * m.c + n.c * m.d;
	r.d = n.b * m.c + n.d * m.d;
	r.e = n.a * m.e + n.c * m.f + n.e;
	r.f = n.b * m.e + n.d * m.f + n.f;
	return r;
}

static Mat
mat_id(void)
{
	Mat m = { 1, 0, 0, 1, 0, 0 };

	return m;
}

static void
apply(const Mat *m, double x, double y, double *ox, double *oy)
{
	*ox = m->a * x + m->c * y + m->e;
	*oy = m->b * x + m->d * y + m->f;
}

static int
mat_inv(Mat m, Mat *r)
{
	double det = m.a * m.d - m.b * m.c;

	if (fabs(det) < 1e-12)
		return -1;
	r->a = m.d / det;
	r->b = -m.b / det;
	r->c = -m.c / det;
	r->d = m.a / det;
	r->e = -(r->a * m.e + r->c * m.f);
	r->f = -(r->b * m.e + r->d * m.f);
	return 0;
}

/* --------------------------------------------------------------- numbers */

static const char *
skip_sep(const char *p)
{
	while (*p && (isspace((unsigned char)*p) || *p == ','))
		p++;
	return p;
}

/* parses a number; returns NULL if there is none */
static const char *
number(const char *p, double *v)
{
	char *end;

	p = skip_sep(p);
	*v = strtod(p, &end);
	if (end == p)
		return NULL;
	return end;          /* units (px, pt, %) are left to the caller */
}

static double
num_attr(const char *s, double def)
{
	double v;

	if (!s || !number(s, &v))
		return def;
	if (strchr(s, '%'))
		v /= 100.0;
	return v;
}

/* a length with its unit, in px (96 per inch) */
static double
length_px(const char *s, double def)
{
	double v;
	char *end;

	if (!s)
		return def;
	v = strtod(s, &end);
	if (end == s)
		return def;
	if (!strncmp(end, "pt", 2))
		v *= 96.0 / 72;
	else if (!strncmp(end, "pc", 2))
		v *= 16;
	else if (!strncmp(end, "mm", 2))
		v *= 96 / 25.4;
	else if (!strncmp(end, "cm", 2))
		v *= 96 / 2.54;
	else if (!strncmp(end, "in", 2))
		v *= 96;
	else if (*end == '%')
		return def;
	return v;
}

static Mat
parse_transform(const char *p)
{
	Mat m = mat_id();

	while (p && *p) {
		char name[16];
		double v[6];
		int n = 0, k = 0;
		Mat t = mat_id();

		p = skip_sep(p);
		while (isalpha((unsigned char)*p) && k < 15)
			name[k++] = *p++;
		name[k] = '\0';
		if (!k)
			break;
		p = skip_sep(p);
		if (*p != '(')
			break;
		p++;
		while (n < 6) {
			const char *q = number(p, &v[n]);

			if (!q)
				break;
			p = q;
			n++;
		}
		p = skip_sep(p);
		if (*p == ')')
			p++;
		if (!strcmp(name, "matrix") && n == 6) {
			t.a = v[0]; t.b = v[1]; t.c = v[2];
			t.d = v[3]; t.e = v[4]; t.f = v[5];
		} else if (!strcmp(name, "translate") && n >= 1) {
			t.e = v[0];
			t.f = n > 1 ? v[1] : 0;
		} else if (!strcmp(name, "scale") && n >= 1) {
			t.a = v[0];
			t.d = n > 1 ? v[1] : v[0];
		} else if (!strcmp(name, "rotate") && n >= 1) {
			double r = v[0] * M_PI / 180, cx = n >= 3 ? v[1] : 0,
			    cy = n >= 3 ? v[2] : 0;

			t.a = cos(r); t.b = sin(r); t.c = -sin(r); t.d = cos(r);
			t.e = cx - t.a * cx - t.c * cy;
			t.f = cy - t.b * cx - t.d * cy;
		} else if (!strcmp(name, "skewX") && n >= 1) {
			t.c = tan(v[0] * M_PI / 180);
		} else if (!strcmp(name, "skewY") && n >= 1) {
			t.b = tan(v[0] * M_PI / 180);
		}
		m = mat_mul(t, m);                 /* later ones apply first */
	}
	return m;
}

/* ---------------------------------------------------------------- colours */

static const struct { const char *name; unsigned rgb; } named[] = {
	{ "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 },
	{ "green", 0x008000 }, { "lime", 0x00ff00 }, { "blue", 0x0000ff },
	{ "yellow", 0xffff00 }, { "cyan", 0x00ffff }, { "aqua", 0x00ffff },
	{ "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "gray", 0x808080 },
	{ "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "maroon", 0x800000 },
	{ "olive", 0x808000 }, { "navy", 0x000080 }, { "purple", 0x800080 },
	{ "teal", 0x008080 }, { "orange", 0xffa500 }, { "brown", 0xa52a2a },
	{ "pink", 0xffc0cb }, { "gold", 0xffd700 }, { "darkgray", 0xa9a9a9 },
	{ "lightgray", 0xd3d3d3 }, { "darkgrey", 0xa9a9a9 },
	{ "lightgrey", 0xd3d3d3 }, { "transparent", 0x1000000 },
};

/* 0 ok (rgb set), -1 not a colour; 0x1000000 = transparent */
static int
parse_color(const char *s, unsigned *rgb)
{
	size_t i;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '#') {
		unsigned v = 0;
		int n = 0;

		for (s++; isxdigit((unsigned char)s[n]) && n < 8; n++)
			v = v * 16 + (unsigned)(isdigit((unsigned char)s[n]) ?
			    s[n] - '0' : (tolower((unsigned char)s[n]) - 'a' + 10));
		if (n == 3 || n == 4) {
			if (n == 4)
				v >>= 4;
			*rgb = ((v >> 8) & 0xf) * 0x110000 + ((v >> 4) & 0xf) * 0x1100 +
			    (v & 0xf) * 0x11;
			return 0;
		}
		if (n == 6 || n == 8) {
			*rgb = n == 8 ? v >> 8 : v;
			return 0;
		}
		return -1;
	}
	if (!strncmp(s, "rgb", 3)) {
		double c[3];
		const char *p = strchr(s, '(');
		int k;

		if (!p)
			return -1;
		p++;
		for (k = 0; k < 3; k++) {
			const char *q = number(p, &c[k]);

			if (!q)
				return -1;
			if (*q == '%')
				c[k] *= 2.55;
			p = q;
			if (*p == '%')
				p++;
		}
		for (k = 0; k < 3; k++)
			c[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
		*rgb = (unsigned)c[0] << 16 | (unsigned)c[1] << 8 | (unsigned)c[2];
		return 0;
	}
	for (i = 0; i < sizeof named / sizeof named[0]; i++) {
		size_t n = strlen(named[i].name);

		if (!strncasecmp(s, named[i].name, n) && !isalpha((unsigned char)s[n])) {
			*rgb = named[i].rgb;
			return 0;
		}
	}
	return -1;
}

static int
find_grad(Svg *sv, const char *id)
{
	int i;

	for (i = 0; i < sv->ngrads; i++)
		if (!strcmp(sv->grads[i].id, id))
			return i;
	return -1;
}

static void
parse_paint(Svg *sv, const char *v, Paint *p, const Style *st)
{
	unsigned rgb;

	while (isspace((unsigned char)*v))
		v++;
	if (!strncmp(v, "none", 4)) {
		p->type = 0;
	} else if (!strncmp(v, "currentColor", 12)) {
		p->type = 1;
		p->rgb = st->color;
	} else if (!strncmp(v, "url(", 4)) {
		char id[64];
		const char *q = v + 4;
		int k = 0, g;

		while (*q == ' ' || *q == '#' || *q == '\'' || *q == '"')
			q++;
		while (*q && *q != ')' && *q != '\'' && *q != '"' && k < 63)
			id[k++] = *q++;
		id[k] = '\0';
		if ((g = find_grad(sv, id)) >= 0) {
			p->type = 2;
			p->grad = g;
		} else {
			/* a fallback colour after the url, else leave it alone */
			const char *fb = strchr(v, ')');

			if (fb && parse_color(fb + 1, &rgb) == 0) {
				p->type = 1;
				p->rgb = rgb;
			}
		}
	} else if (parse_color(v, &rgb) == 0) {
		p->type = rgb == 0x1000000 ? 0 : 1;
		p->rgb = rgb & 0xffffff;
	}
}

static double
clamp01(double v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* one "name: value" (from an attribute or style="") */
static void
style_prop(Svg *sv, Style *st, const char *name, size_t nlen, const char *val)
{
#define IS(s) (nlen == strlen(s) && !strncmp(name, s, nlen))
	if (IS("fill"))
		parse_paint(sv, val, &st->fill, st);
	else if (IS("stroke"))
		parse_paint(sv, val, &st->stroke, st);
	else if (IS("color")) {
		unsigned rgb;

		if (parse_color(val, &rgb) == 0)
			st->color = rgb & 0xffffff;
	} else if (IS("opacity"))
		st->opacity *= clamp01(num_attr(val, 1));
	else if (IS("fill-opacity"))
		st->fill_op = clamp01(num_attr(val, 1));
	else if (IS("stroke-opacity"))
		st->stroke_op = clamp01(num_attr(val, 1));
	else if (IS("stroke-width"))
		st->stroke_w = num_attr(val, 1);
	else if (IS("fill-rule"))
		st->evenodd = !strncmp(skip_sep(val), "evenodd", 7);
	else if (IS("display"))
		st->display = strncmp(skip_sep(val), "none", 4) != 0;
	else if (IS("visibility"))
		st->display = st->display && strncmp(skip_sep(val), "hidden", 6);
#undef IS
}

/* "a: b; c: d" */
static void
style_decl(Svg *sv, Style *st, const char *s)
{
	while (s && *s) {
		const char *colon = strchr(s, ':'), *semi;
		char val[256];
		size_t n;

		while (*s == ' ' || *s == ';' || *s == '\n' || *s == '\t')
			s++;
		if (!colon)
			break;
		semi = strchr(colon, ';');
		n = (size_t)((semi ? semi : colon + strlen(colon)) - (colon + 1));
		if (n >= sizeof val)
			n = sizeof val - 1;
		memcpy(val, colon + 1, n);
		val[n] = '\0';
		n = (size_t)(colon - s);
		while (n && isspace((unsigned char)s[n - 1]))
			n--;
		style_prop(sv, st, s, n, val);
		s = semi ? semi + 1 : NULL;
	}
}

/* ------------------------------------------------------------ XML tokens */

typedef struct Attr { const char *name, *val; size_t nlen, vlen; } Attr;

typedef struct Tag {
	char name[32];
	Attr attr[48];
	int nattr;
	int closing, selfclose;
	const char *end;                    /* just past '>'                  */
} Tag;

/* next tag at or after p; NULL at the end */
static const char *
next_tag(const char *p, const char *lim, Tag *t)
{
	for (;;) {
		const char *q;
		int k = 0;

		while (p < lim && *p != '<')
			p++;
		if (p >= lim)
			return NULL;
		if (!strncmp(p, "<!--", 4)) {
			q = strstr(p, "-->");
			if (!q || q >= lim)
				return NULL;
			p = q + 3;
			continue;
		}
		if (!strncmp(p, "<![CDATA[", 9)) {
			q = strstr(p, "]]>");
			p = q && q < lim ? q + 3 : lim;
			continue;
		}
		if (p[1] == '?' || p[1] == '!') {
			q = memchr(p, '>', (size_t)(lim - p));
			if (!q)
				return NULL;
			p = q + 1;
			continue;
		}
		memset(t, 0, sizeof *t);
		p++;
		if (*p == '/') {
			t->closing = 1;
			p++;
		}
		while (p < lim && !isspace((unsigned char)*p) && *p != '>' &&
		    *p != '/') {
			if (k < 31)
				t->name[k++] = *p;
			p++;
		}
		t->name[k] = '\0';
		/* drop a namespace prefix: svg:path -> path */
		if ((q = strchr(t->name, ':')))
			memmove(t->name, q + 1, strlen(q));
		for (;;) {
			Attr *a;

			while (p < lim && isspace((unsigned char)*p))
				p++;
			if (p >= lim)
				return NULL;
			if (*p == '>') {
				t->end = p + 1;
				return p;
			}
			if (*p == '/') {
				t->selfclose = 1;
				p++;
				continue;
			}
			a = t->nattr < 48 ? &t->attr[t->nattr] : NULL;
			q = p;
			while (p < lim && *p != '=' && !isspace((unsigned char)*p) &&
			    *p != '>' && *p != '/')
				p++;
			if (a) {
				a->name = q;
				a->nlen = (size_t)(p - q);
			}
			while (p < lim && isspace((unsigned char)*p))
				p++;
			if (*p != '=')
				continue;
			p++;
			while (p < lim && isspace((unsigned char)*p))
				p++;
			if (*p == '"' || *p == '\'') {
				char quote = *p++;

				q = p;
				while (p < lim && *p != quote)
					p++;
				if (a) {
					a->val = q;
					a->vlen = (size_t)(p - q);
					t->nattr++;
				}
				if (p < lim)
					p++;
			}
		}
	}
}

/* a copy of the attribute's value (malloc'd), or NULL */
static char *
attr(const Tag *t, const char *name)
{
	int i;
	size_t n = strlen(name);

	for (i = 0; i < t->nattr; i++) {
		const Attr *a = &t->attr[i];
		const char *nm = a->name;
		size_t nl = a->nlen;
		const char *colon = memchr(nm, ':', nl);

		/* xlink:href and href are the same thing */
		if (colon && !strchr(name, ':')) {
			nl -= (size_t)(colon + 1 - nm);
			nm = colon + 1;
		}
		if (nl == n && !strncmp(nm, name, n)) {
			char *v = rui_malloc(a->vlen + 1);

			memcpy(v, a->val, a->vlen);
			v[a->vlen] = '\0';
			return v;
		}
	}
	return NULL;
}

static double
attr_num(const Tag *t, const char *name, double def)
{
	char *v = attr(t, name);
	double r = num_attr(v, def);

	free(v);
	return r;
}

/* ------------------------------------------------------------- gradients */

static void
read_gradients(Svg *sv)
{
	const char *p = sv->src, *lim = sv->src + sv->len;
	Tag t;
	Grad *g = NULL;

	while ((p = next_tag(p, lim, &t))) {
		p = t.end;
		if (!t.closing && (!strcmp(t.name, "linearGradient") ||
		    !strcmp(t.name, "radialGradient")) && sv->ngrads < MAX_GRAD) {
			char *v;

			g = &sv->grads[sv->ngrads++];
			memset(g, 0, sizeof *g);
			g->radial = t.name[0] == 'r';
			if ((v = attr(&t, "id"))) {
				rui_strlcpy(g->id, v, sizeof g->id);
				free(v);
			}
			if ((v = attr(&t, "href"))) {
				rui_strlcpy(g->href, v[0] == '#' ? v + 1 : v, sizeof g->href);
				free(v);
			}
			v = attr(&t, "gradientUnits");
			g->user_space = v && !strcmp(v, "userSpaceOnUse");
			free(v);
			v = attr(&t, "gradientTransform");
			g->xf = v ? parse_transform(v) : mat_id();
			free(v);
			g->x1 = attr_num(&t, "x1", 0);
			g->y1 = attr_num(&t, "y1", 0);
			g->x2 = attr_num(&t, "x2", 1);
			g->y2 = attr_num(&t, "y2", 0);
			g->cx = attr_num(&t, "cx", 0.5);
			g->cy = attr_num(&t, "cy", 0.5);
			g->r = attr_num(&t, "r", 0.5);
			g->fx = attr_num(&t, "fx", g->cx);
			g->fy = attr_num(&t, "fy", g->cy);
			{
				char *a1 = attr(&t, "x1"), *a2 = attr(&t, "cx");

				g->have_coords = a1 || a2;
				free(a1);
				free(a2);
			}
			if (t.selfclose)
				g = NULL;
		} else if (!t.closing && !strcmp(t.name, "stop") && g &&
		    g->nstops < MAX_STOPS) {
			Stop *s = &g->stops[g->nstops++];
			Style st;
			char *v;

			memset(&st, 0, sizeof st);
			st.fill.type = 1;
			st.fill_op = 1;
			s->off = clamp01(attr_num(&t, "offset", 0));
			s->rgb = 0;
			s->op = 1;
			if ((v = attr(&t, "stop-color"))) {
				parse_color(v, &s->rgb);
				s->rgb &= 0xffffff;
				free(v);
			}
			s->op = clamp01(attr_num(&t, "stop-opacity", 1));
			if ((v = attr(&t, "style"))) {
				const char *c = strstr(v, "stop-color:"),
				    *o = strstr(v, "stop-opacity:");

				if (c && parse_color(c + 11, &s->rgb) == 0)
					s->rgb &= 0xffffff;
				if (o)
					s->op = clamp01(num_attr(o + 13, 1));
				free(v);
			}
		} else if (t.closing && (!strcmp(t.name, "linearGradient") ||
		    !strcmp(t.name, "radialGradient"))) {
			g = NULL;
		} else if (!t.closing && !strcmp(t.name, "pattern") &&
		    sv->ngrads < MAX_GRAD && !t.selfclose) {
			/* not drawn as a pattern: the average colour of its fills */
			Tag u;
			const char *q = t.end;
			unsigned long sum[3] = { 0, 0, 0 };
			int n = 0, level = 1, k;
			char *v = attr(&t, "id");

			while (level > 0 && (q = next_tag(q, lim, &u))) {
				char *f;
				unsigned rgb;

				q = u.end;
				if (u.closing) {
					level--;
					continue;
				}
				if (!u.selfclose)
					level++;
				if ((f = attr(&u, "fill")) && parse_color(f, &rgb) == 0 &&
				    rgb < 0x1000000) {
					for (k = 0; k < 3; k++)
						sum[k] += (rgb >> (16 - 8 * k)) & 0xff;
					n++;
				}
				free(f);
			}
			if (v && n) {
				Grad *pg = &sv->grads[sv->ngrads++];

				memset(pg, 0, sizeof *pg);
				rui_strlcpy(pg->id, v, sizeof pg->id);
				pg->xf = mat_id();
				pg->x2 = 1;
				pg->nstops = 1;
				pg->stops[0].op = 1;
				for (k = 0; k < 3; k++)
					pg->stops[0].rgb |= (unsigned)(sum[k] / (unsigned long)n) <<
					    (16 - 8 * k);
			}
			free(v);
			if (q)
				p = q;
		}
	}
	/* inherit stops (and coordinates) through href */
	{
		int i, round;

		for (round = 0; round < 4; round++) {
			for (i = 0; i < sv->ngrads; i++) {
				Grad *a = &sv->grads[i];
				int k;

				if (!a->href[0] || (k = find_grad(sv, a->href)) < 0 || k == i)
					continue;
				if (!a->nstops && sv->grads[k].nstops) {
					memcpy(a->stops, sv->grads[k].stops, sizeof a->stops);
					a->nstops = sv->grads[k].nstops;
				}
			}
		}
	}
}

/* <style> .name { decls } (class selectors only) */
static void
read_classes(Svg *sv)
{
	const char *p = sv->src, *lim = sv->src + sv->len;
	Tag t;

	while ((p = next_tag(p, lim, &t))) {
		const char *end, *q;

		p = t.end;
		if (t.closing || strcmp(t.name, "style") || t.selfclose)
			continue;
		end = strstr(p, "</");
		if (!end || end > lim)
			return;
		for (q = p; q < end && sv->ncls < MAX_CLASSES; ) {
			const char *ob = memchr(q, '{', (size_t)(end - q)), *cb, *dot;
			size_t n;

			if (!ob || !(cb = memchr(ob, '}', (size_t)(end - ob))))
				break;
			/* every ".name" before the brace gets the declarations */
			for (dot = q; dot < ob; dot++) {
				if (*dot != '.')
					continue;
				n = 0;
				while (dot + 1 + n < ob && (isalnum((unsigned char)dot[1 + n]) ||
				    dot[1 + n] == '-' || dot[1 + n] == '_'))
					n++;
				if (n && n < 48 && sv->ncls < MAX_CLASSES) {
					size_t dl = (size_t)(cb - ob - 1);

					memcpy(sv->cls[sv->ncls].name, dot + 1, n);
					sv->cls[sv->ncls].name[n] = '\0';
					if (dl >= sizeof sv->cls[0].decl)
						dl = sizeof sv->cls[0].decl - 1;
					memcpy(sv->cls[sv->ncls].decl, ob + 1, dl);
					sv->cls[sv->ncls].decl[dl] = '\0';
					sv->ncls++;
				}
			}
			q = cb + 1;
		}
		p = end;
	}
}

/* --------------------------------------------------------------- polygon */

#define MAX_POINTS (1 << 20)            /* per shape: a broken file stops */

static void
pt_add(Svg *sv, const Mat *m, double x, double y)
{
	if (sv->npts >= MAX_POINTS)
		return;
	if (sv->npts + 1 > sv->cappts) {
		sv->cappts = sv->cappts ? sv->cappts * 2 : 256;
		sv->pts = rui_realloc(sv->pts, (size_t)sv->cappts * 2 * sizeof *sv->pts);
	}
	apply(m, x, y, &sv->pts[sv->npts * 2], &sv->pts[sv->npts * 2 + 1]);
	sv->npts++;
}

static void
sub_start(Svg *sv)
{
	if (sv->nstarts && sv->starts[sv->nstarts - 1] == sv->npts)
		return;                            /* empty subpath: reuse it */
	if (sv->nstarts + 1 > sv->capstarts) {
		sv->capstarts = sv->capstarts ? sv->capstarts * 2 : 16;
		sv->starts = rui_realloc(sv->starts, (size_t)sv->capstarts *
		    sizeof *sv->starts);
	}
	sv->starts[sv->nstarts++] = sv->npts;
}

static void
poly_reset(Svg *sv)
{
	sv->npts = 0;
	sv->nstarts = 0;
}

static int
curve_steps(const Mat *m, double len)
{
	double scale = sqrt(fabs(m->a * m->d - m->b * m->c));
	int n = (int)(len * scale / 3) + 4;

	return n > 64 ? 64 : n;
}

static void
cubic(Svg *sv, const Mat *m, double x0, double y0, double x1, double y1,
    double x2, double y2, double x3, double y3)
{
	double len = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) +
	    hypot(x3 - x2, y3 - y2);
	int n = curve_steps(m, len), i;

	for (i = 1; i <= n; i++) {
		double t = (double)i / n, u = 1 - t;

		pt_add(sv, m, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 +
		    t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 +
		    3 * u * t * t * y2 + t * t * t * y3);
	}
}

static void
arc(Svg *sv, const Mat *m, double x1, double y1, double rx, double ry,
    double phi, int large, int sweep, double x2, double y2)
{
	double cp, sp, dx, dy, x1p, y1p, lam, sq, num, den, cxp, cyp, cx, cy;
	double th1, dth, ux, uy, vx, vy;
	int n, i;

	if (rx == 0 || ry == 0) {
		pt_add(sv, m, x2, y2);
		return;
	}
	rx = fabs(rx);
	ry = fabs(ry);
	phi *= M_PI / 180;
	cp = cos(phi);
	sp = sin(phi);
	dx = (x1 - x2) / 2;
	dy = (y1 - y2) / 2;
	x1p = cp * dx + sp * dy;
	y1p = -sp * dx + cp * dy;
	lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
	if (lam > 1) {
		rx *= sqrt(lam);
		ry *= sqrt(lam);
	}
	num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	sq = den > 0 && num > 0 ? sqrt(num / den) : 0;
	if (large == sweep)
		sq = -sq;
	cxp = sq * rx * y1p / ry;
	cyp = -sq * ry * x1p / rx;
	cx = cp * cxp - sp * cyp + (x1 + x2) / 2;
	cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
	ux = (x1p - cxp) / rx;
	uy = (y1p - cyp) / ry;
	vx = (-x1p - cxp) / rx;
	vy = (-y1p - cyp) / ry;
	th1 = atan2(uy, ux);
	dth = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
	if (!sweep && dth > 0)
		dth -= 2 * M_PI;
	else if (sweep && dth < 0)
		dth += 2 * M_PI;
	n = curve_steps(m, fabs(dth) * (rx > ry ? rx : ry));
	for (i = 1; i <= n; i++) {
		double t = th1 + dth * i / n;
		double ex = rx * cos(t), ey = ry * sin(t);

		pt_add(sv, m, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
	}
}

/* path data -> subpaths; closed[] marks the closed ones for stroking */
static void
path_data(Svg *sv, const Mat *m, const char *d)
{
	double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
	char cmd = 0, prev = 0;
	const char *p = d;

	while (p && *p) {
		double v[7];
		int need, i, rel;
		char c;

		p = skip_sep(p);
		if (!*p)
			break;
		if (isalpha((unsigned char)*p))
			cmd = *p++;
		else if (!cmd)
			break;
		c = (char)toupper((unsigned char)cmd);
		rel = cmd != c;
		need = c == 'M' || c == 'L' || c == 'T' ? 2 : c == 'H' || c == 'V' ?
		    1 : c == 'C' ? 6 : c == 'S' || c == 'Q' ? 4 : c == 'A' ? 7 : 0;
		if (c == 'Z') {
			pt_add(sv, m, sx, sy);
			cx = sx;
			cy = sy;
			sub_start(sv);
			pt_add(sv, m, cx, cy);
			prev = 'Z';
			cmd = 0;                       /* numbers after Z are an error */
			continue;
		}
		for (i = 0; i < need; i++) {
			const char *q;

			if (c == 'A' && (i == 3 || i == 4)) {
				/* flags may be written without separators: "a1 1 0 01 1 1" */
				p = skip_sep(p);
				if (*p != '0' && *p != '1')
					return;
				v[i] = *p++ - '0';
				continue;
			}
			if (!(q = number(p, &v[i])))
				return;
			p = q;
		}
		switch (c) {
		case 'M':
			if (rel) { v[0] += cx; v[1] += cy; }
			cx = sx = v[0];
			cy = sy = v[1];
			sub_start(sv);
			pt_add(sv, m, cx, cy);
			cmd = rel ? 'l' : 'L';             /* more pairs are lines */
			break;
		case 'L':
			if (rel) { v[0] += cx; v[1] += cy; }
			cx = v[0];
			cy = v[1];
			pt_add(sv, m, cx, cy);
			break;
		case 'H':
			cx = rel ? cx + v[0] : v[0];
			pt_add(sv, m, cx, cy);
			break;
		case 'V':
			cy = rel ? cy + v[0] : v[0];
			pt_add(sv, m, cx, cy);
			break;
		case 'C':
			if (rel)
				for (i = 0; i < 6; i += 2) { v[i] += cx; v[i + 1] += cy; }
			cubic(sv, m, cx, cy, v[0], v[1], v[2], v[3], v[4], v[5]);
			lcx = v[2]; lcy = v[3];
			cx = v[4]; cy = v[5];
			break;
		case 'S': {
			double x1 = cx, y1 = cy;

			if (rel)
				for (i = 0; i < 4; i += 2) { v[i] += cx; v[i + 1] += cy; }
			if (prev == 'C' || prev == 'S') {
				x1 = 2 * cx - lcx;
				y1 = 2 * cy - lcy;
			}
			cubic(sv, m, cx, cy, x1, y1, v[0], v[1], v[2], v[3]);
			lcx = v[0]; lcy = v[1];
			cx = v[2]; cy = v[3];
			break;
		}
		case 'Q':
		case 'T': {
			double qx, qy, ex, ey;

			if (c == 'Q') {
				if (rel)
					for (i = 0; i < 4; i += 2) { v[i] += cx; v[i + 1] += cy; }
				qx = v[0]; qy = v[1]; ex = v[2]; ey = v[3];
			} else {
				if (rel) { v[0] += cx; v[1] += cy; }
				qx = prev == 'Q' || prev == 'T' ? 2 * cx - lcx : cx;
				qy = prev == 'Q' || prev == 'T' ? 2 * cy - lcy : cy;
				ex = v[0]; ey = v[1];
			}
			cubic(sv, m, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 *
			    (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey),
			    ex, ey);
			lcx = qx; lcy = qy;
			cx = ex; cy = ey;
			break;
		}
		case 'A':
			if (rel) { v[5] += cx; v[6] += cy; }
			arc(sv, m, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], v[5],
			    v[6]);
			cx = v[5];
			cy = v[6];
			break;
		default:
			return;
		}
		prev = c;
	}
}

static void
ellipse(Svg *sv, const Mat *m, double cx, double cy, double rx, double ry)
{
	int n = curve_steps(m, 2 * M_PI * (rx > ry ? rx : ry)), i;

	sub_start(sv);
	for (i = 0; i <= n; i++) {
		double t = 2 * M_PI * i / n;

		pt_add(sv, m, cx + rx * cos(t), cy + ry * sin(t));
	}
}

/* ------------------------------------------------------------ rasterise */

typedef struct PaintCtx {
	const Grad *g;
	Mat inv;                            /* device -> gradient space       */
	unsigned rgb;
	double op;
} PaintCtx;

static void
grad_color(const Grad *g, double t, unsigned *rgb, double *op)
{
	int i;

	if (!g->nstops) {
		*rgb = 0;
		*op = 0;
		return;
	}
	t = clamp01(t);
	if (t <= g->stops[0].off) {
		*rgb = g->stops[0].rgb;
		*op = g->stops[0].op;
		return;
	}
	for (i = 1; i < g->nstops; i++) {
		const Stop *a = &g->stops[i - 1], *b = &g->stops[i];

		if (t <= b->off) {
			double f = b->off > a->off ? (t - a->off) / (b->off - a->off) : 1;
			int k;

			*rgb = 0;
			for (k = 0; k < 3; k++) {
				int ca = (int)(a->rgb >> (16 - 8 * k)) & 0xff,
				    cb = (int)(b->rgb >> (16 - 8 * k)) & 0xff;

				*rgb |= (unsigned)(ca + (cb - ca) * f + 0.5) << (16 - 8 * k);
			}
			*op = a->op + (b->op - a->op) * f;
			return;
		}
	}
	*rgb = g->stops[g->nstops - 1].rgb;
	*op = g->stops[g->nstops - 1].op;
}

static void
blend(Svg *sv, int x, int y, double cov, const PaintCtx *pc)
{
	unsigned rgb = pc->rgb;
	double a = pc->op, sa, da, oa;
	unsigned char *d;
	int k;

	if (pc->g) {
		double gx, gy, t, op;
		const Grad *g = pc->g;

		apply(&pc->inv, x + 0.5, y + 0.5, &gx, &gy);
		if (g->radial) {
			double dx = gx - g->fx, dy = gy - g->fy;

			t = g->r > 0 ? hypot(dx, dy) / g->r : 1;
		} else {
			double vx = g->x2 - g->x1, vy = g->y2 - g->y1,
			    l2 = vx * vx + vy * vy;

			t = l2 > 0 ? ((gx - g->x1) * vx + (gy - g->y1) * vy) / l2 : 0;
		}
		grad_color(g, t, &rgb, &op);
		a *= op;
	}
	sa = a * cov;
	if (sa <= 0)
		return;
	d = sv->px + ((size_t)y * (size_t)sv->w + (size_t)x) * 4;
	da = d[3] / 255.0;
	oa = sa + da * (1 - sa);
	for (k = 0; k < 3; k++) {
		double sc = (rgb >> (16 - 8 * k)) & 0xff;

		d[k] = (unsigned char)((sc * sa + d[k] * da * (1 - sa)) / oa + 0.5);
	}
	d[3] = (unsigned char)(oa * 255 + 0.5);
}

static int
cmp_double(const void *a, const void *b)
{
	double x = ((const double *)a)[0], y = ((const double *)b)[0];

	return x < y ? -1 : x > y;
}

/* fills the current polygon; closes every subpath */
static void
fill_poly(Svg *sv, int evenodd, const PaintCtx *pc)
{
	Edge *e;
	int ne = 0, i, s, y, miny = sv->h, maxy = -1;
	double *row, *xs;
	int cap_x;

	if (sv->npts < 3)
		return;
	e = rui_malloc((size_t)(sv->npts + sv->nstarts + 1) * sizeof *e);
	for (s = 0; s < sv->nstarts; s++) {
		int a = sv->starts[s], b = s + 1 < sv->nstarts ? sv->starts[s + 1] :
		    sv->npts;

		for (i = a; i < b; i++) {
			int j = i + 1 < b ? i + 1 : a;
			double x0 = sv->pts[i * 2], y0 = sv->pts[i * 2 + 1],
			    x1 = sv->pts[j * 2], y1 = sv->pts[j * 2 + 1];

			if (y0 == y1 || !isfinite(x0) || !isfinite(y0) ||
			    !isfinite(x1) || !isfinite(y1))
				continue;
			e[ne].dir = y1 > y0 ? 1 : -1;
			if (y0 > y1) {
				double t;

				t = x0; x0 = x1; x1 = t;
				t = y0; y0 = y1; y1 = t;
			}
			e[ne].x0 = x0; e[ne].y0 = y0; e[ne].x1 = x1; e[ne].y1 = y1;
			if ((int)floor(y0) < miny)
				miny = (int)floor(y0);
			if ((int)ceil(y1) > maxy)
				maxy = (int)ceil(y1);
			ne++;
		}
	}
	if (miny < 0)
		miny = 0;
	if (maxy > sv->h)
		maxy = sv->h;
	cap_x = ne * 2 + 2;
	xs = rui_malloc((size_t)cap_x * 2 * sizeof *xs);
	row = rui_malloc((size_t)(sv->w + 2) * sizeof *row);
	for (y = miny; y < maxy; y++) {
		int sub, x, any = 0;

		memset(row, 0, (size_t)(sv->w + 2) * sizeof *row);
		for (sub = 0; sub < SUB; sub++) {
			double sy = y + (sub + 0.5) / SUB;
			int n = 0, wind = 0;

			for (i = 0; i < ne; i++) {
				if (sy < e[i].y0 || sy >= e[i].y1)
					continue;
				xs[n * 2] = e[i].x0 + (sy - e[i].y0) * (e[i].x1 - e[i].x0) /
				    (e[i].y1 - e[i].y0);
				xs[n * 2 + 1] = e[i].dir;
				n++;
			}
			if (n < 2)
				continue;
			qsort(xs, (size_t)n, 2 * sizeof *xs, cmp_double);
			for (i = 0; i + 1 < n; i++) {
				int inside;
				double xa, xb;
				int ia, ib;

				wind += (int)xs[i * 2 + 1];
				inside = evenodd ? (i + 1) % 2 : wind != 0;
				if (!inside)
					continue;
				xa = xs[i * 2];
				xb = xs[(i + 1) * 2];
				if (xa < 0)
					xa = 0;
				if (xb > sv->w)
					xb = sv->w;
				if (xb <= xa)
					continue;
				any = 1;
				ia = (int)xa;
				ib = (int)xb;
				if (ia == ib) {
					row[ia] += (xb - xa) / SUB;
					continue;
				}
				row[ia] += (ia + 1 - xa) / SUB;
				for (x = ia + 1; x < ib; x++)
					row[x] += 1.0 / SUB;
				if (ib < sv->w)
					row[ib] += (xb - ib) / SUB;
			}
		}
		if (!any)
			continue;
		for (x = 0; x < sv->w; x++)
			if (row[x] > 0.002)
				blend(sv, x, y, row[x] > 1 ? 1 : row[x], pc);
	}
	free(row);
	free(xs);
	free(e);
}

static void
paint_ctx(Svg *sv, PaintCtx *pc, const Paint *p, double op, const Mat *m,
    double bx0, double by0, double bx1, double by1)
{
	memset(pc, 0, sizeof *pc);
	pc->op = op;
	pc->rgb = p->rgb;
	if (p->type == 2) {
		const Grad *g = &sv->grads[p->grad];
		Mat gm = g->xf, inv;

		if (!g->user_space) {              /* objectBoundingBox */
			Mat bb = { bx1 - bx0, 0, 0, by1 - by0, bx0, by0 };

			gm = mat_mul(gm, bb);
		}
		gm = mat_mul(gm, *m);
		if (mat_inv(gm, &inv) == 0 && g->nstops) {
			pc->g = g;
			pc->inv = inv;
		} else if (g->nstops) {
			pc->rgb = g->stops[g->nstops - 1].rgb;
		}
	}
}

/* the user-space bounding box of the current polygon, via the inverse */
static void
bbox(Svg *sv, const Mat *m, double *x0, double *y0, double *x1, double *y1)
{
	Mat inv;
	int i;

	*x0 = *y0 = 1e30;
	*x1 = *y1 = -1e30;
	if (mat_inv(*m, &inv) < 0)
		return;
	for (i = 0; i < sv->npts; i++) {
		double x, y;

		apply(&inv, sv->pts[i * 2], sv->pts[i * 2 + 1], &x, &y);
		if (x < *x0) *x0 = x;
		if (y < *y0) *y0 = y;
		if (x > *x1) *x1 = x;
		if (y > *y1) *y1 = y;
	}
}

/* a stroke: each segment as a quadrilateral plus a round join, unioned */
static void
stroke_poly(Svg *sv, const Mat *m, double width, const PaintCtx *pc)
{
	double scale = sqrt(fabs(m->a * m->d - m->b * m->c)), hw;
	double *pts = sv->pts;
	int *starts = sv->starts, npts = sv->npts, nstarts = sv->nstarts, s, i;
	Mat id = mat_id();

	hw = width * scale / 2;
	if (hw < 0.35)
		hw = 0.35;                         /* hairlines stay visible */
	sv->pts = NULL;
	sv->starts = NULL;
	sv->cappts = sv->capstarts = 0;
	sv->npts = sv->nstarts = 0;
	for (s = 0; s < nstarts; s++) {
		int a = starts[s], b = s + 1 < nstarts ? starts[s + 1] : npts;

		for (i = a; i < b; i++) {
			double x0 = pts[i * 2], y0 = pts[i * 2 + 1], x1, y1, dx, dy, l;
			int k;

			/* round joins and caps */
			sub_start(sv);
			for (k = 0; k < 12; k++)
				pt_add(sv, &id, x0 + hw * cos(k * M_PI / 6),
				    y0 + hw * sin(k * M_PI / 6));
			if (i + 1 >= b)
				continue;
			x1 = pts[(i + 1) * 2];
			y1 = pts[(i + 1) * 2 + 1];
			dx = x1 - x0;
			dy = y1 - y0;
			if ((l = hypot(dx, dy)) < 1e-9)
				continue;
			dx = dx / l * hw;
			dy = dy / l * hw;
			sub_start(sv);
			pt_add(sv, &id, x0 - dy, y0 + dx);
			pt_add(sv, &id, x1 - dy, y1 + dx);
			pt_add(sv, &id, x1 + dy, y1 - dx);
			pt_add(sv, &id, x0 + dy, y0 - dx);
		}
	}
	/* every piece is wound the same way except circles: nonzero is wrong
	 * for mixed windings, so fill each piece's union by counting any
	 * crossing - even-odd per piece would leave holes.  Use nonzero with
	 * consistent orientation: make all pieces counter-clockwise. */
	for (s = 0; s < sv->nstarts; s++) {
		int a = sv->starts[s], b = s + 1 < sv->nstarts ? sv->starts[s + 1] :
		    sv->npts, j;
		double area = 0;

		for (j = a; j < b; j++) {
			int k = j + 1 < b ? j + 1 : a;

			area += sv->pts[j * 2] * sv->pts[k * 2 + 1] -
			    sv->pts[k * 2] * sv->pts[j * 2 + 1];
		}
		if (area < 0) {                    /* reverse this piece */
			int lo = a, hi = b - 1;

			while (lo < hi) {
				double tx = sv->pts[lo * 2], ty = sv->pts[lo * 2 + 1];

				sv->pts[lo * 2] = sv->pts[hi * 2];
				sv->pts[lo * 2 + 1] = sv->pts[hi * 2 + 1];
				sv->pts[hi * 2] = tx;
				sv->pts[hi * 2 + 1] = ty;
				lo++;
				hi--;
			}
		}
	}
	fill_poly(sv, 0, pc);
	free(sv->pts);
	free(sv->starts);
	sv->pts = pts;
	sv->starts = starts;
	sv->npts = npts;
	sv->nstarts = nstarts;
	sv->cappts = npts;
	sv->capstarts = nstarts;
}

static void
draw_shape(Svg *sv, const Mat *m, const Style *st)
{
	double bx0, by0, bx1, by1;
	PaintCtx pc;

	if (!st->display || sv->npts < 2)
		return;
	bbox(sv, m, &bx0, &by0, &bx1, &by1);
	if (st->fill.type) {
		paint_ctx(sv, &pc, &st->fill, st->opacity * st->fill_op, m, bx0, by0,
		    bx1, by1);
		fill_poly(sv, st->evenodd, &pc);
	}
	if (st->stroke.type && st->stroke_w > 0) {
		paint_ctx(sv, &pc, &st->stroke, st->opacity * st->stroke_op, m, bx0,
		    by0, bx1, by1);
		stroke_poly(sv, m, st->stroke_w, &pc);
	}
}

/* ---------------------------------------------------------------- render */

/* applies an element's presentation attributes, class rules and style="" */
static void
element_style(Svg *sv, const Tag *t, Style *st)
{
	static const char *const props[] = { "fill", "stroke", "color", "opacity",
		"fill-opacity", "stroke-opacity", "stroke-width", "fill-rule",
		"display", "visibility" };
	size_t i;
	char *v;

	/* color first: currentColor in the same element refers to it */
	for (i = 0; i < sizeof props / sizeof props[0]; i++) {
		if ((v = attr(t, props[i]))) {
			style_prop(sv, st, props[i], strlen(props[i]), v);
			free(v);
		}
	}
	if ((v = attr(t, "class"))) {
		char *tok = v, *save = NULL;
		int k;

		for (tok = strtok_r(v, " \t", &save); tok;
		    tok = strtok_r(NULL, " \t", &save))
			for (k = 0; k < sv->ncls; k++)
				if (!strcmp(sv->cls[k].name, tok))
					style_decl(sv, st, sv->cls[k].decl);
		free(v);
	}
	if ((v = attr(t, "style"))) {
		style_decl(sv, st, v);
		free(v);
	}
}

static const char *render_from(Svg *sv, const char *p, const char *lim,
    Mat m, Style st, int depth, int single);

/* draws one element (already parsed into t); returns where to go on */
static const char *
element(Svg *sv, const Tag *t, const char *lim, Mat m, Style st, int depth)
{
	const char *p = t->end;
	char *v;

	if ((v = attr(t, "transform"))) {
		m = mat_mul(parse_transform(v), m);
		free(v);
	}
	element_style(sv, t, &st);
	poly_reset(sv);
	if (!strcmp(t->name, "g") || !strcmp(t->name, "a") ||
	    !strcmp(t->name, "switch")) {
		if (t->selfclose)
			return p;
		return render_from(sv, p, lim, m, st, depth + 1, 0);
	}
	if (!strcmp(t->name, "defs") || !strcmp(t->name, "symbol") ||
	    !strcmp(t->name, "clipPath") || !strcmp(t->name, "mask") ||
	    !strcmp(t->name, "pattern") || !strcmp(t->name, "marker") ||
	    !strcmp(t->name, "linearGradient") ||
	    !strcmp(t->name, "radialGradient") || !strcmp(t->name, "style") ||
	    !strcmp(t->name, "text") || !strcmp(t->name, "metadata") ||
	    !strcmp(t->name, "title") || !strcmp(t->name, "desc") ||
	    !strcmp(t->name, "filter") || !strcmp(t->name, "foreignObject")) {
		/* skip to the matching close tag */
		int level = 1;
		Tag u;

		if (t->selfclose)
			return p;
		while (level > 0 && (p = next_tag(p, lim, &u))) {
			p = u.end;
			if (!u.closing && !u.selfclose)
				level++;
			else if (u.closing)
				level--;
		}
		return p ? p : lim;
	}
	if (!strcmp(t->name, "path")) {
		if ((v = attr(t, "d"))) {
			path_data(sv, &m, v);
			free(v);
		}
	} else if (!strcmp(t->name, "rect")) {
		double x = attr_num(t, "x", 0), y = attr_num(t, "y", 0),
		    w = attr_num(t, "width", 0), h = attr_num(t, "height", 0),
		    rx = attr_num(t, "rx", -1), ry = attr_num(t, "ry", -1);

		if (rx < 0)
			rx = ry;
		if (ry < 0)
			ry = rx;
		if (rx < 0)
			rx = ry = 0;
		if (rx > w / 2) rx = w / 2;
		if (ry > h / 2) ry = h / 2;
		if (w > 0 && h > 0) {
			sub_start(sv);
			if (rx > 0 && ry > 0) {
				char d[512];

				snprintf(d, sizeof d, "M%g %gH%gA%g %g 0 0 1 %g %gV%gA%g %g 0 0 "
				    "1 %g %gH%gA%g %g 0 0 1 %g %gV%gA%g %g 0 0 1 %g %gZ",
				    x + rx, y, x + w - rx, rx, ry, x + w, y + ry, y + h - ry,
				    rx, ry, x + w - rx, y + h, x + rx, rx, ry, x, y + h - ry,
				    y + ry, rx, ry, x + rx, y);
				path_data(sv, &m, d);
			} else {
				pt_add(sv, &m, x, y);
				pt_add(sv, &m, x + w, y);
				pt_add(sv, &m, x + w, y + h);
				pt_add(sv, &m, x, y + h);
				pt_add(sv, &m, x, y);
			}
		}
	} else if (!strcmp(t->name, "circle")) {
		double r = attr_num(t, "r", 0);

		if (r > 0)
			ellipse(sv, &m, attr_num(t, "cx", 0), attr_num(t, "cy", 0), r, r);
	} else if (!strcmp(t->name, "ellipse")) {
		double rx = attr_num(t, "rx", 0), ry = attr_num(t, "ry", 0);

		if (rx > 0 && ry > 0)
			ellipse(sv, &m, attr_num(t, "cx", 0), attr_num(t, "cy", 0), rx,
			    ry);
	} else if (!strcmp(t->name, "line")) {
		sub_start(sv);
		pt_add(sv, &m, attr_num(t, "x1", 0), attr_num(t, "y1", 0));
		pt_add(sv, &m, attr_num(t, "x2", 0), attr_num(t, "y2", 0));
		st.fill.type = 0;
	} else if (!strcmp(t->name, "polyline") || !strcmp(t->name, "polygon")) {
		if ((v = attr(t, "points"))) {
			const char *q = v;
			double x, y;

			sub_start(sv);
			while ((q = number(q, &x)) && (q = number(q, &y)))
				pt_add(sv, &m, x, y);
			free(v);
		}
	} else if (!strcmp(t->name, "use") && sv->use_depth < 8) {
		char *href = attr(t, "href");
		double ux = attr_num(t, "x", 0), uy = attr_num(t, "y", 0);

		if (href && href[0] == '#') {
			/* find the element with that id and draw it here */
			const char *q = sv->src, *slim = sv->src + sv->len;
			Tag u;

			while ((q = next_tag(q, slim, &u))) {
				char *id;

				q = u.end;
				if (u.closing || !(id = attr(&u, "id")))
					continue;
				if (!strcmp(id, href + 1)) {
					Mat tm = { 1, 0, 0, 1, ux, uy };

					free(id);
					sv->use_depth++;
					if (!strcmp(u.name, "symbol") || !strcmp(u.name, "g"))
						render_from(sv, u.end, slim, mat_mul(tm, m), st,
						    depth + 1, 0);
					else
						element(sv, &u, slim, mat_mul(tm, m), st, depth + 1);
					sv->use_depth--;
					break;
				}
				free(id);
			}
		}
		free(href);
		poly_reset(sv);
	}
	draw_shape(sv, &m, &st);
	poly_reset(sv);
	if (!t->selfclose && !t->closing) {
		/* an element with children we do not understand: skip them */
		int level = 1;
		Tag u;
		const char *q = p;

		while (level > 0 && (q = next_tag(q, lim, &u))) {
			q = u.end;
			if (!u.closing && !u.selfclose)
				level++;
			else if (u.closing)
				level--;
		}
		return q ? q : lim;
	}
	return p;
}

/* draws siblings until the parent's close tag; returns past it */
static const char *
render_from(Svg *sv, const char *p, const char *lim, Mat m, Style st,
    int depth, int single)
{
	Tag t;

	(void)single;
	if (depth > MAX_DEPTH)
		return lim;
	while ((p = next_tag(p, lim, &t))) {
		if (t.closing)
			return t.end;
		p = element(sv, &t, lim, m, st, depth);
	}
	return lim;
}

/* SVG text -> RGBA w*h (malloc'd).  *w / *h: wanted size (0 = the
 * document's own); the picture keeps its aspect ratio, centred. */
unsigned char *
rui_svg_render(const char *src, size_t len, int *w, int *h)
{
	Svg *sv;
	const char *p, *lim = src + len;
	Tag t;
	double vx = 0, vy = 0, vw = 0, vh = 0, dw, dh, sc;
	Style st;
	Mat m;
	char *v;
	unsigned char *out;

	/* find <svg> */
	for (p = src; (p = next_tag(p, lim, &t)); p = t.end)
		if (!t.closing && !strcmp(t.name, "svg"))
			break;
	if (!p)
		return NULL;
	v = attr(&t, "width");
	dw = length_px(v, 0);
	free(v);
	v = attr(&t, "height");
	dh = length_px(v, 0);
	free(v);
	if ((v = attr(&t, "viewBox"))) {
		const char *q = v;

		if (!(q = number(q, &vx)) || !(q = number(q, &vy)) ||
		    !(q = number(q, &vw)) || !number(q, &vh))
			vw = vh = 0;
		free(v);
	}
	if (vw <= 0 || vh <= 0) {
		vx = vy = 0;
		vw = dw > 0 ? dw : 100;
		vh = dh > 0 ? dh : 100;
	}
	if (dw <= 0)
		dw = vw;
	if (dh <= 0)
		dh = vh;
	if (*w <= 0 || *h <= 0) {
		*w = (int)ceil(dw);
		*h = (int)ceil(dh);
	}
	if (*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192)
		return NULL;
	sv = rui_calloc(1, sizeof *sv);
	sv->src = src;
	sv->len = len;
	sv->w = *w;
	sv->h = *h;
	sv->px = rui_calloc((size_t)*w * (size_t)*h, 4);
	read_gradients(sv);
	read_classes(sv);
	sc = fmin(*w / vw, *h / vh);
	m.a = sc; m.b = 0; m.c = 0; m.d = sc;
	m.e = (*w - vw * sc) / 2 - vx * sc;
	m.f = (*h - vh * sc) / 2 - vy * sc;
	memset(&st, 0, sizeof st);
	st.fill.type = 1;                      /* black by default */
	st.fill.rgb = 0;
	st.opacity = st.fill_op = st.stroke_op = 1;
	st.stroke_w = 1;
	st.display = 1;
	element_style(sv, &t, &st);
	if (!t.selfclose)
		render_from(sv, t.end, lim, m, st, 0, 0);
	out = sv->px;
	free(sv->pts);
	free(sv->starts);
	free(sv);
	return out;
}
