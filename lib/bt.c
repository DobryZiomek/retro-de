/*
 * libretroui Bluetooth engine: picks a Bluetooth module from config.h and
 * offers NULL-safe front ends.  Like the network modules, a module only
 * builds command lines and parses the tool's output; callers run the
 * commands as async jobs.
 */
#include "retroui.h"

#include <dirent.h>
#include <stdio.h>
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

/* Linux: a summary of /sys/class/bluetooth (controllers and, as hciN:M,
 * open connections) and the Bluetooth rfkill switches.  Unchanged means
 * the panel need not ask bluetoothctl again.  0 where there is no sysfs. */
unsigned long
rui_bt_fingerprint(void)
{
	unsigned long h = 5381;
	struct dirent *de;
	DIR *d = opendir("/sys/class/bluetooth");
	const char *p;

	if (!d)
		return 0;
	while ((de = readdir(d)))
		for (p = de->d_name; *p; p++)
			h = h * 33 + (unsigned char)*p;
	closedir(d);
	if ((d = opendir("/sys/class/rfkill"))) {
		while ((de = readdir(d))) {
			char path[300], buf[32] = "";
			FILE *f;

			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof path, "/sys/class/rfkill/%s/type",
			    de->d_name);
			if (!(f = fopen(path, "r")))
				continue;
			if (!fgets(buf, sizeof buf, f))
				buf[0] = '\0';
			fclose(f);
			if (strncmp(buf, "bluetooth", 9))
				continue;
			snprintf(path, sizeof path, "/sys/class/rfkill/%s/state",
			    de->d_name);
			if ((f = fopen(path, "r"))) {
				int c = fgetc(f);

				h = h * 33 + (unsigned long)(c == EOF ? 0 : c);
				fclose(f);
			}
		}
		closedir(d);
	}
	return h ? h : 1;
}
