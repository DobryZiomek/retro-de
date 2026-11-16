/* power module: FreeBSD / DragonFly shutdown(8) and acpiconf(8) */
#include "../lib/retroui.h"

static int
usable(void)
{
#if defined(__FreeBSD__) || defined(__DragonFly__)
	return 1;
#else
	return 0;
#endif
}

const RuiPowerModule rui_power_freebsd = {
	"freebsd", "shutdown", usable,
	{ "shutdown -p now", "shutdown -r now", "acpiconf -s 3",
	  "acpiconf -s 4" }, 1
};
