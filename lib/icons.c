/*
 * libretroui pictograms: 32x32 icons drawn from rectangles in the theme's
 * colours, so they need no image files, icon themes or XDG lookups.  Used
 * by the desktop, the Programs window and the About box.
 */
#include "retroui.h"

#include <stdlib.h>
#include <string.h>

static const char *const names[RUI_ICON_COUNT] = {
	"app", "terminal", "folder", "file", "computer", "network",
	"calculator", "clock", "browser", "settings", "info", "programs",
	"bluetooth", "display", "editor", "game",
};

int
rui_icon_kind(const char *name)
{
	int i;

	if (!name)
		return -1;
	for (i = 0; i < RUI_ICON_COUNT; i++)
		if (!strcmp(name, names[i]))
			return i;
	if (!strcmp(name, "calc"))
		return RUI_ICON_CALC;
	if (!strcmp(name, "about"))
		return RUI_ICON_INFO;
	return -1;
}

const char *
rui_icon_name(int kind)
{
	return kind >= 0 && kind < RUI_ICON_COUNT ? names[kind] : "app";
}

/* the first word of a command without its directory */
static void
first_word(const char *cmd, char *out, size_t n)
{
	const char *s, *e, *slash;
	size_t len;

	s = cmd + strspn(cmd, " \t'\"");
	e = s + strcspn(s, " \t'\"");
	for (slash = s; slash < e; slash++)
		if (*slash == '/')
			s = slash + 1;
	len = (size_t)(e - s) < n - 1 ? (size_t)(e - s) : n - 1;
	memcpy(out, s, len);
	out[len] = '\0';
}

static int
word_in(const char *w, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(w, *list))
			return 1;
	return 0;
}

int
rui_icon_guess(const char *cmd)
{
	static const char *const terms[] = { "@terminal", "xterm", "uxterm",
		"urxvt", "rxvt", "st", "alacritty", "kitty", "foot", "ghostty",
		"wezterm", "konsole", "aterm", "eterm", NULL };
	static const char *const files[] = { "retro-files", "@filemanager",
		"pcmanfm", "thunar", "caja", "nemo", "nautilus", "dolphin",
		"xfe", "rox", "rox-filer", "mc", "ranger", NULL };
	static const char *const editors[] = { "@editor", "vi", "vim", "nvim",
		"nano", "emacs", "mg", "ed", "gvim", "mousepad", "leafpad",
		"geany", "kate", "gedit", "xedit", "nedit", "@open", NULL };
	static const char *const calcs[] = { "retro-calc", "xcalc", "bc",
		"dc", "galculator", "qalculate-gtk", NULL };
	static const char *const clocks[] = { "@clock", "xclock", "oclock",
		"dclock", NULL };
	static const char *const browsers[] = { "@browser", "firefox",
		"chromium", "chrome", "google-chrome", "librewolf", "qutebrowser",
		"surf", "links", "lynx", "w3m", "netsurf", "dillo", "epiphany",
		"midori", "falkon", "brave", "seamonkey", "palemoon", NULL };
	static const char *const settings[] = { "retro-settings", "lxappearance",
		"xfce4-settings-manager", NULL };
	static const char *const displays[] = { "retro-display", "@screensaver",
		"xscreensaver-settings", "xscreensaver-demo", "arandr", "xrandr",
		NULL };
	static const char *const monitors[] = { "top", "htop", "btop", "xload",
		"xosview", "gkrellm", NULL };
	char w[128];
	const char *e;

	if (!cmd || !*cmd)
		return RUI_ICON_APP;
	first_word(cmd, w, sizeof w);
	/* "@terminal -e top": what runs inside matters more */
	if (!strcmp(w, "@terminal") && (e = strstr(cmd, " -e ")) != NULL) {
		char inner[128];

		first_word(e + 4, inner, sizeof inner);
		if (word_in(inner, monitors))
			return RUI_ICON_COMPUTER;
		if (word_in(inner, editors))
			return RUI_ICON_EDITOR;
		if (word_in(inner, files))
			return RUI_ICON_FOLDER;
		return RUI_ICON_TERMINAL;
	}
	if (word_in(w, terms))
		return RUI_ICON_TERMINAL;
	if (word_in(w, files))
		return RUI_ICON_FOLDER;
	if (word_in(w, editors))
		return RUI_ICON_EDITOR;
	if (word_in(w, calcs))
		return RUI_ICON_CALC;
	if (word_in(w, clocks))
		return RUI_ICON_CLOCK;
	if (word_in(w, browsers))
		return RUI_ICON_BROWSER;
	if (word_in(w, settings))
		return RUI_ICON_SETTINGS;
	if (word_in(w, displays))
		return RUI_ICON_DISPLAY;
	if (word_in(w, monitors))
		return RUI_ICON_COMPUTER;
	if (!strcmp(w, "retro-net"))
		return RUI_ICON_NETWORK;
	if (!strcmp(w, "retro-bt") || !strcmp(w, "blueman-manager"))
		return RUI_ICON_BT;
	if (!strcmp(w, "retro-about"))
		return RUI_ICON_INFO;
	if (!strcmp(w, "retro-programs"))
		return RUI_ICON_PROGRAMS;
	if (strstr(w, "doom") || strstr(w, "hack") || strstr(w, "rogue") ||
	    strstr(w, "chess") || strstr(w, "xboard") || strstr(w, "mines") ||
	    strstr(w, "tetris") || strstr(w, "solitaire") || strstr(w, "quake"))
		return RUI_ICON_GAME;
	return RUI_ICON_APP;
}

/* --------------------------------------------------------------- drawing */

enum { P_YELLOW, P_YELLOW_DK, P_BLUE, P_GREEN, P_PAPER, P_RED, P_WHITE,
       P_SKY, P_COUNT };

static unsigned long pal[P_COUNT];
static RuiApp *pal_app;

static void
palette(RuiApp *a)
{
	static const char *const pal_names[P_COUNT] = {
		"#e8c850", "#a08020", "#2040a0", "#30d030", "#f8f8f0", "#c03030",
		"#ffffff", "#5080d0"
	};
	int i;

	if (pal_app == a)
		return;
	for (i = 0; i < P_COUNT; i++) {
		XColor xc, exact;

		pal[i] = a->color[RUI_C_FACE].pixel;
		if (XAllocNamedColor(a->dpy, a->cmap, pal_names[i], &xc, &exact))
			pal[i] = xc.pixel;
	}
	pal_app = a;
}

#define PX(x_, y_, w_, h_, c_) rui_fill_pixel(s, x + (x_), y + (y_), w_, h_, \
    pal[c_])
#define FC(x_, y_, w_, h_, c_) rui_fill(s, x + (x_), y + (y_), w_, h_, c_)

static void
small_window(RuiSurf *s, int x, int y, int c)
{
	FC(0, 0, 12, 10, RUI_C_FACE);
	rui_bevel(s, x, y, 12, 10, RUI_RAISED_THIN);
	FC(1, 1, 10, 2, RUI_C_TITLE_ACTIVE);
	PX(2, 4, 8, 5, c);
}

void
rui_icon_draw(RuiSurf *s, int kind, int x, int y)
{
	int i;

	palette(s->app);
	switch (kind) {
	case RUI_ICON_TERMINAL:
		FC(0, 3, 32, 26, RUI_C_FACE);
		rui_bevel(s, x, y + 3, 32, 26, RUI_RAISED);
		FC(3, 6, 26, 20, RUI_C_DARK);
		PX(6, 10, 2, 2, P_GREEN);
		PX(8, 12, 2, 2, P_GREEN);
		PX(6, 14, 2, 2, P_GREEN);
		PX(12, 15, 7, 2, P_GREEN);
		break;
	case RUI_ICON_FOLDER:
		PX(2, 6, 12, 4, P_YELLOW_DK);
		PX(1, 9, 30, 18, P_YELLOW);
		FC(1, 9, 30, 1, RUI_C_LIGHT);
		FC(1, 9, 1, 18, RUI_C_LIGHT);
		FC(1, 27, 30, 1, RUI_C_DARK);
		FC(31, 9, 1, 19, RUI_C_DARK);
		PX(2, 26, 29, 1, P_YELLOW_DK);
		break;
	case RUI_ICON_FILE:
	case RUI_ICON_EDITOR:
		PX(6, 1, 20, 30, P_PAPER);
		FC(6, 1, 20, 1, RUI_C_DARK);
		FC(6, 1, 1, 30, RUI_C_DARK);
		FC(6, 30, 21, 1, RUI_C_DARK);
		FC(26, 1, 1, 30, RUI_C_DARK);
		for (i = 0; i < 6; i++)
			FC(9, 6 + i * 4, 14, 1, RUI_C_SHADOW);
		if (kind == RUI_ICON_EDITOR) {         /* and a pencil across it */
			for (i = 0; i < 10; i++)
				PX(14 + i, 25 - i * 2, 3, 3, P_YELLOW);
			PX(24, 5, 3, 3, P_RED);            /* eraser */
			FC(13, 27, 2, 2, RUI_C_DARK);      /* lead */
		}
		break;
	case RUI_ICON_COMPUTER:
	case RUI_ICON_DISPLAY:
		FC(2, 2, 28, 20, RUI_C_FACE);
		rui_bevel(s, x + 2, y + 2, 28, 20, RUI_RAISED);
		PX(5, 5, 22, 14, P_BLUE);
		if (kind == RUI_ICON_DISPLAY) {        /* a desktop on the screen */
			PX(5, 5, 22, 14, P_SKY);
			PX(7, 7, 3, 3, P_YELLOW);
			PX(7, 12, 3, 3, P_YELLOW);
			PX(5, 17, 22, 2, P_PAPER);
			FC(12, 22, 8, 3, RUI_C_SHADOW);
			FC(8, 25, 16, 3, RUI_C_FACE);
			rui_bevel(s, x + 8, y + 25, 16, 3, RUI_RAISED_THIN);
		} else {
			FC(12, 22, 8, 3, RUI_C_SHADOW);
			FC(5, 25, 22, 5, RUI_C_FACE);
			rui_bevel(s, x + 5, y + 25, 22, 5, RUI_RAISED_THIN);
		}
		break;
	case RUI_ICON_NETWORK:
		for (i = 0; i < 2; i++) {
			int ox = i * 16, oy = 2 + i * 12;

			FC(ox + 1, oy, 14, 11, RUI_C_FACE);
			rui_bevel(s, x + ox + 1, y + oy, 14, 11, RUI_RAISED);
			PX(ox + 3, oy + 2, 10, 7, P_BLUE);
			FC(ox + 4, oy + 12, 8, 2, RUI_C_SHADOW);
		}
		FC(8, 16, 2, 8, RUI_C_DARK);
		FC(8, 23, 9, 2, RUI_C_DARK);
		break;
	case RUI_ICON_CALC:
		FC(5, 1, 22, 30, RUI_C_FACE);
		rui_bevel(s, x + 5, y + 1, 22, 30, RUI_RAISED);
		PX(8, 4, 16, 6, P_GREEN);
		rui_bevel(s, x + 8, y + 4, 16, 6, RUI_SUNKEN_THIN);
		for (i = 0; i < 12; i++) {
			int bx = 8 + (i % 3) * 6, by = 13 + (i / 3) * 4;

			if (i == 11)
				PX(bx, by, 4, 3, P_RED);
			else
				FC(bx, by, 4, 3, RUI_C_SHADOW);
		}
		break;
	case RUI_ICON_CLOCK:
		/* an octagon-ish face built from bands */
		PX(10, 2, 12, 28, P_PAPER);
		PX(6, 4, 20, 24, P_PAPER);
		PX(4, 6, 24, 20, P_PAPER);
		PX(2, 10, 28, 12, P_PAPER);
		FC(10, 1, 12, 1, RUI_C_DARK);
		FC(10, 30, 12, 1, RUI_C_DARK);
		FC(1, 10, 1, 12, RUI_C_DARK);
		FC(30, 10, 1, 12, RUI_C_DARK);
		FC(6, 3, 4, 1, RUI_C_DARK);
		FC(22, 3, 4, 1, RUI_C_DARK);
		FC(6, 28, 4, 1, RUI_C_DARK);
		FC(22, 28, 4, 1, RUI_C_DARK);
		FC(3, 6, 1, 4, RUI_C_DARK);
		FC(28, 6, 1, 4, RUI_C_DARK);
		FC(3, 22, 1, 4, RUI_C_DARK);
		FC(28, 22, 1, 4, RUI_C_DARK);
		FC(15, 4, 2, 2, RUI_C_DARK);         /* 12, 3, 6, 9 */
		FC(26, 15, 2, 2, RUI_C_DARK);
		FC(15, 26, 2, 2, RUI_C_DARK);
		FC(4, 15, 2, 2, RUI_C_DARK);
		FC(15, 8, 2, 9, RUI_C_DARK);         /* hands */
		FC(15, 15, 8, 2, RUI_C_DARK);
		PX(15, 15, 2, 2, P_RED);
		break;
	case RUI_ICON_BROWSER:
		/* a globe: blue disc, green land, white grid */
		PX(10, 2, 12, 28, P_BLUE);
		PX(6, 4, 20, 24, P_BLUE);
		PX(4, 6, 24, 20, P_BLUE);
		PX(2, 10, 28, 12, P_BLUE);
		PX(8, 7, 7, 6, P_GREEN);
		PX(6, 12, 6, 8, P_GREEN);
		PX(18, 15, 8, 9, P_GREEN);
		PX(20, 7, 4, 3, P_GREEN);
		PX(2, 15, 28, 1, P_SKY);
		PX(15, 2, 1, 28, P_SKY);
		PX(6, 9, 20, 1, P_SKY);
		PX(6, 22, 20, 1, P_SKY);
		break;
	case RUI_ICON_SETTINGS:
		/* a control panel with three sliders */
		FC(1, 3, 30, 26, RUI_C_FACE);
		rui_bevel(s, x + 1, y + 3, 30, 26, RUI_RAISED);
		for (i = 0; i < 3; i++) {
			int sx = 7 + i * 9, ky = 8 + ((i * 7) % 13);

			FC(sx, 7, 2, 18, RUI_C_DARK);
			FC(sx - 3, ky, 8, 4, RUI_C_FACE);
			rui_bevel(s, x + sx - 3, y + ky, 8, 4, RUI_RAISED_THIN);
			PX(sx - 2, ky + 1, 6, 2, i == 0 ? P_RED : i == 1 ? P_GREEN :
			    P_BLUE);
		}
		break;
	case RUI_ICON_INFO:
		/* a speech-bubble "i" */
		PX(10, 2, 12, 24, P_BLUE);
		PX(6, 4, 20, 20, P_BLUE);
		PX(4, 6, 24, 16, P_BLUE);
		PX(2, 9, 28, 10, P_BLUE);
		PX(9, 24, 6, 3, P_BLUE);
		PX(9, 27, 3, 3, P_BLUE);
		PX(14, 6, 4, 4, P_WHITE);
		PX(14, 12, 4, 10, P_WHITE);
		PX(12, 12, 2, 2, P_WHITE);
		PX(12, 20, 8, 2, P_WHITE);
		break;
	case RUI_ICON_PROGRAMS:
		/* a program group: a window holding four small programs */
		FC(0, 1, 32, 30, RUI_C_FACE);
		rui_bevel(s, x, y + 1, 32, 30, RUI_RAISED);
		FC(2, 3, 28, 5, RUI_C_TITLE_ACTIVE);
		FC(3, 9, 26, 20, RUI_C_FIELD);
		rui_bevel(s, x + 3, y + 9, 26, 20, RUI_SUNKEN_THIN);
		small_window(s, x + 4, y + 10, P_BLUE);
		small_window(s, x + 17, y + 10, P_GREEN);
		PX(5, 21, 10, 6, P_YELLOW);
		PX(5, 20, 4, 2, P_YELLOW_DK);
		small_window(s, x + 17, y + 18, P_RED);
		break;
	case RUI_ICON_BT:
		PX(10, 1, 12, 30, P_BLUE);
		PX(8, 3, 16, 26, P_BLUE);
		for (i = 0; i < 6; i++) {
			PX(15, 5 + i * 4, 2, 4, P_WHITE);  /* the stem */
		}
		for (i = 0; i < 5; i++) {
			PX(16 + i, 6 + i, 2, 2, P_WHITE);  /* upper arrow */
			PX(20 - i, 11 + i, 2, 2, P_WHITE);
			PX(16 + i, 16 + i, 2, 2, P_WHITE); /* lower arrow */
			PX(20 - i, 21 + i, 2, 2, P_WHITE);
			PX(11 + i, 11 + i, 2, 2, P_WHITE); /* the crossing */
			PX(15 - i, 16 + i, 2, 2, P_WHITE);
		}
		break;
	case RUI_ICON_GAME:
		/* a joystick on a base */
		FC(3, 20, 26, 10, RUI_C_FACE);
		rui_bevel(s, x + 3, y + 20, 26, 10, RUI_RAISED);
		FC(15, 8, 3, 13, RUI_C_DARK);
		PX(12, 2, 9, 8, P_RED);
		PX(13, 3, 3, 2, P_WHITE);
		PX(7, 23, 4, 3, P_RED);
		PX(22, 23, 4, 3, P_GREEN);
		break;
	default:                                   /* a generic program window */
		FC(1, 3, 30, 26, RUI_C_FACE);
		rui_bevel(s, x + 1, y + 3, 30, 26, RUI_RAISED);
		FC(4, 6, 24, 6, RUI_C_TITLE_ACTIVE);
		FC(4, 14, 24, 12, RUI_C_FIELD);
		rui_bevel(s, x + 4, y + 14, 24, 12, RUI_SUNKEN_THIN);
		break;
	}
}
