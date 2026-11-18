/*
 * libretroui - tiny Xlib/Xft widget toolkit shared by the retro-de suite.
 *
 * This header must be the first include of every translation unit: it sets
 * the feature-test macros needed on glibc and leaves the BSDs on their
 * default (full) namespace.
 */
#ifndef RETROUI_H
#define RETROUI_H

#if defined(__linux__) || defined(__GLIBC__)
# ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
# endif
# ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
# endif
# ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
# endif
#endif

#include <stddef.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#define RUI_VERSION "1.3"

/* ------------------------------------------------------------------ util */

void  *rui_malloc(size_t n);
void  *rui_calloc(size_t n, size_t sz);
void  *rui_realloc(void *p, size_t n);
char  *rui_strdup(const char *s);
size_t rui_strlcpy(char *dst, const char *src, size_t n);
void   rui_warn(const char *fmt, ...);
void   rui_die(const char *fmt, ...);
long long rui_now_ms(void);

const char *rui_config_dir(void);              /* ~/.config/retro-de          */
char  *rui_config_path(const char *name);      /* malloc'd                    */
int    rui_have_cmd(const char *name);         /* executable found in $PATH?  */
char  *rui_shell_quote(const char *s);         /* malloc'd 'single quoted'    */
void   rui_spawn(const char *cmd);             /* detached /bin/sh -c cmd     */
const char *rui_terminal(void);                /* best terminal emulator      */
void   rui_strip_ansi(char *s);                /* remove ESC[...m sequences   */
char  *rui_strdupf(const char *fmt, ...);      /* malloc'd printf             */
int    rui_next_line(const char **p, char *buf, size_t n);  /* 0 at the end   */
char  *rui_ltrim(char *s);
char  *rui_rtrim(char *s);
/* the full name from a passwd GECOS field: text before the first comma, with
 * BSD's '&' standing for the login name, capitalised; "" when there is none */
void   rui_gecos_name(const char *gecos, const char *login, char *buf,
                      size_t n);
/* first decimal number in s (may be fractional); pointer past it or NULL */
const char *rui_first_number(const char *s, double *val);

/* key = value config files; '#' comments.  Returns 0 if file was read. */
typedef void (*RuiConfFn)(const char *key, const char *val, void *ud);
int    rui_conf_parse(const char *path, RuiConfFn fn, void *ud);
int    rui_conf_bool(const char *val);
extern const char *const rui_conf_parts[];     /* keys.conf, windows.conf... */
int    rui_glob(const char *pat, const char *s);   /* * and ?, no case   */

/* --------------------------------------------------------------- signals */

void   rui_signals_init(void);                 /* TERM/INT/HUP -> quit flag   */
int    rui_quit_requested(void);
void   rui_request_quit(void);

/* ---------------------------------------------------------- async jobs */

/* Runs `/bin/sh -c cmd` with stdout on a non-blocking pipe.  `fn` is called
 * from rui_wait() once the command has finished. */
typedef void (*RuiJobFn)(const char *out, int status, void *ud);
int    rui_job_start(const char *cmd, RuiJobFn fn, void *ud);
/* A command that keeps running (pactl subscribe, alsactl monitor ...):
 * fn gets each output line as it arrives (status 0), then NULL and the
 * exit status when it ends.  Returns an id for rui_watch_stop, or -1. */
int    rui_watch_start(const char *cmd, RuiJobFn fn, void *ud);
void   rui_watch_stop(int id);
int    rui_jobs_active(void);
void   rui_jobs_shutdown(void);

/* ------------------------------------------------------------ retro bus */

/* A small message bus in place of D-Bus.  retro-bus (started by
 * retro-session) listens on a Unix socket: $RETRO_BUS, else
 * ${TMPDIR:-/tmp}/retro-de-UID/bus-DISPLAY.  Programs claim service
 * names, call methods on each other and broadcast events.  The protocol is
 * one line of text per message (docs/BUS.md), so a shell script with
 * retro-msg(1) is a full peer.  Arguments are fields separated by tabs.
 * Everything here quietly does nothing when no bus is running. */
typedef struct RuiBusMsg {
	const char *kind;                  /* "call", "event", "reply", "error" */
	const char *id;                    /* call, reply, error              */
	const char *from;                  /* call, event: the sender         */
	const char *service;               /* call: the name that was called  */
	const char *name;                  /* call: the method, event: topic  */
	char *args;                        /* unescaped, may be modified      */
} RuiBusMsg;
typedef void (*RuiBusFn)(const RuiBusMsg *m, void *ud);

char *rui_bus_path(void);                      /* malloc'd                    */
int   rui_bus_connect(void);                   /* 0 = connected               */
int   rui_bus_fd(void);                        /* -1 when not connected       */
void  rui_bus_close(void);
/* incoming calls and events go to fn (from rui_wait or rui_bus_dispatch) */
void  rui_bus_handler(RuiBusFn fn, void *ud);
/* claim a service name / follow topics ("notify.*"); both are kept and
 * renewed when the bus comes back after a restart */
int   rui_bus_claim(const char *name);         /* 0 ok, -1 taken / no bus */
void  rui_bus_listen(const char *pattern);
int   rui_bus_emit(const char *topic, const char *args);
int   rui_bus_reply(const char *id, int ok, const char *text);
/* a call that waits (dispatching anything else that arrives meanwhile):
 * malloc'd answer, NULL without a bus or after timeout_ms; *ok is 1 for a
 * reply, 0 for an error */
char *rui_bus_call(const char *service, const char *method, const char *args,
                   int timeout_ms, int *ok);
/* a call whose answer is not wanted */
int   rui_bus_send(const char *service, const char *method, const char *args);
void  rui_bus_dispatch(void);                  /* handle what has arrived    */
/* split args at tabs in place; returns the number of fields (<= max) */
int   rui_bus_fields(char *args, char **v, int max);
/* the wire escaping: \\ \n \t \r (malloc'd) */
char *rui_bus_escape(const char *s);
void  rui_bus_unescape(char *s);
/* a word usable as a name, method, topic or id: [A-Za-z0-9._:@+-]{1,128}
 * (topics given to listen may also hold * and ?) */
int   rui_bus_word_ok(const char *w, int glob);
/* for rui_wait: the socket to watch (or -1), shortening *timeout_ms while
 * a reconnect is pending */
int   rui_bus_wait_fd(int *timeout_ms);

/* ----------------------------------------------------------------- atoms */

#define RUI_ATOMS(X) \
	X(WM_PROTOCOLS) X(WM_DELETE_WINDOW) X(WM_TAKE_FOCUS) X(WM_STATE) \
	X(WM_CHANGE_STATE) X(UTF8_STRING) X(_MOTIF_WM_HINTS) X(MANAGER) \
	X(_NET_SUPPORTED) X(_NET_SUPPORTING_WM_CHECK) X(_NET_WM_NAME) \
	X(_NET_CLIENT_LIST) X(_NET_CLIENT_LIST_STACKING) \
	X(_NET_NUMBER_OF_DESKTOPS) X(_NET_CURRENT_DESKTOP) X(_NET_DESKTOP_NAMES) \
	X(_NET_DESKTOP_GEOMETRY) X(_NET_DESKTOP_VIEWPORT) X(_NET_WORKAREA) \
	X(_NET_ACTIVE_WINDOW) X(_NET_WM_DESKTOP) X(_NET_CLOSE_WINDOW) \
	X(_NET_MOVERESIZE_WINDOW) X(_NET_WM_MOVERESIZE) X(_NET_FRAME_EXTENTS) \
	X(_NET_REQUEST_FRAME_EXTENTS) X(_NET_WM_ALLOWED_ACTIONS) \
	X(_NET_WM_STRUT) X(_NET_WM_STRUT_PARTIAL) X(_NET_WM_PID) X(_NET_WM_ICON) \
	X(_NET_WM_STATE) X(_NET_WM_STATE_MODAL) X(_NET_WM_STATE_STICKY) \
	X(_NET_WM_STATE_MAXIMIZED_VERT) X(_NET_WM_STATE_MAXIMIZED_HORZ) \
	X(_NET_WM_STATE_SHADED) X(_NET_WM_STATE_SKIP_TASKBAR) \
	X(_NET_WM_STATE_SKIP_PAGER) X(_NET_WM_STATE_HIDDEN) \
	X(_NET_WM_STATE_FULLSCREEN) X(_NET_WM_STATE_ABOVE) \
	X(_NET_WM_STATE_BELOW) X(_NET_WM_STATE_DEMANDS_ATTENTION) \
	X(_NET_WM_STATE_FOCUSED) \
	X(_NET_WM_WINDOW_TYPE) X(_NET_WM_WINDOW_TYPE_DESKTOP) \
	X(_NET_WM_WINDOW_TYPE_DOCK) X(_NET_WM_WINDOW_TYPE_TOOLBAR) \
	X(_NET_WM_WINDOW_TYPE_MENU) X(_NET_WM_WINDOW_TYPE_UTILITY) \
	X(_NET_WM_WINDOW_TYPE_SPLASH) X(_NET_WM_WINDOW_TYPE_DIALOG) \
	X(_NET_WM_WINDOW_TYPE_NORMAL) \
	X(_NET_WM_ACTION_MOVE) X(_NET_WM_ACTION_RESIZE) \
	X(_NET_WM_ACTION_MINIMIZE) X(_NET_WM_ACTION_SHADE) \
	X(_NET_WM_ACTION_STICK) X(_NET_WM_ACTION_MAXIMIZE_HORZ) \
	X(_NET_WM_ACTION_MAXIMIZE_VERT) X(_NET_WM_ACTION_FULLSCREEN) \
	X(_NET_WM_ACTION_CHANGE_DESKTOP) X(_NET_WM_ACTION_CLOSE) \
	X(_NET_SYSTEM_TRAY_OPCODE) X(_NET_SYSTEM_TRAY_ORIENTATION) \
	X(_XEMBED) X(_XEMBED_INFO) X(_XROOTPMAP_ID) X(ESETROOT_PMAP_ID) \
	X(_RETRO_WM_COMMAND) X(_RETRO_SELECTION) X(PRIMARY_TEXT) X(_RETRO_LAYOUT) \
	X(_RETRO_RELOAD) X(_RETRO_FP_HOTSPOTS) X(_RETRO_DND) X(_RETRO_AWAKE)

#define RUI_ATOM_ENUM(n) A_##n,
enum { RUI_ATOMS(RUI_ATOM_ENUM) A_COUNT };

/* ----------------------------------------------------------------- theme */

enum {
	RUI_C_FACE, RUI_C_LIGHT, RUI_C_SHADOW, RUI_C_DARK, RUI_C_TEXT,
	RUI_C_DISABLED, RUI_C_FIELD, RUI_C_SEL_BG, RUI_C_SEL_FG,
	RUI_C_TITLE_ACTIVE, RUI_C_TITLE_INACTIVE, RUI_C_TITLE_FG,
	RUI_C_TITLE_FG_INACTIVE, RUI_C_DESKTOP, RUI_C_DESKTOP_FG,
	RUI_C_ALERT, RUI_C_GOOD,
	/* derived from the title colours (Motif frame bevels), not themable */
	RUI_C_ACT_LIGHT, RUI_C_ACT_SHADOW, RUI_C_INACT_LIGHT, RUI_C_INACT_SHADOW,
	RUI_C_COUNT
};
#define RUI_C_THEMED RUI_C_ACT_LIGHT        /* number of theme colours */

/* look and feel: CDE/Motif (front panel, Motif frames, 2-pixel bevels,
 * bevel-highlighted menus, iconified windows on the desktop) or Classic
 * (taskbar, Windows-95-style frames and flat menu highlight) */
enum { RUI_STYLE_CDE, RUI_STYLE_CLASSIC };
enum { RUI_OFF, RUI_ON, RUI_AUTO };

/* ------------------------------------------------------------------ themes */

typedef struct RuiTheme {
	const char *name;
	int style;
	const char *colors[RUI_C_THEMED];
	const char *pattern;               /* default backdrop pattern        */
	const char *font;                  /* NULL: keep the configured font  */
} RuiTheme;

extern const RuiTheme rui_themes[];    /* built in, NULL name terminated  */

/* Theme modules: a theme can be compiled C code.  A module is a shared
 * object NAME.so that exports
 *
 *     const RuiThemeModule retro_theme = { RUI_THEME_ABI, { ... }, ... };
 *
 * and is looked up in $RETRO_DE_THEME_PATH, ~/.config/retro-de/themes and
 * $(LIBDIR)/themes.  Besides the palette it may bring a default wallpaper
 * and its own bevel drawing (it can call every rui_* function).  See
 * themes/README and `retro-settings --install-theme`. */
#define RUI_THEME_ABI 1
struct RuiSurf;
typedef struct RuiThemeModule {
	int abi;                           /* RUI_THEME_ABI                   */
	RuiTheme theme;
	const char *wallpaper;             /* name or path, NULL: none        */
	/* draws a bevel itself and returns 1, or returns 0 for the default */
	int (*bevel)(struct RuiSurf *s, int x, int y, int w, int h, int style);
	const char *author, *description;  /* shown by --list-themes -v       */
} RuiThemeModule;

/* a theme module, a ~/.config/retro-de/themes/NAME.theme file or a
 * built-in theme (NULL if none) */
const RuiTheme *rui_theme_find(const char *name);
/* the loaded module behind a theme name, NULL for file/built-in themes */
const RuiThemeModule *rui_theme_module(const char *name);
/* "module", "file" or "built-in" (NULL: no such theme) */
const char *rui_theme_kind(const char *name);
/* shared data ($RETRO_DE_DATA_DIR or the install DATADIR) */
const char *rui_data_dir(void);
/* wallpaper by path or by name ("Clouds"): user wallpapers/ first, then
 * the installed ones; malloc'd path or NULL */
char  *rui_wallpaper_find(const char *name);
/* NULL-terminated, malloc'd list of the paths of every wallpaper found */
char **rui_wallpaper_list(void);
/* NULL-terminated, malloc'd list of every theme name (free with the list) */
char **rui_theme_list(void);
void   rui_strv_free(char **v);
int    rui_style_parse(const char *s);  /* "cde"/"motif" or "classic"      */

/* --------------------------------------------------------- user settings */

enum { RUI_TOOL_TERMINAL, RUI_TOOL_BROWSER, RUI_TOOL_FILES, RUI_TOOL_EDITOR,
       RUI_TOOL_LOCKER, RUI_TOOL_CLOCK, RUI_TOOL_COUNT };

/* the shared part of retro.conf, read once per process */
typedef struct RuiSettings {
	char theme[64];
	int style;                         /* RUI_STYLE_*                    */
	char font[256], font_bold[256];
	char color[RUI_C_THEMED][64];      /* "" = from the theme           */
	char pattern[32];
	char tool[RUI_TOOL_COUNT][256];    /* "auto" or a command           */
	int dbus, xdg_menus, xdg_autostart, confirm;
	char power[32], power_priv[64];
	char workspace_names[256];
	char programs_dir[512];            /* "" = <config dir>/programs     */
	char screensaver[32];              /* auto xscreensaver blank off    */
	int saver_minutes;                 /* blank / start the saver after  */
	int dpms_minutes;                  /* monitor off after; 0 = never   */
	int bus;                           /* start retro-bus (default on)   */
	int notify;                        /* show notification popups       */
	int notify_seconds;                /* default popup time             */
	char icon_theme[64];               /* "" or e.g. Papirus, Adwaita    */
	char dbus_server[32];              /* auto, retro, dbus-daemon       */
	int dbus_portal;                   /* retro-dbus portals (and GTK's)  */
} RuiSettings;

const RuiSettings *rui_settings(void);
int   rui_xdg_menus(void);              /* compiled in and switched on    */
/* the screen saver in use: "xscreensaver" (installed and wanted), "blank"
 * (the X server's own blanking) or "off" */
const char *rui_saver_kind(void);
/* writes "key = value" into a retro.conf-style file, keeping the rest */
int   rui_conf_set(const char *path, const char *key, const char *value);
/* ask every component to reload retro.conf (they restart themselves) */
void  rui_request_reload(Display *dpy);
extern const char *const rui_tool_keys[RUI_TOOL_COUNT];
/* command for a default tool: the configured one, the XDG default
 * application (xdg-mime) or the first installed fallback; malloc'd */
char *rui_tool_command(int tool);

typedef struct RuiFont {
	XftFont *xft;        /* scalable/bitmap font through Xft, or ...      */
	XFontSet set;        /* ... a core X bitmap font ("core:" / XLFD)     */
	int ascent, descent, height;
} RuiFont;

typedef struct RuiApp {
	Display *dpy;
	int screen;
	Window root;
	Visual *vis;
	Colormap cmap;
	int depth;
	int sw, sh;                       /* screen size                       */
	GC gc;
	Pixmap checker;                   /* 2x2 stipple for dithered fills    */
	RuiFont *font, *bold;
	XftColor color[RUI_C_COUNT];
	Atom atom[A_COUNT];
	XIM xim;
	Cursor cur_arrow;
	int xerrors;                      /* X errors seen (non fatal)         */
	int style;                        /* RUI_STYLE_*                       */
	const RuiTheme *theme;
	const RuiThemeModule *theme_mod;  /* set when the theme is C code      */
	const char *name;
	void (*dispatch)(XEvent *ev, void *ud);   /* used by modal loops       */
	void *dispatch_ud;
} RuiApp;

RuiApp *rui_open(const char *name);          /* connects, loads the theme  */
void    rui_close(RuiApp *app);
/* Flush, then sleep until an X event, a finished job, a signal or timeout
 * (milliseconds, <0 = forever).  Finished jobs are dispatched from here. */
void    rui_wait(RuiApp *app, int timeout_ms);
int     rui_xerror_count(void);
void    rui_run_command(RuiApp *app, const char *cmd);   /* @quit etc.     */
void    rui_wm_command(RuiApp *app, const char *verb);
/* modal OK/Cancel box; 1 when confirmed */
int     rui_confirm(RuiApp *app, const char *title, const char *text,
                    const char *ok_label);

/* Monitors (RandR 1.5, else Xinerama, else the whole screen).  Mirrored
 * outputs count once.  Always returns at least one; exactly one entry has
 * `primary` set. */
typedef struct RuiMonitor {
	int x, y, w, h;
	int primary;
	char name[32];                    /* output name ("HDMI-1"), maybe "" */
} RuiMonitor;
#define RUI_MAX_MONITORS 16
int     rui_monitors(RuiApp *app, RuiMonitor *out, int max);
/* the monitor a setting names: "primary", a number from 1, or an output
 * name; falls back to the primary one */
RuiMonitor rui_monitor_pick(RuiApp *app, const char *spec);
/* asks for RandR change events on the root window */
void    rui_monitors_watch(RuiApp *app);
/* 1 when ev is such a notification (the layout or the primary changed) */
int     rui_monitors_event(RuiApp *app, XEvent *ev);

RuiFont *rui_font_open(RuiApp *app, const char *spec);
void     rui_font_close(RuiApp *app, RuiFont *f);
int      rui_text_width(RuiApp *app, RuiFont *f, const char *s, int len);

/* window / property helpers */
Window rui_window_new(RuiApp *app, Window parent, int x, int y, int w, int h,
                      int override_redirect, long event_mask);
int    rui_get_prop32(RuiApp *app, Window w, Atom prop, unsigned long **out);
void   rui_set_prop32(RuiApp *app, Window w, Atom prop, Atom type,
                      const unsigned long *vals, int n);
char  *rui_get_text_prop(RuiApp *app, Window w, Atom prop);
char  *rui_window_title(RuiApp *app, Window w);
void   rui_set_title(RuiApp *app, Window w, const char *title, const char *cls);
void   rui_set_window_type(RuiApp *app, Window w, int type_atom_index);
void   rui_send_root_msg(RuiApp *app, Window w, Atom type,
                         long d0, long d1, long d2, long d3, long d4);
int    rui_wm_delete_event(RuiApp *app, const XEvent *ev);

/* ------------------------------------------------- double-buffered drawing */

typedef struct RuiRect { int x, y, w, h; } RuiRect;

typedef struct RuiSurf {
	RuiApp *app;
	Window win;
	Pixmap buf;                        /* back buffer; everything draws here */
	XftDraw *xd;
	int w, h;
} RuiSurf;

enum { RUI_RAISED, RUI_SUNKEN, RUI_PRESSED, RUI_RAISED_THIN, RUI_SUNKEN_THIN,
       RUI_GROOVE };
enum { RUI_LEFT, RUI_CENTER, RUI_RIGHT };

RuiSurf *rui_surf_new(RuiApp *app, Window win, int w, int h);
void rui_surf_resize(RuiSurf *s, int w, int h);
void rui_surf_free(RuiSurf *s);
void rui_surf_flush(RuiSurf *s);                    /* back buffer -> window */

void rui_fill(RuiSurf *s, int x, int y, int w, int h, int color);
void rui_fill_pixel(RuiSurf *s, int x, int y, int w, int h, unsigned long px);
void rui_fill_dither(RuiSurf *s, int x, int y, int w, int h, int c1, int c2);
void rui_line(RuiSurf *s, int x1, int y1, int x2, int y2, int color);
void rui_bevel(RuiSurf *s, int x, int y, int w, int h, int style);
/* bevel in explicit colours; thick = 1 or 2 pixels per edge */
void rui_bevel_c(RuiSurf *s, int x, int y, int w, int h, int raised,
                 int light, int shadow, int thick);
void rui_text(RuiSurf *s, RuiFont *f, int x, int y, const char *str, int len,
              int color);
/* the same in any colour, e.g. a preview palette's */
void rui_text_c(RuiSurf *s, RuiFont *f, int x, int y, const char *str, int len,
                const XftColor *c);
/* draws at most maxw pixels, eliding with "..."; returns drawn width */
int  rui_text_fit(RuiSurf *s, RuiFont *f, RuiRect r, const char *str,
                  int color, int align);
void rui_arrow(RuiSurf *s, int cx, int cy, int size, int dir, int color);
int  rui_in_rect(RuiRect r, int x, int y);

/* ----------------------------------------------------------------- widgets */

#define RUI_EV_REDRAW   1
#define RUI_EV_ACTIVATE 2
#define RUI_EV_CHANGED  4
#define RUI_EV_CANCEL   8

typedef struct RuiButton {
	RuiRect r;
	const char *label;
	int pressed, armed, toggled, disabled, bold;
} RuiButton;
void rui_button_draw(RuiSurf *s, const RuiButton *b);
int  rui_button_event(RuiButton *b, const XEvent *ev);

typedef struct RuiScrollbar {
	RuiRect r;
	int total, page, pos;
	int drag, drag_off;
} RuiScrollbar;
void rui_scrollbar_draw(RuiSurf *s, const RuiScrollbar *sb);
int  rui_scrollbar_event(RuiScrollbar *sb, const XEvent *ev);

typedef struct RuiSlider {
	RuiRect r;
	double value;                      /* 0..1 */
	int vertical, drag;
} RuiSlider;
void rui_slider_draw(RuiSurf *s, const RuiSlider *sl);
int  rui_slider_event(RuiSlider *sl, const XEvent *ev);

#define RUI_INPUT_MAX 1024
typedef struct RuiInput {
	RuiRect r;
	char text[RUI_INPUT_MAX];
	int len, cursor, scroll;
	int focused, password;
	XIC xic;
	Window win;
} RuiInput;
void rui_input_init(RuiApp *app, RuiInput *in, Window win);
void rui_input_destroy(RuiInput *in);
void rui_input_set(RuiInput *in, const char *text);
void rui_input_draw(RuiSurf *s, RuiInput *in);
/* handles KeyPress, ButtonPress and SelectionNotify (paste) */
int  rui_input_event(RuiApp *app, RuiInput *in, XEvent *ev);

typedef struct RuiList {
	RuiRect r;
	char **items;                      /* borrowed */
	int n, sel, top;
	RuiScrollbar sb;
	long long last_click;
	int last_idx;
} RuiList;
void rui_list_draw(RuiSurf *s, RuiList *l);
int  rui_list_event(RuiApp *app, RuiList *l, const XEvent *ev);
int  rui_list_key(RuiApp *app, RuiList *l, KeySym ks);

/* ------------------------------------------------------------- popup menus */

typedef struct RuiMenu RuiMenu;
typedef struct RuiMenuItem {
	char *label;
	char *cmd;
	RuiMenu *sub;
	int separator, disabled, checked;
	int id;
} RuiMenuItem;
struct RuiMenu {
	RuiMenuItem *items;
	int n, cap;
};

#define RUI_MENU_UPWARD 1               /* open above the given y */

RuiMenu *rui_menu_new(void);
RuiMenuItem *rui_menu_add(RuiMenu *m, const char *label, const char *cmd, int id);
RuiMenuItem *rui_menu_add_sub(RuiMenu *m, const char *label, RuiMenu *sub);
void     rui_menu_add_sep(RuiMenu *m);
void     rui_menu_free(RuiMenu *m);
RuiMenu *rui_menu_parse(const char *text, const char *origin);
RuiMenu *rui_menu_load(const char *path);     /* file, or embedded default */
int      rui_menu_count(const RuiMenu *m);    /* leaf items, recursive     */
extern const char rui_default_menu[];
/* Modal popup.  Returns the chosen item (owned by the menu) or NULL. */
/* root position and time of the click that dismissed the last popup */
extern int rui_menu_outside_x, rui_menu_outside_y;
extern long long rui_menu_outside_ms;
const RuiMenuItem *rui_menu_popup(RuiApp *app, RuiMenu *m, int x, int y,
                                  int flags);

/* ---------------------------------------------------------- system probes */

int  rui_sys_cpu(double *busy);                    /* 0..1 since last call  */
int  rui_sys_mem(double *used, unsigned long *total_mb);

/* =================================================================== modules
 *
 * Every hardware integration is a small module in modules/.  A module
 * drives a command-line tool or reads a kernel interface; none links a
 * library.  The lists in config.h choose which modules are built into the
 * programs (the rest are never linked) and the order they are probed in.
 */

/* ------------------------------------------------------------------ audio */

typedef struct RuiAudioModule {
	const char *name;                  /* "pulseaudio", "pipewire", ...    */
	const char *program;               /* must be found in $PATH           */
	const char *get_cmd;               /* prints volume and mute state     */
	const char *mute_cmd;              /* toggles mute                     */
	void (*set_cmd)(int percent, char *buf, size_t n);
	int  (*parse)(const char *out, int *vol, int *muted);
	/* optional: prints a line whenever the volume may have changed, so
	 * the panel need not ask every few seconds */
	const char *watch_cmd;
} RuiAudioModule;

extern const RuiAudioModule rui_audio_sndio, rui_audio_pipewire,
    rui_audio_pulseaudio, rui_audio_alsa, rui_audio_oss, rui_audio_mixerctl;

typedef struct RuiAudio {
	const RuiAudioModule *mod;         /* NULL until one answered          */
	int probing;                       /* index of the candidate being run */
	int busy;
	int valid, volume, muted;          /* volume in percent                */
	long long last_set;
	char forced[32];
	void (*changed)(void *ud);
	void *ud;
} RuiAudio;
void rui_audio_init(RuiAudio *a, const char *forced_module,
                    void (*changed)(void *), void *ud);
void rui_audio_refresh(RuiAudio *a);
void rui_audio_set(RuiAudio *a, int percent);
void rui_audio_toggle_mute(RuiAudio *a);
const char *rui_audio_backend_name(const RuiAudio *a);

/* ---------------------------------------------------------------- network */

typedef struct RuiNetwork {
	char ssid[64];
	int signal;                        /* percent, -1 unknown */
	int secure, connected;
} RuiNetwork;

typedef struct RuiNetStatus {
	char iface[32];
	char ssid[64];
	int wireless, connected;
} RuiNetStatus;

typedef struct RuiNetModule {
	const char *name;                  /* "iwd", "wpa", "ifconfig"         */
	const char *program;
	int  (*usable)(void);              /* extra platform check, or NULL    */
	int  pass_optional;                /* can rejoin known networks        */
	char *(*iface_cmd)(void);          /* NULL: needs no device name       */
	int   (*parse_iface)(const char *out, char *iface, size_t n);
	char *(*status_cmd)(const char *iface);
	int   (*parse_status)(const char *out, RuiNetStatus *st);
	char *(*scan_cmd)(const char *iface);
	int   (*parse_scan)(const char *out, RuiNetwork *list, int max);
	char *(*connect_cmd)(const char *iface, const char *ssid,
	                     const char *pass, const char *priv);
	char *(*disconnect_cmd)(const char *iface, const char *priv);
} RuiNetModule;

extern const RuiNetModule rui_net_iwd, rui_net_wpa, rui_net_ifconfig;

/* NULL-safe front ends; `forced` is a module or program name or "auto" */
const RuiNetModule *rui_net_detect(const char *forced);
const char *rui_net_backend_name(const RuiNetModule *m);
char *rui_net_iface_cmd(const RuiNetModule *m);
int   rui_net_parse_iface(const RuiNetModule *m, const char *out, char *iface,
                          size_t n);
char *rui_net_status_cmd(const RuiNetModule *m, const char *iface);
int   rui_net_parse_status(const RuiNetModule *m, const char *out,
                           RuiNetStatus *st);
char *rui_net_scan_cmd(const RuiNetModule *m, const char *iface);
int   rui_net_parse_scan(const RuiNetModule *m, const char *out,
                         RuiNetwork *list, int max);
char *rui_net_connect_cmd(const RuiNetModule *m, const char *iface,
                          const char *ssid, const char *pass, const char *priv);
char *rui_net_disconnect_cmd(const RuiNetModule *m, const char *iface,
                             const char *priv);
/* for modules: add or merge a network into a scan list */
void  rui_net_add(RuiNetwork *list, int *n, int max, const char *ssid,
                  int signal, int secure, int connected);
/* any non-loopback interface that is up with an address (getifaddrs) */
int   rui_net_link_up(char *ifname, size_t n);

/* ---------------------------------------------------------------- battery */

typedef struct RuiBatteryModule {
	const char *name;
	/* direct probe: 1 = battery found (values set), 0 = none here */
	int (*read)(int *percent, int *charging);
	/* ... or a command whose output `parse` understands */
	const char *program, *cmd;
	int (*parse)(const char *out, int *percent, int *charging);
} RuiBatteryModule;

extern const RuiBatteryModule rui_battery_sysfs, rui_battery_acpi,
    rui_battery_apm;

/* -------------------------------------------------------------- bluetooth */

typedef struct RuiBtStatus {
	int present;                       /* a controller exists             */
	int powered;
	int nconnected;
	char device[64];                   /* first connected device's name   */
} RuiBtStatus;

typedef struct RuiBtDevice {
	char addr[24];
	char name[64];
	int paired, trusted, connected;
} RuiBtDevice;

typedef struct RuiBtModule {
	const char *name;                  /* "bluez"                         */
	const char *program;               /* must be found in $PATH          */
	const char *status_cmd;            /* controller + connected devices  */
	int   (*parse_status)(const char *out, RuiBtStatus *st);
	const char *power_cmd[2];          /* off, on                         */
	const char *list_cmd;              /* every known device, with state  */
	const char *scan_cmd;              /* discover for a while, then list */
	int   (*parse_devices)(const char *out, RuiBtDevice *list, int max);
	char *(*connect_cmd)(const char *addr, int paired);  /* pairs first   */
	char *(*disconnect_cmd)(const char *addr);
	char *(*forget_cmd)(const char *addr);
} RuiBtModule;

extern const RuiBtModule rui_bt_bluez;

/* `forced` is a module or program name, "auto" or "none" */
const RuiBtModule *rui_bt_detect(const char *forced);
int   rui_bt_parse_status(const RuiBtModule *m, const char *out,
                          RuiBtStatus *st);
/* sorted: connected first, then paired, then by name */
int   rui_bt_parse_devices(const RuiBtModule *m, const char *out,
                           RuiBtDevice *list, int max);

/* ------------------------------------------------------- programs folder */

/* The user's own launcher: a folder whose sub-folders are the categories
 * and whose files are the programs, arranged with any file manager.  An
 * entry is a .desktop file, an executable (or a link to either), or a text
 * file whose first line is the command ("@clock", "xterm -e top").  A
 * leading "NN-" in a name only sorts.  `programs "Label"` in menu.conf
 * shows it. */
char *rui_programs_dir(void);                   /* malloc'd                */
int   rui_programs_init(void);                  /* 1 = created defaults    */
RuiMenu *rui_programs_menu(void);
long long rui_programs_stamp(void);

/* One entry of a programs (sub-)folder, as the Programs window shows it.
 * The picture comes, first match wins, from a "# icon: SPEC" line in a
 * launcher or script, an Icon= of a .desktop file that is an image path,
 * an image beside the entry with the same name ("Chess.xpm" for "Chess",
 * ".icon.xpm" inside a category), or a pictogram guessed from the command.
 * SPEC is a pictogram name (see rui_icon_kind), an image path (absolute,
 * ~/..., or relative to the entry's folder) or an image NAME looked up as
 * <config dir>/icons/NAME.xpm|.ppm and <data dir>/icons/NAME.xpm|.ppm.
 * No icon themes, XDG directories or D-Bus are involved. */
typedef struct RuiProgram {
	char *label;
	char *path;                        /* the file or folder              */
	char *cmd;                         /* NULL for a category             */
	char *image;                       /* XPM/PPM file to show, or NULL   */
	int icon;                          /* RUI_ICON_* when there is none   */
	int is_dir;
} RuiProgram;
/* the entries of dir, categories first, sorted like the menu; returns the
 * count (free with rui_programs_free) */
int   rui_programs_read(const char *dir, RuiProgram **out);
void  rui_programs_free(RuiProgram *v, int n);

/* -------------------------------------------------------------- pictograms */

enum { RUI_ICON_APP, RUI_ICON_TERMINAL, RUI_ICON_FOLDER, RUI_ICON_FILE,
       RUI_ICON_COMPUTER, RUI_ICON_NETWORK, RUI_ICON_CALC, RUI_ICON_CLOCK,
       RUI_ICON_BROWSER, RUI_ICON_SETTINGS, RUI_ICON_INFO, RUI_ICON_PROGRAMS,
       RUI_ICON_BT, RUI_ICON_DISPLAY, RUI_ICON_EDITOR, RUI_ICON_GAME,
       RUI_ICON_COUNT };
#define RUI_ICON_SIZE 32
/* "terminal", "folder", ... -> RUI_ICON_*, -1 if not a pictogram name */
int   rui_icon_kind(const char *name);
const char *rui_icon_name(int kind);
/* the pictogram that suits a command ("retro-calc", "@browser", "xterm") */
int   rui_icon_guess(const char *cmd);
/* draws a 32x32 pictogram with its top left corner at (x, y) */
void  rui_icon_draw(RuiSurf *s, int kind, int x, int y);

/* Icons without XDG: a window's own _NET_WM_ICON, scaled to size x size
 * RGBA (malloc'd, NULL if it has none); learned copies of those as
 * <config dir>/icons/learned/CLASS.xpm; and a lookup by name in
 * <config dir>/icons, <data dir>/icons and the learned ones. */
unsigned char *rui_netwm_icon(RuiApp *app, Window w, int size);
int   rui_icon_save_xpm(const char *path, int w, int h,
                        const unsigned char *rgba);
char *rui_icon_learned_path(const char *name);  /* malloc'd, NULL if bad */
int   rui_icon_learn(RuiApp *app, Window w);     /* files written         */
char *rui_icon_find(const char *name);           /* image path or NULL    */
char *rui_icon_for_cmd(const char *cmd);         /* by the program's name */

/* lib/png.c and lib/svg.c: images without libpng, zlib or librsvg */
unsigned char *rui_inflate(const unsigned char *in, size_t inlen,
                           size_t *outlen, size_t *used);
unsigned char *rui_zlib_inflate(const unsigned char *in, size_t inlen,
                                size_t *outlen);
unsigned char *rui_gunzip(const unsigned char *in, size_t inlen,
                          size_t *outlen);
unsigned char *rui_gzip_store(const unsigned char *in, size_t inlen,
                              size_t *outlen);
unsigned long  rui_crc32(unsigned long crc, const unsigned char *p, size_t n);
/* PNG -> RGBA (malloc'd) */
unsigned char *rui_png_decode(const unsigned char *buf, size_t len,
                              int *w, int *h);
/* SVG text (NUL-terminated) -> RGBA; *w, *h: wanted size, 0 = its own */
unsigned char *rui_svg_render(const char *src, size_t len, int *w, int *h);

/* -------------------------------------------- freedesktop .desktop entries */

typedef struct RuiDesktopEntry {
	char *id;                          /* "firefox.desktop"                */
	char *name, *generic, *comment;    /* localized                        */
	char *exec;                        /* shell command, field codes gone  */
	char *icon, *categories, *path;
	int terminal;
	int nodisplay;                     /* launchable, but not in menus     */
	int hidden;                        /* deleted / not for this desktop   */
	int autostart_off;                 /* X-GNOME-Autostart-enabled=false  */
} RuiDesktopEntry;

typedef struct RuiAppList {
	RuiDesktopEntry **v;
	int n;
} RuiAppList;

/* NULL unless `text` is a valid Type=Application entry (Hidden ones are
 * returned with hidden=1 so they can mask entries of the same id). */
RuiDesktopEntry *rui_desktop_parse(const char *text, const char *id,
                                   const char *file);
RuiDesktopEntry *rui_desktop_load(const char *path, const char *id);
void  rui_desktop_free(RuiDesktopEntry *e);
char *rui_desktop_command(const RuiDesktopEntry *e);  /* malloc'd sh command */
/* $XDG_DATA_HOME and $XDG_DATA_DIRS applications/, first id wins, sorted */
RuiAppList *rui_apps_scan(void);
void  rui_apps_free(RuiAppList *l);
RuiDesktopEntry *rui_app_find(const char *id);  /* "foo" or "foo.desktop" */
RuiMenu *rui_apps_menu(const RuiAppList *l);    /* by category          */
long long rui_apps_stamp(void);                 /* changes with the dirs */
long long rui_menu_stamp(const char *menu_path);
char *rui_icon_lookup(const char *icon);        /* XPM/PPM path or NULL  */
int   rui_autostart_xdg(void);                  /* ~/.config/autostart   */

/* cheap change detectors for the panel (0: not available here) */
unsigned long rui_net_fingerprint(void);
unsigned long rui_bt_fingerprint(void);

/* ---------------------------------------------- compile-time config.h */

typedef struct RuiBlock {
	const char *name;                  /* used in the applets list         */
	int interval;                      /* seconds                          */
	const char *cmd;                   /* first output line is shown       */
	const char *click;                 /* run on left click, or NULL       */
} RuiBlock;

typedef struct RuiLayoutDef {
	const char *name;                  /* float | tile | monocle           */
	const char *symbol;                /* shown in the panel               */
} RuiLayoutDef;

/* ------------------------------------------------------------------ power */

enum { RUI_POWEROFF, RUI_REBOOT, RUI_SUSPEND, RUI_HIBERNATE, RUI_POWER_ACTIONS };

/* how this system shuts down: independent of the init system in use */
typedef struct RuiPowerModule {
	const char *name;                  /* "systemd", "elogind", ...       */
	const char *program;               /* must be found in $PATH          */
	int (*usable)(void);               /* e.g. is systemd PID 1?          */
	const char *cmd[RUI_POWER_ACTIONS];/* NULL: action not available      */
	int needs_priv;                    /* prefix power_priv (doas, sudo)  */
} RuiPowerModule;

extern const RuiPowerModule rui_power_systemd, rui_power_elogind,
    rui_power_consolekit, rui_power_openbsd, rui_power_freebsd,
    rui_power_netbsd, rui_power_generic;

const RuiPowerModule *rui_power_detect(const char *forced);
/* malloc'd command for an action, NULL if unsupported */
char *rui_power_command(const RuiPowerModule *m, int action, const char *priv);
extern const char *const rui_power_action_names[RUI_POWER_ACTIONS];

/* ---------------------------------------------- compile-time config.h */

typedef struct RuiConfig {
	const char *theme, *style, *font;
	const RuiAudioModule *const *audio;        /* NULL-terminated          */
	const RuiNetModule *const *net;
	const RuiBatteryModule *const *battery;
	const RuiPowerModule *const *power;
	const char *applets;
	const RuiBlock *blocks;            /* terminated by a NULL name        */
	unsigned workspaces;
	const char *workspace_names;       /* comma separated                 */
	const RuiLayoutDef *layouts;
	int nlayouts;
	const char *const *keys;           /* NULL-terminated bind lines       */
	const char *const *terminals;
	const char *tools[RUI_TOOL_COUNT]; /* default tools ("auto")          */
	const char *dbus;                  /* "auto", "on", "off"             */
	int desktop_entries, xdg_autostart;
	const char *desktop_name;
	const char *fp_left, *fp_right;    /* CDE front panel controls        */
	const RuiBtModule *const *bt;      /* NULL-terminated                 */
} RuiConfig;

extern const RuiConfig rui_config;     /* lib/config.c, from config.h      */

#endif
