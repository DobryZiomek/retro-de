/* retro-bar internals shared by the taskbar and the CDE front panel */
#ifndef RETRO_BAR_H
#define RETRO_BAR_H

#include "../lib/retroui.h"
#include "../desk/image.h"

#define MAX_ICONS 32
#define CPU_HIST 36
#define ALL_DESKS 0xffffffffUL
#define MAX_APPLETS 24
#define MAX_BLOCKS 16

enum { AP_CPU, AP_MEM, AP_BAT, AP_NET, AP_VOL, AP_CLOCK, AP_BT, AP_DND,
       AP_AWAKE, AP_NOTES, AP_BLOCK };
extern const char *const applet_names[];
#define NBUILTIN ((int)(sizeof applet_names / sizeof applet_names[0]))

typedef struct Task {
	Window win;
	char *title;
	unsigned long desk;
	int hidden, skip, urgent;
	RuiRect r;
	int shown;
	Image *icon;                       /* TASK_ICON square, or NULL      */
} Task;

#define TASK_ICON 16

typedef struct Applet {
	int kind, block;
	RuiRect r;
} Applet;

typedef struct Block {
	char name[32];
	char *cmd, *click;
	int interval, busy, used;
	long long last;
	char text[128];
} Block;

typedef struct Bar {
	RuiApp *app;
	Display *dpy;
	Window win;
	RuiSurf *s;
	int x, y, w, h, top;
	int dirty, running;
	RuiMenu *menu;
	char *menu_path;
	long long menu_stamp;
	int start_down;
	char start_label[64];

	Task *tasks;
	int ntasks;
	Window active;
	unsigned long cur, ndesk;
	char layout_sym[32];

	RuiRect r_start, r_pager, r_layout, r_tasks, r_tray;
	char applets_spec[256];
	Applet ap[MAX_APPLETS];
	int nap;
	Block blocks[MAX_BLOCKS];
	int nblocks;

	double cpu_hist[CPU_HIST], mem;
	int have_cpu, have_mem;
	unsigned long mem_total;
	int bat_pct, bat_chg, have_bat, bat_busy;
	const RuiBatteryModule *bat_mod;
	const RuiNetModule *net_mod;
	int net_busy, net_up;
	char net_iface[32], net_text[80], net_forced[32];
	RuiAudio audio;
	char audio_forced[32];
	char clock_fmt[64], clock[96];
	const RuiBtModule *bt_mod;
	RuiBtStatus bt;
	int bt_busy;
	char bt_forced[32];
	char monitor[32];                  /* panel_monitor: primary, N, name */
	int dnd, awake;                    /* root properties set by retro-wm */

	Atom tray_atom;
	int tray_ok;
	Window icons[MAX_ICONS];
	int nicons;

	Window cal_win;
	RuiSurf *cal_s;
	int cal_year, cal_mon;
	Window vol_win;
	RuiSurf *vol_s;
	RuiSlider vol_slider;
	RuiButton vol_mute;
	int cal_anchor;                    /* root x for the calendar, or -1 */

	int fp;                            /* CDE front panel instead of bar */
	char desk_names[32][32];
} Bar;

extern Bar bar;

#define A(n) (bar.app->atom[A_##n])

/* retro-bar.c */
void ap_text(const Applet *a, char *buf, size_t n);
void draw_applet(RuiSurf *s, const Applet *ap);
void applet_click(Applet *ap, XButtonEvent *e);
Applet *applet_of(int kind);
int  has_applet(int kind);
void cal_toggle(void);
void cal_close(void);
void vol_toggle(const Applet *ap);
void vol_close(void);
void menu_refresh(void);
void tray_layout(void);
void switch_desk_rel(int d);

/* notify.c: notification popups and the launcher on retro-bus */
void notify_init(void);
int  notify_event(XEvent *ev);         /* 1 when it was for a popup */
void notify_tick(long long now);
void notify_shutdown(void);
int  notify_unread(void);
void notify_center_toggle(int anchor_x);

/* frontpanel.c */
void fp_setup(void);
int  fp_height(void);
int  fp_width(void);
void fp_draw(void);
void fp_click(XButtonEvent *e);
int  fp_busy(void);

#endif
