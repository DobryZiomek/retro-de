/*
 * retro-bus - the desktop's message bus, in place of D-Bus.
 *
 * A single process listening on a Unix socket (rui_bus_path(): $RETRO_BUS,
 * else ${TMPDIR:-/tmp}/retro-de-UID/bus-DISPLAY, in a 0700 directory).
 * Clients claim service names, call methods on each other and broadcast
 * events; the bus only routes lines of text and never looks inside the
 * arguments.  retro-session starts it and exports RETRO_BUS.
 *
 *   retro-bus [-s path]      run in the foreground until SIGTERM
 *   retro-bus -v
 *
 * The protocol is described in docs/BUS.md.  The bus itself is the service
 * "bus" with the methods name, release, listen, unlisten, list, ping and
 * version, and it emits bus.join / bus.leave when names come and go.
 */
#include "../lib/retroui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MAX_CLIENTS 256
#define MAX_NAMES 16
#define MAX_PATS 32
#define MAX_PENDING 1024
#define IN_LIMIT (1 << 20)            /* one line                        */
#define OUT_LIMIT (8 << 20)           /* a reader that stopped reading   */

typedef struct Client {
	int fd;                           /* -1: free slot                   */
	unsigned long serial;             /* ":N", never reused              */
	char *in, *out;
	size_t inlen, incap, outlen, outcap;
	char *names[MAX_NAMES];
	int nnames;
	char *pats[MAX_PATS];
	int npats;
	int dead;
} Client;

typedef struct Pending {
	unsigned long id;                 /* "bN" towards the service; 0 free */
	unsigned long caller, service;    /* client serials                  */
	char caller_id[132];
} Pending;

static Client cl[MAX_CLIENTS];
static Pending pend[MAX_PENDING];
static unsigned long next_serial = 1, next_call = 1;
static int lfd = -1, debug;
static char sock_path[256];
static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* ---------------------------------------------------------------- output */

static void
put(Client *c, const char *s, size_t n)
{
	if (c->fd < 0 || c->dead)
		return;
	if (c->outlen + n > OUT_LIMIT) {
		c->dead = 1;                      /* not reading: drop it */
		return;
	}
	if (c->outlen + n + 1 > c->outcap) {
		while (c->outlen + n + 1 > c->outcap)
			c->outcap = c->outcap ? c->outcap * 2 : 4096;
		c->out = rui_realloc(c->out, c->outcap);
	}
	memcpy(c->out + c->outlen, s, n);
	c->outlen += n;
}

/* "a b c rest\n"; rest is already escaped (or NULL) */
static void
sendf(Client *c, const char *head, const char *rest)
{
	put(c, head, strlen(head));
	if (rest && *rest) {
		put(c, " ", 1);
		put(c, rest, strlen(rest));
	}
	put(c, "\n", 1);
	if (debug)
		fprintf(stderr, "retro-bus: -> :%lu %s %s\n", c->serial, head,
		    rest ? rest : "");
}

static void
flush_out(Client *c)
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

static void
answer(Client *c, int ok, const char *id, const char *text)
{
	char head[160], *esc;

	if (!strcmp(id, "-"))
		return;
	snprintf(head, sizeof head, "%s %s", ok ? "reply" : "error", id);
	esc = rui_bus_escape(text);
	sendf(c, head, esc);
	free(esc);
}

/* ---------------------------------------------------------------- lookup */

static Client *
by_serial(unsigned long s)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0 && cl[i].serial == s && !cl[i].dead)
			return &cl[i];
	return NULL;
}

static Client *
owner(const char *name)
{
	int i, k;

	if (name[0] == ':')
		return by_serial(strtoul(name + 1, NULL, 10));
	for (i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0 && !cl[i].dead)
			for (k = 0; k < cl[i].nnames; k++)
				if (!strcmp(cl[i].names[k], name))
					return &cl[i];
	return NULL;
}

static const char *
sender(const Client *c)
{
	static char buf[32];

	if (c->nnames)
		return c->names[0];
	snprintf(buf, sizeof buf, ":%lu", c->serial);
	return buf;
}

/* --------------------------------------------------------------- routing */

static void
emit(const char *from, const char *topic, const char *rest)
{
	char head[300];
	int i, k;

	snprintf(head, sizeof head, "event %s %s", from, topic);
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (cl[i].fd < 0 || cl[i].dead)
			continue;
		for (k = 0; k < cl[i].npats; k++) {
			if (rui_glob(cl[i].pats[k], topic)) {
				sendf(&cl[i], head, rest);
				break;
			}
		}
	}
}

static void
bus_method(Client *c, const char *id, const char *method, char *args)
{
	int i, k;

	rui_bus_unescape(args);
	if (!strcmp(method, "ping")) {
		answer(c, 1, id, "pong");
	} else if (!strcmp(method, "version")) {
		answer(c, 1, id, RUI_VERSION);
	} else if (!strcmp(method, "name")) {
		Client *o;

		if (!rui_bus_word_ok(args, 0) || args[0] == ':' ||
		    !strcmp(args, "bus")) {
			answer(c, 0, id, "bad name");
		} else if ((o = owner(args)) == c) {
			answer(c, 1, id, args);
		} else if (o) {
			answer(c, 0, id, "name taken");
		} else if (c->nnames >= MAX_NAMES) {
			answer(c, 0, id, "too many names");
		} else {
			c->names[c->nnames++] = rui_strdup(args);
			answer(c, 1, id, args);
			emit("bus", "bus.join", args);
		}
	} else if (!strcmp(method, "release")) {
		for (k = 0; k < c->nnames; k++)
			if (!strcmp(c->names[k], args))
				break;
		if (k == c->nnames) {
			answer(c, 0, id, "not yours");
			return;
		}
		emit("bus", "bus.leave", args);
		free(c->names[k]);
		c->names[k] = c->names[--c->nnames];
		answer(c, 1, id, "");
	} else if (!strcmp(method, "listen")) {
		for (k = 0; k < c->npats; k++)
			if (!strcmp(c->pats[k], args))
				break;
		if (!rui_bus_word_ok(args, 1))
			answer(c, 0, id, "bad pattern");
		else if (k == c->npats && c->npats >= MAX_PATS)
			answer(c, 0, id, "too many patterns");
		else {
			if (k == c->npats)
				c->pats[c->npats++] = rui_strdup(args);
			answer(c, 1, id, "");
		}
	} else if (!strcmp(method, "unlisten")) {
		for (k = 0; k < c->npats; k++) {
			if (!strcmp(c->pats[k], args)) {
				free(c->pats[k]);
				c->pats[k] = c->pats[--c->npats];
				break;
			}
		}
		answer(c, 1, id, "");
	} else if (!strcmp(method, "list")) {
		size_t len = 0, cap = 256;
		char *buf = rui_malloc(cap);

		buf[0] = '\0';
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (cl[i].fd < 0 || cl[i].dead)
				continue;
			for (k = 0; k < cl[i].nnames; k++) {
				size_t n = strlen(cl[i].names[k]) + 2;

				if (len + n >= cap)
					buf = rui_realloc(buf, cap = (len + n) * 2);
				len += (size_t)sprintf(buf + len, "%s%s", len ? "\n" : "",
				    cl[i].names[k]);
			}
		}
		answer(c, 1, id, buf);
		free(buf);
	} else {
		answer(c, 0, id, "no such method");
	}
}

static char *
word(char **p)
{
	char *w = *p, *sp;

	if (!w || !*w)
		return NULL;
	if ((sp = strchr(w, ' '))) {
		*sp = '\0';
		*p = sp + 1;
	} else {
		*p = w + strlen(w);
	}
	return w;
}

static void
do_call(Client *c, char *p)
{
	char *id = word(&p), *svc = word(&p), *method = word(&p), head[400];
	Client *s;
	int i;

	if (!id || !svc || !method || !rui_bus_word_ok(id, 0))
		return;
	if (!rui_bus_word_ok(svc, 0) || !rui_bus_word_ok(method, 0)) {
		answer(c, 0, id, "bad call");
		return;
	}
	if (!strcmp(svc, "bus")) {
		bus_method(c, id, method, p);
		return;
	}
	if (!(s = owner(svc))) {
		char msg[200];

		snprintf(msg, sizeof msg, "no service %s", svc);
		answer(c, 0, id, msg);
		return;
	}
	if (!strcmp(id, "-")) {
		snprintf(head, sizeof head, "call - %s %s %s", sender(c), svc,
		    method);
		sendf(s, head, p);
		return;
	}
	for (i = 0; i < MAX_PENDING && pend[i].id; i++)
		;
	if (i == MAX_PENDING) {
		answer(c, 0, id, "bus busy");
		return;
	}
	pend[i].id = next_call++;
	pend[i].caller = c->serial;
	pend[i].service = s->serial;
	rui_strlcpy(pend[i].caller_id, id, sizeof pend[i].caller_id);
	snprintf(head, sizeof head, "call b%lu %s %s %s", pend[i].id,
	    sender(c), svc, method);
	sendf(s, head, p);
}

static void
do_answer(Client *c, int ok, char *p)
{
	char *id = word(&p), head[160];
	unsigned long n;
	Client *caller;
	int i;

	if (!id || id[0] != 'b')
		return;
	n = strtoul(id + 1, NULL, 10);
	for (i = 0; i < MAX_PENDING; i++)
		if (pend[i].id == n && pend[i].service == c->serial)
			break;
	if (i == MAX_PENDING)
		return;
	if ((caller = by_serial(pend[i].caller))) {
		snprintf(head, sizeof head, "%s %s", ok ? "reply" : "error",
		    pend[i].caller_id);
		sendf(caller, head, p);
	}
	pend[i].id = 0;
}

static void
handle_line(Client *c, char *line)
{
	char *p = line, *verb = word(&p);

	if (debug)
		fprintf(stderr, "retro-bus: <- :%lu %s %s\n", c->serial,
		    verb ? verb : "", p);
	if (!verb)
		return;
	if (!strcmp(verb, "call"))
		do_call(c, p);
	else if (!strcmp(verb, "reply"))
		do_answer(c, 1, p);
	else if (!strcmp(verb, "error"))
		do_answer(c, 0, p);
	else if (!strcmp(verb, "emit")) {
		char *topic = word(&p);

		if (topic && rui_bus_word_ok(topic, 0))
			emit(sender(c), topic, p);
	}
}

/* ---------------------------------------------------------------- clients */

static void
drop(Client *c)
{
	int i;

	for (i = 0; i < MAX_PENDING; i++) {
		if (!pend[i].id)
			continue;
		if (pend[i].service == c->serial) {
			Client *caller = by_serial(pend[i].caller);

			if (caller)
				answer(caller, 0, pend[i].caller_id, "service gone");
			pend[i].id = 0;
		} else if (pend[i].caller == c->serial) {
			pend[i].id = 0;                /* the answer goes nowhere */
		}
	}
	close(c->fd);
	c->fd = -1;
	c->dead = 1;                           /* invisible to emit() below */
	for (i = 0; i < c->nnames; i++) {
		emit("bus", "bus.leave", c->names[i]);
		free(c->names[i]);
	}
	for (i = 0; i < c->npats; i++)
		free(c->pats[i]);
	free(c->in);
	free(c->out);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

static void
read_client(Client *c)
{
	size_t start = 0;
	char *nl;

	for (;;) {
		ssize_t r;

		if (c->incap - c->inlen < 4096) {
			c->incap = c->incap ? c->incap * 2 : 8192;
			c->in = rui_realloc(c->in, c->incap);
		}
		r = read(c->fd, c->in + c->inlen, c->incap - c->inlen);
		if (r > 0) {
			c->inlen += (size_t)r;
			if (c->inlen > IN_LIMIT && !memchr(c->in, '\n', c->inlen)) {
				c->dead = 1;
				return;
			}
			if (c->inlen > IN_LIMIT)
				break;                     /* handle these lines first */
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r == 0 || !(errno == EAGAIN || errno == EWOULDBLOCK))
			c->dead = 1;
		break;
	}
	while (!c->dead && (nl = memchr(c->in + start, '\n', c->inlen - start))) {
		*nl = '\0';
		if (nl > c->in + start && nl[-1] == '\r')
			nl[-1] = '\0';
		handle_line(c, c->in + start);
		start = (size_t)(nl - c->in) + 1;
	}
	if (start) {
		memmove(c->in, c->in + start, c->inlen - start);
		c->inlen -= start;
	}
}

static void
accept_client(void)
{
	int s, i, fl;

	while ((s = accept(lfd, NULL, NULL)) >= 0) {
		for (i = 0; i < MAX_CLIENTS && cl[i].fd >= 0; i++)
			;
		if (i == MAX_CLIENTS) {
			close(s);
			continue;
		}
		fcntl(s, F_SETFD, FD_CLOEXEC);
		if ((fl = fcntl(s, F_GETFL)) >= 0)
			fcntl(s, F_SETFL, fl | O_NONBLOCK);
		memset(&cl[i], 0, sizeof cl[i]);
		cl[i].fd = s;
		cl[i].serial = next_serial++;
	}
}

/* ----------------------------------------------------------------- socket */

/* the directory must be ours and private */
static int
make_dir(const char *path)
{
	char dir[256], *slash;
	struct stat st;

	rui_strlcpy(dir, path, sizeof dir);
	if (!(slash = strrchr(dir, '/')) || slash == dir)
		return 0;
	*slash = '\0';
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		fprintf(stderr, "retro-bus: mkdir %s: %s\n", dir, strerror(errno));
		return -1;
	}
	if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode) ||
	    st.st_uid != getuid() || (st.st_mode & 077)) {
		fprintf(stderr, "retro-bus: %s must be a directory owned by you "
		    "with mode 0700\n", dir);
		return -1;
	}
	return 0;
}

static int
open_socket(const char *path)
{
	struct sockaddr_un sa;
	int s, fl;
	mode_t old;

	if (strlen(path) >= sizeof sa.sun_path) {
		fprintf(stderr, "retro-bus: socket path too long: %s\n", path);
		return -1;
	}
	if (make_dir(path) < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	rui_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	/* another bus for this display already answers: leave it be */
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
		close(s);
		return -2;
	}
	close(s);
	unlink(path);                          /* a stale socket */
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	old = umask(077);
	if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		umask(old);
		fprintf(stderr, "retro-bus: bind %s: %s\n", path, strerror(errno));
		close(s);
		return -1;
	}
	umask(old);
	if (listen(s, 64) < 0) {
		close(s);
		unlink(path);
		return -1;
	}
	fcntl(s, F_SETFD, FD_CLOEXEC);
	if ((fl = fcntl(s, F_GETFL)) >= 0)
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
	return s;
}

int
main(int argc, char **argv)
{
	static struct pollfd pfd[MAX_CLIENTS + 1];
	static int slot[MAX_CLIENTS + 1];
	struct sigaction sa;
	char *path = NULL;
	const char *dbg;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("retro-bus " RUI_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			path = rui_strdup(argv[++i]);
		} else {
			fprintf(stderr, "usage: retro-bus [-s socket] [-v]\n");
			return 2;
		}
	}
	if (!path)
		path = rui_bus_path();
	rui_strlcpy(sock_path, path, sizeof sock_path);
	free(path);
	debug = (dbg = getenv("RETRO_DEBUG")) && *dbg && strcmp(dbg, "0");

	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	lfd = open_socket(sock_path);
	if (lfd == -2) {
		fprintf(stderr, "retro-bus: already running on %s\n", sock_path);
		return 0;
	}
	if (lfd < 0)
		return 1;
#ifdef __OpenBSD__
	if (pledge("stdio cpath unix", NULL) == -1) {
		perror("retro-bus: pledge");
		unlink(sock_path);
		return 1;
	}
#endif
	for (i = 0; i < MAX_CLIENTS; i++)
		cl[i].fd = -1;

	while (!stop) {
		int n = 0, r;

		pfd[n].fd = lfd;
		pfd[n].events = POLLIN;
		slot[n++] = -1;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (cl[i].fd < 0)
				continue;
			pfd[n].fd = cl[i].fd;
			pfd[n].events = POLLIN | (cl[i].outlen ? POLLOUT : 0);
			pfd[n].revents = 0;
			slot[n++] = i;
		}
		r = poll(pfd, (nfds_t)n, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 1; i < n; i++) {
			Client *c = &cl[slot[i]];

			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
				read_client(c);
		}
		if (pfd[0].revents & POLLIN)
			accept_client();
		/* flush everything (routing may have queued for anyone), then
		 * drop the clients that went away */
		for (i = 0; i < MAX_CLIENTS; i++)
			if (cl[i].fd >= 0)
				flush_out(&cl[i]);
		for (;;) {
			int again = 0;

			for (i = 0; i < MAX_CLIENTS; i++) {
				if (cl[i].fd >= 0 && cl[i].dead) {
					drop(&cl[i]);
					again = 1;
				}
			}
			if (!again)
				break;
			for (i = 0; i < MAX_CLIENTS; i++)
				if (cl[i].fd >= 0)
					flush_out(&cl[i]); /* the bus.leave events */
		}
	}
	for (i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0)
			close(cl[i].fd);
	close(lfd);
	unlink(sock_path);
	return 0;
}
