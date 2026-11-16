/* network module: Linux iwd via iwctl */
#include "../lib/retroui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *
iface_cmd(void)
{
	return rui_strdup("iwctl station list");
}

/* iwctl prints tables: a title, a dashed rule, column names, a rule, rows */
static int
parse_iface(const char *out, char *iface, size_t n)
{
	char line[512];
	const char *p = out;
	int rules = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char *s;

		rui_strip_ansi(line);
		s = rui_ltrim(line);
		if (*s == '-') {
			rules++;
			continue;
		}
		if (rules < 2 || !*s)
			continue;
		s[strcspn(s, " \t")] = '\0';
		rui_strlcpy(iface, s, n);
		return 1;
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
	cmd = rui_strdupf("iwctl station %s show", q);
	free(q);
	return cmd;
}

static int
parse_status(const char *out, RuiNetStatus *st)
{
	char line[512];
	const char *p = out;

	while (rui_next_line(&p, line, sizeof line)) {
		char *s;

		rui_strip_ansi(line);
		s = rui_ltrim(rui_rtrim(line));
		if (!strncmp(s, "State", 5) && isspace((unsigned char)s[5]))
			st->connected = !strcmp(rui_ltrim(s + 5), "connected");
		else if (!strncmp(s, "Connected network", 17))
			rui_strlcpy(st->ssid, rui_ltrim(s + 17), sizeof st->ssid);
	}
	return 1;
}

static char *
scan_cmd(const char *iface)
{
	char *q, *cmd;

	if (!*iface)
		return NULL;
	q = rui_shell_quote(iface);
	cmd = rui_strdupf("iwctl station %s scan >/dev/null 2>&1; sleep 2; "
	    "iwctl station %s get-networks", q, q);
	free(q);
	return cmd;
}

static int
parse_scan(const char *out, RuiNetwork *list, int max)
{
	char line[512], raw[512];
	const char *p = out;
	int n = 0, rules = 0;

	while (rui_next_line(&p, raw, sizeof raw)) {
		char *s, *sec, *sig, *e;
		const char *star = NULL;
		int connected = 0, bars = 0, i;

		rui_strlcpy(line, raw, sizeof line);
		rui_strip_ansi(line);
		s = rui_ltrim(rui_rtrim(line));
		if (*s == '-') {
			rules++;
			continue;
		}
		if (rules < 2 || !*s)
			continue;
		if (*s == '>') {
			connected = 1;
			s = rui_ltrim(s + 1);
		}
		/* columns from the right: signal (****), security, name */
		e = s + strlen(s);
		while (e > s && !isspace((unsigned char)e[-1]))
			e--;
		sig = e;
		if (e == s || *sig != '*')
			continue;
		while (e > s && isspace((unsigned char)e[-1]))
			e--;
		*e = '\0';
		while (e > s && !isspace((unsigned char)e[-1]))
			e--;
		sec = e;
		if (e == s)
			continue;
		while (e > s && isspace((unsigned char)e[-1]))
			e--;
		*e = '\0';
		/* bright stars follow a blank; dimmed ones follow a colour
		 * escape in the raw line and are not counted */
		for (i = 0; raw[i]; i++)
			if (raw[i] == '*' && (i == 0 || raw[i - 1] == ' '))
				star = raw + i;
		while (star && *star == '*') {
			bars++;
			star++;
		}
		if (bars == 0)
			bars = (int)strlen(sig);
		if (bars > 4)
			bars = 4;
		rui_net_add(list, &n, max, s, bars * 25, strcmp(sec, "open") != 0,
		    connected);
	}
	return n;
}

static char *
connect_cmd(const char *iface, const char *ssid, const char *pass,
    const char *priv)
{
	char *qi = rui_shell_quote(iface), *qs = rui_shell_quote(ssid);
	char *qp = rui_shell_quote(pass), *cmd;

	(void)priv;
	if (*pass)
		cmd = rui_strdupf("iwctl --passphrase %s station %s connect %s 2>&1",
		    qp, qi, qs);
	else
		cmd = rui_strdupf("iwctl station %s connect %s 2>&1", qi, qs);
	free(qi);
	free(qs);
	free(qp);
	return cmd;
}

static char *
disconnect_cmd(const char *iface, const char *priv)
{
	char *qi = rui_shell_quote(iface);
	char *cmd = rui_strdupf("iwctl station %s disconnect 2>&1", qi);

	(void)priv;
	free(qi);
	return cmd;
}

const RuiNetModule rui_net_iwd = {
	"iwd", "iwctl", NULL, 1,
	iface_cmd, parse_iface, status_cmd, parse_status, scan_cmd, parse_scan,
	connect_cmd, disconnect_cmd
};
