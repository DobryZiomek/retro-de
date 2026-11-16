/* Bluetooth module: BlueZ via bluetoothctl (Linux) */
#include "../lib/retroui.h"

#include <stdlib.h>
#include <string.h>

/* "Device AA:BB:CC:DD:EE:FF rest": copies the address, returns the rest */
static const char *
device_line(const char *s, char *addr, size_t n)
{
	size_t len;

	if (strncmp(s, "Device ", 7))
		return NULL;
	s += 7;
	len = strcspn(s, " \t");
	if (len != 17 || s[2] != ':' || s[5] != ':' || len >= n)
		return NULL;
	memcpy(addr, s, len);
	addr[len] = '\0';
	s += len;
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* `bluetoothctl show`, then `bluetoothctl devices Connected`:
 *   Controller 00:1A:7D:DA:71:13 (public)
 *           Powered: yes
 *   Device 11:22:33:44:55:66 Headphones                                  */
static int
parse_status(const char *out, RuiBtStatus *st)
{
	char line[512], addr[24];
	const char *p = out;

	while (rui_next_line(&p, line, sizeof line)) {
		const char *rest;
		char *s;

		rui_strip_ansi(line);
		s = rui_ltrim(rui_rtrim(line));
		if (!strncmp(s, "Controller ", 11)) {
			st->present = 1;
		} else if (!strncmp(s, "Powered:", 8)) {
			st->powered = !strcmp(rui_ltrim(s + 8), "yes");
		} else if ((rest = device_line(s, addr, sizeof addr))) {
			if (!st->nconnected++)
				rui_strlcpy(st->device, *rest ? rest : addr,
				    sizeof st->device);
		}
	}
	if (!st->powered)
		st->nconnected = 0;
	return st->present;
}

/* `bluetoothctl info` of every device, one after another:
 *   Device 11:22:33:44:55:66 (public)
 *           Name: WH-1000XM4
 *           Alias: Headphones
 *           Paired: yes
 *           Trusted: yes
 *           Connected: no                                                */
static int
parse_devices(const char *out, RuiBtDevice *list, int max)
{
	char line[512], addr[24];
	const char *p = out;
	RuiBtDevice *d = NULL;
	int n = 0;

	while (rui_next_line(&p, line, sizeof line)) {
		char *s;

		rui_strip_ansi(line);
		s = rui_ltrim(rui_rtrim(line));
		if (device_line(s, addr, sizeof addr)) {
			d = NULL;
			if (n < max) {
				d = &list[n++];
				memset(d, 0, sizeof *d);
				rui_strlcpy(d->addr, addr, sizeof d->addr);
				rui_strlcpy(d->name, addr, sizeof d->name);
			}
		} else if (!d) {
			continue;
		} else if (!strncmp(s, "Alias:", 6)) {
			if (*rui_ltrim(s + 6))
				rui_strlcpy(d->name, rui_ltrim(s + 6), sizeof d->name);
		} else if (!strncmp(s, "Name:", 5)) {
			if (!strcmp(d->name, d->addr) && *rui_ltrim(s + 5))
				rui_strlcpy(d->name, rui_ltrim(s + 5), sizeof d->name);
		} else if (!strncmp(s, "Paired:", 7)) {
			d->paired = !strcmp(rui_ltrim(s + 7), "yes");
		} else if (!strncmp(s, "Trusted:", 8)) {
			d->trusted = !strcmp(rui_ltrim(s + 8), "yes");
		} else if (!strncmp(s, "Connected:", 10)) {
			d->connected = !strcmp(rui_ltrim(s + 10), "yes");
		}
	}
	return n;
}

static char *
connect_cmd(const char *addr, int paired)
{
	char *q = rui_shell_quote(addr), *cmd;

	if (paired)
		cmd = rui_strdupf("bluetoothctl --timeout 20 connect %s 2>&1", q);
	else
		cmd = rui_strdupf("bluetoothctl --timeout 30 pair %s 2>&1 && "
		    "bluetoothctl trust %s >/dev/null 2>&1; "
		    "bluetoothctl --timeout 20 connect %s 2>&1", q, q, q);
	free(q);
	return cmd;
}

static char *
disconnect_cmd(const char *addr)
{
	char *q = rui_shell_quote(addr);
	char *cmd = rui_strdupf("bluetoothctl disconnect %s 2>&1", q);

	free(q);
	return cmd;
}

static char *
forget_cmd(const char *addr)
{
	char *q = rui_shell_quote(addr);
	char *cmd = rui_strdupf("bluetoothctl remove %s 2>&1", q);

	free(q);
	return cmd;
}

#define BLUEZ_LIST \
	"bluetoothctl devices 2>/dev/null | while read -r _ mac _; do " \
	"bluetoothctl info \"$mac\" 2>/dev/null; done"

const RuiBtModule rui_bt_bluez = {
	"bluez", "bluetoothctl",
	"bluetoothctl show 2>&1; bluetoothctl devices Connected 2>/dev/null",
	parse_status,
	{ "bluetoothctl power off 2>&1",
	  "rfkill unblock bluetooth 2>/dev/null; bluetoothctl power on 2>&1" },
	BLUEZ_LIST,
	"bluetoothctl --timeout 8 scan on >/dev/null 2>&1; " BLUEZ_LIST,
	parse_devices,
	connect_cmd, disconnect_cmd, forget_cmd
};
