/* audio module: NetBSD audio(4) via mixerctl */
#include "../lib/retroui.h"

#include <stdio.h>
#include <string.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "mixerctl -q -w outputs.master=%d", pct * 255 / 100);
}

/* "200,200\noff" (0..255) */
static int
parse(const char *out, int *vol, int *muted)
{
	double v;

	if (!rui_first_number(out, &v))
		return 0;
	*vol = (int)(v * 100.0 / 255.0 + 0.5);
	*muted = strstr(out, "on") != NULL;
	return 1;
}

const RuiAudioModule rui_audio_mixerctl = {
	"mixerctl", "mixerctl",
	"mixerctl -n outputs.master && "
	"(mixerctl -n outputs.master.mute 2>/dev/null || true)",
	"if [ \"$(mixerctl -n outputs.master.mute 2>/dev/null)\" = on ]; "
	"then mixerctl -q -w outputs.master.mute=off; "
	"else mixerctl -q -w outputs.master.mute=on; fi",
	set_cmd, parse,
	NULL                               /* no change events: polled */
};
