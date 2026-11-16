/* power module: ConsoleKit2 through the D-Bus command-line client */
#include "../lib/retroui.h"

#include <unistd.h>

static int
usable(void)
{
	return access("/run/ConsoleKit", F_OK) == 0 ||
	    access("/var/run/ConsoleKit", F_OK) == 0;
}

#define CK "dbus-send --system --print-reply " \
	"--dest=org.freedesktop.ConsoleKit /org/freedesktop/ConsoleKit/Manager " \
	"org.freedesktop.ConsoleKit.Manager."

const RuiPowerModule rui_power_consolekit = {
	"consolekit", "dbus-send", usable,
	{ CK "PowerOff boolean:true", CK "Reboot boolean:true",
	  CK "Suspend boolean:true", CK "Hibernate boolean:true" }, 0
};
