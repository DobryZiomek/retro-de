/*
 * libretroui network engine: picks a network module from config.h and
 * offers NULL-safe front ends.  The modules build the command lines and
 * parse the tools' output; callers run the commands as async jobs.
 */
#include "retroui.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>

const RuiNetModule *
rui_net_detect(const char *forced)
{
	const RuiNetModule *const *mp;
	int any = !forced || !*forced || !strcmp(forced, "auto");

	if (forced && !strcmp(forced, "none"))
		return NULL;
	for (mp = rui_config.net; *mp; mp++) {
		const RuiNetModule *m = *mp;

		if (!any && strcmp(forced, m->name) && strcmp(forced, m->program))
			continue;
		if (!rui_have_cmd(m->program) || (m->usable && !m->usable()))
			continue;
		return m;
	}
	return NULL;
}

const char *
rui_net_backend_name(const RuiNetModule *m)
{
	return m ? m->name : "none";
}

char *
rui_net_iface_cmd(const RuiNetModule *m)
{
	return m && m->iface_cmd ? m->iface_cmd() : NULL;
}

int
rui_net_parse_iface(const RuiNetModule *m, const char *out, char *iface,
    size_t n)
{
	iface[0] = '\0';
	return m && m->parse_iface ? m->parse_iface(out ? out : "", iface, n) : 0;
}

char *
rui_net_status_cmd(const RuiNetModule *m, const char *iface)
{
	return m ? m->status_cmd(iface ? iface : "") : NULL;
}

int
rui_net_parse_status(const RuiNetModule *m, const char *out, RuiNetStatus *st)
{
	st->ssid[0] = '\0';
	st->connected = 0;
	st->wireless = m != NULL;
	return m ? m->parse_status(out ? out : "", st) : 0;
}

char *
rui_net_scan_cmd(const RuiNetModule *m, const char *iface)
{
	return m ? m->scan_cmd(iface ? iface : "") : NULL;
}

static int
cmp_network(const void *a, const void *b)
{
	const RuiNetwork *x = a, *y = b;

	if (x->connected != y->connected)
		return y->connected - x->connected;
	if (x->signal != y->signal)
		return y->signal - x->signal;
	return strcmp(x->ssid, y->ssid);
}

int
rui_net_parse_scan(const RuiNetModule *m, const char *out, RuiNetwork *list,
    int max)
{
	int n = m ? m->parse_scan(out ? out : "", list, max) : 0;

	if (n > 1)
		qsort(list, (size_t)n, sizeof *list, cmp_network);
	return n;
}

char *
rui_net_connect_cmd(const RuiNetModule *m, const char *iface, const char *ssid,
    const char *pass, const char *priv)
{
	if (!m || !ssid)
		return NULL;
	return m->connect_cmd(iface ? iface : "", ssid, pass ? pass : "",
	    priv ? priv : "");
}

char *
rui_net_disconnect_cmd(const RuiNetModule *m, const char *iface,
    const char *priv)
{
	return m ? m->disconnect_cmd(iface ? iface : "", priv ? priv : "") : NULL;
}

void
rui_net_add(RuiNetwork *list, int *n, int max, const char *ssid, int signal,
    int secure, int connected)
{
	int i;

	if (!ssid[0])
		return;                            /* hidden network */
	for (i = 0; i < *n; i++) {
		if (!strcmp(list[i].ssid, ssid)) {
			if (signal > list[i].signal)
				list[i].signal = signal;
			list[i].connected |= connected;
			list[i].secure |= secure;
			return;
		}
	}
	if (*n >= max)
		return;
	rui_strlcpy(list[*n].ssid, ssid, sizeof list[*n].ssid);
	list[*n].signal = signal;
	list[*n].secure = secure;
	list[*n].connected = connected;
	(*n)++;
}

int
rui_net_link_up(char *ifname, size_t n)
{
	struct ifaddrs *ifs, *i;
	int up = 0;

	if (getifaddrs(&ifs) < 0)
		return 0;
	for (i = ifs; i && !up; i = i->ifa_next) {
		if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING))
			continue;
		if (i->ifa_addr->sa_family != AF_INET &&
		    i->ifa_addr->sa_family != AF_INET6)
			continue;
		rui_strlcpy(ifname, i->ifa_name, n);
		up = 1;
	}
	freeifaddrs(ifs);
	return up;
}
