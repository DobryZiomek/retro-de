/*
 * libretroui power engine: picks the power module (config.h order) that fits
 * the running system - systemd, elogind, ConsoleKit, the BSDs or a plain
 * init such as sysvinit, runit, s6, OpenRC or dinit.
 */
#include "retroui.h"

#include <string.h>

const char *const rui_power_action_names[RUI_POWER_ACTIONS] = {
	"poweroff", "reboot", "suspend", "hibernate"
};

const RuiPowerModule *
rui_power_detect(const char *forced)
{
	const RuiPowerModule *const *mp;
	int any = !forced || !*forced || !strcmp(forced, "auto");

	if (forced && !strcmp(forced, "none"))
		return NULL;
	for (mp = rui_config.power; *mp; mp++) {
		const RuiPowerModule *m = *mp;

		if (!any && strcmp(forced, m->name))
			continue;
		if (!rui_have_cmd(m->program))
			continue;
		if (any && m->usable && !m->usable())
			continue;                  /* a forced module skips the test */
		return m;
	}
	return NULL;
}

char *
rui_power_command(const RuiPowerModule *m, int action, const char *priv)
{
	char prog[128];
	const char *cmd;

	if (!m || action < 0 || action >= RUI_POWER_ACTIONS ||
	    !(cmd = m->cmd[action]))
		return NULL;
	rui_strlcpy(prog, cmd, sizeof prog);
	prog[strcspn(prog, " \t")] = '\0';
	if (!rui_have_cmd(prog))
		return NULL;                       /* e.g. no zzz installed */
	if (m->needs_priv) {
		if (!priv || !strcmp(priv, "auto"))
			priv = rui_have_cmd("doas") ? "doas -n" :
			    rui_have_cmd("sudo") ? "sudo -n" : "";
		if (*priv)
			return rui_strdupf("%s %s", priv, cmd);
	}
	return rui_strdup(cmd);
}
