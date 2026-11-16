/*
 * mkwall - draws the retro-de wallpapers.
 *
 *   mkwall <output-dir> [width height]
 *
 * retro-de reads only PPM and XPM, and neither compresses, so the pictures
 * are not shipped: this program paints them at build time.  Scenes come out
 * as binary PPM at the requested size (the desktop scales them per
 * monitor); patterns come out as small XPM tiles.  Everything is
 * deterministic - the same build gives the same files.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double r, g, b; } Rgb;

static int W = 1280, H = 800;
static unsigned char *px;

/* ---------------------------------------------------------------- helpers */

static double
clamp01(double v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static Rgb
rgb(int hex)
{
	Rgb c;

	c.r = ((hex >> 16) & 255) / 255.0;
	c.g = ((hex >> 8) & 255) / 255.0;
	c.b = (hex & 255) / 255.0;
	return c;
}

static Rgb
mix(Rgb a, Rgb b, double t)
{
	Rgb c;

	t = clamp01(t);
	c.r = a.r + (b.r - a.r) * t;
	c.g = a.g + (b.g - a.g) * t;
	c.b = a.b + (b.b - a.b) * t;
	return c;
}

/* colour at position t (0..1) of a gradient with n evenly spaced stops */
static Rgb
ramp(const int *stops, int n, double t)
{
	double f = clamp01(t) * (n - 1);
	int i = (int)f;

	if (i >= n - 1)
		return rgb(stops[n - 1]);
	return mix(rgb(stops[i]), rgb(stops[i + 1]), f - i);
}

static double
smooth(double e0, double e1, double x)
{
	double t = clamp01((x - e0) / (e1 - e0));

	return t * t * (3 - 2 * t);
}

static unsigned
hash2(int x, int y, unsigned seed)
{
	unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u +
	    seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static double
rnd2(int x, int y, unsigned seed)
{
	return (hash2(x, y, seed) & 0xffffff) / 16777216.0;
}

/* smooth value noise, 0..1 */
static double
noise(double x, double y, unsigned seed)
{
	int xi = (int)floor(x), yi = (int)floor(y);
	double fx = x - xi, fy = y - yi;
	double u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
	double a = rnd2(xi, yi, seed), b = rnd2(xi + 1, yi, seed);
	double c = rnd2(xi, yi + 1, seed), d = rnd2(xi + 1, yi + 1, seed);

	return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

/* fractal noise: `oct` octaves, 0..1 */
static double
fbm(double x, double y, int oct, unsigned seed)
{
	double sum = 0, amp = 0.5, norm = 0;
	int i;

	for (i = 0; i < oct; i++) {
		sum += amp * noise(x, y, seed + (unsigned)i * 101u);
		norm += amp;
		x *= 2.03;
		y *= 2.03;
		amp *= 0.5;
	}
	return sum / norm;
}

/* a little ordered dither hides banding in 8-bit gradients */
static void
put(int x, int y, Rgb c)
{
	static const int bayer[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
	};
	double d = (bayer[y & 3][x & 3] / 16.0 - 0.47) / 255.0;
	unsigned char *p = px + ((size_t)y * (size_t)W + (size_t)x) * 3;

	p[0] = (unsigned char)(clamp01(c.r + d) * 255.0 + 0.5);
	p[1] = (unsigned char)(clamp01(c.g + d) * 255.0 + 0.5);
	p[2] = (unsigned char)(clamp01(c.b + d) * 255.0 + 0.5);
}

static int
save_ppm(const char *dir, const char *name)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	if (!(f = fopen(path, "wb"))) {
		perror(path);
		return -1;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	fwrite(px, 3, (size_t)W * (size_t)H, f);
	return fclose(f);
}

/* ----------------------------------------------------------------- scenes */

/* Clouds: a blue sky deepening towards the top, soft white cumulus */
static void
scene_clouds(void)
{
	static const int sky[] = { 0x2f6fc4, 0x4a8cd8, 0x7fb4ea, 0xb5d6f5 };
	int x, y;

	for (y = 0; y < H; y++) {
		double v = (double)y / H;

		for (x = 0; x < W; x++) {
			double u = (double)x / H;          /* square units */
			double n = fbm(u * 2.6 + 3.0, v * 4.2, 6, 11);
			double fine = fbm(u * 9.0, v * 12.0, 4, 23);
			double cover = smooth(0.50, 0.74, n + 0.10 * (fine - 0.5) +
			    0.06 * (1.0 - v));
			double shade = smooth(0.45, 0.85, fbm(u * 2.6 + 3.05,
			    v * 4.2 + 0.06, 6, 11));
			Rgb c = ramp(sky, 4, v);
			Rgb cloud = mix(rgb(0xffffff), rgb(0xc9d6e8), shade * 0.7);

			put(x, y, mix(c, cloud, cover * 0.95));
		}
	}
}

/* Dusk: evening sky, a low sun and three ranges of hills */
static void
scene_dusk(void)
{
	static const int sky[] = {
		0x1b1840, 0x3a2360, 0x7a3572, 0xc8566a, 0xf08a52, 0xf9c66b
	};
	static const int hill[] = { 0x5a3560, 0x3a2248, 0x1d1230 };
	double sx = W * 0.68, sy = H * 0.60, sr = H * 0.075;
	int x, y, k;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			double v = (double)y / (H * 0.78);
			double dx = x - sx, dy = y - sy, d = sqrt(dx * dx + dy * dy);
			Rgb c = ramp(sky, 6, v);

			/* glow, then the disc */
			c = mix(c, rgb(0xffe9a8), 0.55 * exp(-d / (sr * 3.2)));
			c = mix(c, rgb(0xfff4c8), smooth(sr + 1.5, sr - 1.5, d));
			/* thin streaks of cloud across the sun */
			c = mix(c, rgb(0x8a3e6e), 0.35 * smooth(0.62, 0.80,
			    fbm(x / (H * 0.9), y / (H * 0.07), 4, 5)) *
			    smooth(0.80, 0.30, (double)y / H));
			for (k = 0; k < 3; k++) {
				double base = H * (0.66 + 0.10 * k);
				double ridge = base - H * (0.14 - 0.03 * k) *
				    fbm(x / (H * (0.55 - 0.12 * k)) + 7.0 * k, 0.5, 5,
				    31u + (unsigned)k);

				c = mix(c, rgb(hill[k]), smooth(ridge - 1.0, ridge + 1.0, y));
			}
			put(x, y, c);
		}
	}
}

/* Night Sky: stars of several sizes, a pale band and a crescent moon */
static void
scene_night(void)
{
	static const int sky[] = { 0x02030a, 0x070b1c, 0x0f1a38, 0x1b2c52 };
	double mx = W * 0.80, my = H * 0.22, mr = H * 0.055;
	int cell = H / 40 > 8 ? H / 40 : 8;
	int x, y;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			double v = (double)y / H;
			double band = smooth(0.50, 0.85, fbm((x + y * 0.6) / (H * 0.5),
			    (y - x * 0.35) / (H * 0.16), 5, 77));
			Rgb c = ramp(sky, 4, v);
			int cx = x / cell, cy = y / cell, i, j;
			double dx, dy, d, dark;

			c = mix(c, rgb(0x4a5a8c), 0.28 * band);
			/* every grid cell holds at most one star */
			for (j = -1; j <= 1; j++) {
				for (i = -1; i <= 1; i++) {
					int gx = cx + i, gy = cy + j;
					double sxp, syp, r, lum;

					if (rnd2(gx, gy, 1) > 0.42 + 0.25 * band)
						continue;
					sxp = (gx + rnd2(gx, gy, 2)) * cell;
					syp = (gy + rnd2(gx, gy, 3)) * cell;
					r = 0.55 + 1.5 * pow(rnd2(gx, gy, 4), 6.0);
					lum = 0.35 + 0.65 * rnd2(gx, gy, 5);
					dx = x + 0.5 - sxp;
					dy = y + 0.5 - syp;
					d = sqrt(dx * dx + dy * dy);
					if (d < r * 3.0)
						c = mix(c, rnd2(gx, gy, 6) > 0.8 ? rgb(0xffe2b8) :
						    rgb(0xe8eeff), lum * exp(-d * d / (r * r)));
				}
			}
			dx = x - mx;
			dy = y - my;
			d = sqrt(dx * dx + dy * dy);
			dark = sqrt((dx - mr * 0.45) * (dx - mr * 0.45) +
			    (dy + mr * 0.2) * (dy + mr * 0.2));
			c = mix(c, rgb(0x9fb4e8), 0.18 * exp(-d / (mr * 2.5)));
			c = mix(c, rgb(0xf4f1dc), smooth(mr + 1.2, mr - 1.2, d) *
			    smooth(mr * 0.92 - 1.2, mr * 0.92 + 1.2, dark));
			put(x, y, c);
		}
	}
}

/* Meadow: sky, far hills and rolling grass */
static void
scene_meadow(void)
{
	static const int sky[] = { 0x3d7fd0, 0x6ea6e4, 0xa9cdf2, 0xdcecf8 };
	static const int grass[] = { 0x7fb24a, 0x5f9a3a, 0x3f7f2e };
	int x, y, k;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			double v = (double)y / (H * 0.62), u = (double)x / H;
			double cl = smooth(0.56, 0.78, fbm(u * 2.2, y / (H * 0.2), 5, 9));
			Rgb c = mix(ramp(sky, 4, v), rgb(0xffffff), cl * 0.85 *
			    smooth(0.60, 0.20, (double)y / H));

			for (k = 0; k < 3; k++) {
				double base = H * (0.56 + 0.13 * k);
				double ridge = base - H * (0.10 - 0.02 * k) *
				    fbm(u * (1.3 + 0.5 * k) + 4.0 * k, 0.3, 4,
				    50u + (unsigned)k) + H * 0.03 * sin(u * 2.2 + k * 1.7);
				double t = smooth(ridge - 1.0, ridge + 1.0, y);
				double light = 0.85 + 0.30 * fbm(u * 6.0, y / (H * 0.05), 3,
				    60u + (unsigned)k) - 0.25 * clamp01((y - ridge) /
				    (H * 0.3));
				Rgb g = rgb(grass[k]);

				g.r *= light;
				g.g *= light;
				g.b *= light;
				if (k == 0)                    /* haze on the far range */
					g = mix(g, rgb(0xa9cdf2), 0.35);
				c = mix(c, g, t);
			}
			put(x, y, c);
		}
	}
}

/* Setup: the installer backdrop - blue fading to black, coarsely dithered */
static void
scene_setup(void)
{
	int x, y;

	for (y = 0; y < H; y++) {
		double v = (double)y / (H - 1);
		int level = (int)(v * 63.0 * 16.0);    /* 64 bands, dithered */

		for (x = 0; x < W; x++) {
			static const int bayer[4][4] = {
				{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 },
				{ 15, 7, 13, 5 }
			};
			int band = (level + bayer[(y / 2) & 3][(x / 2) & 3]) / 16;
			unsigned char *p = px + ((size_t)y * (size_t)W + (size_t)x) * 3;

			if (band > 63)
				band = 63;
			p[0] = p[1] = 0;
			p[2] = (unsigned char)(255 - band * 4);
		}
	}
}

/* Waves: teal bands that swell and overlap */
static void
scene_waves(void)
{
	static const int tone[] = {
		0x0a5c66, 0x0f7480, 0x15909a, 0x2aaab0, 0x5cc4c4, 0x9adcd8
	};
	int x, y, k;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			double u = (double)x / H;
			Rgb c = rgb(0xc6ecea);

			for (k = 5; k >= 0; k--) {
				double edge = H * (0.20 + 0.13 * (5 - k)) +
				    H * 0.045 * sin(u * (2.2 + 0.5 * k) + k * 1.9) +
				    H * 0.02 * sin(u * (5.1 + k) + k * 0.7);
				double t = smooth(edge - 1.0, edge + 1.0, y);
				Rgb w = mix(rgb(tone[k]), rgb(0xffffff), 0.10 *
				    smooth(edge + H * 0.02, edge, y));

				c = mix(c, w, t);
			}
			put(x, y, c);
		}
	}
}

/* ------------------------------------------------------------------ tiles */

#define TMAX 64

typedef struct Tile {
	const char *name;
	int w, h;
	int pal[8];
	int npal;
	int (*at)(int x, int y);           /* palette index of a pixel */
} Tile;

static int
t_bricks(int x, int y)
{
	int row = y / 8, off = (row & 1) ? 8 : 0;

	if (y % 8 == 7 || (x + off) % 16 == 15)
		return 0;                          /* mortar */
	if (y % 8 == 0 || (x + off) % 16 == 0)
		return 2;                          /* lit edge */
	return (hash2(x / 2, y / 2, 3) & 7) == 0 ? 3 : 1;
}

static int
t_houndstooth(int x, int y)
{
	int qx, qy;

	x /= 2;                                /* 8x8 pattern, doubled */
	y /= 2;
	qx = (x / 4) & 1;
	qy = (y / 4) & 1;
	if (!qx && !qy)
		return 1;                          /* dark square */
	if (qx && qy)
		return 0;                          /* light square */
	return ((x + y) & 3) < 2;              /* the teeth between them */
}

static int
t_tiles(int x, int y)
{
	int cx = x / 16, cy = y / 16, lx = x % 16, ly = y % 16;
	int base = (cx + cy) & 1;

	if (lx == 0 || ly == 0)
		return 2 + base;                   /* highlight */
	if (lx == 15 || ly == 15)
		return 4;                          /* shadow */
	return base;
}

static int
t_argyle(int x, int y)
{
	int dx = abs(x - 16), dy = abs(y - 16);

	if (abs((x + y) % 32 - 16) == 0 || abs((x - y + 32) % 32 - 16) == 0)
		return 2;                          /* stitching */
	return dx + dy < 16 ? 1 : 0;
}

static int
t_rivets(int x, int y)
{
	int dx = x - 16, dy = y - 16, d = dx * dx + dy * dy;

	if (x == 0 || y == 0)
		return 2;
	if (x == 31 || y == 31)
		return 3;
	if (d <= 9)
		return dx + dy < 0 ? 2 : 3;        /* domed rivet */
	if (d <= 16)
		return 3;
	return (hash2(x, y, 9) & 15) == 0 ? 1 : 0;
}

static int
t_weave(int x, int y)
{
	int cx = (x / 8) & 1, cy = (y / 8) & 1, lx = x % 8, ly = y % 8;

	if (cx == cy)                          /* horizontal strand */
		return ly == 0 ? 2 : ly == 7 ? 3 : (lx & 1);
	return lx == 0 ? 2 : lx == 7 ? 3 : (ly & 1);
}

static const Tile tiles[] = {
	{ "Bricks", 32, 32, { 0xb8b0a0, 0x9c3a2a, 0xb85040, 0x7e2c20 }, 4,
	  t_bricks },
	{ "Houndstooth", 16, 16, { 0xe8e4d8, 0x202020 }, 2, t_houndstooth },
	{ "Tiles", 32, 32, { 0x1f7f7f, 0x2a9a9a, 0x4fb4b4, 0x6cc8c8, 0x0f5050 },
	  5, t_tiles },
	{ "Argyle", 32, 32, { 0x2f4a6a, 0x8a2f3a, 0xe0d8c0 }, 3, t_argyle },
	{ "Rivets", 32, 32, { 0x5a6a80, 0x546378, 0x8a9ab0, 0x34404f }, 4,
	  t_rivets },
	{ "Weave", 16, 16, { 0xc8a868, 0xb89858, 0xe4cc94, 0x8a6c38 }, 4,
	  t_weave },
};

static int
save_xpm(const char *dir, const Tile *t)
{
	static const char keys[] = "abcdefgh";
	char path[1024];
	FILE *f;
	int x, y, i;

	snprintf(path, sizeof path, "%s/%s.xpm", dir, t->name);
	if (!(f = fopen(path, "w"))) {
		perror(path);
		return -1;
	}
	fprintf(f, "/* XPM */\nstatic char *tile[] = {\n\"%d %d %d 1\",\n", t->w,
	    t->h, t->npal);
	for (i = 0; i < t->npal; i++)
		fprintf(f, "\"%c c #%06X\",\n", keys[i], (unsigned)t->pal[i]);
	for (y = 0; y < t->h; y++) {
		fputc('"', f);
		for (x = 0; x < t->w; x++)
			fputc(keys[t->at(x, y) % t->npal], f);
		fprintf(f, "\"%s\n", y == t->h - 1 ? "" : ",");
	}
	fprintf(f, "};\n");
	return fclose(f);
}

/* ------------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*draw)(void);
	} scenes[] = {
		{ "Clouds", scene_clouds }, { "Dusk", scene_dusk },
		{ "Night Sky", scene_night }, { "Meadow", scene_meadow },
		{ "Setup", scene_setup }, { "Waves", scene_waves },
	};
	size_t i;
	int rc = 0;

	if (argc != 2 && argc != 4) {
		fprintf(stderr, "usage: mkwall <output-dir> [width height]\n");
		return 2;
	}
	if (argc == 4) {
		W = atoi(argv[2]);
		H = atoi(argv[3]);
	}
	if (W < 64 || H < 64 || W > 8192 || H > 8192) {
		fprintf(stderr, "mkwall: size must be 64..8192\n");
		return 2;
	}
	if (!(px = malloc((size_t)W * (size_t)H * 3))) {
		fprintf(stderr, "mkwall: out of memory\n");
		return 1;
	}
	for (i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
		scenes[i].draw();
		if (save_ppm(argv[1], scenes[i].name) != 0)
			rc = 1;
	}
	for (i = 0; i < sizeof tiles / sizeof tiles[0]; i++)
		if (save_xpm(argv[1], &tiles[i]) != 0)
			rc = 1;
	free(px);
	return rc;
}
