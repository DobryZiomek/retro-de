/* audio module: PipeWire via wpctl (WirePlumber) */
#include "../lib/retroui.h"

#include <stdio.h>
#include <string.h>

static void
set_cmd(int pct, char *buf, size_t n)
{
	snprintf(buf, n, "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d.%02d",
	    pct / 100, pct % 100);
}

/* "Volume: 0.40 [MUTED]" */
static int
parse(const char *out, int *vol, int *muted)
{
	double v;

	if (strncmp(out, "Volume:", 7) || !rui_first_number(out, &v))
		return 0;
	*vol = (int)(v * 100.0 + 0.5);
	*muted = strstr(out, "MUTED") != NULL;
	return 1;
}

const RuiAudioModule rui_audio_pipewire = {
	"pipewire", "wpctl",
	"wpctl get-volume @DEFAULT_AUDIO_SINK@",
	"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle",
	set_cmd, parse,
	/* pipewire-pulse reports changes; without pactl the panel polls */
	"command -v pactl >/dev/null && exec pactl subscribe"
};
