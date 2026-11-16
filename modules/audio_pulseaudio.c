/* audio module: PulseAudio (or pipewire-pulse) via pactl */
#include "../lib/retroui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "pactl set-sink-volume @DEFAULT_SINK@ %d%%", pct);
}

/* "Volume: front-left: 32768 /  50% / -18.06 dB, ...\nMute: no" */
static int
parse(const char *out, int *vol, int *muted)
{
	const char *pct = strchr(out, '%'), *p;

	if (!pct)
		return 0;
	p = pct;
	while (p > out && isdigit((unsigned char)p[-1]))
		p--;
	if (p == pct)
		return 0;
	*vol = atoi(p);
	*muted = strstr(out, "Mute: yes") != NULL;
	return 1;
}

const RuiAudioModule rui_audio_pulseaudio = {
	"pulseaudio", "pactl",
	"pactl get-sink-volume @DEFAULT_SINK@ && pactl get-sink-mute @DEFAULT_SINK@",
	"pactl set-sink-mute @DEFAULT_SINK@ toggle",
	set_cmd, parse
};
