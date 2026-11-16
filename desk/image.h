/* Minimal PPM (P3/P6) and XPM3 loaders - no external image libraries. */
#ifndef RETRO_IMAGE_H
#define RETRO_IMAGE_H

#include "../lib/retroui.h"

typedef struct Image {
	int w, h;
	unsigned char *rgb;      /* w*h*3 */
	unsigned char *alpha;    /* w*h, 0 = transparent (XPM "None"), or NULL */
} Image;

/* tile: repeat; center: unscaled; stretch: fill ignoring the aspect ratio;
 * fill: scale to cover, cropping the overflow (keeps the aspect ratio) */
enum { IMG_TILE, IMG_CENTER, IMG_STRETCH, IMG_FILL };
int    image_mode(const char *name);                 /* IMG_TILE if unknown */

Image *image_load(RuiApp *app, const char *path);    /* by file contents */
void   image_free(Image *img);
/* shrinks (nearest neighbour) to fit max x max; frees img if it made a copy */
Image *image_fit(Image *img, int max);
/* Renders onto an existing pixmap of size w x h (already filled with the
 * background).  Returns 0 on success. */
int    image_render(RuiApp *app, const Image *img, Pixmap dst, int w, int h,
                    int mode);
/* the same into the rectangle (x, y, w, h) of dst: one monitor */
int    image_render_at(RuiApp *app, const Image *img, Pixmap dst, int x, int y,
                       int w, int h, int mode);
/* Icon-style: draws at (x, y) unscaled, honouring transparency. */
int    image_draw(RuiApp *app, const Image *img, Drawable dst, int x, int y);

#endif
