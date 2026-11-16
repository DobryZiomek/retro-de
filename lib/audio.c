/*
 * libretroui audio engine: volume get/set/mute through the audio modules
 * listed in config.h.  Candidates whose tool is in $PATH are probed in order;
 * the first one whose "get" command succeeds and parses becomes the backend.
 * Every command runs through an asynchronous pipe so the UI never blocks.
 */
#include "retroui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *
rui_first_number(const char *s, double *val)
{
	char *end;

	if (!s)
		return NULL;
	while (*s && !isdigit((unsigned char)*s))
		s++;
	if (!*s)
		return NULL;
	*val = strtod(s, &end);
	return end;
}

static int
wanted(const RuiAudio *a, const RuiAudioModule *m)
{
	return !a->forced[0] || !strcmp(a->forced, m->name) ||
	    !strcmp(a->forced, m->program);
}

static int
parse(const RuiAudioModule *m, const char *out, int *vol, int *muted)
{
	*vol = 0;
	*muted = 0;
	if (!out || !*out || !m->parse(out, vol, muted))
		return 0;
	if (*vol < 0)
		*vol = 0;
	if (*vol > 150)
		*vol = 150;
	return 1;
}

static void probe_next(RuiAudio *a);

static void
on_get(const char *out, int status, void *ud)
{
	RuiAudio *a = ud;
	int vol, muted;

	a->busy = 0;
	if (!a->mod) {
		/* still detecting: a->probing is the candidate that just ran */
		const RuiAudioModule *m = rui_config.audio[a->probing];

		if (!m)
			return;
		if (status != 0 || !parse(m, out, &vol, &muted)) {
			a->probing++;
			probe_next(a);
			return;
		}
		a->mod = m;
	} else {
		if (status != 0 || !parse(a->mod, out, &vol, &muted))
			return;
		if (rui_now_ms() - a->last_set < 700)
			return;                   /* stale: a set is still in flight */
	}
	if (!a->valid || vol != a->volume || muted != a->muted) {
		a->valid = 1;
		a->volume = vol;
		a->muted = muted;
		if (a->changed)
			a->changed(a->ud);
	}
}

static void
probe_next(RuiAudio *a)
{
	const RuiAudioModule *m;

	while ((m = rui_config.audio[a->probing])) {
		if (wanted(a, m) && rui_have_cmd(m->program)) {
			if (rui_job_start(m->get_cmd, on_get, a) == 0)
				a->busy = 1;
			return;                   /* (retried on refresh if no slot) */
		}
		a->probing++;
	}
	if (a->changed)                       /* nothing usable: report once */
		a->changed(a->ud);
}

void
rui_audio_init(RuiAudio *a, const char *forced_module,
    void (*changed)(void *), void *ud)
{
	memset(a, 0, sizeof *a);
	a->changed = changed;
	a->ud = ud;
	if (forced_module && *forced_module && strcmp(forced_module, "auto"))
		rui_strlcpy(a->forced, forced_module, sizeof a->forced);
	probe_next(a);
}

void
rui_audio_refresh(RuiAudio *a)
{
	if (a->busy)
		return;
	if (!a->mod) {
		if (rui_config.audio[a->probing])
			probe_next(a);
		return;
	}
	if (rui_job_start(a->mod->get_cmd, on_get, a) == 0)
		a->busy = 1;
}

static void
on_set_done(const char *out, int status, void *ud)
{
	(void)out;
	(void)status;
	(void)ud;
}

void
rui_audio_set(RuiAudio *a, int percent)
{
	char cmd[256];

	if (!a->mod)
		return;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	cmd[0] = '\0';
	a->mod->set_cmd(percent, cmd, sizeof cmd);
	if (!cmd[0] || rui_job_start(cmd, on_set_done, NULL) < 0)
		return;
	a->volume = percent;
	a->valid = 1;
	a->last_set = rui_now_ms();
	if (a->changed)
		a->changed(a->ud);
}

void
rui_audio_toggle_mute(RuiAudio *a)
{
	if (!a->mod || rui_job_start(a->mod->mute_cmd, on_set_done, NULL) < 0)
		return;
	a->muted = !a->muted;
	a->last_set = rui_now_ms();
	if (a->changed)
		a->changed(a->ud);
}

const char *
rui_audio_backend_name(const RuiAudio *a)
{
	return a->mod ? a->mod->name : "none";
}
