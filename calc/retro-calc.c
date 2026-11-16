/*
 * retro-calc - scientific calculator.
 *
 * Type an expression or press the keys; Enter (or =) evaluates it.
 *   functions  sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh
 *              exp ln log log2 logb(x,b) sqrt cbrt root(x,n) pow(x,y)
 *              abs floor ceil round trunc frac sign fact gamma deg rad
 *              min max gcd lcm ncr npr hypot atan2 mod not
 *   operators  + - * / % mod ^ ** !   & | xor ~ << >>   2pi 3(4+1)
 *   numbers    1.5e-3 0xff 0b1010 0o17     names  pi e tau phi ans mem
 *   variables  r = 2.5   then   pi r^2
 * Deg/Rad switches the trigonometry, Dec/Hex/Bin/Oct the result's base.
 *
 *   retro-calc                 the window
 *   retro-calc -e EXPR...      print results (radians; -d for degrees)
 */
#include "../lib/retroui.h"
#include "expr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 7
#define ROWS 7
#define WIN_W 420
#define MAX_HIST 16

enum { K_TEXT, K_EVAL, K_CLEAR, K_BACK, K_ANGLE, K_BASE, K_MC, K_MR, K_MADD,
       K_MSUB, K_HIST };

static const struct {
	const char *label, *text;
	int kind;
} keys[ROWS * COLS] = {
	{ "Deg", NULL, K_ANGLE }, { "Dec", NULL, K_BASE }, { "MC", NULL, K_MC },
	{ "MR", "mem", K_MR }, { "M+", NULL, K_MADD }, { "M-", NULL, K_MSUB },
	{ "AC", NULL, K_CLEAR },

	{ "sin", "sin(", 0 }, { "cos", "cos(", 0 }, { "tan", "tan(", 0 },
	{ "(", "(", 0 }, { ")", ")", 0 }, { "Del", NULL, K_BACK }, { "/", "/", 0 },

	{ "asin", "asin(", 0 }, { "acos", "acos(", 0 }, { "atan", "atan(", 0 },
	{ "7", "7", 0 }, { "8", "8", 0 }, { "9", "9", 0 }, { "*", "*", 0 },

	{ "ln", "ln(", 0 }, { "log", "log(", 0 }, { "e^x", "exp(", 0 },
	{ "4", "4", 0 }, { "5", "5", 0 }, { "6", "6", 0 }, { "-", "-", 0 },

	{ "x^2", "^2", 0 }, { "x^y", "^", 0 }, { "sqrt", "sqrt(", 0 },
	{ "1", "1", 0 }, { "2", "2", 0 }, { "3", "3", 0 }, { "+", "+", 0 },

	{ "pi", "pi", 0 }, { "e", "e", 0 }, { "n!", "!", 0 },
	{ "0", "0", 0 }, { ".", ".", 0 }, { "EXP", "e", 0 }, { "=", NULL, K_EVAL },

	{ "ans", "ans", 0 }, { "1/x", "1/(", 0 }, { "abs", "abs(", 0 },
	{ "mod", " mod ", 0 }, { "nCr", "ncr(", 0 }, { ",", ",", 0 },
	{ "Hist", NULL, K_HIST },
};

static struct {
	RuiApp *app;
	Window win;
	RuiSurf *s;
	int running, dirty, win_h;
	CalcState st;
	RuiInput expr;
	RuiButton b[ROWS * COLS];
	RuiFont *big;
	RuiRect disp;
	int base;                          /* 10, 16, 2, 8                    */
	int fresh;                         /* a result is showing             */
	int error;
	double value;
	char result[128];
	char *hist[MAX_HIST];              /* expressions, newest first       */
	char *hist_res[MAX_HIST];
	int nhist;
} ca;

static void
show_value(void)
{
	calc_format(ca.value, ca.base, ca.result, sizeof ca.result);
	ca.error = 0;
	ca.dirty = 1;
}

static void
hist_push(const char *expr, const char *res)
{
	if (ca.nhist && !strcmp(ca.hist[0], expr))
		return;
	if (ca.nhist == MAX_HIST) {
		free(ca.hist[MAX_HIST - 1]);
		free(ca.hist_res[MAX_HIST - 1]);
		ca.nhist--;
	}
	memmove(&ca.hist[1], &ca.hist[0], (size_t)ca.nhist * sizeof ca.hist[0]);
	memmove(&ca.hist_res[1], &ca.hist_res[0], (size_t)ca.nhist *
	    sizeof ca.hist_res[0]);
	ca.hist[0] = rui_strdup(expr);
	ca.hist_res[0] = rui_strdup(res);
	ca.nhist++;
}

/* 1 when the expression gave a value */
static int
evaluate(void)
{
	char err[96];
	double v;

	if (!ca.expr.text[0])
		return 0;
	if (calc_eval(&ca.st, ca.expr.text, &v, err, sizeof err) < 0) {
		rui_strlcpy(ca.result, err, sizeof ca.result);
		ca.error = 1;
		ca.dirty = 1;
		return 0;
	}
	ca.value = v;
	show_value();
	hist_push(ca.expr.text, ca.result);
	ca.fresh = 1;
	return 1;
}

/* inserts at the cursor */
static void
insert(const char *t)
{
	RuiInput *in = &ca.expr;
	size_t n = strlen(t);

	if ((size_t)in->len + n >= RUI_INPUT_MAX)
		return;
	memmove(in->text + in->cursor + n, in->text + in->cursor,
	    (size_t)(in->len - in->cursor) + 1);
	memcpy(in->text + in->cursor, t, n);
	in->len += (int)n;
	in->cursor += (int)n;
	ca.dirty = 1;
}

/* After a result, an operator continues from it ("ans + ..."); anything
 * else starts a new expression. */
static void
begin_input(const char *t)
{
	if (!ca.fresh) {
		/* an empty line that starts with an operator continues, too */
		if (!ca.expr.text[0] && t[0] && strchr("*/^%!", t[0]))
			rui_input_set(&ca.expr, "ans");
		return;
	}
	ca.fresh = 0;
	if (strchr("+-*/^%!&|<>", t[0]) || !strncmp(t, " mod", 4))
		rui_input_set(&ca.expr, "ans");
	else
		rui_input_set(&ca.expr, "");
}

static void
history_popup(const RuiButton *b)
{
	RuiMenu *m = rui_menu_new();
	const RuiMenuItem *it;
	Window child;
	char label[200];
	int x, y, i;

	for (i = 0; i < ca.nhist; i++) {
		snprintf(label, sizeof label, "%.120s  =  %.60s", ca.hist[i],
		    ca.hist_res[i]);
		rui_menu_add(m, label, NULL, i + 1);
	}
	if (!ca.nhist)
		rui_menu_add(m, "(nothing calculated yet)", NULL, 0)->disabled = 1;
	XTranslateCoordinates(ca.app->dpy, ca.win, ca.app->root, b->r.x, b->r.y,
	    &x, &y, &child);
	it = rui_menu_popup(ca.app, m, x, y, RUI_MENU_UPWARD);
	if (it && it->id) {
		rui_input_set(&ca.expr, ca.hist[it->id - 1]);
		ca.fresh = 0;
	}
	rui_menu_free(m);
	ca.dirty = 1;
}

static void
press(int i)
{
	static const int bases[] = { 10, 16, 2, 8 };
	int k;

	switch (keys[i].kind) {
	case K_EVAL:
		evaluate();
		break;
	case K_CLEAR:
		rui_input_set(&ca.expr, "");
		ca.result[0] = '\0';
		ca.fresh = ca.error = 0;
		break;
	case K_BACK:
		ca.fresh = 0;
		if (ca.expr.cursor > 0) {
			RuiInput *in = &ca.expr;
			int c = in->cursor - 1;

			/* a whole UTF-8 character */
			while (c > 0 && ((unsigned char)in->text[c] & 0xc0) == 0x80)
				c--;
			memmove(in->text + c, in->text + in->cursor,
			    (size_t)(in->len - in->cursor) + 1);
			in->len -= in->cursor - c;
			in->cursor = c;
		}
		break;
	case K_ANGLE:
		ca.st.degrees = !ca.st.degrees;
		break;
	case K_BASE:
		for (k = 0; k < 4; k++)
			if (bases[k] == ca.base)
				break;
		ca.base = bases[(k + 1) % 4];
		if (ca.result[0] && !ca.error)
			show_value();
		break;
	case K_MC:
		ca.st.mem = 0;
		break;
	case K_MADD:
	case K_MSUB:
		if (ca.fresh || evaluate())
			ca.st.mem += keys[i].kind == K_MADD ? ca.value : -ca.value;
		break;
	case K_HIST:
		history_popup(&ca.b[i]);
		break;
	default:
		begin_input(keys[i].text);
		insert(keys[i].text);
		break;
	}
	ca.dirty = 1;
}

static void
draw(void)
{
	RuiSurf *s = ca.s;
	RuiApp *a = ca.app;
	RuiRect r;
	char tag[64];
	int i;

	rui_fill(s, 0, 0, WIN_W, ca.win_h, RUI_C_FACE);
	ca.expr.focused = 1;
	rui_input_draw(s, &ca.expr);
	rui_fill(s, ca.disp.x, ca.disp.y, ca.disp.w, ca.disp.h, RUI_C_FIELD);
	rui_bevel(s, ca.disp.x, ca.disp.y, ca.disp.w, ca.disp.h, RUI_SUNKEN);
	r = ca.disp;
	r.x += 8;
	r.w -= 16;
	r.y += 3;
	r.h = a->font->height;
	snprintf(tag, sizeof tag, "%s   %s%s", ca.st.degrees ? "DEG" : "RAD",
	    ca.base == 16 ? "HEX" : ca.base == 2 ? "BIN" : ca.base == 8 ? "OCT" :
	    "DEC", ca.st.mem != 0 ? "   M" : "");
	rui_text_fit(s, a->font, r, tag, RUI_C_DISABLED, RUI_LEFT);
	r.y += a->font->height;
	r.h = ca.disp.y + ca.disp.h - r.y - 3;
	rui_text_fit(s, ca.error ? a->bold : ca.big, r, ca.result[0] ? ca.result :
	    "0", ca.error ? RUI_C_ALERT : RUI_C_TEXT, RUI_RIGHT);
	for (i = 0; i < ROWS * COLS; i++) {
		ca.b[i].label = keys[i].kind == K_ANGLE ? (ca.st.degrees ? "Deg" :
		    "Rad") : keys[i].kind == K_BASE ? (ca.base == 16 ? "Hex" :
		    ca.base == 2 ? "Bin" : ca.base == 8 ? "Oct" : "Dec") :
		    keys[i].label;
		rui_button_draw(s, &ca.b[i]);
	}
	rui_surf_flush(s);
	ca.dirty = 0;
}

static void
handle_event(XEvent *ev)
{
	int i, r;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			rui_surf_flush(ca.s);
		break;
	case KeyPress: {
		KeySym ks = XLookupKeysym(&ev->xkey, 0);
		char buf[8] = "";
		int n = XLookupString(&ev->xkey, buf, sizeof buf - 1, NULL, NULL);

		if (ks == XK_Escape) {
			if (ca.expr.text[0] || ca.result[0])
				press(6);                  /* AC */
			else
				ca.running = 0;
			break;
		}
		if ((ev->xkey.state & ControlMask) && ks == XK_q) {
			ca.running = 0;
			break;
		}
		if (n == 1 && buf[0] == '=' && !(ev->xkey.state & ControlMask)) {
			/* "x = 3" assigns; a trailing "=" evaluates */
			const char *t = ca.expr.text;
			int name = *t != '\0';

			for (; *t; t++)
				if (!((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') ||
				    *t == '_' || *t == ' ' || (*t >= '0' && *t <= '9')))
					name = 0;
			if (!name || ca.fresh || (ca.expr.text[0] >= '0' &&
			    ca.expr.text[0] <= '9')) {
				evaluate();
				ca.dirty = 1;
				break;
			}
		}
		if (n == 1 && (unsigned char)buf[0] >= ' ' && buf[0] != 127) {
			buf[1] = '\0';
			begin_input(buf);
		} else if (ks == XK_BackSpace || ks == XK_Left || ks == XK_Right ||
		    ks == XK_Home || ks == XK_End || ks == XK_Delete) {
			ca.fresh = 0;                  /* editing the old expression */
		}
		r = rui_input_event(ca.app, &ca.expr, ev);
		if (r & RUI_EV_ACTIVATE)
			evaluate();
		ca.dirty = 1;
		break;
	}
	case SelectionNotify:
		ca.fresh = 0;
		if (rui_input_event(ca.app, &ca.expr, ev))
			ca.dirty = 1;
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		for (i = 0; i < ROWS * COLS; i++) {
			r = rui_button_event(&ca.b[i], ev);
			if (r & RUI_EV_ACTIVATE)
				press(i);
			ca.dirty |= r != 0;
		}
		if (ev->type == ButtonPress && rui_in_rect(ca.expr.r, ev->xbutton.x,
		    ev->xbutton.y)) {
			ca.fresh = 0;
			if (rui_input_event(ca.app, &ca.expr, ev))
				ca.dirty = 1;
		}
		break;
	case ClientMessage:
		if (rui_wm_delete_event(ca.app, ev))
			ca.running = 0;
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

static void
calc_conf(const char *key, const char *val, void *ud)
{
	(void)ud;
	if (!strcmp(key, "calc_angle"))
		ca.st.degrees = strcmp(val, "rad") != 0;
}

static void
setup(void)
{
	RuiApp *a = ca.app;
	const RuiSettings *set = rui_settings();
	XSizeHints sh;
	char spec[300], fam[256], *dash, *path;
	int fh = a->font->height, bh = fh + 14, i, y;
	int bw = (WIN_W - 20 - (COLS - 1) * 4) / COLS;

	ca.base = 10;
	ca.st.degrees = 1;
	path = rui_config_path("retro.conf");
	rui_conf_parse(path, calc_conf, NULL);
	free(path);
	/* the result is shown larger: the same family at 16 points */
	rui_strlcpy(fam, set->font, sizeof fam);
	dash = strrchr(fam, '-');
	if (a->font->xft && dash && dash[1] >= '0' && dash[1] <= '9') {
		*dash = '\0';
		snprintf(spec, sizeof spec, "%s-16:bold", fam);
		ca.big = rui_font_open(a, spec);
	} else {
		ca.big = rui_font_open(a, set->font);
	}
	ca.expr.r.x = 10;
	ca.expr.r.y = 10;
	ca.expr.r.w = WIN_W - 20;
	ca.expr.r.h = fh + 12;
	ca.disp.x = 10;
	ca.disp.y = ca.expr.r.y + ca.expr.r.h + 6;
	ca.disp.w = WIN_W - 20;
	ca.disp.h = fh + ca.big->height + 10;
	y = ca.disp.y + ca.disp.h + 10;
	for (i = 0; i < ROWS * COLS; i++) {
		ca.b[i].r.x = 10 + (i % COLS) * (bw + 4);
		ca.b[i].r.y = y + (i / COLS) * (bh + 4);
		ca.b[i].r.w = bw;
		ca.b[i].r.h = bh;
		ca.b[i].bold = keys[i].kind == K_EVAL;
	}
	ca.win_h = y + ROWS * (bh + 4) + 6;

	ca.win = rui_window_new(a, a->root, 60, 60, WIN_W, ca.win_h, 0,
	    ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
	    ButtonMotionMask | StructureNotifyMask);
	ca.s = rui_surf_new(a, ca.win, WIN_W, ca.win_h);
	rui_set_title(a, ca.win, "Calculator", "retro-calc");
	memset(&sh, 0, sizeof sh);
	sh.flags = PMinSize | PMaxSize | PSize;
	sh.width = sh.min_width = sh.max_width = WIN_W;
	sh.height = sh.min_height = sh.max_height = ca.win_h;
	XSetWMNormalHints(a->dpy, ca.win, &sh);
	rui_input_init(a, &ca.expr, ca.win);
	a->dispatch = dispatch_cb;
	XMapWindow(a->dpy, ca.win);
}

int
main(int argc, char **argv)
{
	int i;

	if (argc > 1) {
		CalcState st;
		int rc = 0, any = 0;

		if (!strcmp(argv[1], "-v")) {
			puts("retro-calc " RUI_VERSION);
			return 0;
		}
		memset(&st, 0, sizeof st);
		for (i = 1; i < argc; i++) {
			char err[96], out[128];
			double v;

			if (!strcmp(argv[i], "-d")) {
				st.degrees = 1;
			} else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
				any = 1;
				if (calc_eval(&st, argv[++i], &v, err, sizeof err) < 0) {
					fprintf(stderr, "retro-calc: %s: %s\n", argv[i], err);
					rc = 1;
				} else {
					calc_format(v, 10, out, sizeof out);
					puts(out);
				}
			} else {
				any = 0;
				break;
			}
		}
		if (!any) {
			fprintf(stderr, "usage: retro-calc [-v] | [-d] -e EXPR "
			    "[-e EXPR...]\n");
			return 2;
		}
		return rc;
	}
	rui_signals_init();
	ca.app = rui_open("retro-calc");
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		rui_die("pledge failed");
#endif
	ca.running = 1;
	ca.dirty = 1;
	while (ca.running && !rui_quit_requested()) {
		XEvent ev;

		while (ca.running && XPending(ca.app->dpy)) {
			XNextEvent(ca.app->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}
		if (ca.dirty && ca.running)
			draw();
		if (ca.running)
			rui_wait(ca.app, -1);
	}
	for (i = 0; i < ca.nhist; i++) {
		free(ca.hist[i]);
		free(ca.hist_res[i]);
	}
	rui_input_destroy(&ca.expr);
	rui_font_close(ca.app, ca.big);
	rui_surf_free(ca.s);
	XDestroyWindow(ca.app->dpy, ca.win);
	rui_close(ca.app);
	return 0;
}
