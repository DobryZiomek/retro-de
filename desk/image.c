/* Minimal PPM (P3/P6) and XPM3 loaders - no external image libraries. */
#include "image.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DIM 16384

static char *
read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	size_t cap = 65536, n = 0, r;
	char *buf;

	if (!f)
		return NULL;
	buf = rui_malloc(cap);
	while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
		n += r;
		if (n + 1 >= cap) {
			if (cap > ((size_t)256 << 20)) {
				free(buf);
				fclose(f);
				return NULL;
			}
			cap *= 2;
			buf = rui_realloc(buf, cap);
		}
	}
	fclose(f);
	buf[n] = '\0';
	*len = n;
	return buf;
}

static Image *
image_new(int w, int h)
{
	Image *img;

	if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM)
		return NULL;
	img = rui_calloc(1, sizeof *img);
	img->w = w;
	img->h = h;
	img->rgb = rui_calloc((size_t)w * (size_t)h, 3);
	return img;
}

void
image_free(Image *img)
{
	if (!img)
		return;
	free(img->rgb);
	free(img->alpha);
	free(img);
}

Image *
image_fit(Image *img, int max)
{
	Image *out;
	int w, h, x, y;

	if (!img || (img->w <= max && img->h <= max))
		return img;
	if (img->w >= img->h) {
		w = max;
		h = (int)((long)img->h * max / img->w);
	} else {
		h = max;
		w = (int)((long)img->w * max / img->h);
	}
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (!(out = image_new(w, h)))
		return img;
	if (img->alpha)
		out->alpha = rui_calloc((size_t)w * (size_t)h, 1);
	for (y = 0; y < h; y++) {
		int sy = (int)((long)y * img->h / h);

		for (x = 0; x < w; x++) {
			int sx = (int)((long)x * img->w / w);
			size_t si = (size_t)sy * (size_t)img->w + (size_t)sx;
			size_t di = (size_t)y * (size_t)w + (size_t)x;

			memcpy(out->rgb + di * 3, img->rgb + si * 3, 3);
			if (img->alpha)
				out->alpha[di] = img->alpha[si];
		}
	}
	image_free(img);
	return out;
}

/* -------------------------------------------------------------------- PPM */

/* next unsigned integer in a PNM header/body, skipping blanks and comments */
static int
pnm_int(const char *buf, size_t len, size_t *pos, long *val)
{
	size_t p = *pos;
	long v = 0;

	for (;;) {
		while (p < len && isspace((unsigned char)buf[p]))
			p++;
		if (p < len && buf[p] == '#') {
			while (p < len && buf[p] != '\n')
				p++;
			continue;
		}
		break;
	}
	if (p >= len || !isdigit((unsigned char)buf[p]))
		return 0;
	while (p < len && isdigit((unsigned char)buf[p])) {
		v = v * 10 + (buf[p] - '0');
		if (v > 1000000)
			return 0;
		p++;
	}
	*pos = p;
	*val = v;
	return 1;
}

static Image *
load_ppm(const char *buf, size_t len)
{
	size_t pos = 2, n, i;
	long w, h, maxv, v;
	int binary = buf[1] == '6';
	Image *img;

	if (!pnm_int(buf, len, &pos, &w) || !pnm_int(buf, len, &pos, &h) ||
	    !pnm_int(buf, len, &pos, &maxv) || maxv < 1 || maxv > 65535)
		return NULL;
	img = image_new((int)w, (int)h);
	if (!img)
		return NULL;
	n = (size_t)w * (size_t)h * 3;
	if (binary) {
		size_t bytes = maxv > 255 ? 2 : 1;

		pos++;                              /* single whitespace byte */
		if (pos > len || len - pos < n * bytes) {
			image_free(img);
			return NULL;
		}
		for (i = 0; i < n; i++) {
			if (bytes == 2)
				v = ((unsigned char)buf[pos] << 8) | (unsigned char)buf[pos + 1];
			else
				v = (unsigned char)buf[pos];
			pos += bytes;
			img->rgb[i] = (unsigned char)(v * 255 / maxv);
		}
	} else {
		for (i = 0; i < n; i++) {
			if (!pnm_int(buf, len, &pos, &v)) {
				image_free(img);
				return NULL;
			}
			if (v > maxv)
				v = maxv;
			img->rgb[i] = (unsigned char)(v * 255 / maxv);
		}
	}
	return img;
}

/* -------------------------------------------------------------------- XPM */

/* Copies the next C string literal into out; returns 0 when none is left. */
static int
xpm_string(const char *buf, size_t len, size_t *pos, char **out, size_t *cap)
{
	size_t p = *pos, o = 0;

	for (;;) {
		if (p >= len)
			return 0;
		if (buf[p] == '/' && p + 1 < len && buf[p + 1] == '*') {
			p += 2;
			while (p + 1 < len && !(buf[p] == '*' && buf[p + 1] == '/'))
				p++;
			p += 2;
			continue;
		}
		if (buf[p] == '"')
			break;
		p++;
	}
	p++;
	while (p < len && buf[p] != '"') {
		if (buf[p] == '\\' && p + 1 < len)
			p++;
		if (o + 2 > *cap) {
			*cap = *cap ? *cap * 2 : 256;
			*out = rui_realloc(*out, *cap);
		}
		(*out)[o++] = buf[p++];
	}
	if (p >= len)
		return 0;
	if (o + 1 > *cap) {
		*cap = *cap ? *cap * 2 : 256;
		*out = rui_realloc(*out, *cap);
	}
	(*out)[o] = '\0';
	*pos = p + 1;
	return 1;
}

typedef struct XpmColor {
	char key[8];
	unsigned char r, g, b, transparent;
} XpmColor;

static void
xpm_parse_color(RuiApp *app, const char *spec, XpmColor *c)
{
	/* spec: {key value}..., keys are c m g g4 s; we want the "c" value */
	char name[128] = "";
	const char *p = spec;
	XColor xc;

	while (*p) {
		char key[8], val[128];
		size_t k = 0, v = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		while (*p && *p != ' ' && *p != '\t' && k + 1 < sizeof key)
			key[k++] = *p++;
		key[k] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		/* value runs until the next key token or end */
		while (*p) {
			const char *q = p;
			char tok[8];
			size_t t = 0;

			if (*p == ' ' || *p == '\t') {
				while (*q == ' ' || *q == '\t')
					q++;
				while (q[t] && q[t] != ' ' && q[t] != '\t' && t + 1 < sizeof tok) {
					tok[t] = q[t];
					t++;
				}
				tok[t] = '\0';
				if (!strcmp(tok, "c") || !strcmp(tok, "m") ||
				    !strcmp(tok, "g") || !strcmp(tok, "g4") ||
				    !strcmp(tok, "s") || !*tok)
					break;
			}
			if (v + 1 < sizeof val)
				val[v++] = *p;
			p++;
		}
		val[v] = '\0';
		if (!strcmp(key, "c") || (!name[0] && strcmp(key, "s")))
			rui_strlcpy(name, val, sizeof name);
		if (!strcmp(key, "c"))
			break;
	}
	c->r = c->g = c->b = 0;
	c->transparent = 0;
	if (!name[0] || !strcasecmp(name, "None")) {
		c->transparent = 1;
		return;
	}
	if (XParseColor(app->dpy, app->cmap, name, &xc)) {
		c->r = (unsigned char)(xc.red >> 8);
		c->g = (unsigned char)(xc.green >> 8);
		c->b = (unsigned char)(xc.blue >> 8);
	}
}

static Image *
load_xpm(RuiApp *app, const char *buf, size_t len)
{
	size_t pos = 0, cap = 0;
	char *str = NULL;
	int w, h, ncol, cpp, i, x, y, any_alpha = 0;
	XpmColor *cols = NULL;
	int *index = NULL;                     /* direct lookup for cpp <= 2 */
	Image *img = NULL;

	if (!xpm_string(buf, len, &pos, &str, &cap) ||
	    sscanf(str, "%d %d %d %d", &w, &h, &ncol, &cpp) != 4 ||
	    ncol < 1 || ncol > 65536 || cpp < 1 || cpp > 7)
		goto fail;
	img = image_new(w, h);
	if (!img)
		goto fail;
	cols = rui_calloc((size_t)ncol, sizeof *cols);
	if (cpp <= 2) {
		index = rui_malloc(65536 * sizeof *index);
		for (i = 0; i < 65536; i++)
			index[i] = -1;
	}
	for (i = 0; i < ncol; i++) {
		if (!xpm_string(buf, len, &pos, &str, &cap) ||
		    (int)strlen(str) < cpp)
			goto fail;
		memcpy(cols[i].key, str, (size_t)cpp);
		cols[i].key[cpp] = '\0';
		xpm_parse_color(app, str + cpp, &cols[i]);
		if (cols[i].transparent)
			any_alpha = 1;
		if (index) {
			int k = (unsigned char)str[0];

			if (cpp == 2)
				k = (k << 8) | (unsigned char)str[1];
			index[k] = i;
		}
	}
	if (any_alpha)
		img->alpha = rui_calloc((size_t)w * (size_t)h, 1);
	for (y = 0; y < h; y++) {
		int last = 0;

		if (!xpm_string(buf, len, &pos, &str, &cap) ||
		    (int)strlen(str) < w * cpp)
			goto fail;
		for (x = 0; x < w; x++) {
			const char *px = str + x * cpp;
			int ci = -1;

			if (index) {
				int k = (unsigned char)px[0];

				if (cpp == 2)
					k = (k << 8) | (unsigned char)px[1];
				ci = index[k];
			} else if (!memcmp(cols[last].key, px, (size_t)cpp)) {
				ci = last;
			} else {
				for (i = 0; i < ncol; i++) {
					if (!memcmp(cols[i].key, px, (size_t)cpp)) {
						ci = last = i;
						break;
					}
				}
			}
			if (ci >= 0) {
				unsigned char *d = img->rgb + ((size_t)y * (size_t)w +
				    (size_t)x) * 3;

				d[0] = cols[ci].r;
				d[1] = cols[ci].g;
				d[2] = cols[ci].b;
				if (img->alpha)
					img->alpha[(size_t)y * (size_t)w + (size_t)x] =
					    !cols[ci].transparent;
			}
		}
	}
	free(str);
	free(cols);
	free(index);
	return img;
fail:
	free(str);
	free(cols);
	free(index);
	image_free(img);
	return NULL;
}

Image *
image_load(RuiApp *app, const char *path)
{
	size_t len = 0;
	char *buf = read_file(path, &len);
	Image *img = NULL;

	if (!buf) {
		rui_warn("cannot read image '%s'", path);
		return NULL;
	}
	if (len > 2 && buf[0] == 'P' && (buf[1] == '6' || buf[1] == '3'))
		img = load_ppm(buf, len);
	else if (strstr(buf, "XPM") || strchr(buf, '"'))
		img = load_xpm(app, buf, len);
	free(buf);
	if (!img)
		rui_warn("'%s' is not a valid PPM (P3/P6) or XPM image", path);
	return img;
}

/* -------------------------------------------------------------- rendering */

typedef struct Chan {
	int shift, bits;
} Chan;

static Chan
chan_of(unsigned long mask)
{
	Chan c = { 0, 0 };

	if (!mask)
		return c;
	while (!(mask & 1)) {
		mask >>= 1;
		c.shift++;
	}
	while (mask & 1) {
		mask >>= 1;
		c.bits++;
	}
	return c;
}

static unsigned long
chan_put(Chan c, unsigned v)
{
	if (c.bits >= 8)
		return (unsigned long)v << (c.shift + c.bits - 8);
	return (unsigned long)(v >> (8 - c.bits)) << c.shift;
}

int
image_mode(const char *name)
{
	if (!strcmp(name, "center"))
		return IMG_CENTER;
	if (!strcmp(name, "stretch"))
		return IMG_STRETCH;
	if (!strcmp(name, "fill") || !strcmp(name, "zoom"))
		return IMG_FILL;
	return IMG_TILE;
}

/* Builds an XImage of size w x h.  Unscaled: pixel (x, y) is img(x, y).
 * Scaled: the source rectangle (sx0, sy0, sw, sh) is stretched over it,
 * bilinearly when enlarging so gradients stay smooth. */
static XImage *
make_ximage(RuiApp *app, const Image *img, int w, int h, int scaled,
    double sx0, double sy0, double sw, double sh)
{
	Chan cr, cg, cb;
	XImage *xi;
	int x, y;

	if (app->vis->class != TrueColor && app->vis->class != DirectColor) {
		rui_warn("images need a TrueColor visual");
		return NULL;
	}
	xi = XCreateImage(app->dpy, app->vis, (unsigned)app->depth, ZPixmap, 0,
	    NULL, (unsigned)w, (unsigned)h, 32, 0);
	if (!xi)
		return NULL;
	xi->data = malloc((size_t)xi->bytes_per_line * (size_t)h);
	if (!xi->data) {
		XDestroyImage(xi);
		return NULL;
	}
	cr = chan_of(app->vis->red_mask);
	cg = chan_of(app->vis->green_mask);
	cb = chan_of(app->vis->blue_mask);
	for (y = 0; y < h; y++) {
		double fy = scaled ? sy0 + (y + 0.5) * sh / h - 0.5 : y;
		int y0 = fy < 0 ? 0 : (int)fy, y1;
		int wy;

		if (y0 > img->h - 1)
			y0 = img->h - 1;
		y1 = y0 + 1 < img->h ? y0 + 1 : y0;
		wy = fy < 0 ? 0 : (int)((fy - y0) * 256);
		for (x = 0; x < w; x++) {
			const unsigned char *p;
			unsigned r, g, b;

			if (!scaled) {
				p = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
				r = p[0];
				g = p[1];
				b = p[2];
			} else {
				double fx = sx0 + (x + 0.5) * sw / w - 0.5;
				int x0 = fx < 0 ? 0 : (int)fx, x1, wx;
				const unsigned char *q00, *q01, *q10, *q11;

				if (x0 > img->w - 1)
					x0 = img->w - 1;
				x1 = x0 + 1 < img->w ? x0 + 1 : x0;
				wx = fx < 0 ? 0 : (int)((fx - x0) * 256);
				q00 = img->rgb + ((size_t)y0 * (size_t)img->w + (size_t)x0) * 3;
				q01 = img->rgb + ((size_t)y0 * (size_t)img->w + (size_t)x1) * 3;
				q10 = img->rgb + ((size_t)y1 * (size_t)img->w + (size_t)x0) * 3;
				q11 = img->rgb + ((size_t)y1 * (size_t)img->w + (size_t)x1) * 3;
#define LERP(i) ((unsigned)(((q00[i] * (256 - wx) + q01[i] * wx) * (256 - wy) + \
    (q10[i] * (256 - wx) + q11[i] * wx) * wy) >> 16))
				r = LERP(0);
				g = LERP(1);
				b = LERP(2);
#undef LERP
			}
			XPutPixel(xi, x, y, chan_put(cr, r) | chan_put(cg, g) |
			    chan_put(cb, b));
		}
	}
	return xi;
}

int
image_render_at(RuiApp *app, const Image *img, Pixmap dst, int ox, int oy,
    int w, int h, int mode)
{
	XRectangle clip;
	XImage *xi;
	int x, y;

	if (w <= 0 || h <= 0 || img->w <= 0 || img->h <= 0)
		return -1;
	if (mode == IMG_STRETCH || mode == IMG_FILL) {
		double sx0 = 0, sy0 = 0, sw = img->w, sh = img->h;

		if (mode == IMG_FILL) {
			/* crop the source to the target's aspect ratio */
			if ((double)img->w * h > (double)img->h * w) {
				sw = (double)img->h * w / h;
				sx0 = (img->w - sw) / 2;
			} else {
				sh = (double)img->w * h / w;
				sy0 = (img->h - sh) / 2;
			}
		}
		xi = make_ximage(app, img, w, h, 1, sx0, sy0, sw, sh);
		if (!xi)
			return -1;
		XPutImage(app->dpy, dst, app->gc, xi, 0, 0, ox, oy, (unsigned)w,
		    (unsigned)h);
		XDestroyImage(xi);
		return 0;
	}
	xi = make_ximage(app, img, img->w, img->h, 0, 0, 0, 0, 0);
	if (!xi)
		return -1;
	clip.x = (short)ox;
	clip.y = (short)oy;
	clip.width = (unsigned short)w;
	clip.height = (unsigned short)h;
	XSetClipRectangles(app->dpy, app->gc, 0, 0, &clip, 1, Unsorted);
	if (mode == IMG_CENTER) {
		XPutImage(app->dpy, dst, app->gc, xi, 0, 0, ox + (w - img->w) / 2,
		    oy + (h - img->h) / 2, (unsigned)img->w, (unsigned)img->h);
	} else {
		for (y = 0; y < h; y += img->h)
			for (x = 0; x < w; x += img->w)
				XPutImage(app->dpy, dst, app->gc, xi, 0, 0, ox + x, oy + y,
				    (unsigned)img->w, (unsigned)img->h);
	}
	XSetClipMask(app->dpy, app->gc, None);
	XDestroyImage(xi);
	return 0;
}

int
image_render(RuiApp *app, const Image *img, Pixmap dst, int w, int h, int mode)
{
	return image_render_at(app, img, dst, 0, 0, w, h, mode);
}

int
image_draw(RuiApp *app, const Image *img, Drawable dst, int x, int y)
{
	XImage *xi = make_ximage(app, img, img->w, img->h, 0, 0, 0, 0, 0);
	Pixmap mask = None;

	if (!xi)
		return -1;
	if (img->alpha) {
		int stride = (img->w + 7) / 8, i, j;
		char *bits = rui_calloc((size_t)stride * (size_t)img->h, 1);

		for (j = 0; j < img->h; j++)
			for (i = 0; i < img->w; i++)
				if (img->alpha[(size_t)j * (size_t)img->w + (size_t)i])
					bits[j * stride + i / 8] |= (char)(1 << (i % 8));
		mask = XCreateBitmapFromData(app->dpy, dst, bits, (unsigned)img->w,
		    (unsigned)img->h);
		free(bits);
		XSetClipMask(app->dpy, app->gc, mask);
		XSetClipOrigin(app->dpy, app->gc, x, y);
	}
	XPutImage(app->dpy, dst, app->gc, xi, 0, 0, x, y, (unsigned)img->w,
	    (unsigned)img->h);
	if (mask != None) {
		XSetClipMask(app->dpy, app->gc, None);
		XFreePixmap(app->dpy, mask);
	}
	XDestroyImage(xi);
	return 0;
}
