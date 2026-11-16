/* Lilac - soft purple CDE colours.  Its bevel hook rounds things off a
 * little: raised and sunken edges lose their corner pixels, which reads as
 * a softer, later-Motif look while everything else stays stock. */
#include "retroui.h"

static int
bevel(struct RuiSurf *s, int x, int y, int w, int h, int style)
{
	int raised = style == RUI_RAISED || style == RUI_RAISED_THIN;
	int tl, br;

	if (style == RUI_GROOVE || w < 6 || h < 6)
		return 0;                          /* stock drawing */
	tl = raised ? RUI_C_LIGHT : RUI_C_SHADOW;
	br = raised ? RUI_C_SHADOW : RUI_C_LIGHT;
	rui_fill(s, x + 1, y, w - 2, 1, tl);
	rui_fill(s, x, y + 1, 1, h - 2, tl);
	rui_fill(s, x + 1, y + h - 1, w - 2, 1, br);
	rui_fill(s, x + w - 1, y + 1, 1, h - 2, br);
	if (style == RUI_RAISED || style == RUI_SUNKEN || style == RUI_PRESSED) {
		rui_fill(s, x + 1, y + 1, w - 2, 1, tl);
		rui_fill(s, x + 1, y + 1, 1, h - 2, tl);
		rui_fill(s, x + 1, y + h - 2, w - 2, 1, br);
		rui_fill(s, x + w - 2, y + 1, 1, h - 2, br);
	}
	return 1;
}

const RuiThemeModule retro_theme = {
	RUI_THEME_ABI,
	{ "Lilac", RUI_STYLE_CDE, {
	  "#b9aecb", "#e6e0ef", "#6b5f82", "#2e2740", "#000000", "#857a9c",
	  "#d0c7df", "#9485b3", "#000000", "#7a5aa6", "#b9aecb", "#ffffff",
	  "#000000", "#5e5278", "#ffffff", "#b02040", "#2e8b57" },
	  "weave", NULL },
	"Dusk",
	bevel,
	"retro-de", "Soft purple with rounded bevels (uses the bevel hook)"
};
