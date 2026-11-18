/* audio module: sndio (OpenBSD, also available on Linux/FreeBSD) via sndioctl */
#include "../lib/retroui.h"

#include <stdio.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "sndioctl -q output.level=%d.%02d", pct / 100, pct % 100);
}

/* "0.500\n0\n" */
static int
parse(const char *out, int *vol, int *muted)
{
	double v, m = 0;
	const char *p = rui_first_number(out, &v);

	if (!p)
		return 0;
	if (rui_first_number(p, &m))
		*muted = m >= 0.5;
	*vol = (int)(v * 100.0 + 0.5);
	return 1;
}

const RuiAudioModule rui_audio_sndio = {
	"sndio", "sndioctl",
	"sndioctl -n output.level && sndioctl -n output.mute",
	"sndioctl -q output.mute=!",
	set_cmd, parse,
	"exec sndioctl -m"
};
