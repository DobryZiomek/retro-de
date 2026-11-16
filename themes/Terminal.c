/* Terminal - green phosphor on black.  Shows what code buys you: the bevel
 * hook replaces every 3-D edge with a flat one-pixel outline, so buttons,
 * fields and window frames look like a text-mode interface. */
#include "retroui.h"

static int
bevel(struct RuiSurf *s, int x, int y, int w, int h, int style)
{
	/* raised things get a bright outline, sunken ones a dim one */
	int c = style == RUI_PRESSED || style == RUI_SUNKEN ||
	    style == RUI_SUNKEN_THIN ? RUI_C_LIGHT : RUI_C_SHADOW;

	if (w < 2 || h < 2)
		return 1;
	rui_fill(s, x, y, w, 1, c);
	rui_fill(s, x, y + h - 1, w, 1, c);
	rui_fill(s, x, y, 1, h, c);
	rui_fill(s, x + w - 1, y, 1, h, c);
	if (style == RUI_PRESSED && w > 4 && h > 4) {  /* pressed: double line */
		rui_fill(s, x + 2, y + 2, w - 4, 1, RUI_C_SHADOW);
		rui_fill(s, x + 2, y + h - 3, w - 4, 1, RUI_C_SHADOW);
		rui_fill(s, x + 2, y + 2, 1, h - 4, RUI_C_SHADOW);
		rui_fill(s, x + w - 3, y + 2, 1, h - 4, RUI_C_SHADOW);
	}
	return 1;
}

const RuiThemeModule retro_theme = {
	RUI_THEME_ABI,
	{ "Terminal", RUI_STYLE_CLASSIC, {
	  "#000000", "#0f5a26", "#1a9940", "#0d4d20", "#33ff66", "#1a7030",
	  "#001a08", "#33ff66", "#000000", "#33ff66", "#0d4d20", "#000000",
	  "#33ff66", "#000000", "#33ff66", "#ffb000", "#33ff66" },
	  "solid", "monospace-10" },
	NULL,
	bevel,
	"retro-de", "Green phosphor with flat outlines (uses the bevel hook)"
};
