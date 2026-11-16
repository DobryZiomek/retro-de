/* power module: systemd as PID 1 (systemctl) */
#include "../lib/retroui.h"

#include <unistd.h>

static int
usable(void)
{
	return access("/run/systemd/system", F_OK) == 0;
}

const RuiPowerModule rui_power_systemd = {
	"systemd", "systemctl", usable,
	{ "systemctl poweroff", "systemctl reboot", "systemctl suspend",
	  "systemctl hibernate" }, 0
};
