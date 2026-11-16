/*
 * The only translation unit that includes config.h; everything else reads
 * the settings through rui_config.  Modules named here are the only ones
 * pulled out of libretroui.a by the linker.
 */
#include "retroui.h"
#include "../config.h"

const RuiConfig rui_config = {
	DEFAULT_THEME,
	DEFAULT_STYLE,
	DEFAULT_FONT,
	audio_modules,
	net_modules,
	battery_modules,
	power_modules,
	bar_applets,
	blocks,
	WORKSPACES,
	WORKSPACE_NAMES,
	layouts,
	(int)(sizeof layouts / sizeof layouts[0]),
	keys,
	terminals,
	{ DEFAULT_TERMINAL, DEFAULT_BROWSER, DEFAULT_FILEMANAGER, DEFAULT_EDITOR,
	  DEFAULT_LOCKER, DEFAULT_CLOCK },
	DBUS_MODE,
	DESKTOP_ENTRIES,
	XDG_AUTOSTART,
	desktop_name,
	fp_left,
	fp_right,
	bt_modules,
};
