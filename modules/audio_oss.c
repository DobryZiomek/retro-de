/* audio module: FreeBSD OSS mixer(8), new (14.x) and old syntax */
#include "../lib/retroui.h"

#include <stdio.h>
#include <string.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "mixer vol.volume=%d.%02d >/dev/null 2>&1 || "
	    "mixer vol %d >/dev/null 2>&1", pct / 100, pct % 100, pct);
}

/* new: "vol.volume=0.75:0.75\nvol.mute=off"; old: "vol 75:75" */
static int
parse(const char *out, int *vol, int *muted)
{
	const char *eq = strstr(out, "volume="), *mu = strstr(out, "mute=");
	double v;

	if (eq) {
		if (!rui_first_number(eq, &v))
			return 0;
		*vol = (int)(v * 100.0 + 0.5);
	} else {
		if (!rui_first_number(out, &v))
			return 0;
		*vol = (int)(v + 0.5);
	}
	if (mu)
		*muted = mu[5] == '1' || !strncmp(mu + 5, "on", 2);
	return 1;
}

const RuiAudioModule rui_audio_oss = {
	"oss", "mixer",
	"mixer -o vol.volume vol.mute 2>/dev/null || mixer -s vol",
	"mixer vol.mute=toggle >/dev/null 2>&1 || mixer vol.mute=^ >/dev/null 2>&1",
	set_cmd, parse
};
