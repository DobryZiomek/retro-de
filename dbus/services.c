/*
 * retro-dbus built-in services: what desktop programs expect to find on
 * the session bus, answered by the desktop's own parts.
 *
 *   org.freedesktop.Notifications (spec 1.2)  -> retro-bar's "notify"
 *       on retro-bus: urgency, actions, replaces_id, icons, timeouts;
 *       NotificationClosed and ActionInvoked come back as signals.
 *   org.freedesktop.portal.Desktop
 *       OpenURI.OpenURI      -> retro-bar's "launcher open"
 *       FileChooser.OpenFile / SaveFile / SaveFiles
 *                            -> a retro-files --choose window
 *       Settings.Read / ReadOne / ReadAll (color-scheme: no preference)
 *
 * retro.conf: dbus_notifications = on|off, dbus_portal = on|off (to leave
 * the names to another notification daemon or xdg-desktop-portal).
 */
#include "dbus.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define NPATH "/org/freedesktop/Notifications"
#define NIFACE "org.freedesktop.Notifications"
#define PPATH "/org/freedesktop/portal/desktop"
#define PNAME "org.freedesktop.portal.Desktop"
#define MAX_CHOOSERS 8

static Conn *notif, *portal;
static uint32_t local_id = 1u << 30;    /* ids when retro-bar is not there */

typedef struct Chooser {
	pid_t pid;
	int fd;                            /* its stdout                     */
	char *out;
	size_t len;
	int exited, status, eof;
	char *dest, *handle;
} Chooser;

static Chooser choosers[MAX_CHOOSERS];

/* ----------------------------------------------------------------- helpers */

static int
conf_on(const char *key)
{
	char *path = rui_config_path("retro.conf"), line[512];
	FILE *f = fopen(path, "r");
	int on = 1;

	free(path);
	if (!f)
		return 1;
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '='), *k = line, *v;

		if (!eq || line[0] == '#')
			continue;
		*eq = '\0';
		k = rui_rtrim(rui_ltrim(k));
		v = rui_rtrim(rui_ltrim(eq + 1));
		v[strcspn(v, "#\n")] = '\0';
		rui_rtrim(v);
		if (!strcmp(k, key))
			on = rui_conf_bool(v);
	}
	fclose(f);
	return on;
}

static void
reply_empty(Conn *c, const DMsg *m)
{
	bus_reply(c, m, "", NULL);
}

static void
reply_s(Conn *c, const DMsg *m, const char *s)
{
	DBuf b;

	db_init(&b, 0);
	db_str(&b, s);
	bus_reply(c, m, "s", &b);
	db_free(&b);
}

static int
is(const DMsg *m, const char *iface, const char *member)
{
	return m->member && !strcmp(m->member, member) &&
	    (!iface || !m->iface || !strcmp(m->iface, iface));
}

/* one entry of an a{sv}: key read, the variant's value as text (strings,
 * numbers, booleans; byte arrays as a string); returns 0 at the end */
static int
dict_next(DRd *r, size_t end, const char **key, char *val, size_t n,
    const char **vsig_out)
{
	const char *vs;

	if (r->err || r->pos >= end)
		return 0;
	dr_align(r, 8);
	if (r->pos >= end)
		return 0;
	*key = dr_str(r);
	vs = dr_sig(r);
	if (r->err)
		return 0;
	if (vsig_out)
		*vsig_out = vs;
	val[0] = '\0';
	if (!strcmp(vs, "ay")) {               /* a path: bytes, NUL-terminated */
		uint32_t len = dr_u32(r);
		size_t i;

		if (r->err || len > r->len - r->pos)
			return 0;
		for (i = 0; i < len && i + 1 < n; i++)
			val[i] = (char)r->p[r->pos + i];
		val[i] = '\0';
		r->pos += len;
		return 1;
	}
	return dr_value_text(r, &vs, val, n) < 0 ? 0 : 1;
}

/* ------------------------------------------------------- notifications */

static const char notif_xml[] =
    "<node>\n <interface name=\"org.freedesktop.Notifications\">\n"
    "  <method name=\"GetCapabilities\"><arg direction=\"out\" type=\"as\"/>"
    "</method>\n"
    "  <method name=\"Notify\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"u\"/><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"as\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"in\" type=\"i\"/><arg direction=\"out\" type=\"u\"/>"
    "</method>\n"
    "  <method name=\"CloseNotification\"><arg direction=\"in\" type=\"u\"/>"
    "</method>\n"
    "  <method name=\"GetServerInformation\"><arg direction=\"out\" "
    "type=\"s\"/><arg direction=\"out\" type=\"s\"/><arg direction=\"out\" "
    "type=\"s\"/><arg direction=\"out\" type=\"s\"/></method>\n"
    "  <signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/>"
    "</signal>\n"
    "  <signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/>"
    "</signal>\n </interface>\n</node>\n";

static void
notify_call(Conn *c, DMsg *m)
{
	DRd r;
	DBuf b;

	if (is(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		reply_s(c, m, notif_xml);
	} else if (is(m, "org.freedesktop.DBus.Peer", "Ping")) {
		reply_empty(c, m);
	} else if (is(m, NIFACE, "GetCapabilities")) {
		static const char *const caps[] = { "body", "actions",
			"icon-static", "persistence" };
		size_t mark, i;

		db_init(&b, 0);
		mark = db_array_begin(&b, 4);
		for (i = 0; i < sizeof caps / sizeof caps[0]; i++)
			db_str(&b, caps[i]);
		db_array_end(&b, mark);
		bus_reply(c, m, "as", &b);
		db_free(&b);
	} else if (is(m, NIFACE, "GetServerInformation")) {
		db_init(&b, 0);
		db_str(&b, "retro-de");
		db_str(&b, "retro-de");
		db_str(&b, RUI_VERSION);
		db_str(&b, "1.2");
		bus_reply(c, m, "ssss", &b);
		db_free(&b);
	} else if (is(m, NIFACE, "CloseNotification")) {
		char id[16];

		dr_init(&r, m->body, m->body_len, m->big);
		snprintf(id, sizeof id, "%lu", (unsigned long)dr_u32(&r));
		rui_bus_send("notify", "close", id);
		reply_empty(c, m);
	} else if (is(m, NIFACE, "Notify")) {
		const char *app, *icon, *summary, *body, *f[8];
		char actions[512] = "", image[1024] = "", secs[16] = "",
		    urg[16] = "", repl[16] = "", *args, *ans;
		uint32_t replaces, n;
		int32_t timeout;
		size_t start, len;
		int ok = 0;

		if (strcmp(m->sig, "susssasa{sv}i")) {
			bus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "Notify takes (susssasa{sv}i)");
			return;
		}
		dr_init(&r, m->body, m->body_len, m->big);
		app = dr_str(&r);
		replaces = dr_u32(&r);
		icon = dr_str(&r);
		summary = dr_str(&r);
		body = dr_str(&r);
		/* actions: key, label, key, label ... */
		n = dr_u32(&r);
		start = r.pos;
		while (!r.err && r.pos < start + n) {
			const char *key = dr_str(&r), *label;

			if (r.err)
				break;
			label = dr_str(&r);
			len = strlen(actions);
			snprintf(actions + len, sizeof actions - len, "%s%s:%s",
			    len ? "|" : "", key, label);
		}
		/* hints */
		n = dr_u32(&r);
		dr_align(&r, 8);
		start = r.pos;
		{
			const char *key, *vs;
			char val[1024];

			while (dict_next(&r, start + n, &key, val, sizeof val, &vs)) {
				if (!strcmp(key, "urgency"))
					rui_strlcpy(urg, val, sizeof urg);
				else if ((!strcmp(key, "image-path") ||
				    !strcmp(key, "image_path")) && *val)
					rui_strlcpy(image, val, sizeof image);
				else if (!strcmp(key, "desktop-entry") && !*image && !*icon)
					rui_strlcpy(image, val, sizeof image);
			}
			r.err = 0;
			r.pos = start + n;
		}
		timeout = (int32_t)dr_u32(&r);
		if (r.err) {
			bus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "bad Notify arguments");
			return;
		}
		if (timeout == 0)
			snprintf(secs, sizeof secs, "0");
		else if (timeout > 0)
			snprintf(secs, sizeof secs, "%ld", ((long)timeout + 999) / 1000);
		if (replaces)
			snprintf(repl, sizeof repl, "%lu", (unsigned long)replaces);
		f[0] = *summary ? summary : app;
		f[1] = body;
		f[2] = secs;
		f[3] = *image ? image : icon;
		f[4] = urg;
		f[5] = actions;
		f[6] = app;
		f[7] = repl;
		/* fields joined with tabs; tabs inside become spaces */
		{
			size_t total = 8;
			int i;
			char *p;

			for (i = 0; i < 8; i++)
				total += strlen(f[i]) + 1;
			args = rui_malloc(total);
			args[0] = '\0';
			for (i = 0; i < 8; i++) {
				if (i)
					strcat(args, "\t");
				p = args + strlen(args);
				strcat(args, f[i]);
				for (; *p; p++)
					if (*p == '\t')
						*p = ' ';
			}
		}
		ans = rui_bus_call("notify", "show", args, 3000, &ok);
		free(args);
		db_init(&b, 0);
		db_u32(&b, ans && ok ? (uint32_t)strtoul(ans, NULL, 10) :
		    replaces ? replaces : local_id++);
		free(ans);
		bus_reply(c, m, "u", &b);
		db_free(&b);
	} else if (m->type == DM_CALL) {
		bus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod",
		    "no such method");
	}
}

/* notify.closed / notify.action from retro-bus -> D-Bus signals */
static void
on_retro_bus(const RuiBusMsg *m, void *ud)
{
	char *f[2] = { "", "" };
	DBuf b;

	(void)ud;
	if (!notif || strcmp(m->kind, "event"))
		return;
	rui_bus_fields(m->args, f, 2);
	if (!strcmp(m->name, "notify.closed")) {
		uint32_t reason = !strcmp(f[1], "expired") ? 1 :
		    !strcmp(f[1], "dismissed") ? 2 : !strcmp(f[1], "closed") ? 3 : 4;

		db_init(&b, 0);
		db_u32(&b, (uint32_t)strtoul(f[0], NULL, 10));
		db_u32(&b, reason);
		bus_signal(notif, NULL, NPATH, NIFACE, "NotificationClosed", "uu", &b);
		db_free(&b);
	} else if (!strcmp(m->name, "notify.action")) {
		db_init(&b, 0);
		db_u32(&b, (uint32_t)strtoul(f[0], NULL, 10));
		db_str(&b, f[1]);
		bus_signal(notif, NULL, NPATH, NIFACE, "ActionInvoked", "us", &b);
		db_free(&b);
	}
}

/* -------------------------------------------------------------- portal */

static const char portal_xml[] =
    "<node>\n"
    " <interface name=\"org.freedesktop.portal.OpenURI\">\n"
    "  <method name=\"OpenURI\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"out\" type=\"o\"/></method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.FileChooser\">\n"
    "  <method name=\"OpenFile\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"out\" type=\"o\"/></method>\n"
    "  <method name=\"SaveFile\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"out\" type=\"o\"/></method>\n"
    "  <method name=\"SaveFiles\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"out\" type=\"o\"/></method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.Settings\">\n"
    "  <method name=\"ReadAll\"><arg direction=\"in\" type=\"as\"/>"
    "<arg direction=\"out\" type=\"a{sa{sv}}\"/></method>\n"
    "  <method name=\"Read\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"out\" type=\"v\"/>"
    "</method>\n"
    "  <method name=\"ReadOne\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"out\" type=\"v\"/>"
    "</method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    "</node>\n";

static uint32_t
iface_version(const char *iface)
{
	if (!strcmp(iface, "org.freedesktop.portal.FileChooser"))
		return 3;
	if (!strcmp(iface, "org.freedesktop.portal.OpenURI"))
		return 3;
	if (!strcmp(iface, "org.freedesktop.portal.Settings"))
		return 2;
	return 0;
}

/* /org/freedesktop/portal/desktop/request/SENDER/TOKEN */
static char *
request_path(const char *sender, const char *token)
{
	char s[64], t[64];
	static unsigned long n;
	size_t i, k = 0;

	for (i = 0; sender[i] && k < sizeof s - 1; i++) {
		if (sender[i] == ':')
			continue;
		s[k++] = sender[i] == '.' ? '_' : sender[i];
	}
	s[k] = '\0';
	if (token && *token) {
		for (i = 0, k = 0; token[i] && k < sizeof t - 1; i++) {
			char c = token[i];

			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			    (c >= '0' && c <= '9') || c == '_')
				t[k++] = c;
		}
		t[k] = '\0';
	} else {
		snprintf(t, sizeof t, "retro%lu", ++n);
	}
	if (!t[0])
		snprintf(t, sizeof t, "retro%lu", ++n);
	return rui_strdupf("%s/request/%s/%s", PPATH, s, t);
}

static void
reply_handle(Conn *c, const DMsg *m, const char *handle)
{
	DBuf b;

	db_init(&b, 0);
	db_str(&b, handle);
	bus_reply(c, m, "o", &b);
	db_free(&b);
}

/* Request.Response(u code, a{sv} results); uris may be NULL */
static void
respond(const char *dest, const char *handle, uint32_t code,
    char **uris, int nuris)
{
	DBuf b;
	size_t mark, amark;
	int i;

	db_init(&b, 0);
	db_u32(&b, code);
	mark = db_array_begin(&b, 8);
	if (nuris) {
		db_pad(&b, 8);
		db_str(&b, "uris");
		db_sig(&b, "as");
		amark = db_array_begin(&b, 4);
		for (i = 0; i < nuris; i++)
			db_str(&b, uris[i]);
		db_array_end(&b, amark);
	}
	db_array_end(&b, mark);
	bus_signal(portal, dest, handle, "org.freedesktop.portal.Request",
	    "Response", "ua{sv}", &b);
	db_free(&b);
}

static char *
file_uri(const char *path)
{
	static const char hx[] = "0123456789ABCDEF";
	size_t n = strlen(path), k = 0;
	char *out = rui_malloc(n * 3 + 8);

	memcpy(out, "file://", 7);
	k = 7;
	for (; *path; path++) {
		unsigned char c = (unsigned char)*path;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || strchr("/-._~!$&'()*+,;=:@", c)) {
			out[k++] = (char)c;
		} else {
			out[k++] = '%';
			out[k++] = hx[c >> 4];
			out[k++] = hx[c & 15];
		}
	}
	out[k] = '\0';
	return out;
}

static void
start_chooser(Conn *c, const DMsg *m, int save)
{
	const char *parent, *title, *key, *vs;
	char val[1024], token[128] = "", name[256] = "", folder[1024] = "";
	int multiple = 0, directory = 0, i, p[2];
	uint32_t n;
	size_t start;
	DRd r;
	Chooser *ch = NULL;
	char *handle;
	pid_t pid;

	if (strcmp(m->sig, "ssa{sv}")) {
		bus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
		    "takes (ssa{sv})");
		return;
	}
	dr_init(&r, m->body, m->body_len, m->big);
	parent = dr_str(&r);
	title = dr_str(&r);
	n = dr_u32(&r);
	dr_align(&r, 8);
	start = r.pos;
	(void)parent;
	while (dict_next(&r, start + n, &key, val, sizeof val, &vs)) {
		if (!strcmp(key, "handle_token"))
			rui_strlcpy(token, val, sizeof token);
		else if (!strcmp(key, "multiple"))
			multiple = atoi(val) != 0;
		else if (!strcmp(key, "directory"))
			directory = atoi(val) != 0;
		else if (!strcmp(key, "current_name"))
			rui_strlcpy(name, val, sizeof name);
		else if (!strcmp(key, "current_folder"))
			rui_strlcpy(folder, val, sizeof folder);
		else if (!strcmp(key, "current_file") && save) {
			const char *slash = strrchr(val, '/');

			if (slash) {
				rui_strlcpy(name, slash + 1, sizeof name);
				rui_strlcpy(folder, val, sizeof folder);
				folder[slash - val] = '\0';
			}
		}
	}
	handle = request_path(m->sender, token);
	for (i = 0; i < MAX_CHOOSERS; i++)
		if (!choosers[i].pid && choosers[i].fd <= 0)
			ch = &choosers[i];
	if (!ch || pipe(p) < 0) {
		reply_handle(c, m, handle);
		respond(m->sender, handle, 2, NULL, 0);
		free(handle);
		return;
	}
	pid = fork();
	if (pid == 0) {
		char *argv[16];
		int k = 0;

		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		setsid();
		argv[k++] = "retro-files";
		argv[k++] = "--choose";
		if (save) {
			argv[k++] = "--save";
			argv[k++] = *name ? name : "Untitled";
		}
		if (directory)
			argv[k++] = "--dir";
		if (multiple)
			argv[k++] = "--multiple";
		if (*title) {
			argv[k++] = "--title";
			argv[k++] = (char *)title;
		}
		if (*folder)
			argv[k++] = folder;
		argv[k] = NULL;
		execvp("retro-files", argv);
		_exit(127);
	}
	close(p[1]);
	reply_handle(c, m, handle);
	if (pid < 0) {
		close(p[0]);
		respond(m->sender, handle, 2, NULL, 0);
		free(handle);
		return;
	}
	memset(ch, 0, sizeof *ch);
	ch->pid = pid;
	ch->fd = p[0];
	ch->dest = rui_strdup(m->sender);
	ch->handle = handle;
}

static void
chooser_finish(Chooser *ch)
{
	char *uris[256], *line, *save = NULL;
	int n = 0, ok = WIFEXITED(ch->status) && WEXITSTATUS(ch->status) == 0;

	if (ok && ch->out) {
		for (line = strtok_r(ch->out, "\n", &save); line && n < 256;
		    line = strtok_r(NULL, "\n", &save))
			if (*line == '/')
				uris[n++] = file_uri(line);
	}
	respond(ch->dest, ch->handle, ok && n ? 0 : 1, uris, n);
	while (n > 0)
		free(uris[--n]);
	free(ch->out);
	free(ch->dest);
	free(ch->handle);
	if (ch->fd > 0)
		close(ch->fd);
	memset(ch, 0, sizeof *ch);
}

static void
settings_read(Conn *c, const DMsg *m, int one)
{
	DRd r;
	const char *ns, *key;
	DBuf b;

	dr_init(&r, m->body, m->body_len, m->big);
	ns = dr_str(&r);
	key = dr_str(&r);
	if (r.err || strcmp(ns, "org.freedesktop.appearance") ||
	    strcmp(key, "color-scheme")) {
		bus_error(c, m, "org.freedesktop.portal.Error.NotFound",
		    "Requested setting not found");
		return;
	}
	db_init(&b, 0);
	if (one) {
		db_variant_u32(&b, 0);             /* no preference */
	} else {
		db_sig(&b, "v");                   /* Read wraps it once more */
		db_variant_u32(&b, 0);
	}
	bus_reply(c, m, "v", &b);
	db_free(&b);
}

static void
portal_call(Conn *c, DMsg *m)
{
	DBuf b;
	DRd r;
	int i;

	if (m->type != DM_CALL)
		return;
	if (is(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		reply_s(c, m, portal_xml);
	} else if (is(m, "org.freedesktop.DBus.Peer", "Ping")) {
		reply_empty(c, m);
	} else if (is(m, "org.freedesktop.DBus.Properties", "Get")) {
		const char *iface, *prop;

		dr_init(&r, m->body, m->body_len, m->big);
		iface = dr_str(&r);
		prop = dr_str(&r);
		if (r.err || strcmp(prop, "version") || !iface_version(iface)) {
			bus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "No such property");
			return;
		}
		db_init(&b, 0);
		db_variant_u32(&b, iface_version(iface));
		bus_reply(c, m, "v", &b);
		db_free(&b);
	} else if (is(m, "org.freedesktop.DBus.Properties", "GetAll")) {
		const char *iface;
		size_t mark;

		dr_init(&r, m->body, m->body_len, m->big);
		iface = dr_str(&r);
		db_init(&b, 0);
		mark = db_array_begin(&b, 8);
		if (!r.err && iface_version(iface)) {
			db_pad(&b, 8);
			db_str(&b, "version");
			db_variant_u32(&b, iface_version(iface));
		}
		db_array_end(&b, mark);
		bus_reply(c, m, "a{sv}", &b);
		db_free(&b);
	} else if (is(m, "org.freedesktop.portal.OpenURI", "OpenURI")) {
		const char *parent, *uri, *key, *vs;
		char val[256], token[128] = "", *handle;
		uint32_t n;
		size_t start;
		int ok = 0;
		char *ans;

		if (strcmp(m->sig, "ssa{sv}")) {
			bus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "takes (ssa{sv})");
			return;
		}
		dr_init(&r, m->body, m->body_len, m->big);
		parent = dr_str(&r);
		uri = dr_str(&r);
		n = dr_u32(&r);
		dr_align(&r, 8);
		start = r.pos;
		(void)parent;
		while (dict_next(&r, start + n, &key, val, sizeof val, &vs))
			if (!strcmp(key, "handle_token"))
				rui_strlcpy(token, val, sizeof token);
		handle = request_path(m->sender, token);
		reply_handle(c, m, handle);
		ans = rui_bus_call("launcher", "open", uri, 3000, &ok);
		free(ans);
		respond(m->sender, handle, ok ? 0 : 2, NULL, 0);
		free(handle);
	} else if (is(m, "org.freedesktop.portal.FileChooser", "OpenFile")) {
		start_chooser(c, m, 0);
	} else if (is(m, "org.freedesktop.portal.FileChooser", "SaveFile") ||
	    is(m, "org.freedesktop.portal.FileChooser", "SaveFiles")) {
		start_chooser(c, m, 1);
	} else if (is(m, "org.freedesktop.portal.Request", "Close")) {
		for (i = 0; i < MAX_CHOOSERS; i++)
			if (choosers[i].pid && choosers[i].handle &&
			    !strcmp(choosers[i].handle, m->path))
				kill(choosers[i].pid, SIGTERM);
		reply_empty(c, m);
	} else if (is(m, "org.freedesktop.portal.Settings", "ReadOne")) {
		settings_read(c, m, 1);
	} else if (is(m, "org.freedesktop.portal.Settings", "Read")) {
		settings_read(c, m, 0);
	} else if (is(m, "org.freedesktop.portal.Settings", "ReadAll")) {
		size_t mark, inner;

		db_init(&b, 0);
		mark = db_array_begin(&b, 8);
		db_pad(&b, 8);
		db_str(&b, "org.freedesktop.appearance");
		inner = db_array_begin(&b, 8);
		db_pad(&b, 8);
		db_str(&b, "color-scheme");
		db_variant_u32(&b, 0);
		db_array_end(&b, inner);
		db_array_end(&b, mark);
		bus_reply(c, m, "a{sa{sv}}", &b);
		db_free(&b);
	} else {
		bus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod",
		    "retro-dbus does not provide this portal method");
	}
}

/* ----------------------------------------------------------- the hooks */

void
services_init(void)
{
	rui_bus_handler(on_retro_bus, NULL);
	if (conf_on("dbus_notifications")) {
		notif = bus_builtin(notify_call);
		if (bus_own(notif, NIFACE) < 0)
			notif = NULL;
		else
			rui_bus_listen("notify.*");
	}
	if (conf_on("dbus_portal")) {
		portal = bus_builtin(portal_call);
		if (bus_own(portal, PNAME) < 0)
			portal = NULL;
	}
}

int
services_fds(struct pollfd *p, int max, int *timeout_ms)
{
	int n = 0, i, bfd = rui_bus_wait_fd(timeout_ms);

	if (bfd >= 0 && n < max) {
		p[n].fd = bfd;
		p[n++].events = POLLIN;
	}
	for (i = 0; i < MAX_CHOOSERS && n < max; i++) {
		if (choosers[i].fd > 0 && !choosers[i].eof) {
			p[n].fd = choosers[i].fd;
			p[n++].events = POLLIN;
		}
	}
	return n;
}

void
services_handle(struct pollfd *p, int n)
{
	int i, k;

	for (i = 0; i < n; i++) {
		if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		if (p[i].fd == rui_bus_fd()) {
			rui_bus_dispatch();
			continue;
		}
		for (k = 0; k < MAX_CHOOSERS; k++) {
			Chooser *ch = &choosers[k];
			char buf[4096];
			ssize_t r;

			if (ch->fd != p[i].fd || ch->eof)
				continue;
			r = read(ch->fd, buf, sizeof buf);
			if (r > 0) {
				ch->out = rui_realloc(ch->out, ch->len + (size_t)r + 1);
				memcpy(ch->out + ch->len, buf, (size_t)r);
				ch->len += (size_t)r;
				ch->out[ch->len] = '\0';
			} else if (r == 0 || errno != EINTR) {
				ch->eof = 1;
			}
			if (ch->eof && ch->exited)
				chooser_finish(ch);
		}
	}
}

void
services_child(pid_t pid, int status)
{
	int i;

	for (i = 0; i < MAX_CHOOSERS; i++) {
		Chooser *ch = &choosers[i];

		if (ch->pid != pid)
			continue;
		ch->exited = 1;
		ch->status = status;
		ch->pid = 0;
		if (ch->eof)
			chooser_finish(ch);
	}
}
