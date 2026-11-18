/*
 * libretroui bus client: talks to retro-bus over a Unix socket.  One
 * connection per process, used from the program's own event loop (rui_wait
 * watches the socket), with an optional blocking call for tools.  When no
 * bus is running every function returns quietly; services and listeners
 * are remembered and renewed once the bus (re)appears.
 *
 * Wire format (docs/BUS.md): one message per line, words separated by one
 * space, the last field running to the end of the line with \\ \n \t \r
 * escaped:
 *     call ID SERVICE METHOD [ARGS]     to the bus
 *     call ID SENDER SERVICE METHOD [ARGS]   from the bus, to a service
 *     reply ID [TEXT]   error ID [TEXT]
 *     emit TOPIC [ARGS]                 to the bus
 *     event SENDER TOPIC [ARGS]         from the bus, to listeners
 */
#include "retroui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_KEEP 16
#define LINE_MAX_LEN (1 << 20)
#define RETRY_MS 2000

static int fd = -1;
static char *in;
static size_t inlen, incap;
static RuiBusFn handler;
static void *handler_ud;
static char *claims[MAX_KEEP], *listens[MAX_KEEP];
static int nclaims, nlistens;
static unsigned long next_id = 1;
static long long last_try;

/* what a blocking call waits for */
static const char *wait_id;
static char *wait_text;
static int wait_done, wait_ok;

/* ---------------------------------------------------------------- escape */

char *
rui_bus_escape(const char *s)
{
	size_t n = 0;
	const char *p;
	char *out, *o;

	if (!s)
		s = "";
	for (p = s; *p; p++)
		n += (*p == '\\' || *p == '\n' || *p == '\t' || *p == '\r') ? 2 : 1;
	o = out = rui_malloc(n + 1);
	for (p = s; *p; p++) {
		switch (*p) {
		case '\\': *o++ = '\\'; *o++ = '\\'; break;
		case '\n': *o++ = '\\'; *o++ = 'n'; break;
		case '\t': *o++ = '\\'; *o++ = 't'; break;
		case '\r': *o++ = '\\'; *o++ = 'r'; break;
		default: *o++ = *p;
		}
	}
	*o = '\0';
	return out;
}

void
rui_bus_unescape(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s != '\\' || !s[1]) {
			*o++ = *s;
			continue;
		}
		s++;
		*o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
	}
	*o = '\0';
}

int
rui_bus_word_ok(const char *w, int glob)
{
	size_t n = 0;

	if (!w || !*w)
		return 0;
	for (; *w; w++, n++) {
		unsigned char c = (unsigned char)*w;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || strchr("._:@+-", c) ||
		    (glob && (c == '*' || c == '?'))))
			return 0;
	}
	return n <= 128;
}

int
rui_bus_fields(char *args, char **v, int max)
{
	int n = 0;

	if (!args || max <= 0)
		return 0;
	v[n++] = args;
	while (n < max && (args = strchr(args, '\t'))) {
		*args++ = '\0';
		v[n++] = args;
	}
	return n;
}

/* ------------------------------------------------------------ connection */

char *
rui_bus_path(void)
{
	const char *e = getenv("RETRO_BUS"), *tmp = getenv("TMPDIR"),
	    *disp = getenv("DISPLAY");
	char d[64], *colon, *dot, *p;

	if (e && *e)
		return rui_strdup(e);
	if (!tmp || !*tmp || strlen(tmp) > 40)
		tmp = "/tmp";
	rui_strlcpy(d, disp && *disp ? disp : "none", sizeof d);
	/* ":0.0" and ":0" are the same display: drop the screen */
	if ((colon = strrchr(d, ':')) && (dot = strchr(colon, '.')))
		*dot = '\0';
	for (p = d; *p; p++)
		if (*p == '/' || *p == ':')
			*p = '_';
	return rui_strdupf("%s/retro-de-%lu/bus-%s", tmp,
	    (unsigned long)getuid(), d[0] == '_' ? d + 1 : d);
}

int
rui_bus_fd(void)
{
	return fd;
}

void
rui_bus_close(void)
{
	if (fd >= 0)
		close(fd);
	fd = -1;
	inlen = 0;
}

static int
send_all(const char *buf, size_t len)
{
	if (fd < 0)
		return -1;
	while (len) {
#ifdef MSG_NOSIGNAL
		ssize_t r = send(fd, buf, len, MSG_NOSIGNAL);
#else
		ssize_t r = send(fd, buf, len, 0);
#endif

		if (r > 0) {
			buf += r;
			len -= (size_t)r;
		} else if (r < 0 && errno == EINTR) {
			continue;
		} else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd p;

			p.fd = fd;
			p.events = POLLOUT;
			if (poll(&p, 1, 2000) <= 0) {
				rui_bus_close();
				return -1;
			}
		} else {
			rui_bus_close();
			return -1;
		}
	}
	return 0;
}

/* "verb w1 w2 ... [escaped rest]" */
static int
send_msg(const char *head, const char *rest)
{
	char *esc = rest ? rui_bus_escape(rest) : NULL;
	char *line = esc && *esc ? rui_strdupf("%s %s\n", head, esc) :
	    rui_strdupf("%s\n", head);
	int r = send_all(line, strlen(line));

	free(line);
	free(esc);
	return r;
}

static void
renew(void)
{
	int i;

	for (i = 0; i < nclaims; i++)
		send_msg("call - bus name", claims[i]);
	for (i = 0; i < nlistens; i++)
		send_msg("call - bus listen", listens[i]);
}

int
rui_bus_connect(void)
{
	struct sockaddr_un sa;
	char *path;
	int s, fl;

	if (fd >= 0)
		return 0;
	last_try = rui_now_ms();
	path = rui_bus_path();
	if (strlen(path) >= sizeof sa.sun_path) {
		free(path);
		return -1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	rui_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	free(path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(s);
		return -1;
	}
#ifndef MSG_NOSIGNAL
	signal(SIGPIPE, SIG_IGN);
#endif
	fcntl(s, F_SETFD, FD_CLOEXEC);
	if ((fl = fcntl(s, F_GETFL)) >= 0)
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
	fd = s;
	inlen = 0;
	renew();
	return fd >= 0 ? 0 : -1;
}

/* the event loop's share: the socket to watch, and a shorter timeout while
 * a service waits for the bus to come back */
int
rui_bus_wait_fd(int *timeout_ms)
{
	if (fd < 0 && (nclaims || nlistens)) {
		long long now = rui_now_ms();

		if (now - last_try >= RETRY_MS)
			rui_bus_connect();
		if (fd < 0 && (*timeout_ms < 0 || *timeout_ms > RETRY_MS))
			*timeout_ms = RETRY_MS;
	}
	return fd;
}

/* -------------------------------------------------------------- incoming */

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
handle_line(char *line)
{
	char *p = line, *kind = word(&p);
	RuiBusMsg m;

	if (!kind)
		return;
	memset(&m, 0, sizeof m);
	m.kind = kind;
	if (!strcmp(kind, "reply") || !strcmp(kind, "error")) {
		if (!(m.id = word(&p)))
			return;
		rui_bus_unescape(p);
		m.args = p;
		if (wait_id && !strcmp(m.id, wait_id)) {
			wait_text = rui_strdup(p);
			wait_ok = kind[0] == 'r';
			wait_done = 1;
			return;
		}
	} else if (!strcmp(kind, "call")) {
		if (!(m.id = word(&p)) || !(m.from = word(&p)) ||
		    !(m.service = word(&p)) || !(m.name = word(&p)))
			return;
		rui_bus_unescape(p);
		m.args = p;
	} else if (!strcmp(kind, "event")) {
		if (!(m.from = word(&p)) || !(m.name = word(&p)))
			return;
		rui_bus_unescape(p);
		m.args = p;
	} else {
		return;
	}
	if (handler)
		handler(&m, handler_ud);
	else if (!strcmp(kind, "call") && strcmp(m.id, "-"))
		rui_bus_reply(m.id, 0, "no handler");
}

/* reads what is there; -1 when the bus went away */
static int
read_some(void)
{
	for (;;) {
		ssize_t r;

		if (incap - inlen < 4096) {
			incap = incap ? incap * 2 : 8192;
			in = rui_realloc(in, incap);
		}
		r = read(fd, in + inlen, incap - inlen - 1);
		if (r > 0) {
			inlen += (size_t)r;
			if (inlen > LINE_MAX_LEN && !memchr(in, '\n', inlen))
				return -1;                     /* runaway line */
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
}

static void
process_lines(void)
{
	size_t start = 0;
	char *nl;

	while (fd >= 0 && (nl = memchr(in + start, '\n', inlen - start))) {
		size_t len = (size_t)(nl - (in + start));
		char *line = rui_malloc(len + 1);

		memcpy(line, in + start, len);
		line[len] = '\0';
		start += len + 1;
		handle_line(line);
		free(line);
	}
	if (fd >= 0 && start) {
		memmove(in, in + start, inlen - start);
		inlen -= start;
	}
}

void
rui_bus_dispatch(void)
{
	int gone;

	if (fd < 0)
		return;
	gone = read_some() < 0;
	process_lines();
	if (gone)
		rui_bus_close();
}

void
rui_bus_handler(RuiBusFn fn, void *ud)
{
	handler = fn;
	handler_ud = ud;
}

/* -------------------------------------------------------------- outgoing */

char *
rui_bus_call(const char *service, const char *method, const char *args,
    int timeout_ms, int *ok)
{
	char id[32], *head;
	const char *saved_id = wait_id;
	long long end;
	char *text = NULL;

	if (ok)
		*ok = 0;
	if (!rui_bus_word_ok(service, 0) || !rui_bus_word_ok(method, 0) ||
	    rui_bus_connect() < 0)
		return NULL;
	snprintf(id, sizeof id, "c%lu", next_id++);
	head = rui_strdupf("call %s %s %s", id, service, method);
	if (send_msg(head, args) < 0) {
		free(head);
		return NULL;
	}
	free(head);
	wait_id = id;
	wait_done = 0;
	wait_text = NULL;
	end = rui_now_ms() + (timeout_ms < 0 ? 0x7fffffff : timeout_ms);
	while (!wait_done && fd >= 0) {
		struct pollfd p;
		long long left = end - rui_now_ms();

		if (left <= 0)
			break;
		process_lines();                       /* already buffered? */
		if (wait_done)
			break;
		p.fd = fd;
		p.events = POLLIN;
		if (poll(&p, 1, (int)(left > 60000 ? 60000 : left)) > 0)
			rui_bus_dispatch();
	}
	if (wait_done) {
		text = wait_text;
		if (ok)
			*ok = wait_ok;
	}
	wait_id = saved_id;
	wait_done = 0;
	wait_text = NULL;
	return text;
}

int
rui_bus_send(const char *service, const char *method, const char *args)
{
	char *head;
	int r;

	if (!rui_bus_word_ok(service, 0) || !rui_bus_word_ok(method, 0) ||
	    rui_bus_connect() < 0)
		return -1;
	head = rui_strdupf("call - %s %s", service, method);
	r = send_msg(head, args);
	free(head);
	return r;
}

int
rui_bus_reply(const char *id, int ok, const char *text)
{
	char *head;
	int r;

	if (!id || !strcmp(id, "-") || !rui_bus_word_ok(id, 0))
		return 0;                              /* nobody waits for it */
	head = rui_strdupf("%s %s", ok ? "reply" : "error", id);
	r = send_msg(head, text);
	free(head);
	return r;
}

int
rui_bus_emit(const char *topic, const char *args)
{
	char *head;
	int r;

	if (!rui_bus_word_ok(topic, 0) || rui_bus_connect() < 0)
		return -1;
	head = rui_strdupf("emit %s", topic);
	r = send_msg(head, args);
	free(head);
	return r;
}

static int
keep(char **list, int *n, const char *s)
{
	int i;

	for (i = 0; i < *n; i++)
		if (!strcmp(list[i], s))
			return 0;
	if (*n >= MAX_KEEP)
		return -1;
	list[(*n)++] = rui_strdup(s);
	return 0;
}

int
rui_bus_claim(const char *name)
{
	char *ans;
	int ok = 0;

	if (!rui_bus_word_ok(name, 0) || !strcmp(name, "bus") || name[0] == ':')
		return -1;
	if (rui_bus_connect() < 0) {
		keep(claims, &nclaims, name);          /* when the bus starts */
		last_try = rui_now_ms();
		return -1;
	}
	ans = rui_bus_call("bus", "name", name, 3000, &ok);
	free(ans);
	if (!ok && ans)
		return -1;                             /* someone else has it */
	keep(claims, &nclaims, name);
	return ans ? 0 : -1;
}

void
rui_bus_listen(const char *pattern)
{
	if (!rui_bus_word_ok(pattern, 1) || keep(listens, &nlistens, pattern) < 0)
		return;
	if (rui_bus_connect() == 0)
		send_msg("call - bus listen", pattern);
	else
		last_try = rui_now_ms();
}
