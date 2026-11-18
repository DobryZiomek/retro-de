/* network module: OpenBSD native Wi-Fi through ifconfig(8) join/scan */
#include "../lib/retroui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* `ifconfig wlan` (interface group) and `join` are OpenBSD features */
static int
usable(void)
{
#if defined(__OpenBSD__)
	return 1;
#else
	return 0;
#endif
}

static char *
iface_cmd(void)
{
	return rui_strdup("ifconfig wlan 2>/dev/null");
}

static int
parse_iface(const char *out, char *iface, size_t n)
{
	char line[512];
	const char *p = out;

	while (rui_next_line(&p, line, sizeof line)) {
		char *colon = strchr(line, ':');

		if (line[0] && !isspace((unsigned char)line[0]) && colon) {
			*colon = '\0';
			rui_strlcpy(iface, line, n);
			return 1;
		}
	}
	return 0;
}

static char *
status_cmd(const char *iface)
{
	char *q, *cmd;

	if (!*iface)
		return NULL;
	q = rui_shell_quote(iface);
	cmd = rui_strdupf("ifconfig %s", q);
	free(q);
	return cmd;
}

/* reads a network name: bare word or "quoted name" */
static const char *
read_name(const char *s, char *out, size_t n)
{
	size_t o = 0;

	while (*s == ' ')
		s++;
	if (*s == '"') {
		s++;
		while (*s && *s != '"') {
			if (o + 1 < n)
				out[o++] = *s;
			s++;
		}
		if (*s == '"')
			s++;
	} else {
		while (*s && !isspace((unsigned char)*s)) {
			if (o + 1 < n)
				out[o++] = *s;
			s++;
		}
	}
	out[o] = '\0';
	return s;
}

static int
parse_status(const char *out, RuiNetStatus *st)
{
	char line[512];
	const char *p = out;
	int active = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char *s = rui_ltrim(rui_rtrim(line));

		if (!strncmp(s, "status:", 7)) {
			active = !strcmp(rui_ltrim(s + 7), "active");
		} else if (!strncmp(s, "ieee80211:", 10)) {
			char *q = strstr(s, "join ");

			if (!q)
				q = strstr(s, "nwid ");
			if (q)
				read_name(q + 5, st->ssid, sizeof st->ssid);
		}
	}
	st->connected = active && st->ssid[0];
	if (!st->connected)
		st->ssid[0] = '\0';
	return 1;
}

static char *
scan_cmd(const char *iface)
{
	char *q, *cmd;

	if (!*iface)
		return NULL;
	q = rui_shell_quote(iface);
	cmd = rui_strdupf("ifconfig %s scan 2>/dev/null", q);
	free(q);
	return cmd;
}

static int
parse_scan(const char *out, RuiNetwork *list, int max)
{
	char line[512];
	const char *p = out;
	int n = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char *s = rui_ltrim(rui_rtrim(line)), ssid[64];
		const char *rest, *pc;
		int pct = -1;

		if (strncmp(s, "nwid ", 5))
			continue;
		rest = read_name(s + 5, ssid, sizeof ssid);
		if (!strstr(rest, "bssid"))
			continue;                      /* the interface's own line */
		if ((pc = strchr(rest, '%'))) {
			const char *b = pc;
			long v;

			while (b > rest && isdigit((unsigned char)b[-1]))
				b--;
			v = strtol(b, NULL, 10);         /* digits only: >= 0 */
			pct = v > 100 ? 100 : (int)v;
		}
		rui_net_add(list, &n, max, ssid, pct,
		    strstr(rest, "privacy") || strstr(rest, "wpa"), 0);
	}
	return n;
}

static char *
connect_cmd(const char *iface, const char *ssid, const char *pass,
    const char *priv)
{
	char *qi = rui_shell_quote(iface), *qs = rui_shell_quote(ssid);
	char *qp = rui_shell_quote(pass), *cmd;

	if (*pass)
		cmd = rui_strdupf("%s ifconfig %s join %s wpakey %s 2>&1", priv, qi,
		    qs, qp);
	else
		cmd = rui_strdupf("%s ifconfig %s join %s -wpakey 2>&1", priv, qi,
		    qs);
	free(qi);
	free(qs);
	free(qp);
	return cmd;
}

static char *
disconnect_cmd(const char *iface, const char *priv)
{
	char *qi = rui_shell_quote(iface);
	char *cmd = rui_strdupf("%s ifconfig %s -join 2>&1 || "
	    "%s ifconfig %s -nwid 2>&1", priv, qi, priv, qi);

	free(qi);
	return cmd;
}

const RuiNetModule rui_net_ifconfig = {
	"ifconfig", "ifconfig", usable, 0,
	iface_cmd, parse_iface, status_cmd, parse_status, scan_cmd, parse_scan,
	connect_cmd, disconnect_cmd
};
