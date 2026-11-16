/* Hotdog - red, yellow and black; loud on purpose. */
#include "retroui.h"

const RuiThemeModule retro_theme = {
	RUI_THEME_ABI,
	{ "Hotdog", RUI_STYLE_CLASSIC, {
	  "#e00000", "#ff7070", "#800000", "#000000", "#ffffff", "#ffb0b0",
	  "#900000", "#ffff00", "#000000", "#000000", "#800000", "#ffff00",
	  "#ffb0b0", "#ffe000", "#000000", "#ffff00", "#ffffff" },
	  "solid", NULL },
	NULL,
	NULL,
	"retro-de", "Red and yellow"
};
