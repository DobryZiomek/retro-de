/* Redmond - the late-nineties office desktop: grey faces, navy titles,
 * teal backdrop, bitmap-crisp bevels.  A plain palette module: the
 * smallest useful theme. */
#include "retroui.h"

const RuiThemeModule retro_theme = {
	RUI_THEME_ABI,
	{ "Redmond", RUI_STYLE_CLASSIC, {
	  "#c0c0c0", "#ffffff", "#808080", "#000000", "#000000", "#808080",
	  "#ffffff", "#000080", "#ffffff", "#000080", "#808080", "#ffffff",
	  "#c0c0c0", "#008080", "#ffffff", "#c00000", "#008000" },
	  "solid", NULL },
	"Clouds",
	NULL,
	"retro-de", "Grey, navy and teal with the Clouds wallpaper"
};
