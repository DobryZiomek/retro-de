/* network module: wpa_supplicant via wpa_cli (FreeBSD, NetBSD, Linux) */
#include "../lib/retroui.h"

#include <stdlib.h>
#include <string.h>

static char *
status_cmd(const char *iface)
{
	(void)iface;
	return rui_strdup("wpa_cli status 2>/dev/null");
}

static int
parse_status(const char *out, RuiNetStatus *st)
{
	char line[512];
	const char *p = out;
	int seen = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		rui_rtrim(line);
		if (!strncmp(line, "ssid=", 5)) {
			rui_strlcpy(st->ssid, line + 5, sizeof st->ssid);
		} else if (!strncmp(line, "wpa_state=", 10)) {
			st->connected = !strcmp(line + 10, "COMPLETED");
			seen = 1;
		}
	}
	if (!st->connected)
		st->ssid[0] = '\0';
	return seen;
}

static char *
scan_cmd(const char *iface)
{
	(void)iface;
	return rui_strdup("wpa_cli scan >/dev/null 2>&1; sleep 3; "
	    "wpa_cli scan_results 2>/dev/null");
}

/* bssid \t frequency \t signal \t flags \t ssid */
static int
parse_scan(const char *out, RuiNetwork *list, int max)
{
	char line[512];
	const char *p = out;
	int n = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char *f[5], *s = line;
		int i, pct;
		long dbm;

		for (i = 0; i < 5; i++) {
			f[i] = s;
			if (i < 4) {
				s = strchr(s, '\t');
				if (!s)
					break;
				*s++ = '\0';
			}
		}
		if (i < 5 || !strchr(f[0], ':'))
			continue;
		/* dBm (negative) or already a percentage; clamp before the
		 * arithmetic so a nonsense value cannot overflow */
		dbm = strtol(f[2], NULL, 10);
		if (dbm < -100)
			dbm = -100;
		if (dbm > 100)
			dbm = 100;
		pct = dbm < 0 ? 2 * ((int)dbm + 100) : (int)dbm;
		if (pct > 100)
			pct = 100;
		rui_net_add(list, &n, max, rui_rtrim(f[4]), pct,
		    strstr(f[3], "WPA") || strstr(f[3], "WEP") ||
		    strstr(f[3], "SAE"), 0);
	}
	return n;
}

/* wpa_cli wants SSID and PSK as "double quoted" config strings */
static char *
connect_cmd(const char *iface, const char *ssid, const char *pass,
    const char *priv)
{
	char *ds = rui_strdupf("\"%s\"", ssid), *dp = rui_strdupf("\"%s\"", pass);
	char *qs = rui_shell_quote(ds), *qp = rui_shell_quote(dp), *cmd;

	(void)iface;
	(void)priv;
	if (*pass)
		cmd = rui_strdupf("id=$(wpa_cli add_network | tail -n 1) && "
		    "wpa_cli set_network \"$id\" ssid %s && "
		    "wpa_cli set_network \"$id\" psk %s && "
		    "wpa_cli select_network \"$id\" && wpa_cli save_config", qs, qp);
	else
		cmd = rui_strdupf("id=$(wpa_cli add_network | tail -n 1) && "
		    "wpa_cli set_network \"$id\" ssid %s && "
		    "wpa_cli set_network \"$id\" key_mgmt NONE && "
		    "wpa_cli select_network \"$id\" && wpa_cli save_config", qs);
	free(ds);
	free(dp);
	free(qs);
	free(qp);
	return cmd;
}

static char *
disconnect_cmd(const char *iface, const char *priv)
{
	(void)iface;
	(void)priv;
	return rui_strdup("wpa_cli disconnect 2>&1");
}

const RuiNetModule rui_net_wpa = {
	"wpa", "wpa_cli", NULL, 0,
	NULL, NULL, status_cmd, parse_status, scan_cmd, parse_scan,
	connect_cmd, disconnect_cmd
};
