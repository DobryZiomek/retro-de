/* retro-display internals: the monitor layout model, RandR access and the
 * monitors.conf file.  Everything that does not touch the X server is
 * plain data in, data out, so the unit tests can drive it. */
#ifndef RETRO_RANDR_H
#define RETRO_RANDR_H

#include "../lib/retroui.h"

#define DSP_MAX_OUT 16
#define DSP_MAX_MODES 160

typedef struct DspMode {
	unsigned long id;                  /* RandR mode XID                  */
	int w, h;
	double rate;                       /* Hz                              */
	int interlaced;
} DspMode;

typedef struct DspOutput {
	char name[32];
	unsigned long xid;                 /* RandR output XID                */
	int enabled, primary;
	int x, y;
	int rot;                           /* 0, 90, 180 or 270 degrees       */
	int mode;                          /* index into modes, -1: none      */
	int preferred;                     /* index of the native mode        */
	DspMode modes[DSP_MAX_MODES];      /* largest first, fastest first    */
	int nmodes;
} DspOutput;

/* the connected outputs; each has its own mode and so its own refresh rate */
typedef struct DspLayout {
	DspOutput out[DSP_MAX_OUT];
	int n;
} DspLayout;

/* ---- model (no X) ---- */
void  dsp_size(const DspOutput *o, int *w, int *h);     /* after rotation */
/* closest mode: exact size, then nearest rate; -1 if the size is unknown */
int   dsp_find_mode(const DspOutput *o, int w, int h, double rate);
/* enabled outputs get a mode, the layout starts at 0,0, exactly one
 * enabled output is primary, and at least one output stays on */
void  dsp_normalize(DspLayout *l);
/* moves output idx so it touches the nearest edge of another enabled
 * output without overlapping any (dropping it after a drag) */
void  dsp_snap(DspLayout *l, int idx);
void  dsp_bounds(const DspLayout *l, int *w, int *h);
int   dsp_equal(const DspLayout *a, const DspLayout *b);
/* monitors.conf:  output NAME mode WxH rate HZ pos XxY rotate DEG [primary]
 *                 output NAME off                                         */
char *dsp_format(const DspLayout *l);                   /* malloc'd       */
/* applies saved settings to the connected outputs; returns how many
 * outputs the text named */
int   dsp_parse(DspLayout *l, const char *text);
int   dsp_save(const DspLayout *l, const char *path);
int   dsp_load(DspLayout *l, const char *path);         /* -1: no file    */

/* ---- X server ---- */
int   dsp_query(Display *dpy, DspLayout *l);            /* 0 = ok         */
/* sets every output's mode, position and rotation; 0 = ok */
int   dsp_apply(Display *dpy, const DspLayout *l, char *err, size_t n);

#endif
