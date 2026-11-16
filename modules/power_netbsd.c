/* power module: NetBSD shutdown(8) and ACPI sleep through sysctl(8) */
#include "../lib/retroui.h"

static int
usable(void)
{
#if defined(__NetBSD__)
	return 1;
#else
	return 0;
#endif
}

const RuiPowerModule rui_power_netbsd = {
	"netbsd", "shutdown", usable,
	{ "shutdown -p now", "shutdown -r now",
	  "sysctl -w hw.acpi.sleep.state=3", NULL }, 1
};
