/* power module: OpenBSD shutdown(8) and apm zzz/ZZZ */
#include "../lib/retroui.h"

static int
usable(void)
{
#if defined(__OpenBSD__)
	return 1;
#else
	return 0;
#endif
}

const RuiPowerModule rui_power_openbsd = {
	"openbsd", "shutdown", usable,
	{ "shutdown -p now", "shutdown -r now", "zzz", "ZZZ" }, 1
};
