/* battery module: apm(8) on OpenBSD and NetBSD */
#include "../lib/retroui.h"

#include <stdio.h>

/* "apm -l; apm -a; apm -b": the percentage, the AC state (1 = connected)
 * and the battery state, where 4 means there is none - OpenBSD then still
 * reports 0% */
static int
parse(const char *out, int *percent, int *charging)
{
	int pct = -1, ac = 0, state = -1;

	if (!out || sscanf(out, "%d %d %d", &pct, &ac, &state) < 1)
		return 0;
	if (pct < 0 || pct > 100 || state == 4)
		return 0;                       /* 255 / -1 / absent: no battery */
	*percent = pct;
	*charging = ac == 1;
	return 1;
}

const RuiBatteryModule rui_battery_apm = {
	"apm", NULL, "apm",
	"apm -l 2>/dev/null; apm -a 2>/dev/null; apm -b 2>/dev/null", parse
};
