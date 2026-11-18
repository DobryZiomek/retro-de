/*
 * retro-dbus - a D-Bus session bus without D-Bus.
 *
 * Programs that need a session bus (GTK and Qt applications, Electron,
 * Firefox, notify-send ...) connect here as they would to dbus-daemon:
 * the same wire protocol, EXTERNAL authentication, unique and well-known
 * names with queues, match rules (sender, interface, member, path,
 * path_namespace, destination, argN, argNpath, arg0namespace), the
 * org.freedesktop.DBus methods and signals, BecomeMonitor, and activation
 * from the .service files in dbus-1/services.
 *
 * Built in (services.c, each can be switched off in retro.conf):
 *   org.freedesktop.Notifications  -> retro-bar's notifications
 *   org.freedesktop.portal.Desktop -> OpenURI and FileChooser through
 *                                     retro-bar's launcher and retro-files
 *
 * Not supported: passing file descriptors (clients are told so during
 * authentication and do without), the system bus, policies.
 *
 *   retro-dbus [-a unix:path=PATH] [--print-address] [-v]
 *
 * retro-session starts it with `dbus = auto` (or `retro`) and exports
 * DBUS_SESSION_BUS_ADDRESS.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1                  /* struct ucred */
#endif
#include "dbus.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#define MAX_CONNS 512
#define MAX_MATCH 512                  /* per connection                 */
#define MAX_QUEUE 16                   /* waiting owners per name        */
#define OUT_LIMIT (128u << 20)
#define ACTIVATION_MS 25000
#define DRIVER "org.freedesktop.DBus"
#define DPATH "/org/freedesktop/DBus"

enum { AUTH_NUL, AUTH_LINES, RUNNING };

typedef struct Match {
	int type;                          /* 0 any                          */
	char *sender, *iface, *member, *path, *path_ns, *dest, *arg0ns;
	char *arg[4], *argpath[4];
	char *text;                        /* as given, for RemoveMatch     */
} Match;

struct Conn {
	int fd;                            /* -1 for a built-in             */
	BuiltinFn fn;
	int state, hello, dead, monitor;
	uint32_t serial;                   /* a built-in's own messages     */
	unsigned long id;
	char unique[32];
	unsigned char *in, *out;
	size_t inlen, incap, outlen, outcap;
	char line[1024];
	size_t linelen;
	Match *match;
	int nmatch;
	uid_t uid;
	long pid;
};

typedef struct Name {
	char *name;
	Conn *owner;
	int allow_replace;
	Conn *queue[MAX_QUEUE];
	int qflags[MAX_QUEUE];
	int nq;
} Name;

typedef struct Waiting {                /* messages for a starting service */
	char *name;
	unsigned long from;                /* Conn id                       */
	DMsg msg;
	long long deadline;
	struct Waiting *next;
} Waiting;

typedef struct Service { char *name, *exec; } Service;

static Conn *conns[MAX_CONNS];
static Name *names;
static int nnames, capnames;
static Waiting *waiting;
static Service *services;
static int nservices;
static char **act_env;                 /* UpdateActivationEnvironment    */
static int nact_env;
static unsigned long next_conn = 1;
static uint32_t bus_serial = 1;
static char guid[33], machine_id[33], sock_path[256];
static int lfd = -1, debug;
static volatile sig_atomic_t stop, got_child;
const char *bus_address;

static void route(Conn *from, DMsg *m);

/* ----------------------------------------------------------------- utils */

static void
hexid(char *out, const unsigned char *seed, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < 16; i++) {
		unsigned v = seed[i % n];

		out[i * 2] = hx[v >> 4];
		out[i * 2 + 1] = hx[v & 15];
	}
	out[32] = '\0';
}

static void
make_ids(void)
{
	unsigned char rnd[16];
	FILE *f;
	char buf[64];
	int i;

	f = fopen("/dev/urandom", "rb");
	if (!f || fread(rnd, 1, 16, f) != 16) {
		unsigned long v = (unsigned long)time(NULL) ^ ((unsigned long)getpid()
		    << 16);

		for (i = 0; i < 16; i++) {
			v = v * 1103515245UL + 12345UL;
			rnd[i] = (unsigned char)(v >> 16);
		}
	}
	if (f)
		fclose(f);
	hexid(guid, rnd, 16);
	/* the machine id programs expect; made up if this system has none */
	machine_id[0] = '\0';
	if ((f = fopen("/etc/machine-id", "r")) ||
	    (f = fopen("/var/lib/dbus/machine-id", "r"))) {
		if (fgets(buf, sizeof buf, f) && strlen(buf) >= 32) {
			memcpy(machine_id, buf, 32);
			machine_id[32] = '\0';
		}
		fclose(f);
	}
	if (!machine_id[0]) {
		unsigned char h[16];

		memset(h, 0, sizeof h);
		if (gethostname(buf, sizeof buf) == 0)
			for (i = 0; buf[i]; i++)
				h[i % 16] = (unsigned char)(h[i % 16] * 31 + (unsigned char)buf[i]);
		hexid(machine_id, h, 16);
	}
}

static Conn *
conn_by_id(unsigned long id)
{
	int i;

	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i] && conns[i]->id == id && !conns[i]->dead)
			return conns[i];
	return NULL;
}

static Name *
name_find(const char *n)
{
	int i;

	for (i = 0; i < nnames; i++)
		if (!strcmp(names[i].name, n))
			return &names[i];
	return NULL;
}

/* the connection a (unique or well-known) name stands for */
static Conn *
resolve(const char *n)
{
	int i;
	Name *nm;

	if (!n)
		return NULL;
	if (n[0] == ':') {
		for (i = 0; i < MAX_CONNS; i++)
			if (conns[i] && !conns[i]->dead && conns[i]->hello &&
			    !strcmp(conns[i]->unique, n))
				return conns[i];
		return NULL;
	}
	nm = name_find(n);
	return nm ? nm->owner : NULL;
}

const char *
bus_unique(const Conn *c)
{
	return c->unique;
}

/* ---------------------------------------------------------------- output */

static void
conn_put(Conn *c, const unsigned char *p, size_t n)
{
	if (c->fd < 0 || c->dead)
		return;
	if (c->outlen + n > OUT_LIMIT) {
		c->dead = 1;
		return;
	}
	if (c->outlen + n > c->outcap) {
		while (c->outlen + n > c->outcap)
			c->outcap = c->outcap ? c->outcap * 2 : 8192;
		c->out = rui_realloc(c->out, c->outcap);
	}
	memcpy(c->out + c->outlen, p, n);
	c->outlen += n;
}

static void
deliver(Conn *to, DMsg *m)
{
	DBuf b;

	if (!to || to->dead)
		return;
	if (to->fn) {
		to->fn(to, m);
		return;
	}
	db_init(&b, m->big);
	dmsg_write(m, &b);
	conn_put(to, b.p, b.len);
	db_free(&b);
}

static void
flush_conn(Conn *c)
{
	while (c->outlen && !c->dead) {
#ifdef MSG_NOSIGNAL
		ssize_t r = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL);
#else
		ssize_t r = send(c->fd, c->out, c->outlen, 0);
#endif

		if (r > 0) {
			memmove(c->out, c->out + r, c->outlen - (size_t)r);
			c->outlen -= (size_t)r;
		} else if (r < 0 && errno == EINTR) {
			continue;
		} else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		} else {
			c->dead = 1;
		}
	}
}

/* a message from the bus driver itself */
static void
driver_msg(DMsg *m, const char *dest)
{
	Conn *to = dest ? resolve(dest) : NULL;
	int i;

	m->serial = bus_serial++;
	m->sender = rui_strdup(DRIVER);
	if (dest)
		m->dest = rui_strdup(dest);
	if (m->type == DM_SIGNAL && !dest) {
		route(NULL, m);
		return;
	}
	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i] && conns[i]->monitor && conns[i] != to)
			deliver(conns[i], m);
	deliver(to, m);
}

static void
driver_reply(Conn *to, const DMsg *call, const char *sig, const DBuf *body)
{
	DMsg r;

	if (call->flags & DF_NO_REPLY)
		return;
	memset(&r, 0, sizeof r);
	r.type = DM_RETURN;
	r.reply_serial = call->serial;
	r.sig = rui_strdup(sig ? sig : "");
	if (body && body->len) {
		r.body = rui_malloc(body->len);
		memcpy(r.body, body->p, body->len);
		r.body_len = body->len;
	}
	driver_msg(&r, to->unique);
	dmsg_free(&r);
}

static void
driver_error(Conn *to, const DMsg *call, const char *name, const char *text)
{
	DMsg r;
	DBuf b;

	if (call->flags & DF_NO_REPLY)
		return;
	memset(&r, 0, sizeof r);
	r.type = DM_ERROR;
	r.reply_serial = call->serial;
	r.error = rui_strdup(name);
	r.sig = rui_strdup("s");
	db_init(&b, 0);
	db_str(&b, text);
	r.body = b.p;
	r.body_len = b.len;
	driver_msg(&r, to->hello ? to->unique : NULL);
	if (!to->hello)                        /* before Hello: straight to it */
		deliver(to, &r);
	dmsg_free(&r);
}

static void
driver_signal(const char *dest, const char *member, const char *sig,
    const DBuf *body)
{
	DMsg s;

	memset(&s, 0, sizeof s);
	s.type = DM_SIGNAL;
	s.path = rui_strdup(DPATH);
	s.iface = rui_strdup(DRIVER);
	s.member = rui_strdup(member);
	s.sig = rui_strdup(sig);
	s.body = rui_malloc(body->len + 1);
	memcpy(s.body, body->p, body->len);
	s.body_len = body->len;
	driver_msg(&s, dest);
	dmsg_free(&s);
}

static void
owner_changed(const char *name, const char *old, const char *new_)
{
	DBuf b;

	db_init(&b, 0);
	db_str(&b, name);
	db_str(&b, old ? old : "");
	db_str(&b, new_ ? new_ : "");
	driver_signal(NULL, "NameOwnerChanged", "sss", &b);
	db_free(&b);
	if (old && *old && resolve(old)) {
		db_init(&b, 0);
		db_str(&b, name);
		driver_signal(old, "NameLost", "s", &b);
		db_free(&b);
	}
	if (new_ && *new_) {
		db_init(&b, 0);
		db_str(&b, name);
		driver_signal(new_, "NameAcquired", "s", &b);
		db_free(&b);
	}
}

/* ---------------------------------------------------------- built-ins */

Conn *
bus_builtin(BuiltinFn fn)
{
	Conn *c = rui_calloc(1, sizeof *c);
	int i;

	c->fd = -1;
	c->fn = fn;
	c->hello = 1;
	c->state = RUNNING;
	c->id = next_conn++;
	c->uid = getuid();
	c->pid = (long)getpid();
	c->serial = 1;
	snprintf(c->unique, sizeof c->unique, ":1.%lu", c->id);
	for (i = 0; i < MAX_CONNS && conns[i]; i++)
		;
	if (i < MAX_CONNS)
		conns[i] = c;
	owner_changed(c->unique, "", c->unique);
	return c;
}

void
bus_send(Conn *c, DMsg *m)
{
	m->serial = c->serial++;
	route(c, m);
}

void
bus_reply(Conn *c, const DMsg *call, const char *sig, const DBuf *body)
{
	DMsg r;

	if (call->flags & DF_NO_REPLY)
		return;
	memset(&r, 0, sizeof r);
	r.type = DM_RETURN;
	r.reply_serial = call->serial;
	r.dest = rui_strdup(call->sender);
	r.sig = rui_strdup(sig ? sig : "");
	if (body && body->len) {
		r.body = rui_malloc(body->len);
		memcpy(r.body, body->p, body->len);
		r.body_len = body->len;
	}
	bus_send(c, &r);
	dmsg_free(&r);
}

void
bus_error(Conn *c, const DMsg *call, const char *name, const char *text)
{
	DMsg r;
	DBuf b;

	if (call->flags & DF_NO_REPLY)
		return;
	memset(&r, 0, sizeof r);
	r.type = DM_ERROR;
	r.reply_serial = call->serial;
	r.dest = rui_strdup(call->sender);
	r.error = rui_strdup(name);
	r.sig = rui_strdup("s");
	db_init(&b, 0);
	db_str(&b, text);
	r.body = b.p;
	r.body_len = b.len;
	bus_send(c, &r);
	dmsg_free(&r);
}

void
bus_signal(Conn *c, const char *dest, const char *path, const char *iface,
    const char *member, const char *sig, const DBuf *body)
{
	DMsg s;

	memset(&s, 0, sizeof s);
	s.type = DM_SIGNAL;
	s.path = rui_strdup(path);
	s.iface = rui_strdup(iface);
	s.member = rui_strdup(member);
	s.sig = rui_strdup(sig ? sig : "");
	if (dest)
		s.dest = rui_strdup(dest);
	if (body && body->len) {
		s.body = rui_malloc(body->len);
		memcpy(s.body, body->p, body->len);
		s.body_len = body->len;
	}
	bus_send(c, &s);
	dmsg_free(&s);
}

/* ------------------------------------------------------------------ names */

static Name *
name_add(const char *n)
{
	Name *nm;

	if (nnames == capnames) {
		capnames = capnames ? capnames * 2 : 32;
		names = rui_realloc(names, (size_t)capnames * sizeof *names);
	}
	nm = &names[nnames++];
	memset(nm, 0, sizeof *nm);
	nm->name = rui_strdup(n);
	return nm;
}

static void
name_drop(Name *nm)
{
	free(nm->name);
	*nm = names[--nnames];
}

static void deliver_waiting(const char *name);

/* RequestName: 1 primary owner, 2 in queue, 3 exists, 4 already owner */
static int
request_name(Conn *c, const char *n, unsigned flags)
{
	Name *nm = name_find(n);
	int i;

	if (!nm) {
		nm = name_add(n);
		nm->owner = c;
		nm->allow_replace = flags & 1;
		owner_changed(n, "", c->unique);
		deliver_waiting(n);
		return 1;
	}
	if (nm->owner == c) {
		nm->allow_replace = flags & 1;
		return 4;
	}
	if ((flags & 2) && nm->allow_replace) {
		Conn *old = nm->owner;

		/* the old owner waits in the queue unless it asked not to */
		nm->owner = c;
		nm->allow_replace = flags & 1;
		for (i = 0; i < nm->nq; i++)
			if (nm->queue[i] == c) {
				memmove(&nm->queue[i], &nm->queue[i + 1],
				    (size_t)(nm->nq - i - 1) * sizeof nm->queue[0]);
				memmove(&nm->qflags[i], &nm->qflags[i + 1],
				    (size_t)(nm->nq - i - 1) * sizeof nm->qflags[0]);
				nm->nq--;
			}
		owner_changed(n, old->unique, c->unique);
		return 1;
	}
	if (flags & 4)
		return 3;
	for (i = 0; i < nm->nq; i++)
		if (nm->queue[i] == c)
			return 2;
	if (nm->nq == MAX_QUEUE)
		return 3;
	nm->queue[nm->nq] = c;
	nm->qflags[nm->nq++] = (int)flags;
	return 2;
}

/* 1 released, 2 non-existent, 3 not owner */
static int
release_name(Conn *c, const char *n)
{
	Name *nm = name_find(n);
	int i;

	if (!nm)
		return 2;
	if (nm->owner != c) {
		for (i = 0; i < nm->nq; i++)
			if (nm->queue[i] == c) {
				memmove(&nm->queue[i], &nm->queue[i + 1],
				    (size_t)(nm->nq - i - 1) * sizeof nm->queue[0]);
				memmove(&nm->qflags[i], &nm->qflags[i + 1],
				    (size_t)(nm->nq - i - 1) * sizeof nm->qflags[0]);
				nm->nq--;
				return 1;
			}
		return 3;
	}
	if (nm->nq) {
		Conn *next = nm->queue[0];

		nm->owner = next;
		nm->allow_replace = nm->qflags[0] & 1;
		memmove(&nm->queue[0], &nm->queue[1], (size_t)(nm->nq - 1) *
		    sizeof nm->queue[0]);
		memmove(&nm->qflags[0], &nm->qflags[1], (size_t)(nm->nq - 1) *
		    sizeof nm->qflags[0]);
		nm->nq--;
		owner_changed(n, c->unique, next->unique);
	} else {
		char *copy = rui_strdup(n);

		name_drop(nm);
		owner_changed(copy, c->unique, "");
		free(copy);
	}
	return 1;
}

int
bus_own(Conn *c, const char *name)
{
	return request_name(c, name, 4) == 1 ? 0 : -1;
}

/* ---------------------------------------------------------- match rules */

static void
match_free(Match *m)
{
	int i;

	free(m->sender);
	free(m->iface);
	free(m->member);
	free(m->path);
	free(m->path_ns);
	free(m->dest);
	free(m->arg0ns);
	for (i = 0; i < 4; i++) {
		free(m->arg[i]);
		free(m->argpath[i]);
	}
	free(m->text);
}

/* "type='signal',interface='a.b'" ; 0 ok */
static int
match_parse(const char *s, Match *m)
{
	const char *p = s;

	memset(m, 0, sizeof *m);
	m->text = rui_strdup(s);
	while (*p) {
		char key[32], val[512];
		size_t k = 0, v = 0;

		while (*p == ' ' || *p == ',')
			p++;
		if (!*p)
			break;
		while (*p && *p != '=' && k < sizeof key - 1)
			key[k++] = *p++;
		key[k] = '\0';
		if (*p++ != '=')
			goto bad;
		/* values are quoted; \' and \\ work inside, quotes join */
		while (*p && *p != ',') {
			if (*p == '\'') {
				p++;
				while (*p && *p != '\'' && v < sizeof val - 1)
					val[v++] = *p++;
				if (*p != '\'')
					goto bad;
				p++;
			} else if (*p == '\\' && p[1] == '\'') {
				if (v < sizeof val - 1)
					val[v++] = '\'';
				p += 2;
			} else {
				if (v < sizeof val - 1)
					val[v++] = *p;
				p++;
			}
		}
		val[v] = '\0';
		if (!strcmp(key, "type")) {
			m->type = !strcmp(val, "method_call") ? DM_CALL :
			    !strcmp(val, "method_return") ? DM_RETURN :
			    !strcmp(val, "error") ? DM_ERROR :
			    !strcmp(val, "signal") ? DM_SIGNAL : -1;
			if (m->type < 0)
				goto bad;
		} else if (!strcmp(key, "sender")) {
			m->sender = rui_strdup(val);
		} else if (!strcmp(key, "interface")) {
			m->iface = rui_strdup(val);
		} else if (!strcmp(key, "member")) {
			m->member = rui_strdup(val);
		} else if (!strcmp(key, "path")) {
			m->path = rui_strdup(val);
		} else if (!strcmp(key, "path_namespace")) {
			m->path_ns = rui_strdup(val);
		} else if (!strcmp(key, "destination")) {
			m->dest = rui_strdup(val);
		} else if (!strcmp(key, "arg0namespace")) {
			m->arg0ns = rui_strdup(val);
		} else if (!strcmp(key, "eavesdrop")) {
			;                              /* not supported, harmless */
		} else if (!strncmp(key, "arg", 3) && key[3] >= '0' && key[3] <= '9') {
			char *end;
			long n = strtol(key + 3, &end, 10);

			if (n < 0 || n > 63)
				goto bad;
			if (n >= 4) {
				;                          /* rare: accepted, not checked */
			} else if (!strcmp(end, "path")) {
				m->argpath[n] = rui_strdup(val);
			} else if (!*end) {
				m->arg[n] = rui_strdup(val);
			} else {
				goto bad;
			}
		} else {
			goto bad;
		}
	}
	return 0;
bad:
	match_free(m);
	return -1;
}

/* does sender (a Conn) count as the rule's sender name? */
static int
sender_is(const char *want, const Conn *from)
{
	Name *nm;

	if (!from)                             /* the driver itself */
		return !strcmp(want, DRIVER);
	if (want[0] == ':')
		return !strcmp(want, from->unique);
	nm = name_find(want);
	return nm && nm->owner == from;
}

static int
path_in(const char *ns, const char *p)
{
	size_t n = strlen(ns);

	if (!strcmp(ns, "/"))
		return 1;
	return !strncmp(p, ns, n) && (p[n] == '\0' || p[n] == '/');
}

static int
arg_path_match(const char *rule, const char *v)
{
	size_t a = strlen(rule), b = strlen(v);

	if (!strcmp(rule, v))
		return 1;
	if (a && rule[a - 1] == '/' && !strncmp(rule, v, a))
		return 1;
	if (b && v[b - 1] == '/' && !strncmp(rule, v, b))
		return 1;
	return 0;
}

static int
match_msg(const Match *r, const DMsg *m, const Conn *from)
{
	int i, need = -1;

	if (r->type && r->type != m->type)
		return 0;
	if (r->sender && !sender_is(r->sender, from))
		return 0;
	if (r->iface && (!m->iface || strcmp(r->iface, m->iface)))
		return 0;
	if (r->member && (!m->member || strcmp(r->member, m->member)))
		return 0;
	if (r->path && (!m->path || strcmp(r->path, m->path)))
		return 0;
	if (r->path_ns && (!m->path || !path_in(r->path_ns, m->path)))
		return 0;
	if (r->dest && (!m->dest || strcmp(r->dest, m->dest)))
		return 0;
	for (i = 0; i < 4; i++)
		if (r->arg[i] || r->argpath[i])
			need = i;
	if (r->arg0ns && need < 0)
		need = 0;
	if (need >= 0) {
		DRd rd;
		const char *sig = m->sig;
		char val[1024];

		dr_init(&rd, m->body, m->body_len, m->big);
		for (i = 0; i <= need; i++) {
			int is_str = *sig == 's' || *sig == 'o';

			if (!*sig)
				return 0;
			if (dr_value_text(&rd, &sig, val, sizeof val) < 0)
				return 0;
			if (r->arg[i] && (!is_str || strcmp(r->arg[i], val)))
				return 0;
			if (r->argpath[i] && (!is_str || !arg_path_match(r->argpath[i],
			    val)))
				return 0;
			if (i == 0 && r->arg0ns) {
				size_t n = strlen(r->arg0ns);

				if (!is_str || strncmp(val, r->arg0ns, n) ||
				    (val[n] && val[n] != '.'))
					return 0;
			}
		}
	}
	return 1;
}

/* ------------------------------------------------------------ activation */

static void
read_service_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[2048], *name = NULL, *exec = NULL;
	int in_section = 0, i;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *v;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_section = !strcmp(line, "[D-BUS Service]");
			continue;
		}
		if (!in_section || !(v = strchr(line, '=')))
			continue;
		*v++ = '\0';
		if (!strcmp(rui_rtrim(line), "Name") && !name)
			name = rui_strdup(rui_ltrim(v));
		else if (!strcmp(rui_rtrim(line), "Exec") && !exec)
			exec = rui_strdup(rui_ltrim(v));
	}
	fclose(f);
	if (!name || !exec || !valid_name(name)) {
		free(name);
		free(exec);
		return;
	}
	for (i = 0; i < nservices; i++)        /* the first directory wins */
		if (!strcmp(services[i].name, name)) {
			free(name);
			free(exec);
			return;
		}
	services = rui_realloc(services, (size_t)(nservices + 1) *
	    sizeof *services);
	services[nservices].name = name;
	services[nservices].exec = exec;
	nservices++;
}

static void
scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d))) {
		size_t n = strlen(de->d_name);
		char *p;

		if (n < 9 || strcmp(de->d_name + n - 8, ".service"))
			continue;
		p = rui_strdupf("%s/%s", dir, de->d_name);
		read_service_file(p);
		free(p);
	}
	if (d)
		closedir(d);
}

static void
scan_services(void)
{
	const char *home = getenv("HOME"), *dh = getenv("XDG_DATA_HOME"),
	    *dd = getenv("XDG_DATA_DIRS");
	char *p;
	int i;

	for (i = 0; i < nservices; i++) {
		free(services[i].name);
		free(services[i].exec);
	}
	free(services);
	services = NULL;
	nservices = 0;
	if (dh && *dh)
		p = rui_strdupf("%s/dbus-1/services", dh);
	else
		p = rui_strdupf("%s/.local/share/dbus-1/services", home ? home : "/");
	scan_dir(p);
	free(p);
	if (!dd || !*dd)
		dd = "/usr/local/share:/usr/share";
	while (*dd) {
		size_t n = strcspn(dd, ":");

		if (n) {
			p = rui_strdupf("%.*s/dbus-1/services", (int)n, dd);
			scan_dir(p);
			free(p);
		}
		dd += n;
		if (*dd == ':')
			dd++;
	}
}

static Service *
service_find(const char *name)
{
	int i, round;

	for (round = 0; round < 2; round++) {
		for (i = 0; i < nservices; i++)
			if (!strcmp(services[i].name, name))
				return &services[i];
		if (round == 0)
			scan_services();               /* installed since? */
	}
	return NULL;
}

/* starts the program behind a name; 0 started (or already starting) */
static int
activate(const char *name)
{
	Service *s = service_find(name);
	Waiting *w;
	pid_t pid;

	for (w = waiting; w; w = w->next)
		if (!strcmp(w->name, name))
			return 0;                      /* on its way */
	if (!s || !strncmp(s->exec, "/bin/false", 10) ||
	    !strcmp(s->exec, "false"))
		return -1;
	if (debug)
		fprintf(stderr, "retro-dbus: activating %s: %s\n", name, s->exec);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int i;

		setsid();
		signal(SIGPIPE, SIG_DFL);
		setenv("DBUS_STARTER_ADDRESS", bus_address, 1);
		setenv("DBUS_STARTER_BUS_TYPE", "session", 1);
		for (i = 0; i + 1 < nact_env; i += 2)
			setenv(act_env[i], act_env[i + 1], 1);
		execl("/bin/sh", "sh", "-c", s->exec, (char *)NULL);
		_exit(127);
	}
	return 0;
}

static void
wait_add(const char *name, Conn *from, DMsg *m)
{
	Waiting *w = rui_calloc(1, sizeof *w);

	w->name = rui_strdup(name);
	w->from = from ? from->id : 0;
	w->msg = *m;                           /* takes the message over */
	memset(m, 0, sizeof *m);
	w->deadline = rui_now_ms() + ACTIVATION_MS;
	w->next = waiting;
	waiting = w;
}

static void
deliver_waiting(const char *name)
{
	Waiting **pp = &waiting, *w;
	Waiting *list = NULL, **tail = &list;

	/* oldest first: the list is newest first */
	while ((w = *pp)) {
		if (!strcmp(w->name, name)) {
			*pp = w->next;
			w->next = list;
			list = w;
		} else {
			pp = &w->next;
		}
	}
	(void)tail;
	while ((w = list)) {
		Conn *from = conn_by_id(w->from);

		list = w->next;
		if (from)
			route(from, &w->msg);
		dmsg_free(&w->msg);
		free(w->name);
		free(w);
	}
}

static void
expire_waiting(long long now)
{
	Waiting **pp = &waiting, *w;

	while ((w = *pp)) {
		if (now < w->deadline) {
			pp = &w->next;
			continue;
		}
		*pp = w->next;
		{
			Conn *from = conn_by_id(w->from);
			char text[400];

			snprintf(text, sizeof text, "%s did not start in time", w->name);
			if (from && w->msg.type == DM_CALL)
				driver_error(from, &w->msg,
				    "org.freedesktop.DBus.Error.TimedOut", text);
		}
		dmsg_free(&w->msg);
		free(w->name);
		free(w);
	}
}

/* -------------------------------------------------------------- driver */

static const char introspect_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection "
    "1.0//EN\"\n\"http://www.freedesktop.org/standards/dbus/1.0/"
    "introspect.dtd\">\n<node>\n"
    " <interface name=\"org.freedesktop.DBus\">\n"
    "  <method name=\"Hello\"><arg direction=\"out\" type=\"s\"/></method>\n"
    "  <method name=\"RequestName\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"u\"/><arg direction=\"out\" type=\"u\"/>"
    "</method>\n"
    "  <method name=\"ReleaseName\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"out\" type=\"u\"/></method>\n"
    "  <method name=\"ListNames\"><arg direction=\"out\" type=\"as\"/>"
    "</method>\n"
    "  <method name=\"GetNameOwner\"><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"out\" type=\"s\"/></method>\n"
    "  <method name=\"AddMatch\"><arg direction=\"in\" type=\"s\"/></method>\n"
    "  <method name=\"RemoveMatch\"><arg direction=\"in\" type=\"s\"/>"
    "</method>\n"
    "  <signal name=\"NameOwnerChanged\"><arg type=\"s\"/><arg type=\"s\"/>"
    "<arg type=\"s\"/></signal>\n"
    "  <signal name=\"NameLost\"><arg type=\"s\"/></signal>\n"
    "  <signal name=\"NameAcquired\"><arg type=\"s\"/></signal>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "  <method name=\"Introspect\"><arg direction=\"out\" type=\"s\"/>"
    "</method>\n </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "  <method name=\"Ping\"/>\n  <method name=\"GetMachineId\">"
    "<arg direction=\"out\" type=\"s\"/></method>\n </interface>\n"
    "</node>\n";

static void
reply_u32(Conn *c, const DMsg *m, uint32_t v)
{
	DBuf b;

	db_init(&b, 0);
	db_u32(&b, v);
	driver_reply(c, m, "u", &b);
	db_free(&b);
}

static void
reply_str(Conn *c, const DMsg *m, const char *sig, const char *s)
{
	DBuf b;

	db_init(&b, 0);
	db_str(&b, s);
	driver_reply(c, m, sig, &b);
	db_free(&b);
}

static void
reply_names(Conn *c, const DMsg *m, int activatable)
{
	DBuf b;
	size_t mark;
	int i;

	db_init(&b, 0);
	mark = db_array_begin(&b, 4);
	db_str(&b, DRIVER);
	if (activatable) {
		scan_services();
		for (i = 0; i < nservices; i++)
			db_str(&b, services[i].name);
	} else {
		for (i = 0; i < MAX_CONNS; i++)
			if (conns[i] && conns[i]->hello && !conns[i]->dead)
				db_str(&b, conns[i]->unique);
		for (i = 0; i < nnames; i++)
			db_str(&b, names[i].name);
	}
	db_array_end(&b, mark);
	driver_reply(c, m, "as", &b);
	db_free(&b);
}

/* the first string argument; NULL (and an error sent) if it is missing */
static const char *
arg_str(Conn *c, const DMsg *m, DRd *rd)
{
	const char *s;

	if (strncmp(m->sig, "s", 1)) {
		driver_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
		    "a string argument was expected");
		return NULL;
	}
	dr_init(rd, m->body, m->body_len, m->big);
	s = dr_str(rd);
	if (rd->err) {
		driver_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
		    "bad message body");
		return NULL;
	}
	return s;
}

static void
no_owner(Conn *c, const DMsg *m, const char *n)
{
	char text[400];

	snprintf(text, sizeof text, "Could not get owner of name '%s': no such "
	    "name", n);
	driver_error(c, m, "org.freedesktop.DBus.Error.NameHasNoOwner", text);
}

static void
driver(Conn *c, DMsg *m)
{
	const char *mem = m->member, *s;
	DRd rd;
	DBuf b;
	Conn *t;
	int i;

	if (m->type != DM_CALL)
		return;
	if (!strcmp(mem, "Hello")) {
		if (c->hello) {
			driver_error(c, m, "org.freedesktop.DBus.Error.Failed",
			    "Already handled an Hello message");
			return;
		}
		c->hello = 1;
		snprintf(c->unique, sizeof c->unique, ":1.%lu", c->id);
		reply_str(c, m, "s", c->unique);
		owner_changed(c->unique, "", c->unique);
		return;
	}
	if (!c->hello) {
		driver_error(c, m, "org.freedesktop.DBus.Error.AccessDenied",
		    "Client tried to send a message other than Hello without "
		    "being registered");
		c->dead = 1;
		return;
	}
	if (m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Peer")) {
		if (!strcmp(mem, "Ping"))
			driver_reply(c, m, "", NULL);
		else if (!strcmp(mem, "GetMachineId"))
			reply_str(c, m, "s", machine_id);
		else
			driver_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod",
			    "no such method");
		return;
	}
	if (!strcmp(mem, "Introspect")) {
		reply_str(c, m, "s", introspect_xml);
	} else if (!strcmp(mem, "RequestName")) {
		uint32_t flags;

		if (strcmp(m->sig, "su")) {
			driver_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "RequestName takes (su)");
			return;
		}
		dr_init(&rd, m->body, m->body_len, m->big);
		s = dr_str(&rd);
		flags = dr_u32(&rd);
		if (rd.err || !valid_name(s) || s[0] == ':' || !strcmp(s, DRIVER)) {
			driver_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "cannot request that name");
			return;
		}
		/* reply first, then the signals, as dbus-daemon does */
		{
			char *copy = rui_strdup(s);
			Name *nm = name_find(copy);
			int r;

			if (!nm) {
				reply_u32(c, m, 1);
				request_name(c, copy, flags);
			} else {
				r = request_name(c, copy, flags);
				reply_u32(c, m, (uint32_t)r);
			}
			free(copy);
		}
	} else if (!strcmp(mem, "ReleaseName")) {
		if ((s = arg_str(c, m, &rd))) {
			char *copy = rui_strdup(s);
			Name *nm = name_find(copy);
			int r = !nm ? 2 : nm->owner != c ? release_name(c, copy) : 1;

			reply_u32(c, m, (uint32_t)r);
			if (nm && nm->owner == c)
				release_name(c, copy);
			free(copy);
		}
	} else if (!strcmp(mem, "ListNames")) {
		reply_names(c, m, 0);
	} else if (!strcmp(mem, "ListActivatableNames")) {
		reply_names(c, m, 1);
	} else if (!strcmp(mem, "ListQueuedOwners")) {
		if ((s = arg_str(c, m, &rd))) {
			Name *nm = name_find(s);
			size_t mark;

			if (!nm && !(t = resolve(s))) {
				no_owner(c, m, s);
				return;
			}
			db_init(&b, 0);
			mark = db_array_begin(&b, 4);
			if (nm) {
				db_str(&b, nm->owner->unique);
				for (i = 0; i < nm->nq; i++)
					db_str(&b, nm->queue[i]->unique);
			} else {
				db_str(&b, s);
			}
			db_array_end(&b, mark);
			driver_reply(c, m, "as", &b);
			db_free(&b);
		}
	} else if (!strcmp(mem, "NameHasOwner")) {
		if ((s = arg_str(c, m, &rd))) {
			db_init(&b, 0);
			db_bool(&b, !strcmp(s, DRIVER) || resolve(s) != NULL);
			driver_reply(c, m, "b", &b);
			db_free(&b);
		}
	} else if (!strcmp(mem, "GetNameOwner")) {
		if ((s = arg_str(c, m, &rd))) {
			if (!strcmp(s, DRIVER))
				reply_str(c, m, "s", DRIVER);
			else if ((t = resolve(s)))
				reply_str(c, m, "s", t->unique);
			else
				no_owner(c, m, s);
		}
	} else if (!strcmp(mem, "StartServiceByName")) {
		if ((s = arg_str(c, m, &rd))) {
			if (resolve(s))
				reply_u32(c, m, 2);
			else if (activate(s) == 0)
				reply_u32(c, m, 1);
			else {
				char text[400];

				snprintf(text, sizeof text, "The name %s was not provided "
				    "by any .service files", s);
				driver_error(c, m, "org.freedesktop.DBus.Error.ServiceUnknown",
				    text);
			}
		}
	} else if (!strcmp(mem, "UpdateActivationEnvironment")) {
		if (strcmp(m->sig, "a{ss}")) {
			driver_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
			    "takes a{ss}");
			return;
		}
		dr_init(&rd, m->body, m->body_len, m->big);
		{
			uint32_t n = dr_u32(&rd);
			size_t start;

			dr_align(&rd, 8);
			start = rd.pos;
			while (!rd.err && rd.pos < start + n) {
				const char *k, *v;
				int j;

				dr_align(&rd, 8);
				k = dr_str(&rd);
				v = dr_str(&rd);
				if (rd.err)
					break;
				for (j = 0; j + 1 < nact_env; j += 2)
					if (!strcmp(act_env[j], k))
						break;
				if (j + 1 >= nact_env) {
					act_env = rui_realloc(act_env, (size_t)(nact_env + 2) *
					    sizeof *act_env);
					act_env[nact_env++] = rui_strdup(k);
					act_env[nact_env++] = rui_strdup(v);
				} else {
					free(act_env[j + 1]);
					act_env[j + 1] = rui_strdup(v);
				}
			}
		}
		driver_reply(c, m, "", NULL);
	} else if (!strcmp(mem, "GetConnectionUnixUser") ||
	    !strcmp(mem, "GetConnectionUnixProcessID")) {
		if ((s = arg_str(c, m, &rd))) {
			if (!(t = resolve(s))) {
				no_owner(c, m, s);
				return;
			}
			if (mem[16] == 'P' && t->pid <= 0) {
				driver_error(c, m,
				    "org.freedesktop.DBus.Error.UnixProcessIdUnknown",
				    "unknown process");
				return;
			}
			reply_u32(c, m, mem[16] == 'P' ? (uint32_t)t->pid :
			    (uint32_t)t->uid);
		}
	} else if (!strcmp(mem, "GetConnectionCredentials")) {
		if ((s = arg_str(c, m, &rd))) {
			size_t mark;

			if (!(t = resolve(s))) {
				no_owner(c, m, s);
				return;
			}
			db_init(&b, 0);
			mark = db_array_begin(&b, 8);
			db_pad(&b, 8);
			db_str(&b, "UnixUserID");
			db_variant_u32(&b, (uint32_t)t->uid);
			if (t->pid > 0) {
				db_pad(&b, 8);
				db_str(&b, "ProcessID");
				db_variant_u32(&b, (uint32_t)t->pid);
			}
			db_array_end(&b, mark);
			driver_reply(c, m, "a{sv}", &b);
			db_free(&b);
		}
	} else if (!strcmp(mem, "AddMatch")) {
		Match r;

		if (!(s = arg_str(c, m, &rd)))
			return;
		if (c->nmatch >= MAX_MATCH) {
			driver_error(c, m, "org.freedesktop.DBus.Error.LimitsExceeded",
			    "too many match rules");
			return;
		}
		if (match_parse(s, &r) < 0) {
			driver_error(c, m, "org.freedesktop.DBus.Error.MatchRuleInvalid",
			    "cannot parse the match rule");
			return;
		}
		c->match = rui_realloc(c->match, (size_t)(c->nmatch + 1) *
		    sizeof *c->match);
		c->match[c->nmatch++] = r;
		driver_reply(c, m, "", NULL);
	} else if (!strcmp(mem, "RemoveMatch")) {
		if (!(s = arg_str(c, m, &rd)))
			return;
		for (i = 0; i < c->nmatch; i++)
			if (!strcmp(c->match[i].text, s))
				break;
		if (i == c->nmatch) {
			driver_error(c, m, "org.freedesktop.DBus.Error.MatchRuleNotFound",
			    "no such match rule");
			return;
		}
		match_free(&c->match[i]);
		c->match[i] = c->match[--c->nmatch];
		driver_reply(c, m, "", NULL);
	} else if (!strcmp(mem, "GetId")) {
		reply_str(c, m, "s", guid);
	} else if (!strcmp(mem, "ReloadConfig")) {
		scan_services();
		driver_reply(c, m, "", NULL);
	} else if (!strcmp(mem, "BecomeMonitor")) {
		driver_reply(c, m, "", NULL);
		c->monitor = 1;
	} else if (!strcmp(mem, "Get") || !strcmp(mem, "GetAll")) {
		/* org.freedesktop.DBus.Properties: Features and Interfaces */
		size_t mark;

		db_init(&b, 0);
		if (mem[3] == 'A') {
			mark = db_array_begin(&b, 8);
			db_array_end(&b, mark);
			driver_reply(c, m, "a{sv}", &b);
		} else {
			db_sig(&b, "as");
			mark = db_array_begin(&b, 4);
			db_array_end(&b, mark);
			driver_reply(c, m, "v", &b);
		}
		db_free(&b);
	} else {
		char text[300];

		snprintf(text, sizeof text, "%s is not supported by retro-dbus", mem);
		driver_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", text);
	}
}

/* ---------------------------------------------------------------- routing */

static void
route(Conn *from, DMsg *m)
{
	Conn *to = NULL;
	int i;

	if (from) {
		/* the unique name exists from the Hello call on, so that even
		 * that call carries a valid sender */
		if (!from->unique[0])
			snprintf(from->unique, sizeof from->unique, ":1.%lu", from->id);
		free(m->sender);
		m->sender = rui_strdup(from->unique);
	}
	if (debug)
		fprintf(stderr, "retro-dbus: %s %s -> %s %s %s.%s\n",
		    m->type == DM_CALL ? "call" : m->type == DM_RETURN ? "return" :
		    m->type == DM_ERROR ? "error" : "signal", m->sender ? m->sender :
		    "?", m->dest ? m->dest : "*", m->path ? m->path : "",
		    m->iface ? m->iface : "", m->member ? m->member : m->error ?
		    m->error : "");
	if (from && m->dest && !strcmp(m->dest, DRIVER)) {
		for (i = 0; i < MAX_CONNS; i++)
			if (conns[i] && conns[i]->monitor)
				deliver(conns[i], m);
		driver(from, m);
		return;
	}
	if (m->dest) {
		to = resolve(m->dest);
		if (!to) {
			if (m->type == DM_CALL && from && m->dest[0] != ':' &&
			    !(m->flags & DF_NO_AUTO_START) && activate(m->dest) == 0) {
				DMsg copy = *m;

				/* keep a private copy: the caller frees m */
				copy.path = m->path ? rui_strdup(m->path) : NULL;
				copy.iface = m->iface ? rui_strdup(m->iface) : NULL;
				copy.member = m->member ? rui_strdup(m->member) : NULL;
				copy.error = m->error ? rui_strdup(m->error) : NULL;
				copy.dest = rui_strdup(m->dest);
				copy.sender = m->sender ? rui_strdup(m->sender) : NULL;
				copy.sig = rui_strdup(m->sig);
				copy.body = rui_malloc(m->body_len + 1);
				memcpy(copy.body, m->body, m->body_len);
				wait_add(m->dest, from, &copy);
				return;
			}
			if (m->type == DM_CALL && from) {
				char text[400];

				snprintf(text, sizeof text, "The name %s was not provided by "
				    "any .service files", m->dest);
				driver_error(from, m, m->dest[0] == ':' ?
				    "org.freedesktop.DBus.Error.NameHasNoOwner" :
				    "org.freedesktop.DBus.Error.ServiceUnknown", text);
			}
			return;
		}
	}
	for (i = 0; i < MAX_CONNS; i++) {
		Conn *c = conns[i];
		int k;

		if (!c || c->dead || !c->hello || c == to)
			continue;
		if (c->monitor) {
			deliver(c, m);
			continue;
		}
		if (m->type != DM_SIGNAL || m->dest)
			continue;
		for (k = 0; k < c->nmatch; k++)
			if (match_msg(&c->match[k], m, from)) {
				deliver(c, m);
				break;
			}
	}
	if (to)
		deliver(to, m);
}

/* ----------------------------------------------------------- connections */

static uid_t
peer_uid(int fd, long *pid)
{
#if defined(SO_PEERCRED) && defined(__linux__)
	struct ucred uc;
	socklen_t len = sizeof uc;

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0) {
		*pid = (long)uc.pid;
		return uc.uid;
	}
#else
	uid_t u;
	gid_t g;

	if (getpeereid(fd, &u, &g) == 0) {
		*pid = 0;
		return u;
	}
#endif
	*pid = 0;
	return (uid_t)-1;
}

static void
auth_send(Conn *c, const char *s)
{
	conn_put(c, (const unsigned char *)s, strlen(s));
}

/* one SASL line; returns 1 after BEGIN */
static int
auth_line(Conn *c, char *line)
{
	char ok[64];
	uid_t me = getuid();

	snprintf(ok, sizeof ok, "OK %s\r\n", guid);
	if (!strncmp(line, "AUTH EXTERNAL", 13)) {
		const char *hex = line + 13;
		char uidstr[32];
		size_t i, n;

		while (*hex == ' ')
			hex++;
		if (!*hex) {
			auth_send(c, "DATA\r\n");
			return 0;
		}
		n = strlen(hex) / 2;
		if (n >= sizeof uidstr)
			n = sizeof uidstr - 1;
		for (i = 0; i < n; i++) {
			unsigned v;

			if (sscanf(hex + i * 2, "%2x", &v) != 1)
				break;
			uidstr[i] = (char)v;
		}
		uidstr[i] = '\0';
		if (c->uid == me && (uid_t)strtoul(uidstr, NULL, 10) == me)
			auth_send(c, ok);
		else
			auth_send(c, "REJECTED EXTERNAL\r\n");
	} else if (!strncmp(line, "DATA", 4)) {
		/* empty data: the credentials of the socket */
		if (c->uid == me)
			auth_send(c, ok);
		else
			auth_send(c, "REJECTED EXTERNAL\r\n");
	} else if (!strcmp(line, "NEGOTIATE_UNIX_FD")) {
		auth_send(c, "ERROR \"file descriptor passing is not supported\"\r\n");
	} else if (!strcmp(line, "BEGIN")) {
		return 1;
	} else if (!strncmp(line, "AUTH", 4) || !strcmp(line, "CANCEL") ||
	    !strncmp(line, "ERROR", 5)) {
		auth_send(c, "REJECTED EXTERNAL\r\n");
	} else {
		auth_send(c, "ERROR \"unknown command\"\r\n");
	}
	return 0;
}

static void
conn_input(Conn *c)
{
	size_t pos = 0;

	if (c->state != RUNNING) {
		while (pos < c->inlen && c->state != RUNNING) {
			unsigned char ch = c->in[pos++];

			if (c->state == AUTH_NUL) {
				if (ch != 0) {
					c->dead = 1;
					return;
				}
				c->state = AUTH_LINES;
				continue;
			}
			if (ch == '\n') {
				if (c->linelen && c->line[c->linelen - 1] == '\r')
					c->linelen--;
				c->line[c->linelen] = '\0';
				c->linelen = 0;
				if (auth_line(c, c->line))
					c->state = RUNNING;
			} else if (c->linelen < sizeof c->line - 1) {
				c->line[c->linelen++] = (char)ch;
			} else {
				c->dead = 1;
				return;
			}
		}
	}
	while (c->state == RUNNING && !c->dead && c->inlen - pos >= 16) {
		size_t n = dmsg_size(c->in + pos);
		DMsg m;

		if (!n) {
			c->dead = 1;
			return;
		}
		if (c->inlen - pos < n)
			break;
		if (dmsg_parse(&m, c->in + pos, n) < 0) {
			if (debug)
				fprintf(stderr, "retro-dbus: bad message from %s\n",
				    c->unique);
			c->dead = 1;
			return;
		}
		pos += n;
		if (m.nfds) {
			driver_error(c, &m, "org.freedesktop.DBus.Error.NotSupported",
			    "file descriptor passing is not supported");
		} else if (!c->hello && (!m.dest || strcmp(m.dest, DRIVER) ||
		    m.type != DM_CALL || strcmp(m.member, "Hello"))) {
			driver_error(c, &m, "org.freedesktop.DBus.Error.AccessDenied",
			    "Hello first");
			c->dead = 1;
		} else if (!c->monitor) {
			route(c, &m);
		}
		dmsg_free(&m);
	}
	if (pos) {
		memmove(c->in, c->in + pos, c->inlen - pos);
		c->inlen -= pos;
	}
}

static void
conn_read(Conn *c)
{
	for (;;) {
		ssize_t r;

		if (c->incap - c->inlen < 8192) {
			c->incap = c->incap ? c->incap * 2 : 16384;
			if (c->incap > DBUS_MAX_MSG + 65536u) {
				c->dead = 1;
				return;
			}
			c->in = rui_realloc(c->in, c->incap);
		}
		r = read(c->fd, c->in + c->inlen, c->incap - c->inlen);
		if (r > 0) {
			c->inlen += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r == 0 || !(errno == EAGAIN || errno == EWOULDBLOCK))
			c->dead = 1;
		break;
	}
	conn_input(c);
}

static void
conn_drop(int slot)
{
	Conn *c = conns[slot];
	int i;

	c->dead = 1;
	/* its names go to the next in line, or away */
	for (i = 0; i < nnames; ) {
		Name *nm = &names[i];
		int k;

		for (k = 0; k < nm->nq; k++)
			if (nm->queue[k] == c) {
				memmove(&nm->queue[k], &nm->queue[k + 1],
				    (size_t)(nm->nq - k - 1) * sizeof nm->queue[0]);
				memmove(&nm->qflags[k], &nm->qflags[k + 1],
				    (size_t)(nm->nq - k - 1) * sizeof nm->qflags[0]);
				nm->nq--;
				k--;
			}
		if (nm->owner == c) {
			int before = nnames;

			release_name(c, nm->name);
			if (nnames < before)
				continue;                  /* the slot now holds another */
		}
		i++;
	}
	if (c->hello)
		owner_changed(c->unique, c->unique, "");
	if (c->fd >= 0)
		close(c->fd);
	for (i = 0; i < c->nmatch; i++)
		match_free(&c->match[i]);
	free(c->match);
	free(c->in);
	free(c->out);
	free(c);
	conns[slot] = NULL;
}

static void
accept_conns(void)
{
	int s, i, fl;

	while ((s = accept(lfd, NULL, NULL)) >= 0) {
		Conn *c;

		for (i = 0; i < MAX_CONNS && conns[i]; i++)
			;
		if (i == MAX_CONNS) {
			close(s);
			continue;
		}
		fcntl(s, F_SETFD, FD_CLOEXEC);
		if ((fl = fcntl(s, F_GETFL)) >= 0)
			fcntl(s, F_SETFL, fl | O_NONBLOCK);
		c = rui_calloc(1, sizeof *c);
		c->fd = s;
		c->id = next_conn++;
		c->uid = peer_uid(s, &c->pid);
		conns[i] = c;
	}
}

/* ------------------------------------------------------------------ main */

static void
on_signal(int sig)
{
	if (sig == SIGCHLD)
		got_child = 1;
	else
		stop = 1;
}

static int
open_socket(const char *path)
{
	struct sockaddr_un sa;
	char dir[256], *slash;
	struct stat st;
	int s, fl;
	mode_t old;

	if (strlen(path) >= sizeof sa.sun_path)
		return -1;
	rui_strlcpy(dir, path, sizeof dir);
	if ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0700);
		if (stat(dir, &st) < 0 || st.st_uid != getuid() || (st.st_mode & 077)) {
			fprintf(stderr, "retro-dbus: %s must be yours with mode 0700\n",
			    dir);
			return -1;
		}
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	rui_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
		close(s);
		return -2;                         /* one is running there */
	}
	close(s);
	unlink(path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	old = umask(077);
	if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s, 64) < 0) {
		umask(old);
		fprintf(stderr, "retro-dbus: %s: %s\n", path, strerror(errno));
		close(s);
		return -1;
	}
	umask(old);
	fcntl(s, F_SETFD, FD_CLOEXEC);
	if ((fl = fcntl(s, F_GETFL)) >= 0)
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
	return s;
}

/* the default address: beside retro-bus's socket */
static char *
default_path(void)
{
	char *bus = rui_bus_path(), *slash = strrchr(bus, '/'), *p;
	const char *disp = slash ? strstr(slash, "bus-") : NULL;

	if (!slash) {
		free(bus);
		return rui_strdupf("/tmp/retro-de-%lu/dbus", (unsigned long)getuid());
	}
	*slash = '\0';
	p = rui_strdupf("%s/dbus-%s", bus, disp ? disp + 4 : "0");
	free(bus);
	return p;
}

int
main(int argc, char **argv)
{
	static struct pollfd pfd[MAX_CONNS + 64];
	static int slot[MAX_CONNS + 64];
	struct sigaction sa;
	const char *addr = NULL, *dbg;
	char *path = NULL, *full;
	int i, print = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("retro-dbus " RUI_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
			addr = argv[++i];
		} else if (!strcmp(argv[i], "--print-address")) {
			print = 1;
		} else {
			fprintf(stderr, "usage: retro-dbus [-a unix:path=PATH] "
			    "[--print-address] [-v]\n");
			return 2;
		}
	}
	if (addr) {
		if (strncmp(addr, "unix:path=", 10)) {
			fprintf(stderr, "retro-dbus: only unix:path= addresses\n");
			return 2;
		}
		path = rui_strdup(addr + 10);
		if (strchr(path, ','))
			*strchr(path, ',') = '\0';
	} else {
		path = default_path();
	}
	rui_strlcpy(sock_path, path, sizeof sock_path);
	free(path);
	full = rui_strdupf("unix:path=%s", sock_path);
	bus_address = full;
	debug = (dbg = getenv("RETRO_DEBUG")) && *dbg && strcmp(dbg, "0");

	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	lfd = open_socket(sock_path);
	if (lfd == -2) {
		fprintf(stderr, "retro-dbus: already running on %s\n", sock_path);
		if (print)
			puts(full);
		return 0;
	}
	if (lfd < 0)
		return 1;
	setenv("DBUS_SESSION_BUS_ADDRESS", full, 1);
	make_ids();
	scan_services();
	services_init();
	if (print) {
		puts(full);
		fflush(stdout);
	}
#ifdef __OpenBSD__
	if (pledge("stdio rpath cpath unix proc exec", NULL) == -1) {
		perror("retro-dbus: pledge");
		unlink(sock_path);
		return 1;
	}
#endif

	while (!stop) {
		int n = 0, ns, r, perr, timeout = waiting ? 1000 : -1;

		pfd[n].fd = lfd;
		pfd[n].events = POLLIN;
		slot[n++] = -1;
		for (i = 0; i < MAX_CONNS; i++) {
			if (!conns[i] || conns[i]->fd < 0)
				continue;
			pfd[n].fd = conns[i]->fd;
			pfd[n].events = POLLIN | (conns[i]->outlen ? POLLOUT : 0);
			pfd[n].revents = 0;
			slot[n++] = i;
		}
		ns = services_fds(pfd + n, 64, &timeout);
		for (i = 0; i < ns; i++)
			pfd[n + i].revents = 0;
		r = poll(pfd, (nfds_t)(n + ns), timeout);
		perr = errno;                      /* waitpid below changes errno */
		if (got_child) {
			int status;
			pid_t pid;

			got_child = 0;
			while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
				services_child(pid, status);
		}
		if (r < 0) {
			if (perr == EINTR)
				continue;
			fprintf(stderr, "retro-dbus: poll: %s\n", strerror(perr));
			break;
		}
		for (i = 1; i < n; i++)
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR) &&
			    conns[slot[i]] && !conns[slot[i]]->dead)
				conn_read(conns[slot[i]]);
		if (pfd[0].revents & POLLIN)
			accept_conns();
		services_handle(pfd + n, ns);
		expire_waiting(rui_now_ms());
		for (;;) {
			int again = 0;

			for (i = 0; i < MAX_CONNS; i++)
				if (conns[i] && conns[i]->fd >= 0 && !conns[i]->dead)
					flush_conn(conns[i]);
			for (i = 0; i < MAX_CONNS; i++)
				if (conns[i] && conns[i]->dead && conns[i]->fd >= 0) {
					conn_drop(i);
					again = 1;
				}
			if (!again)
				break;
		}
	}
	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i] && conns[i]->fd >= 0)
			close(conns[i]->fd);
	close(lfd);
	unlink(sock_path);
	return 0;
}
