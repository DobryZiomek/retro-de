/* power module: elogind (OpenRC, runit, s6 ... with a logind) via loginctl */
#include "../lib/retroui.h"

#include <unistd.h>

static int
usable(void)
{
	if (access("/run/systemd/system", F_OK) == 0)
		return 0;                         /* that is the systemd module */
	return access("/run/elogind.pid", F_OK) == 0 ||
	    access("/run/systemd/seats", F_OK) == 0;
}

const RuiPowerModule rui_power_elogind = {
	"elogind", "loginctl", usable,
	{ "loginctl poweroff", "loginctl reboot", "loginctl suspend",
	  "loginctl hibernate" }, 0
};
