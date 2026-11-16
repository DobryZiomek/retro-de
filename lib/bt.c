/*
 * libretroui Bluetooth engine: picks a Bluetooth module from config.h and
 * offers NULL-safe front ends.  Like the network modules, a module only
 * builds command lines and parses the tool's output; callers run the
 * commands as async jobs.
 */
#include "retroui.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

const RuiBtModule *
rui_bt_detect(const char *forced)
{
	const RuiBtModule *const *mp;
	int any = !forced || !*forced || !strcmp(forced, "auto");

	if (forced && !strcmp(forced, "none"))
		return NULL;
	for (mp = rui_config.bt; *mp; mp++) {
		const RuiBtModule *m = *mp;

		if (!any && strcmp(forced, m->name) && strcmp(forced, m->program))
			continue;
		if (rui_have_cmd(m->program))
			return m;
	}
	return NULL;
}

int
rui_bt_parse_status(const RuiBtModule *m, const char *out, RuiBtStatus *st)
{
	memset(st, 0, sizeof *st);
	return m ? m->parse_status(out ? out : "", st) : 0;
}

static int
cmp_device(const void *a, const void *b)
{
	const RuiBtDevice *x = a, *y = b;

	if (x->connected != y->connected)
		return y->connected - x->connected;
	if (x->paired != y->paired)
		return y->paired - x->paired;
	return strcasecmp(x->name, y->name);
}

int
rui_bt_parse_devices(const RuiBtModule *m, const char *out, RuiBtDevice *list,
    int max)
{
	int n = m ? m->parse_devices(out ? out : "", list, max) : 0;

	if (n > 1)
		qsort(list, (size_t)n, sizeof *list, cmp_device);
	return n;
}
