/* audio module: ALSA via amixer */
#include "../lib/retroui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "amixer -q sset Master %d%%", pct);
}

/* "  Mono: Playback 87 [69%] [-18.00dB] [on]" */
static int
parse(const char *out, int *vol, int *muted)
{
	const char *br = out;

	while ((br = strchr(br, '['))) {
		if (isdigit((unsigned char)br[1])) {
			char *end;
			long n = strtol(br + 1, &end, 10);

			if (*end == '%') {
				*vol = (int)n;
				*muted = strstr(out, "[off]") != NULL;
				return 1;
			}
		}
		br++;
	}
	return 0;
}

const RuiAudioModule rui_audio_alsa = {
	"alsa", "amixer",
	"amixer sget Master",
	"amixer -q sset Master toggle",
	set_cmd, parse,
	"command -v alsactl >/dev/null && exec alsactl monitor"
};
