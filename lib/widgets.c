/* libretroui widgets: button, scrollbar, slider, text input, list. */
#include "retroui.h"

#include <stdlib.h>
#include <string.h>

static int
ev_xy(const XEvent *ev, int *x, int *y)
{
	switch (ev->type) {
	case ButtonPress:
	case ButtonRelease:
		*x = ev->xbutton.x;
		*y = ev->xbutton.y;
		return 1;
	case MotionNotify:
		*x = ev->xmotion.x;
		*y = ev->xmotion.y;
		return 1;
	default:
		return 0;
	}
}

/* ----------------------------------------------------------------- button */

void
rui_button_draw(RuiSurf *s, const RuiButton *b)
{
	RuiApp *a = s->app;
	int down = (b->pressed && b->armed) || b->toggled;
	RuiRect t = b->r;

	if (b->toggled && !(b->pressed && b->armed))
		rui_fill_dither(s, b->r.x, b->r.y, b->r.w, b->r.h, RUI_C_FACE,
		    RUI_C_LIGHT);
	else
		rui_fill(s, b->r.x, b->r.y, b->r.w, b->r.h, RUI_C_FACE);
	rui_bevel(s, b->r.x, b->r.y, b->r.w, b->r.h, down ? RUI_PRESSED : RUI_RAISED);
	if (!b->label)
		return;
	t.x += 4 + (down ? 1 : 0);
	t.y += down ? 1 : 0;
	t.w -= 8;
	if (b->disabled) {
		RuiRect t2 = t;

		t2.x++;
		t2.y++;
		rui_text_fit(s, a->font, t2, b->label, RUI_C_LIGHT, RUI_CENTER);
		rui_text_fit(s, a->font, t, b->label, RUI_C_DISABLED, RUI_CENTER);
	} else {
		rui_text_fit(s, b->bold ? a->bold : a->font, t, b->label, RUI_C_TEXT,
		    RUI_CENTER);
	}
}

int
rui_button_event(RuiButton *b, const XEvent *ev)
{
	int x, y, in;

	if (b->disabled || !ev_xy(ev, &x, &y))
		return 0;
	in = rui_in_rect(b->r, x, y);
	switch (ev->type) {
	case ButtonPress:
		if (ev->xbutton.button == Button1 && in) {
			b->pressed = b->armed = 1;
			return RUI_EV_REDRAW;
		}
		break;
	case MotionNotify:
		if (b->pressed && b->armed != in) {
			b->armed = in;
			return RUI_EV_REDRAW;
		}
		break;
	case ButtonRelease:
		if (ev->xbutton.button == Button1 && b->pressed) {
			b->pressed = b->armed = 0;
			return in ? (RUI_EV_REDRAW | RUI_EV_ACTIVATE) : RUI_EV_REDRAW;
		}
		break;
	default:
		break;
	}
	return 0;
}

/* -------------------------------------------------------------- scrollbar */

static int
sb_max(const RuiScrollbar *sb)
{
	return sb->total > sb->page ? sb->total - sb->page : 0;
}

static void
sb_geom(const RuiScrollbar *sb, int *trough_y, int *trough_h, int *thumb_y,
    int *thumb_h)
{
	int ty = sb->r.y + sb->r.w, th = sb->r.h - 2 * sb->r.w, kh, ky;
	int max = sb_max(sb);

	if (th < 0)
		th = 0;
	if (max == 0 || sb->total <= 0) {
		kh = th;
		ky = ty;
	} else {
		kh = (int)((long)th * sb->page / sb->total);
		if (kh < 12)
			kh = 12;
		if (kh > th)
			kh = th;
		ky = ty + (int)((long)(th - kh) * sb->pos / max);
	}
	*trough_y = ty;
	*trough_h = th;
	*thumb_y = ky;
	*thumb_h = kh;
}

static void
sb_clamp(RuiScrollbar *sb)
{
	int max = sb_max(sb);

	if (sb->pos > max)
		sb->pos = max;
	if (sb->pos < 0)
		sb->pos = 0;
}

void
rui_scrollbar_draw(RuiSurf *s, const RuiScrollbar *sb)
{
	int ty, th, ky, kh, bw = sb->r.w;
	int col = sb_max(sb) ? RUI_C_TEXT : RUI_C_DISABLED;

	sb_geom(sb, &ty, &th, &ky, &kh);
	rui_fill_dither(s, sb->r.x, sb->r.y, sb->r.w, sb->r.h, RUI_C_FACE,
	    RUI_C_LIGHT);
	rui_fill(s, sb->r.x, sb->r.y, bw, bw, RUI_C_FACE);
	rui_bevel(s, sb->r.x, sb->r.y, bw, bw, RUI_RAISED);
	rui_arrow(s, sb->r.x + bw / 2, sb->r.y + bw / 2, 4, 0, col);
	rui_fill(s, sb->r.x, sb->r.y + sb->r.h - bw, bw, bw, RUI_C_FACE);
	rui_bevel(s, sb->r.x, sb->r.y + sb->r.h - bw, bw, bw, RUI_RAISED);
	rui_arrow(s, sb->r.x + bw / 2, sb->r.y + sb->r.h - bw + bw / 2, 4, 1, col);
	if (sb_max(sb) && kh > 0) {
		rui_fill(s, sb->r.x, ky, bw, kh, RUI_C_FACE);
		rui_bevel(s, sb->r.x, ky, bw, kh, RUI_RAISED);
	}
}

int
rui_scrollbar_event(RuiScrollbar *sb, const XEvent *ev)
{
	int x, y, ty, th, ky, kh, old = sb->pos;

	if (!ev_xy(ev, &x, &y))
		return 0;
	sb_geom(sb, &ty, &th, &ky, &kh);
	switch (ev->type) {
	case ButtonPress:
		if (!rui_in_rect(sb->r, x, y))
			return 0;
		if (ev->xbutton.button == Button4) {
			sb->pos -= 3;
		} else if (ev->xbutton.button == Button5) {
			sb->pos += 3;
		} else if (ev->xbutton.button == Button1) {
			if (y < ty)
				sb->pos--;
			else if (y >= ty + th)
				sb->pos++;
			else if (y < ky)
				sb->pos -= sb->page;
			else if (y >= ky + kh)
				sb->pos += sb->page;
			else {
				sb->drag = 1;
				sb->drag_off = y - ky;
			}
		}
		break;
	case MotionNotify:
		if (!sb->drag)
			return 0;
		if (th - kh > 0)
			sb->pos = (int)((long)(y - sb->drag_off - ty) * sb_max(sb) /
			    (th - kh));
		break;
	case ButtonRelease:
		if (!sb->drag)
			return 0;
		sb->drag = 0;
		break;
	default:
		return 0;
	}
	sb_clamp(sb);
	return sb->pos != old ? (RUI_EV_REDRAW | RUI_EV_CHANGED) : RUI_EV_REDRAW;
}

/* ----------------------------------------------------------------- slider */

#define KNOB_LEN 11

void
rui_slider_draw(RuiSurf *s, const RuiSlider *sl)
{
	RuiRect r = sl->r;

	rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_FACE);
	if (sl->vertical) {
		int span = r.h - KNOB_LEN;
		int ky = r.y + (int)((1.0 - sl->value) * span + 0.5);
		int i;

		rui_bevel(s, r.x + r.w / 2 - 2, r.y, 4, r.h, RUI_SUNKEN);
		for (i = 0; i <= 4; i++)
			rui_fill(s, r.x, r.y + KNOB_LEN / 2 + span * i / 4, 3, 1,
			    RUI_C_SHADOW);
		rui_fill(s, r.x + 4, ky, r.w - 8, KNOB_LEN, RUI_C_FACE);
		rui_bevel(s, r.x + 4, ky, r.w - 8, KNOB_LEN, RUI_RAISED);
	} else {
		int span = r.w - KNOB_LEN;
		int kx = r.x + (int)(sl->value * span + 0.5);

		rui_bevel(s, r.x, r.y + r.h / 2 - 2, r.w, 4, RUI_SUNKEN);
		rui_fill(s, kx, r.y + 2, KNOB_LEN, r.h - 4, RUI_C_FACE);
		rui_bevel(s, kx, r.y + 2, KNOB_LEN, r.h - 4, RUI_RAISED);
	}
}

static void
slider_set_from(RuiSlider *sl, int x, int y)
{
	double v;

	if (sl->vertical) {
		int span = sl->r.h - KNOB_LEN;

		v = span > 0 ? 1.0 - (double)(y - sl->r.y - KNOB_LEN / 2) / span : 0;
	} else {
		int span = sl->r.w - KNOB_LEN;

		v = span > 0 ? (double)(x - sl->r.x - KNOB_LEN / 2) / span : 0;
	}
	sl->value = v < 0 ? 0 : v > 1 ? 1 : v;
}

int
rui_slider_event(RuiSlider *sl, const XEvent *ev)
{
	int x, y;
	double old = sl->value;

	if (!ev_xy(ev, &x, &y))
		return 0;
	switch (ev->type) {
	case ButtonPress:
		if (!rui_in_rect(sl->r, x, y))
			return 0;
		if (ev->xbutton.button == Button4)
			sl->value = sl->value + 0.05 > 1 ? 1 : sl->value + 0.05;
		else if (ev->xbutton.button == Button5)
			sl->value = sl->value - 0.05 < 0 ? 0 : sl->value - 0.05;
		else if (ev->xbutton.button == Button1) {
			sl->drag = 1;
			slider_set_from(sl, x, y);
		}
		break;
	case MotionNotify:
		if (!sl->drag)
			return 0;
		slider_set_from(sl, x, y);
		break;
	case ButtonRelease:
		if (!sl->drag)
			return 0;
		sl->drag = 0;
		break;
	default:
		return 0;
	}
	return sl->value != old ? (RUI_EV_REDRAW | RUI_EV_CHANGED) : RUI_EV_REDRAW;
}

/* ------------------------------------------------------------- text input */

void
rui_input_init(RuiApp *app, RuiInput *in, Window win)
{
	in->win = win;
	in->xic = NULL;
	if (app->xim)
		in->xic = XCreateIC(app->xim, XNInputStyle,
		    XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win,
		    XNFocusWindow, win, (char *)NULL);
	if (in->xic)
		XSetICFocus(in->xic);
}

void
rui_input_destroy(RuiInput *in)
{
	if (in->xic)
		XDestroyIC(in->xic);
	in->xic = NULL;
}

void
rui_input_set(RuiInput *in, const char *text)
{
	rui_strlcpy(in->text, text, sizeof in->text);
	in->len = (int)strlen(in->text);
	in->cursor = in->len;
	in->scroll = 0;
}

static int
utf8_prev(const char *s, int i)
{
	if (i <= 0)
		return 0;
	i--;
	while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		i--;
	return i;
}

static int
utf8_next(const char *s, int len, int i)
{
	if (i >= len)
		return len;
	i++;
	while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80)
		i++;
	return i;
}

static void
input_insert(RuiInput *in, const char *s, int n)
{
	int i;

	/* never accept control characters; keep whole UTF-8 sequences */
	for (i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) {
			n = i;
			break;
		}
	if (n <= 0 || in->len + n >= RUI_INPUT_MAX)
		return;
	memmove(in->text + in->cursor + n, in->text + in->cursor,
	    (size_t)(in->len - in->cursor + 1));
	memcpy(in->text + in->cursor, s, (size_t)n);
	in->len += n;
	in->cursor += n;
}

static void
input_delete(RuiInput *in, int from, int to)
{
	if (from >= to)
		return;
	memmove(in->text + from, in->text + to, (size_t)(in->len - to + 1));
	in->len -= to - from;
	in->cursor = from;
}

/* visible string (password fields show asterisks) and cursor offset in it */
static const char *
input_visible(RuiInput *in, char *buf, size_t n, int *cur)
{
	int i, chars = 0, cchars = 0;

	if (!in->password) {
		*cur = in->cursor;
		return in->text;
	}
	for (i = 0; i < in->len; i = utf8_next(in->text, in->len, i)) {
		if (i < in->cursor)
			cchars++;
		chars++;
	}
	if (chars > (int)n - 1)
		chars = (int)n - 1;
	memset(buf, '*', (size_t)chars);
	buf[chars] = '\0';
	*cur = cchars > chars ? chars : cchars;
	return buf;
}

void
rui_input_draw(RuiSurf *s, RuiInput *in)
{
	RuiApp *a = s->app;
	RuiRect r = in->r;
	char pw[RUI_INPUT_MAX];
	int cur, cx, inner = r.w - 8, ty;
	const char *vis = input_visible(in, pw, sizeof pw, &cur);
	XRectangle clip;

	rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_FIELD);
	rui_bevel(s, r.x, r.y, r.w, r.h, RUI_SUNKEN);
	cx = rui_text_width(a, a->font, vis, cur);
	if (cx - in->scroll > inner - 2)
		in->scroll = cx - inner + 2;
	if (cx - in->scroll < 0)
		in->scroll = cx;
	if (in->scroll < 0)
		in->scroll = 0;
	clip.x = (short)(r.x + 3);
	clip.y = (short)(r.y + 2);
	clip.width = (unsigned short)(r.w > 6 ? r.w - 6 : 1);
	clip.height = (unsigned short)(r.h > 4 ? r.h - 4 : 1);
	XftDrawSetClipRectangles(s->xd, 0, 0, &clip, 1);
	XSetClipRectangles(a->dpy, a->gc, 0, 0, &clip, 1, Unsorted);
	ty = r.y + (r.h - a->font->height) / 2;
	rui_text(s, a->font, r.x + 4 - in->scroll, ty, vis, -1, RUI_C_TEXT);
	if (in->focused)
		rui_fill(s, r.x + 4 - in->scroll + cx, ty, 1, a->font->height,
		    RUI_C_TEXT);
	XftDrawSetClip(s->xd, None);
	XSetClipMask(a->dpy, a->gc, None);
}

static void
input_paste_request(RuiApp *app, RuiInput *in, Time t)
{
	XConvertSelection(app->dpy, XA_PRIMARY, app->atom[A_UTF8_STRING],
	    app->atom[A__RETRO_SELECTION], in->win, t);
}

static int
input_click(RuiApp *app, RuiInput *in, int x)
{
	char pw[RUI_INPUT_MAX];
	int cur, i, best = 0, target = x - (in->r.x + 4) + in->scroll;
	const char *vis = input_visible(in, pw, sizeof pw, &cur);
	int vlen = (int)strlen(vis);

	for (i = 0;; i = utf8_next(vis, vlen, i)) {
		if (rui_text_width(app, app->font, vis, i) <= target)
			best = i;
		else
			break;
		if (i >= vlen)
			break;
	}
	if (in->password) {
		/* map character index back to byte offset */
		int c = 0, b = 0;

		while (c < best && b < in->len) {
			b = utf8_next(in->text, in->len, b);
			c++;
		}
		best = b;
	}
	in->cursor = best;
	return RUI_EV_REDRAW;
}

int
rui_input_event(RuiApp *app, RuiInput *in, XEvent *ev)
{
	char buf[128];
	KeySym ks = NoSymbol;
	int n, ctrl;

	if (ev->type == ButtonPress) {
		if (!rui_in_rect(in->r, ev->xbutton.x, ev->xbutton.y))
			return 0;
		if (ev->xbutton.button == Button2) {
			input_paste_request(app, in, ev->xbutton.time);
			return 0;
		}
		if (ev->xbutton.button == Button1)
			return input_click(app, in, ev->xbutton.x);
		return 0;
	}
	if (ev->type == SelectionNotify) {
		Atom type;
		int fmt;
		unsigned long nitems, after;
		unsigned char *data = NULL;

		if (ev->xselection.property != app->atom[A__RETRO_SELECTION])
			return 0;
		if (XGetWindowProperty(app->dpy, in->win,
		    app->atom[A__RETRO_SELECTION], 0, 4096, True, AnyPropertyType,
		    &type, &fmt, &nitems, &after, &data) == Success && data) {
			if (fmt == 8 && nitems > 0) {
				int len = (int)strcspn((char *)data, "\r\n");

				input_insert(in, (char *)data, len);
			}
			XFree(data);
			return RUI_EV_REDRAW | RUI_EV_CHANGED;
		}
		return 0;
	}
	if (ev->type != KeyPress)
		return 0;

	if (in->xic) {
		Status st;

		n = Xutf8LookupString(in->xic, &ev->xkey, buf, sizeof buf - 1, &ks,
		    &st);
		if (st == XBufferOverflow)
			n = 0;
		if (st != XLookupKeySym && st != XLookupBoth)
			ks = NoSymbol;
		if (st != XLookupChars && st != XLookupBoth)
			n = 0;
	} else {
		char latin[32];
		int i, ln = XLookupString(&ev->xkey, latin, sizeof latin, &ks, NULL);

		n = 0;
		for (i = 0; i < ln; i++) {
			unsigned char c = (unsigned char)latin[i];

			if (c < 0x80) {
				buf[n++] = (char)c;
			} else {
				buf[n++] = (char)(0xc0 | (c >> 6));
				buf[n++] = (char)(0x80 | (c & 0x3f));
			}
		}
	}
	buf[n] = '\0';
	ctrl = (ev->xkey.state & ControlMask) != 0;

	if (ctrl) {
		switch (ks) {
		case XK_a: ks = XK_Home; break;
		case XK_e: ks = XK_End; break;
		case XK_b: ks = XK_Left; break;
		case XK_f: ks = XK_Right; break;
		case XK_h: ks = XK_BackSpace; break;
		case XK_d: ks = XK_Delete; break;
		case XK_u:
			input_delete(in, 0, in->cursor);
			return RUI_EV_REDRAW | RUI_EV_CHANGED;
		case XK_k:
			input_delete(in, in->cursor, in->len);
			return RUI_EV_REDRAW | RUI_EV_CHANGED;
		case XK_w: {
			int i = in->cursor;

			while (i > 0 && in->text[i - 1] == ' ')
				i--;
			while (i > 0 && in->text[i - 1] != ' ')
				i--;
			input_delete(in, i, in->cursor);
			return RUI_EV_REDRAW | RUI_EV_CHANGED;
		}
		case XK_v:
		case XK_y:
			input_paste_request(app, in, ev->xkey.time);
			return 0;
		default:
			return 0;
		}
	}
	switch (ks) {
	case XK_Return:
	case XK_KP_Enter:
		return RUI_EV_ACTIVATE;
	case XK_Escape:
		return RUI_EV_CANCEL;
	case XK_Left:
	case XK_KP_Left:
		in->cursor = utf8_prev(in->text, in->cursor);
		return RUI_EV_REDRAW;
	case XK_Right:
	case XK_KP_Right:
		in->cursor = utf8_next(in->text, in->len, in->cursor);
		return RUI_EV_REDRAW;
	case XK_Home:
	case XK_KP_Home:
		in->cursor = 0;
		return RUI_EV_REDRAW;
	case XK_End:
	case XK_KP_End:
		in->cursor = in->len;
		return RUI_EV_REDRAW;
	case XK_BackSpace:
		input_delete(in, utf8_prev(in->text, in->cursor), in->cursor);
		return RUI_EV_REDRAW | RUI_EV_CHANGED;
	case XK_Delete:
	case XK_KP_Delete:
		input_delete(in, in->cursor, utf8_next(in->text, in->len, in->cursor));
		return RUI_EV_REDRAW | RUI_EV_CHANGED;
	default:
		break;
	}
	if (n > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
		input_insert(in, buf, n);
		return RUI_EV_REDRAW | RUI_EV_CHANGED;
	}
	return 0;
}

/* ------------------------------------------------------------------- list */

#define LIST_SB_W 16

static int
list_row_h(RuiApp *a)
{
	return a->font->height + 2;
}

static int
list_rows(RuiApp *a, const RuiList *l)
{
	int rows = (l->r.h - 4) / list_row_h(a);

	return rows < 1 ? 1 : rows;
}

static void
list_sync(RuiApp *a, RuiList *l)
{
	int rows = list_rows(a, l), max = l->n > rows ? l->n - rows : 0;

	if (l->sel >= l->n)
		l->sel = l->n - 1;
	if (l->top > max)
		l->top = max;
	if (l->top < 0)
		l->top = 0;
	l->sb.r.x = l->r.x + l->r.w - 2 - LIST_SB_W;
	l->sb.r.y = l->r.y + 2;
	l->sb.r.w = LIST_SB_W;
	l->sb.r.h = l->r.h - 4;
	l->sb.total = l->n;
	l->sb.page = rows;
	l->sb.pos = l->top;
}

static void
list_show_sel(RuiApp *a, RuiList *l)
{
	int rows = list_rows(a, l);

	if (l->sel < 0)
		return;
	if (l->sel < l->top)
		l->top = l->sel;
	if (l->sel >= l->top + rows)
		l->top = l->sel - rows + 1;
}

void
rui_list_draw(RuiSurf *s, RuiList *l)
{
	RuiApp *a = s->app;
	int rh = list_row_h(a), rows, i, has_sb;

	list_sync(a, l);
	rows = list_rows(a, l);
	has_sb = l->n > rows;
	rui_fill(s, l->r.x, l->r.y, l->r.w, l->r.h, RUI_C_FIELD);
	rui_bevel(s, l->r.x, l->r.y, l->r.w, l->r.h, RUI_SUNKEN);
	for (i = 0; i < rows && l->top + i < l->n; i++) {
		int idx = l->top + i;
		RuiRect r;

		r.x = l->r.x + 2;
		r.y = l->r.y + 2 + i * rh;
		r.w = l->r.w - 4 - (has_sb ? LIST_SB_W : 0);
		r.h = rh;
		if (idx == l->sel)
			rui_fill(s, r.x, r.y, r.w, r.h, RUI_C_SEL_BG);
		r.x += 3;
		r.w -= 6;
		rui_text_fit(s, a->font, r, l->items[idx],
		    idx == l->sel ? RUI_C_SEL_FG : RUI_C_TEXT, RUI_LEFT);
	}
	if (has_sb)
		rui_scrollbar_draw(s, &l->sb);
}

int
rui_list_event(RuiApp *app, RuiList *l, const XEvent *ev)
{
	int rows, has_sb, x, y;

	list_sync(app, l);
	rows = list_rows(app, l);
	has_sb = l->n > rows;
	if (!ev_xy(ev, &x, &y))
		return 0;
	if (has_sb && (l->sb.drag || (rui_in_rect(l->sb.r, x, y) &&
	    ev->type == ButtonPress))) {
		int r = rui_scrollbar_event(&l->sb, ev);

		l->top = l->sb.pos;
		return r ? RUI_EV_REDRAW : 0;
	}
	if (ev->type != ButtonPress || !rui_in_rect(l->r, x, y))
		return 0;
	if (ev->xbutton.button == Button4 || ev->xbutton.button == Button5) {
		l->top += ev->xbutton.button == Button4 ? -3 : 3;
		list_sync(app, l);
		return RUI_EV_REDRAW;
	}
	if (ev->xbutton.button == Button1) {
		int idx = l->top + (y - l->r.y - 2) / list_row_h(app);
		long long now = rui_now_ms();
		int res = RUI_EV_REDRAW;

		if (idx < 0 || idx >= l->n)
			return 0;
		if (idx != l->sel)
			res |= RUI_EV_CHANGED;
		l->sel = idx;
		if (idx == l->last_idx && now - l->last_click < 400)
			res |= RUI_EV_ACTIVATE;
		l->last_idx = idx;
		l->last_click = now;
		return res;
	}
	return 0;
}

int
rui_list_key(RuiApp *app, RuiList *l, KeySym ks)
{
	int rows = list_rows(app, l), old = l->sel;

	if (l->n <= 0)
		return 0;
	switch (ks) {
	case XK_Up:
	case XK_KP_Up:
		l->sel = l->sel > 0 ? l->sel - 1 : 0;
		break;
	case XK_Down:
	case XK_KP_Down:
		l->sel = l->sel < l->n - 1 ? l->sel + 1 : l->n - 1;
		break;
	case XK_Prior:
		l->sel = l->sel > rows ? l->sel - rows : 0;
		break;
	case XK_Next:
		l->sel = l->sel + rows < l->n ? l->sel + rows : l->n - 1;
		break;
	case XK_Home:
		l->sel = 0;
		break;
	case XK_End:
		l->sel = l->n - 1;
		break;
	default:
		return 0;
	}
	list_show_sel(app, l);
	list_sync(app, l);
	return l->sel != old ? (RUI_EV_REDRAW | RUI_EV_CHANGED) : RUI_EV_REDRAW;
}
