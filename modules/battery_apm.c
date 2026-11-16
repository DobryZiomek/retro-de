/* battery module: apm(8) on OpenBSD and NetBSD */
#include "../lib/retroui.h"

#include <stdio.h>

/* "apm -l; apm -a": a percentage line, then the AC state (1 = connected) */
static int
parse(const char *out, int *percent, int *charging)
{
	int pct = -1, ac = 0;

	if (!out || sscanf(out, "%d %d", &pct, &ac) < 1)
		return 0;
	if (pct < 0 || pct > 100)
		return 0;                             /* 255 / -1: no battery */
	*percent = pct;
	*charging = ac == 1;
	return 1;
}

const RuiBatteryModule rui_battery_apm = {
	"apm", NULL, "apm", "apm -l 2>/dev/null; apm -a 2>/dev/null", parse
};
