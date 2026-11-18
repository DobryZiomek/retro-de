/* libretroui core: utilities, config, async jobs, display, fonts, drawing. */
#include "retroui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>

/* ------------------------------------------------------------------ util */

static const char *prog_name = "retro";

void
rui_warn(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", prog_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void
rui_die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", prog_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void *
rui_malloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		rui_die("out of memory");
	return p;
}

void *
rui_calloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);

	if (!p)
		rui_die("out of memory");
	return p;
}

void *
rui_realloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		rui_die("out of memory");
	return p;
}

char *
rui_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = rui_malloc(n);

	memcpy(p, s, n);
	return p;
}

size_t
rui_strlcpy(char *dst, const char *src, size_t n)
{
	size_t len = strlen(src);

	if (n) {
		size_t c = len >= n ? n - 1 : len;

		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return len;
}

long long
rui_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *
rui_config_dir(void)
{
	static char dir[1024];
	const char *e;

	if (dir[0])
		return dir;
	if ((e = getenv("RETRO_DE_CONFIG_DIR")) && *e)
		rui_strlcpy(dir, e, sizeof dir);
	else if ((e = getenv("XDG_CONFIG_HOME")) && *e)
		snprintf(dir, sizeof dir, "%s/retro-de", e);
	else if ((e = getenv("HOME")) && *e)
		snprintf(dir, sizeof dir, "%s/.config/retro-de", e);
	else
		rui_strlcpy(dir, "/tmp/retro-de", sizeof dir);
	return dir;
}

char *
rui_config_path(const char *name)
{
	const char *d = rui_config_dir();
	size_t n = strlen(d) + strlen(name) + 2;
	char *p = rui_malloc(n);

	snprintf(p, n, "%s/%s", d, name);
	return p;
}

int
rui_have_cmd(const char *name)
{
	const char *path = getenv("PATH");
	char buf[1024];

	if (!name || !*name)
		return 0;
	if (strchr(name, '/'))
		return access(name, X_OK) == 0;
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	while (*path) {
		size_t len = strcspn(path, ":");

		if (len && len + strlen(name) + 2 < sizeof buf) {
			struct stat st;

			snprintf(buf, sizeof buf, "%.*s/%s", (int)len, path, name);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) &&
			    access(buf, X_OK) == 0)
				return 1;
		}
		path += len;
		if (*path == ':')
			path++;
	}
	return 0;
}

void
rui_gecos_name(const char *gecos, const char *login, char *buf, size_t n)
{
	size_t o = 0;

	if (!n)
		return;
	for (; gecos && *gecos && *gecos != ','; gecos++) {
		if (*gecos == '&' && login) {
			const char *l;

			for (l = login; *l && o + 1 < n; l++)
				buf[o++] = l == login ?
				    (char)toupper((unsigned char)*l) : *l;
		} else if (o + 1 < n) {
			buf[o++] = *gecos;
		}
	}
	buf[o] = '\0';
	rui_rtrim(buf);
}

char *
rui_shell_quote(const char *s)
{
	size_t n = 3;
	const char *p;
	char *out, *o;

	for (p = s; *p; p++)
		n += (*p == '\'') ? 4 : 1;
	o = out = rui_malloc(n);
	*o++ = '\'';
	for (p = s; *p; p++) {
		if (*p == '\'') {
			memcpy(o, "'\\''", 4);
			o += 4;
		} else {
			*o++ = *p;
		}
	}
	*o++ = '\'';
	*o = '\0';
	return out;
}

void
rui_spawn(const char *cmd)
{
	pid_t pid;

	if (!cmd || !*cmd)
		return;
	pid = fork();
	if (pid < 0) {
		rui_warn("fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		/* double fork so the grandchild is reparented to init */
		pid_t p2 = fork();

		if (p2 == 0) {
			setsid();
			signal(SIGPIPE, SIG_DFL);
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

const char *
rui_terminal(void)
{
	const char *const *cands = rui_config.terminals;
	static char found[256];
	const char *e;
	int i;

	if (found[0])
		return found;
	e = rui_settings()->tool[RUI_TOOL_TERMINAL];
	if (*e && strcmp(e, "auto")) {
		char prog[256];

		rui_strlcpy(prog, e, sizeof prog);
		prog[strcspn(prog, " \t")] = '\0';
		if (rui_have_cmd(prog)) {
			rui_strlcpy(found, e, sizeof found);
			return found;
		}
	}
	if ((e = getenv("TERMINAL")) && *e && rui_have_cmd(e)) {
		rui_strlcpy(found, e, sizeof found);
		return found;
	}
	for (i = 0; cands[i]; i++) {
		if (rui_have_cmd(cands[i])) {
			rui_strlcpy(found, cands[i], sizeof found);
			return found;
		}
	}
	rui_strlcpy(found, "xterm", sizeof found);
	return found;
}

char *
rui_strdupf(const char *fmt, ...)
{
	va_list ap;
	int n;
	char *s;

	if (!fmt)
		return rui_strdup("");
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return rui_strdup("");
	s = rui_malloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

int
rui_next_line(const char **p, char *buf, size_t n)
{
	size_t len, c;

	if (!*p || !**p)
		return 0;
	len = strcspn(*p, "\n");
	c = len < n - 1 ? len : n - 1;
	memcpy(buf, *p, c);
	buf[c] = '\0';
	*p += len;
	if (**p == '\n')
		(*p)++;
	return 1;
}

char *
rui_rtrim(char *s)
{
	char *e = s + strlen(s);

	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
	    e[-1] == '\r' || e[-1] == '\v' || e[-1] == '\f'))
		*--e = '\0';
	return s;
}

char *
rui_ltrim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' ||
	    *s == '\v' || *s == '\f')
		s++;
	return s;
}

void
rui_strip_ansi(char *s)
{
	char *o = s;

	while (*s) {
		if (*s == '\033' && s[1] == '[') {
			s += 2;
			while (*s && !((*s >= '@' && *s <= '~')))
				s++;
			if (*s)
				s++;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

/* ---------------------------------------------------------------- config */

static char *
trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
	    e[-1] == '\r'))
		*--e = '\0';
	return s;
}

/* one file; `include = FILE` (relative to this file) nests up to 8 deep */
static int
conf_file(const char *path, RuiConfFn fn, void *ud, int depth)
{
	FILE *f = fopen(path, "r");
	char line[2048];

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line), *eq, *key, *val, *hash;

		if (!*s || *s == '#')
			continue;
		eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		/* trailing comment: '#' preceded by a blank (so "#c0c0c0" is safe) */
		for (hash = val; (hash = strchr(hash, '#')); hash++) {
			if (hash > val && (hash[-1] == ' ' || hash[-1] == '\t')) {
				*hash = '\0';
				val = trim(val);
				break;
			}
		}
		if (!strcmp(key, "include")) {
			char *inc;
			const char *slash = strrchr(path, '/');

			if (val[0] == '/' || !slash)
				inc = rui_strdup(val);
			else if (val[0] == '~' && val[1] == '/' && getenv("HOME"))
				inc = rui_strdupf("%s%s", getenv("HOME"), val + 1);
			else
				inc = rui_strdupf("%.*s/%s", (int)(slash - path), path, val);
			if (depth >= 8)
				rui_warn("%s: includes nested too deeply", path);
			else if (conf_file(inc, fn, ud, depth + 1) < 0)
				rui_warn("%s: cannot include '%s'", path, val);
			free(inc);
		} else if (*key) {
			fn(key, val, ud);
		}
	}
	fclose(f);
	return 0;
}

/* The settings can be split by subject.  Next to retro.conf these files
 * are read as well when they exist, in this order, with the same syntax
 * (any key may go in any of them; later lines win):
 *   keys.conf      key bindings        (bind = ..., unbind = ...)
 *   windows.conf   window behaviour    (focus, snapping, placement ...)
 *   programs.conf  per-program rules   (rule = class=... => ...)
 * and `include = FILE` pulls in any other file. */
const char *const rui_conf_parts[] = {
	"keys.conf", "windows.conf", "programs.conf", NULL
};

int
rui_conf_parse(const char *path, RuiConfFn fn, void *ud)
{
	const char *base = strrchr(path, '/');
	int rc = conf_file(path, fn, ud, 0), i;

	if (base && !strcmp(base + 1, "retro.conf")) {
		for (i = 0; rui_conf_parts[i]; i++) {
			char *part = rui_strdupf("%.*s/%s", (int)(base - path), path,
			    rui_conf_parts[i]);

			if (conf_file(part, fn, ud, 0) == 0)
				rc = 0;
			free(part);
		}
	}
	return rc;
}

/* case-insensitive glob: * and ? */
int
rui_glob(const char *pat, const char *s)
{
	for (; *pat; pat++, s++) {
		if (*pat == '*') {
			while (pat[1] == '*')
				pat++;
			if (!pat[1])
				return 1;
			for (; *s; s++)
				if (rui_glob(pat + 1, s))
					return 1;
			return 0;
		}
		if (!*s)
			return 0;
		if (*pat != '?' && tolower((unsigned char)*pat) !=
		    tolower((unsigned char)*s))
			return 0;
	}
	return !*s;
}

int
rui_conf_bool(const char *v)
{
	return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") ||
	    !strcmp(v, "on");
}

/* --------------------------------------------------------------- signals */

static volatile sig_atomic_t quit_flag;
static int sig_pipe[2] = { -1, -1 };

static void
on_signal(int sig)
{
	int saved = errno;
	char c = 'q';

	(void)sig;
	quit_flag = 1;
	if (sig_pipe[1] >= 0) {
		ssize_t r = write(sig_pipe[1], &c, 1);

		(void)r;
	}
	errno = saved;
}

static void
set_cloexec_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFD);

	if (fl >= 0)
		fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
	fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void
rui_signals_init(void)
{
	struct sigaction sa;

	if (sig_pipe[0] < 0 && pipe(sig_pipe) == 0) {
		set_cloexec_nonblock(sig_pipe[0]);
		set_cloexec_nonblock(sig_pipe[1]);
	}
	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
}

int
rui_quit_requested(void)
{
	return quit_flag != 0;
}

void
rui_request_quit(void)
{
	quit_flag = 1;
}

/* ---------------------------------------------------------- async jobs */

#define MAX_JOBS 16
#define JOB_OUT_MAX (128 * 1024)

typedef struct Job {
	pid_t pid;
	int fd;
	char *out;
	size_t len, cap;
	RuiJobFn fn;
	void *ud;
	int stream;                       /* rui_watch_start: line by line */
} Job;

static Job jobs[MAX_JOBS];
/* /dev/null for the jobs' stdin and stderr, opened before the programs
 * pledge(2): a child that opened it for writing itself would be killed in
 * a program without "wpath" (retro-net, retro-bt, retro-wm, the session) */
static int null_fd = -1;

static void
null_open(void)
{
	int fd;

	/* kept above 2, so a program started with stdin closed still gives
	 * its children a real stdin rather than this close-on-exec copy */
	if (null_fd < 0 && (fd = open("/dev/null", O_RDWR)) >= 0) {
		null_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		close(fd);
	}
}

static int job_spawn(const char *cmd, RuiJobFn fn, void *ud, int stream);

int
rui_job_start(const char *cmd, RuiJobFn fn, void *ud)
{
	return job_spawn(cmd, fn, ud, 0);
}

int
rui_watch_start(const char *cmd, RuiJobFn fn, void *ud)
{
	int i;

	if (job_spawn(cmd, fn, ud, 1) < 0)
		return -1;
	for (i = MAX_JOBS - 1; i >= 0; i--)
		if (jobs[i].pid && jobs[i].stream && jobs[i].fn == fn &&
		    jobs[i].ud == ud)
			return (int)jobs[i].pid;
	return -1;
}

void
rui_watch_stop(int id)
{
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].pid != (pid_t)id || !jobs[i].stream || id <= 0)
			continue;
		kill(-jobs[i].pid, SIGTERM);       /* its own process group */
		jobs[i].fn = NULL;                 /* no callback for this end */
	}
}

static int
job_spawn(const char *cmd, RuiJobFn fn, void *ud, int stream)
{
	int i, p[2];
	pid_t pid;

	for (i = 0; i < MAX_JOBS; i++)
		if (jobs[i].pid == 0)
			break;
	if (i == MAX_JOBS)
		return -1;
	null_open();                        /* no-op after rui_open() */
	if (pipe(p) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		setsid();
		signal(SIGPIPE, SIG_DFL);
		dup2(p[1], 1);
		if (null_fd >= 0) {
			dup2(null_fd, 0);
			dup2(null_fd, 2);
		}
		close(p[0]);
		close(p[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	set_cloexec_nonblock(p[0]);
	jobs[i].pid = pid;
	jobs[i].fd = p[0];
	jobs[i].cap = 1024;
	jobs[i].len = 0;
	jobs[i].out = rui_malloc(jobs[i].cap);
	jobs[i].out[0] = '\0';
	jobs[i].fn = fn;
	jobs[i].ud = ud;
	jobs[i].stream = stream;
	return 0;
}

int
rui_jobs_active(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_JOBS; i++)
		if (jobs[i].pid)
			n++;
	return n;
}

static void
job_finish(Job *j)
{
	int status = 0;
	Job done = *j;

	close(j->fd);
	while (waitpid(j->pid, &status, 0) < 0 && errno == EINTR)
		;
	memset(j, 0, sizeof *j);          /* slot is free before the callback */
	if (done.fn)
		done.fn(done.stream ? NULL : done.out, WIFEXITED(status) ?
		    WEXITSTATUS(status) : -1, done.ud);
	free(done.out);
}

static void
job_read(Job *j)
{
	char buf[4096];

	for (;;) {
		ssize_t r = read(j->fd, buf, sizeof buf);

		if (r > 0) {
			if (j->len + (size_t)r + 1 > j->cap && j->cap < JOB_OUT_MAX) {
				while (j->cap < j->len + (size_t)r + 1)
					j->cap *= 2;
				j->out = rui_realloc(j->out, j->cap);
			}
			if (j->len + (size_t)r + 1 <= j->cap) {
				memcpy(j->out + j->len, buf, (size_t)r);
				j->len += (size_t)r;
				j->out[j->len] = '\0';
			} else if (j->stream) {
				j->len = 0;               /* a runaway line: drop it */
			}
			if (j->stream) {              /* hand over complete lines */
				char *nl;

				while (j->pid && (nl = memchr(j->out, '\n', j->len))) {
					size_t n = (size_t)(nl - j->out) + 1;

					*nl = '\0';
					if (j->fn)
						j->fn(j->out, 0, j->ud);
					memmove(j->out, j->out + n, j->len - n + 1);
					j->len -= n;
				}
			}
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		job_finish(j);                /* EOF or hard error */
		return;
	}
}

void
rui_jobs_shutdown(void)
{
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		if (!jobs[i].pid)
			continue;
		kill(jobs[i].stream ? -jobs[i].pid : jobs[i].pid, SIGTERM);
		close(jobs[i].fd);
		while (waitpid(jobs[i].pid, NULL, 0) < 0 && errno == EINTR)
			;
		free(jobs[i].out);
		memset(&jobs[i], 0, sizeof jobs[i]);
	}
}

/* ------------------------------------------------------------ X display */

static const char *atom_names[A_COUNT] = {
#define RUI_ATOM_NAME(n) #n,
	RUI_ATOMS(RUI_ATOM_NAME)
};

/* last-resort colours if a theme names an unknown one */
static const char *const fallback_colors[RUI_C_THEMED] = {
	"#c0c0c0", "#ffffff", "#808080", "#000000", "#000000", "#808080",
	"#ffffff", "#000080", "#ffffff", "#000080", "#808080", "#ffffff",
	"#c0c0c0", "#008080", "#ffffff", "#c00000", "#008000",
};

static int xerror_total;
static int xerror_verbose;

static int
on_xerror(Display *dpy, XErrorEvent *e)
{
	/* Races every window manager has to live with: a client destroys its
	 * window while requests about it are in flight (the same list dwm
	 * ignores).  Anything else is counted and, with RETRO_DEBUG, logged. */
	if (e->error_code == BadWindow ||
	    (e->request_code == X_SetInputFocus && e->error_code == BadMatch) ||
	    (e->request_code == X_PolyText8 && e->error_code == BadDrawable) ||
	    (e->request_code == X_PolyFillRectangle &&
	    e->error_code == BadDrawable) ||
	    (e->request_code == X_PolySegment && e->error_code == BadDrawable) ||
	    (e->request_code == X_ConfigureWindow && e->error_code == BadMatch) ||
	    (e->request_code == X_GrabButton && e->error_code == BadAccess) ||
	    (e->request_code == X_GrabKey && e->error_code == BadAccess) ||
	    (e->request_code == X_CopyArea && e->error_code == BadDrawable))
		return 0;
	xerror_total++;
	if (xerror_verbose) {
		char msg[128];

		XGetErrorText(dpy, e->error_code, msg, sizeof msg);
		fprintf(stderr, "%s: X error: %s (request %d.%d, resource 0x%lx)\n",
		    prog_name, msg, e->request_code, e->minor_code,
		    (unsigned long)e->resourceid);
	}
	return 0;
}

int
rui_xerror_count(void)
{
	return xerror_total;
}

/* lighter (f > 0) or darker (f < 0) variant of an allocated colour */
static void
shade(RuiApp *app, int src, int dst, double f)
{
	XRenderColor c = app->color[src].color;
	unsigned short *ch[3];
	int i;

	ch[0] = &c.red;
	ch[1] = &c.green;
	ch[2] = &c.blue;
	for (i = 0; i < 3; i++) {
		double v = *ch[i];

		v = f > 0 ? v + (65535.0 - v) * f : v * (1.0 + f);
		*ch[i] = (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v);
	}
	c.alpha = 0xffff;
	if (!XftColorAllocValue(app->dpy, app->vis, app->cmap, &c, &app->color[dst]))
		app->color[dst] = app->color[src];
}

RuiFont *
rui_font_open(RuiApp *app, const char *spec)
{
	RuiFont *f = rui_calloc(1, sizeof *f);

	if (!strncmp(spec, "core:", 5) || spec[0] == '-') {
		const char *name = spec[0] == '-' ? spec : spec + 5;
		char **missing = NULL, *def = NULL;
		int nmissing = 0;

		f->set = XCreateFontSet(app->dpy, name, &missing, &nmissing, &def);
		if (missing)
			XFreeStringList(missing);
		if (f->set) {
			XFontSetExtents *e = XExtentsOfFontSet(f->set);

			f->ascent = -e->max_logical_extent.y;
			f->descent = e->max_logical_extent.height - f->ascent;
		} else {
			rui_warn("cannot load core font '%s'", name);
		}
	}
	if (!f->set) {
		if (strncmp(spec, "core:", 5) && spec[0] != '-')
			f->xft = XftFontOpenName(app->dpy, app->screen, spec);
		if (!f->xft)
			f->xft = XftFontOpenName(app->dpy, app->screen, "sans-10");
		if (!f->xft)
			f->xft = XftFontOpenName(app->dpy, app->screen, "fixed");
		if (!f->xft)
			rui_die("no usable font");
		f->ascent = f->xft->ascent;
		f->descent = f->xft->descent;
	}
	f->height = f->ascent + f->descent;
	return f;
}

void
rui_font_close(RuiApp *app, RuiFont *f)
{
	if (!f)
		return;
	if (f->xft)
		XftFontClose(app->dpy, f->xft);
	if (f->set)
		XFreeFontSet(app->dpy, f->set);
	free(f);
}

int
rui_text_width(RuiApp *app, RuiFont *f, const char *s, int len)
{
	if (len < 0)
		len = (int)strlen(s);
	if (len == 0)
		return 0;
	if (f->xft) {
		XGlyphInfo gi;

		XftTextExtentsUtf8(app->dpy, f->xft, (const FcChar8 *)s, len, &gi);
		return gi.xOff;
	}
	return Xutf8TextEscapement(f->set, s, len);
}

RuiApp *
rui_open(const char *name)
{
	static const char bits[] = { 0x01, 0x02 };
	RuiApp *app = rui_calloc(1, sizeof *app);
	const RuiSettings *set;
	const char *dbg;
	int i, fl;

	prog_name = name;
	app->name = name;
	null_open();
	setlocale(LC_CTYPE, "");
	setlocale(LC_TIME, "");
	if ((dbg = getenv("RETRO_DEBUG")) && *dbg && strcmp(dbg, "0"))
		xerror_verbose = 1;
	app->dpy = XOpenDisplay(NULL);
	if (!app->dpy)
		rui_die("cannot open display '%s'", XDisplayName(NULL));
	fl = fcntl(ConnectionNumber(app->dpy), F_GETFD);
	if (fl >= 0)
		fcntl(ConnectionNumber(app->dpy), F_SETFD, fl | FD_CLOEXEC);
	XSetErrorHandler(on_xerror);
	app->screen = DefaultScreen(app->dpy);
	app->root = RootWindow(app->dpy, app->screen);
	app->vis = DefaultVisual(app->dpy, app->screen);
	app->cmap = DefaultColormap(app->dpy, app->screen);
	app->depth = DefaultDepth(app->dpy, app->screen);
	app->sw = DisplayWidth(app->dpy, app->screen);
	app->sh = DisplayHeight(app->dpy, app->screen);
	app->gc = XCreateGC(app->dpy, app->root, 0, NULL);
	XSetGraphicsExposures(app->dpy, app->gc, False);
	app->checker = XCreateBitmapFromData(app->dpy, app->root, bits, 2, 2);
	XInternAtoms(app->dpy, (char **)atom_names, A_COUNT, False, app->atom);

	set = rui_settings();
	app->style = set->style;
	app->theme = rui_theme_find(set->theme);
	app->theme_mod = rui_theme_module(set->theme);
	for (i = 0; i < RUI_C_THEMED; i++) {
		const char *c = set->color[i][0] ? set->color[i] : fallback_colors[i];

		if (!XftColorAllocName(app->dpy, app->vis, app->cmap, c,
		    &app->color[i])) {
			rui_warn("bad colour '%s'", c);
			if (!XftColorAllocName(app->dpy, app->vis, app->cmap,
			    fallback_colors[i], &app->color[i]))
				rui_die("cannot allocate colour %s", fallback_colors[i]);
		}
	}
	shade(app, RUI_C_TITLE_ACTIVE, RUI_C_ACT_LIGHT, 0.45);
	shade(app, RUI_C_TITLE_ACTIVE, RUI_C_ACT_SHADOW, -0.45);
	shade(app, RUI_C_TITLE_INACTIVE, RUI_C_INACT_LIGHT, 0.45);
	shade(app, RUI_C_TITLE_INACTIVE, RUI_C_INACT_SHADOW, -0.45);
	app->font = rui_font_open(app, set->font);
	if (set->font_bold[0]) {
		app->bold = rui_font_open(app, set->font_bold);
	} else if (app->font->xft) {
		char spec[300];

		snprintf(spec, sizeof spec, "%s:bold", set->font);
		app->bold = rui_font_open(app, spec);
	} else {
		app->bold = rui_font_open(app, set->font);
	}

	if (XSupportsLocale()) {
		XSetLocaleModifiers("");
		app->xim = XOpenIM(app->dpy, NULL, NULL, NULL);
		if (!app->xim) {
			XSetLocaleModifiers("@im=none");
			app->xim = XOpenIM(app->dpy, NULL, NULL, NULL);
		}
	}
	app->cur_arrow = XCreateFontCursor(app->dpy, 68 /* XC_left_ptr */);
	return app;
}

void
rui_close(RuiApp *app)
{
	int i;

	if (!app)
		return;
	rui_jobs_shutdown();
	rui_font_close(app, app->font);
	rui_font_close(app, app->bold);
	for (i = 0; i < RUI_C_COUNT; i++)
		XftColorFree(app->dpy, app->vis, app->cmap, &app->color[i]);
	if (app->xim)
		XCloseIM(app->xim);
	XFreeCursor(app->dpy, app->cur_arrow);
	XFreePixmap(app->dpy, app->checker);
	XFreeGC(app->dpy, app->gc);
	XSync(app->dpy, False);
	XCloseDisplay(app->dpy);
	if (sig_pipe[0] >= 0) {
		close(sig_pipe[0]);
		close(sig_pipe[1]);
		sig_pipe[0] = sig_pipe[1] = -1;
	}
	free(app);
}

void
rui_wait(RuiApp *app, int timeout_ms)
{
	fd_set rfds;
	struct timeval tv, *tvp = NULL;
	int xfd = ConnectionNumber(app->dpy), maxfd = xfd, i, r, bfd;

	if (XPending(app->dpy) || quit_flag)
		return;
	FD_ZERO(&rfds);
	FD_SET(xfd, &rfds);
	if ((bfd = rui_bus_wait_fd(&timeout_ms)) >= 0) {
		FD_SET(bfd, &rfds);
		if (bfd > maxfd)
			maxfd = bfd;
	}
	if (sig_pipe[0] >= 0) {
		FD_SET(sig_pipe[0], &rfds);
		if (sig_pipe[0] > maxfd)
			maxfd = sig_pipe[0];
	}
	for (i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].pid) {
			FD_SET(jobs[i].fd, &rfds);
			if (jobs[i].fd > maxfd)
				maxfd = jobs[i].fd;
		}
	}
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}
	r = select(maxfd + 1, &rfds, NULL, NULL, tvp);
	if (r <= 0)
		return;
	if (sig_pipe[0] >= 0 && FD_ISSET(sig_pipe[0], &rfds)) {
		char buf[16];

		while (read(sig_pipe[0], buf, sizeof buf) > 0)
			;
	}
	for (i = 0; i < MAX_JOBS; i++)
		if (jobs[i].pid && FD_ISSET(jobs[i].fd, &rfds))
			job_read(&jobs[i]);
	if (bfd >= 0 && bfd == rui_bus_fd() && FD_ISSET(bfd, &rfds))
		rui_bus_dispatch();
}

void
rui_wm_command(RuiApp *app, const char *verb)
{
	Atom a = XInternAtom(app->dpy, verb, False);

	rui_send_root_msg(app, app->root, app->atom[A__RETRO_WM_COMMAND],
	    (long)a, 0, 0, 0, 0);
	XFlush(app->dpy);
}

/* "@word rest": does cmd start with the special command `word`? */
static const char *
special(const char *cmd, const char *word)
{
	size_t n = strlen(word);

	if (strncmp(cmd, word, n) || (cmd[n] && cmd[n] != ' '))
		return NULL;
	cmd += n;
	while (*cmd == ' ')
		cmd++;
	return cmd;                            /* the arguments, maybe "" */
}

static void
run_tool(RuiApp *app, int tool, const char *args)
{
	char *cmd = rui_tool_command(tool), *full;

	if (!cmd) {
		rui_warn("no %s found; set one with the Style Manager or `%s =` in "
		    "retro.conf", rui_tool_keys[tool], rui_tool_keys[tool]);
		return;
	}
	if (cmd[0] == '@' && !strncmp(cmd, "@terminal", 9) && *args &&
	    tool != RUI_TOOL_TERMINAL)
		full = rui_strdupf("%s", cmd);     /* console tools take no args */
	else
		full = *args ? rui_strdupf("%s %s", cmd, args) : rui_strdup(cmd);
	free(cmd);
	if (full[0] == '@')
		rui_run_command(app, full);
	else
		rui_spawn(full);
	free(full);
}

static void
run_power(RuiApp *app, int action)
{
	static const char *const titles[RUI_POWER_ACTIONS] = {
		"Shut Down", "Restart", "Suspend", "Hibernate"
	};
	const RuiSettings *set = rui_settings();
	const RuiPowerModule *m = rui_power_detect(set->power);
	char *cmd = rui_power_command(m, action, set->power_priv), text[128];

	if (!cmd) {
		rui_warn("%s: not available (power module: %s)",
		    rui_power_action_names[action], m ? m->name : "none");
		return;
	}
	snprintf(text, sizeof text, "%s the computer now?", titles[action]);
	if (action > RUI_REBOOT || !set->confirm ||
	    rui_confirm(app, titles[action], text, titles[action]))
		rui_spawn(cmd);
	free(cmd);
}

/* Commands from menus, panels and key bindings.  Plain text runs through
 * /bin/sh; these special forms are understood:
 *   @terminal [args]  @browser [url]  @filemanager [dir]  @editor [file]
 *   @open target      @app ID [args]  @lock  @clock  @logout  @quit
 *   @restart
 *   @poweroff @reboot @suspend @hibernate   @<wm-action> (e.g. @next_layout) */
void
rui_run_command(RuiApp *app, const char *cmd)
{
	const char *a;
	int i;

	if (!cmd || !*cmd)
		return;
	if (cmd[0] != '@') {
		rui_spawn(cmd);
	} else if ((a = special(cmd, "@logout")) || (a = special(cmd, "@quit"))) {
		if (!rui_settings()->confirm ||
		    rui_confirm(app, "Log Out", "Continue logout?", "Log Out"))
			rui_wm_command(app, "quit");
	} else if ((a = special(cmd, "@terminal"))) {
		char *c = *a ? rui_strdupf("%s %s", rui_terminal(), a) :
		    rui_strdup(rui_terminal());

		rui_spawn(c);
		free(c);
	} else if ((a = special(cmd, "@browser"))) {
		run_tool(app, RUI_TOOL_BROWSER, a);
	} else if ((a = special(cmd, "@filemanager"))) {
		run_tool(app, RUI_TOOL_FILES, a);
	} else if ((a = special(cmd, "@editor"))) {
		run_tool(app, RUI_TOOL_EDITOR, a);
	} else if ((a = special(cmd, "@lock"))) {
		run_tool(app, RUI_TOOL_LOCKER, "");
	} else if ((a = special(cmd, "@clock"))) {
		run_tool(app, RUI_TOOL_CLOCK, a);
	} else if ((a = special(cmd, "@screensaver"))) {
		/* the screen saver's own settings program, else ours */
		if (rui_have_cmd("xscreensaver-settings"))
			rui_spawn("xscreensaver-settings");
		else if (rui_have_cmd("xscreensaver-demo"))
			rui_spawn("xscreensaver-demo");
		else
			rui_spawn("retro-settings --page 4");
	} else if ((a = special(cmd, "@open"))) {
		char *c;

		if (rui_xdg_menus() && rui_have_cmd("xdg-open")) {
			c = rui_strdupf("xdg-open %s", a);
			rui_spawn(c);
			free(c);
		} else if (!strncmp(a, "http", 4) || !strncmp(a, "'http", 5)) {
			run_tool(app, RUI_TOOL_BROWSER, a);
		} else {
			struct stat st;
			char path[1024];

			rui_strlcpy(path, a, sizeof path);
			run_tool(app, stat(path, &st) == 0 && S_ISDIR(st.st_mode) ?
			    RUI_TOOL_FILES : RUI_TOOL_EDITOR, a);
		}
	} else if ((a = special(cmd, "@app"))) {
		char id[256];
		RuiDesktopEntry *e;

		rui_strlcpy(id, a, sizeof id);
		id[strcspn(id, " ")] = '\0';
		a += strlen(id);
		while (*a == ' ')
			a++;
		e = rui_xdg_menus() ? rui_app_find(id) : NULL;
		if (e) {
			char *c = rui_desktop_command(e), *full;

			full = *a ? rui_strdupf("%s %s", c, a) : rui_strdup(c);
			rui_spawn(full);
			free(full);
			free(c);
			rui_desktop_free(e);
		} else {
			rui_warn("no application '%s'", id);
		}
	} else {
		for (i = 0; i < RUI_POWER_ACTIONS; i++) {
			char word[32];

			snprintf(word, sizeof word, "@%s", rui_power_action_names[i]);
			if (special(cmd, word)) {
				run_power(app, i);
				return;
			}
		}
		rui_wm_command(app, cmd + 1);      /* @restart, @next_layout ... */
	}
}

/* --------------------------------------------------------------- monitors */

static int rr_base = -1;

static int
mon_add(RuiMonitor *out, int n, int max, int x, int y, int w, int h,
    int primary, const char *name)
{
	int i;

	if (w <= 0 || h <= 0)
		return n;
	for (i = 0; i < n; i++) {
		if (out[i].x == x && out[i].y == y) {  /* mirrored: keep the larger */
			if (w > out[i].w)
				out[i].w = w;
			if (h > out[i].h)
				out[i].h = h;
			out[i].primary |= primary;
			return n;
		}
	}
	if (n >= max)
		return n;
	out[n].x = x;
	out[n].y = y;
	out[n].w = w;
	out[n].h = h;
	out[n].primary = primary;
	rui_strlcpy(out[n].name, name ? name : "", sizeof out[n].name);
	return n + 1;
}

int
rui_monitors(RuiApp *app, RuiMonitor *out, int max)
{
	int n = 0, i, evb, erb, prim = -1;

	const char *fake = getenv("RETRO_DE_MONITORS");

	if (max < 1)
		return 0;
	/* RETRO_DE_MONITORS="1920x1080+0+0,1280x1024+1920+0": a made-up
	 * layout, the first entry primary - splits one wide screen in two and
	 * lets the tests run a multi-monitor desktop on a virtual server */
	while (fake && *fake) {
		int w, h, x, y, len = 0;

		if (sscanf(fake, "%dx%d+%d+%d%n", &w, &h, &x, &y, &len) < 4 || !len)
			break;
		n = mon_add(out, n, max, x, y, w, h, n == 0, NULL);
		fake += len;
		if (*fake == ',')
			fake++;
	}
	if (n > 0)
		return n;
#if RANDR_MAJOR > 1 || (RANDR_MAJOR == 1 && RANDR_MINOR >= 5)
	if (XRRQueryExtension(app->dpy, &evb, &erb)) {
		int major = 0, minor = 0, nm = 0;

		if (XRRQueryVersion(app->dpy, &major, &minor) &&
		    (major > 1 || (major == 1 && minor >= 5))) {
			XRRMonitorInfo *mi = XRRGetMonitors(app->dpy, app->root, True,
			    &nm);

			for (i = 0; mi && i < nm; i++) {
				char *nm_s = mi[i].name ? XGetAtomName(app->dpy,
				    mi[i].name) : NULL;

				n = mon_add(out, n, max, mi[i].x, mi[i].y, mi[i].width,
				    mi[i].height, mi[i].primary, nm_s);
				if (nm_s)
					XFree(nm_s);
			}
			if (mi)
				XRRFreeMonitors(mi);
		}
	}
#else
	(void)evb;
	(void)erb;
#endif
	if (n == 0 && XineramaIsActive(app->dpy)) {
		int nx = 0;
		XineramaScreenInfo *xi = XineramaQueryScreens(app->dpy, &nx);

		for (i = 0; xi && i < nx; i++)
			n = mon_add(out, n, max, xi[i].x_org, xi[i].y_org, xi[i].width,
			    xi[i].height, 0, NULL);
		if (xi)
			XFree(xi);
	}
	if (n == 0)
		n = mon_add(out, n, max, 0, 0, DisplayWidth(app->dpy, app->screen),
		    DisplayHeight(app->dpy, app->screen), 1, NULL);
	for (i = 0; i < n; i++) {
		if (out[i].primary && prim < 0)
			prim = i;
		out[i].primary = 0;
	}
	out[prim < 0 ? 0 : prim].primary = 1;
	return n;
}

RuiMonitor
rui_monitor_pick(RuiApp *app, const char *spec)
{
	RuiMonitor m[RUI_MAX_MONITORS];
	int n = rui_monitors(app, m, RUI_MAX_MONITORS), i, prim = 0;
	char *end;
	long idx;

	for (i = 0; i < n; i++)
		if (m[i].primary)
			prim = i;
	if (!spec || !*spec || !strcmp(spec, "primary"))
		return m[prim];
	idx = strtol(spec, &end, 10);
	if (!*end)
		return idx >= 1 && idx <= n ? m[idx - 1] : m[prim];
	for (i = 0; i < n; i++)
		if (!strcmp(m[i].name, spec))
			return m[i];
	return m[prim];
}

void
rui_monitors_watch(RuiApp *app)
{
	int evb, erb;

	if (!XRRQueryExtension(app->dpy, &evb, &erb))
		return;
	rr_base = evb;
	XRRSelectInput(app->dpy, app->root, RRScreenChangeNotifyMask |
	    RRCrtcChangeNotifyMask | RROutputChangeNotifyMask |
	    RROutputPropertyNotifyMask);
}

int
rui_monitors_event(RuiApp *app, XEvent *ev)
{
	(void)app;
	if (rr_base < 0)
		return 0;
	if (ev->type == rr_base + RRScreenChangeNotify) {
		XRRUpdateConfiguration(ev);
		return 1;
	}
	return ev->type == rr_base + RRNotify;
}

/* ---------------------------------------------------- windows / properties */

Window
rui_window_new(RuiApp *app, Window parent, int x, int y, int w, int h,
    int override_redirect, long event_mask)
{
	XSetWindowAttributes wa;

	wa.override_redirect = override_redirect ? True : False;
	wa.background_pixmap = None;
	wa.event_mask = event_mask;
	wa.bit_gravity = NorthWestGravity;
	wa.save_under = override_redirect ? True : False;
	wa.cursor = app->cur_arrow;
	return XCreateWindow(app->dpy, parent, x, y, (unsigned)(w > 0 ? w : 1),
	    (unsigned)(h > 0 ? h : 1), 0, CopyFromParent, InputOutput,
	    CopyFromParent, CWOverrideRedirect | CWBackPixmap | CWEventMask |
	    CWBitGravity | CWSaveUnder | CWCursor, &wa);
}

int
rui_get_prop32(RuiApp *app, Window w, Atom prop, unsigned long **out)
{
	Atom type;
	int fmt, n = 0;
	unsigned long nitems = 0, after, i;
	unsigned char *data = NULL;

	*out = NULL;
	if (XGetWindowProperty(app->dpy, w, prop, 0, 65536, False,
	    AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success)
		return 0;
	if (data && fmt == 32 && nitems > 0) {
		unsigned long *src = (unsigned long *)(void *)data;

		*out = rui_malloc(nitems * sizeof **out);
		for (i = 0; i < nitems; i++)
			(*out)[i] = src[i] & 0xffffffffUL;
		n = (int)nitems;
	}
	if (data)
		XFree(data);
	return n;
}

void
rui_set_prop32(RuiApp *app, Window w, Atom prop, Atom type,
    const unsigned long *vals, int n)
{
	XChangeProperty(app->dpy, w, prop, type, 32, PropModeReplace,
	    (const unsigned char *)vals, n);
}

char *
rui_get_text_prop(RuiApp *app, Window w, Atom prop)
{
	XTextProperty tp;
	char *res = NULL;

	memset(&tp, 0, sizeof tp);
	if (!XGetTextProperty(app->dpy, w, &tp, prop) || !tp.value)
		return NULL;
	if (tp.nitems > 0 && tp.format == 8) {
		if (tp.encoding == app->atom[A_UTF8_STRING]) {
			res = rui_strdup((char *)tp.value);
		} else {
			char **list = NULL;
			int n = 0;

			if (Xutf8TextPropertyToTextList(app->dpy, &tp, &list, &n) >=
			    Success && n > 0 && list && list[0])
				res = rui_strdup(list[0]);
			if (list)
				XFreeStringList(list);
		}
	}
	XFree(tp.value);
	return res;
}

char *
rui_window_title(RuiApp *app, Window w)
{
	char *t = rui_get_text_prop(app, w, app->atom[A__NET_WM_NAME]);

	if (!t || !*t) {
		free(t);
		t = rui_get_text_prop(app, w, XA_WM_NAME);
	}
	if (!t)
		t = rui_strdup("(untitled)");
	return t;
}

void
rui_set_title(RuiApp *app, Window w, const char *title, const char *cls)
{
	XClassHint ch;
	char res_name[64], res_class[64];
	Atom protos[1];

	XStoreName(app->dpy, w, title);
	XChangeProperty(app->dpy, w, app->atom[A__NET_WM_NAME],
	    app->atom[A_UTF8_STRING], 8, PropModeReplace,
	    (const unsigned char *)title, (int)strlen(title));
	rui_strlcpy(res_name, cls, sizeof res_name);
	rui_strlcpy(res_class, cls, sizeof res_class);
	if (res_class[0] >= 'a' && res_class[0] <= 'z')
		res_class[0] = (char)(res_class[0] - 'a' + 'A');
	ch.res_name = res_name;
	ch.res_class = res_class;
	XSetClassHint(app->dpy, w, &ch);
	protos[0] = app->atom[A_WM_DELETE_WINDOW];
	XSetWMProtocols(app->dpy, w, protos, 1);
}

void
rui_set_window_type(RuiApp *app, Window w, int type_atom_index)
{
	unsigned long v = app->atom[type_atom_index];

	rui_set_prop32(app, w, app->atom[A__NET_WM_WINDOW_TYPE], XA_ATOM, &v, 1);
}

void
rui_send_root_msg(RuiApp *app, Window w, Atom type, long d0, long d1, long d2,
    long d3, long d4)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	ev.xclient.data.l[3] = d3;
	ev.xclient.data.l[4] = d4;
	XSendEvent(app->dpy, app->root, False,
	    SubstructureNotifyMask | SubstructureRedirectMask, &ev);
}

int
rui_wm_delete_event(RuiApp *app, const XEvent *ev)
{
	return ev->type == ClientMessage &&
	    ev->xclient.message_type == app->atom[A_WM_PROTOCOLS] &&
	    (Atom)ev->xclient.data.l[0] == app->atom[A_WM_DELETE_WINDOW];
}

/* ---------------------------------------------------------------- drawing */

RuiSurf *
rui_surf_new(RuiApp *app, Window win, int w, int h)
{
	RuiSurf *s = rui_calloc(1, sizeof *s);

	s->app = app;
	s->win = win;
	s->w = w > 0 ? w : 1;
	s->h = h > 0 ? h : 1;
	s->buf = XCreatePixmap(app->dpy, app->root, (unsigned)s->w, (unsigned)s->h,
	    (unsigned)app->depth);
	s->xd = XftDrawCreate(app->dpy, s->buf, app->vis, app->cmap);
	return s;
}

void
rui_surf_resize(RuiSurf *s, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (w == s->w && h == s->h)
		return;
	XFreePixmap(s->app->dpy, s->buf);
	s->w = w;
	s->h = h;
	s->buf = XCreatePixmap(s->app->dpy, s->app->root, (unsigned)w, (unsigned)h,
	    (unsigned)s->app->depth);
	XftDrawChange(s->xd, s->buf);
}

void
rui_surf_free(RuiSurf *s)
{
	if (!s)
		return;
	XftDrawDestroy(s->xd);
	XFreePixmap(s->app->dpy, s->buf);
	free(s);
}

void
rui_surf_flush(RuiSurf *s)
{
	XCopyArea(s->app->dpy, s->buf, s->win, s->app->gc, 0, 0, (unsigned)s->w,
	    (unsigned)s->h, 0, 0);
}

void
rui_fill_pixel(RuiSurf *s, int x, int y, int w, int h, unsigned long px)
{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(s->app->dpy, s->app->gc, px);
	XFillRectangle(s->app->dpy, s->buf, s->app->gc, x, y, (unsigned)w,
	    (unsigned)h);
}

void
rui_fill(RuiSurf *s, int x, int y, int w, int h, int color)
{
	rui_fill_pixel(s, x, y, w, h, s->app->color[color].pixel);
}

void
rui_fill_dither(RuiSurf *s, int x, int y, int w, int h, int c1, int c2)
{
	RuiApp *a = s->app;

	if (w <= 0 || h <= 0)
		return;
	XSetForeground(a->dpy, a->gc, a->color[c1].pixel);
	XSetBackground(a->dpy, a->gc, a->color[c2].pixel);
	XSetStipple(a->dpy, a->gc, a->checker);
	XSetFillStyle(a->dpy, a->gc, FillOpaqueStippled);
	XFillRectangle(a->dpy, s->buf, a->gc, x, y, (unsigned)w, (unsigned)h);
	XSetFillStyle(a->dpy, a->gc, FillSolid);
}

void
rui_line(RuiSurf *s, int x1, int y1, int x2, int y2, int color)
{
	XSetForeground(s->app->dpy, s->app->gc, s->app->color[color].pixel);
	XDrawLine(s->app->dpy, s->buf, s->app->gc, x1, y1, x2, y2);
}

/* one-pixel rectangle outline: top/left in `tl`, bottom/right in `br` */
static void
edge(RuiSurf *s, int x, int y, int w, int h, int tl, int br)
{
	if (w < 2 || h < 2)
		return;
	rui_fill(s, x, y, w - 1, 1, tl);
	rui_fill(s, x, y, 1, h - 1, tl);
	rui_fill(s, x, y + h - 1, w, 1, br);
	rui_fill(s, x + w - 1, y, 1, h, br);
}

void
rui_bevel_c(RuiSurf *s, int x, int y, int w, int h, int raised, int light,
    int shadow, int thick)
{
	int t;

	for (t = 0; t < thick; t++)
		edge(s, x + t, y + t, w - 2 * t, h - 2 * t, raised ? light : shadow,
		    raised ? shadow : light);
}

void
rui_bevel(RuiSurf *s, int x, int y, int w, int h, int style)
{
	const RuiThemeModule *tm = s->app->theme_mod;

	if (tm && tm->bevel && tm->bevel(s, x, y, w, h, style))
		return;                            /* the theme drew it */
	if (s->app->style == RUI_STYLE_CDE && style != RUI_GROOVE) {
		/* Motif: two-pixel light/shadow bevels, no black outline */
		int thin = style == RUI_RAISED_THIN || style == RUI_SUNKEN_THIN;
		int raised = style == RUI_RAISED || style == RUI_RAISED_THIN;

		rui_bevel_c(s, x, y, w, h, raised, RUI_C_LIGHT, RUI_C_SHADOW,
		    thin ? 1 : 2);
		return;
	}
	switch (style) {
	case RUI_RAISED:
		edge(s, x, y, w, h, RUI_C_LIGHT, RUI_C_DARK);
		edge(s, x + 1, y + 1, w - 2, h - 2, RUI_C_FACE, RUI_C_SHADOW);
		break;
	case RUI_SUNKEN:
		edge(s, x, y, w, h, RUI_C_SHADOW, RUI_C_LIGHT);
		edge(s, x + 1, y + 1, w - 2, h - 2, RUI_C_DARK, RUI_C_FACE);
		break;
	case RUI_PRESSED:
		edge(s, x, y, w, h, RUI_C_DARK, RUI_C_LIGHT);
		edge(s, x + 1, y + 1, w - 2, h - 2, RUI_C_SHADOW, RUI_C_FACE);
		break;
	case RUI_RAISED_THIN:
		edge(s, x, y, w, h, RUI_C_LIGHT, RUI_C_SHADOW);
		break;
	case RUI_SUNKEN_THIN:
		edge(s, x, y, w, h, RUI_C_SHADOW, RUI_C_LIGHT);
		break;
	case RUI_GROOVE:
		edge(s, x, y, w, h, RUI_C_SHADOW, RUI_C_LIGHT);
		edge(s, x + 1, y + 1, w - 2, h - 2, RUI_C_LIGHT, RUI_C_SHADOW);
		break;
	default:
		break;
	}
}

/* Xft blends glyphs into the back buffer with RENDER, and on a PseudoColor
 * (8-bit, palette) screen the server's indexed pictures turn every glyph box
 * into a solid block.  There the text goes through a 1-bit mask instead:
 * same font, no antialiasing, no blending.  Honours a clip set on app->gc. */
static void
text_through_mask(RuiSurf *s, RuiFont *f, int x, int y, const char *str,
    int len, unsigned long pixel)
{
	RuiApp *a = s->app;
	XGlyphInfo gi;
	XftColor one;
	XftDraw *md;
	Pixmap m;
	GC mg;
	int ox, w, h = f->height + 2;

	XftTextExtentsUtf8(a->dpy, f->xft, (const FcChar8 *)str, len, &gi);
	ox = gi.x > 0 ? gi.x : 0;               /* ink left of the origin */
	w = ox + (gi.width - gi.x > gi.xOff ? gi.width - gi.x : gi.xOff) + 2;
	if (w <= 0)
		return;
	m = XCreatePixmap(a->dpy, s->buf, (unsigned)w, (unsigned)h, 1);
	mg = XCreateGC(a->dpy, m, 0, NULL);
	XSetForeground(a->dpy, mg, 0);
	XFillRectangle(a->dpy, m, mg, 0, 0, (unsigned)w, (unsigned)h);
	memset(&one, 0, sizeof one);
	one.pixel = 1;
	one.color.alpha = 0xffff;
	if ((md = XftDrawCreateBitmap(a->dpy, m))) {
		XftDrawStringUtf8(md, &one, f->xft, ox, 1 + f->ascent,
		    (const FcChar8 *)str, len);
		XftDrawDestroy(md);
	}
	XSetForeground(a->dpy, a->gc, pixel);
	XSetStipple(a->dpy, a->gc, m);
	XSetTSOrigin(a->dpy, a->gc, x - ox, y - 1);
	XSetFillStyle(a->dpy, a->gc, FillStippled);
	XFillRectangle(a->dpy, s->buf, a->gc, x - ox, y - 1, (unsigned)w,
	    (unsigned)h);
	XSetFillStyle(a->dpy, a->gc, FillSolid);
	XSetTSOrigin(a->dpy, a->gc, 0, 0);
	XFreeGC(a->dpy, mg);
	XFreePixmap(a->dpy, m);
}

void
rui_text_c(RuiSurf *s, RuiFont *f, int x, int y, const char *str, int len,
    const XftColor *c)
{
	RuiApp *a = s->app;

	if (len < 0)
		len = (int)strlen(str);
	if (len == 0)
		return;
	if (f->xft && (a->vis->class == TrueColor ||
	    a->vis->class == DirectColor)) {
		XftDrawStringUtf8(s->xd, c, f->xft, x, y + f->ascent,
		    (const FcChar8 *)str, len);
	} else if (f->xft) {
		text_through_mask(s, f, x, y, str, len, c->pixel);
	} else {
		XSetForeground(a->dpy, a->gc, c->pixel);
		Xutf8DrawString(a->dpy, s->buf, f->set, a->gc, x, y + f->ascent, str,
		    len);
	}
}

void
rui_text(RuiSurf *s, RuiFont *f, int x, int y, const char *str, int len,
    int color)
{
	rui_text_c(s, f, x, y, str, len, &s->app->color[color]);
}

int
rui_text_fit(RuiSurf *s, RuiFont *f, RuiRect r, const char *str, int color,
    int align)
{
	RuiApp *a = s->app;
	char buf[512];
	int len = (int)strlen(str), w, x, y;

	if (r.w <= 0)
		return 0;
	w = rui_text_width(a, f, str, len);
	if (w > r.w) {
		int dots = rui_text_width(a, f, "...", 3);

		if (len > (int)sizeof buf - 4)
			len = (int)sizeof buf - 4;
		while (len > 0) {
			len--;
			while (len > 0 && ((unsigned char)str[len] & 0xc0) == 0x80)
				len--;
			if (rui_text_width(a, f, str, len) + dots <= r.w)
				break;
		}
		memcpy(buf, str, (size_t)len);
		memcpy(buf + len, "...", 4);
		str = buf;
		len += 3;
		w = rui_text_width(a, f, str, len);
		if (w > r.w)
			return 0;
	}
	x = r.x;
	if (align == RUI_CENTER)
		x = r.x + (r.w - w) / 2;
	else if (align == RUI_RIGHT)
		x = r.x + r.w - w;
	y = r.y + (r.h - f->height) / 2;
	rui_text(s, f, x, y, str, len, color);
	return w;
}

/* small solid triangle; dir: 0 up, 1 down, 2 left, 3 right */
void
rui_arrow(RuiSurf *s, int cx, int cy, int size, int dir, int color)
{
	int i;

	for (i = 0; i < size; i++) {
		int half = i;

		switch (dir) {
		case 0:
			rui_fill(s, cx - half, cy - size / 2 + i, 2 * half + 1, 1, color);
			break;
		case 1:
			rui_fill(s, cx - half, cy + size / 2 - i, 2 * half + 1, 1, color);
			break;
		case 2:
			rui_fill(s, cx - size / 2 + i, cy - half, 1, 2 * half + 1, color);
			break;
		default:
			rui_fill(s, cx + size / 2 - i, cy - half, 1, 2 * half + 1, color);
			break;
		}
	}
}

int
rui_in_rect(RuiRect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
