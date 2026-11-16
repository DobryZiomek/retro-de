/* battery module: Linux /sys/class/power_supply */
#include "../lib/retroui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>

static int
read_line(const char *dir, const char *file, char *buf, size_t n)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", dir, file);
	if (!(f = fopen(path, "r")))
		return 0;
	if (!fgets(buf, (int)n, f))
		buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return buf[0] != '\0';
}

static int
sysfs_read(int *percent, int *charging)
{
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		char buf[64];

		if (de->d_name[0] == '.')
			continue;
		if (!read_line(de->d_name, "type", buf, sizeof buf) ||
		    strcmp(buf, "Battery") ||
		    !read_line(de->d_name, "capacity", buf, sizeof buf))
			continue;
		*percent = atoi(buf);
		*charging = 0;
		if (read_line(de->d_name, "status", buf, sizeof buf))
			*charging = !strcmp(buf, "Charging") || !strcmp(buf, "Full");
		found = 1;
	}
	closedir(d);
	return found;
}
#else
/* sysfs exists only on Linux: elsewhere this module never finds a battery */
static int
sysfs_read(int *percent, int *charging)
{
	(void)percent;
	(void)charging;
	return 0;
}
#endif

const RuiBatteryModule rui_battery_sysfs = {
	"sysfs", sysfs_read, NULL, NULL, NULL
};
