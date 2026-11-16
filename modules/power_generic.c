/* power module: plain init systems (sysvinit, runit, s6, OpenRC, dinit):
 * the poweroff/reboot commands they all provide and zzz where it exists */
#include "../lib/retroui.h"

#include <unistd.h>

static int
usable(void)
{
	return access("/run/systemd/system", F_OK) != 0;
}

const RuiPowerModule rui_power_generic = {
	"generic", "reboot", usable,
	{ "poweroff", "reboot", "zzz", "zzz -Z" }, 1
};
