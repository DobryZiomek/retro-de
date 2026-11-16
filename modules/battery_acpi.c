/* battery module: FreeBSD / DragonFly hw.acpi.battery sysctls */
#include "../lib/retroui.h"

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>

static int
acpi_read(int *percent, int *charging)
{
	int life = -1, state = 0;
	size_t sz = sizeof life;

	if (sysctlbyname("hw.acpi.battery.life", &life, &sz, NULL, 0) < 0 ||
	    life < 0 || life > 100)
		return 0;
	sz = sizeof state;
	if (sysctlbyname("hw.acpi.battery.state", &state, &sz, NULL, 0) < 0)
		state = 0;
	*percent = life;
	*charging = (state & 1) == 0;             /* bit 0: discharging */
	return 1;
}
#else
/* these sysctls exist only on FreeBSD-derived systems */
static int
acpi_read(int *percent, int *charging)
{
	(void)percent;
	(void)charging;
	return 0;
}
#endif

const RuiBatteryModule rui_battery_acpi = {
	"acpi", acpi_read, NULL, NULL, NULL
};
