/*
 * retro-msg - talk to retro-bus from the shell.
 *
 *   retro-msg call SERVICE METHOD [ARG...]   print the reply
 *   retro-msg send SERVICE METHOD [ARG...]   the same, not waiting
 *   retro-msg emit TOPIC [ARG...]            broadcast an event
 *   retro-msg listen [PATTERN...]            print events (default "*")
 *   retro-msg serve NAME COMMAND             be a service: each call runs
 *                                            COMMAND METHOD ARG... with sh;
 *                                            its output is the reply
 *   retro-msg list                           the services on the bus
 *   retro-msg notify [-t SEC] [-i ICON] [-u low|normal|critical]
 *                    [-A key:Label|key2:Label2] [-a APP] [-r ID]
 *                    SUMMARY [BODY]          prints the notification's number
 *   retro-msg path                           where the bus socket is
 *
 * Options before the command: -w MS (call timeout, default 5000), -q.
 * Exit status: 0 reply, 1 error answer, 2 no bus or no answer, 64 usage.
 */
#include "../lib/retroui.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int timeout_ms = 5000, quiet;
static const char *serve_cmd;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: retro-msg [-w ms] [-q] call SERVICE METHOD [ARG...]\n"
	    "       retro-msg send SERVICE METHOD [ARG...]\n"
	    "       retro-msg emit TOPIC [ARG...]\n"
	    "       retro-msg listen [PATTERN...]\n"
	    "       retro-msg serve NAME COMMAND\n"
	    "       retro-msg list | path\n"
	    "       retro-msg notify [-t seconds] [-i icon] [-u urgency] "
	    "[-A actions] [-a app]\n"
	    "                        [-r id] SUMMARY [BODY]\n");
	exit(64);
}

/* ARG... joined with tabs (the field separator) */
static char *
join(int argc, char **argv)
{
	size_t n = 1;
	char *s;
	int i;

	for (i = 0; i < argc; i++)
		n += strlen(argv[i]) + 1;
	s = rui_malloc(n);
	s[0] = '\0';
	for (i = 0; i < argc; i++) {
		if (i)
			strcat(s, "\t");
		strcat(s, argv[i]);
	}
	return s;
}

static int
no_bus(void)
{
	if (!quiet) {
		char *p = rui_bus_path();

		fprintf(stderr, "retro-msg: no bus at %s\n", p);
		free(p);
	}
	return 2;
}

static int
do_call(const char *svc, const char *method, const char *args)
{
	int ok = 0;
	char *ans;

	if (rui_bus_connect() < 0)
		return no_bus();
	ans = rui_bus_call(svc, method, args, timeout_ms, &ok);
	if (!ans) {
		if (!quiet)
			fprintf(stderr, "retro-msg: no answer from %s\n", svc);
		return 2;
	}
	if (ok) {
		if (*ans)
			printf("%s\n", ans);
	} else if (!quiet) {
		fprintf(stderr, "retro-msg: %s: %s\n", svc, ans);
	}
	free(ans);
	return ok ? 0 : 1;
}

static void
print_event(const RuiBusMsg *m, void *ud)
{
	(void)ud;
	if (strcmp(m->kind, "event"))
		return;
	printf("%s\t%s%s%s\n", m->name, m->from, *m->args ? "\t" : "",
	    m->args);
	fflush(stdout);
}

/* runs serve_cmd with the method and the fields as $1, $2 ... */
static void
serve_call(const RuiBusMsg *m, void *ud)
{
	char *f[64], *out = NULL;
	char *argv[70];
	size_t len = 0, cap = 0;
	int nf, i, p[2], status = 0;
	pid_t pid;

	(void)ud;
	if (strcmp(m->kind, "call"))
		return;
	nf = *m->args ? rui_bus_fields(m->args, f, 64) : 0;
	if (pipe(p) < 0) {
		rui_bus_reply(m->id, 0, "pipe failed");
		return;
	}
	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = rui_strdupf("%s \"$@\"", serve_cmd);
	argv[3] = "retro-msg";
	argv[4] = (char *)m->name;
	for (i = 0; i < nf; i++)
		argv[5 + i] = f[i];
	argv[5 + nf] = NULL;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		setenv("RETRO_MSG_FROM", m->from, 1);
		setenv("RETRO_MSG_SERVICE", m->service, 1);
		execv("/bin/sh", argv);
		_exit(127);
	}
	free(argv[2]);
	close(p[1]);
	for (;;) {
		ssize_t r;

		if (cap - len < 4096)
			out = rui_realloc(out, cap = cap ? cap * 2 : 4096);
		r = read(p[0], out + len, cap - len - 1);
		if (r > 0)
			len += (size_t)r;
		else if (r < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	close(p[0]);
	out[len] = '\0';
	while (len && out[len - 1] == '\n')
		out[--len] = '\0';
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	rui_bus_reply(m->id, pid > 0 && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, out);
	free(out);
}

static int
loop(void)
{
	while (rui_bus_fd() >= 0) {
		struct pollfd p;

		p.fd = rui_bus_fd();
		p.events = POLLIN;
		if (poll(&p, 1, -1) < 0 && errno != EINTR)
			break;
		rui_bus_dispatch();
	}
	if (!quiet)
		fprintf(stderr, "retro-msg: the bus went away\n");
	return 2;
}

int
main(int argc, char **argv)
{
	const char *cmd;
	char *args;
	int i = 1, r;

	signal(SIGPIPE, SIG_IGN);
	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			timeout_ms = atoi(argv[i += 1]);
		else if (!strcmp(argv[i], "-q"))
			quiet = 1;
		else if (!strcmp(argv[i], "-v")) {
			puts("retro-msg " RUI_VERSION);
			return 0;
		} else
			usage();
		i++;
	}
	if (i >= argc)
		usage();
	cmd = argv[i++];
	argc -= i;
	argv += i;

	if (!strcmp(cmd, "path")) {
		char *p = rui_bus_path();

		puts(p);
		free(p);
		return 0;
	}
	if (!strcmp(cmd, "call") || !strcmp(cmd, "send")) {
		if (argc < 2)
			usage();
		args = join(argc - 2, argv + 2);
		if (cmd[0] == 'c')
			r = do_call(argv[0], argv[1], args);
		else
			r = rui_bus_send(argv[0], argv[1], args) < 0 ? no_bus() : 0;
		free(args);
		return r;
	}
	if (!strcmp(cmd, "emit")) {
		if (argc < 1)
			usage();
		args = join(argc - 1, argv + 1);
		r = rui_bus_emit(argv[0], args) < 0 ? no_bus() : 0;
		free(args);
		return r;
	}
	if (!strcmp(cmd, "list"))
		return do_call("bus", "list", "");
	if (!strcmp(cmd, "notify")) {
		/* summary body seconds icon urgency actions app replaces */
		char *f[8] = { "", "", "", "", "", "", "", "" };

		while (argc > 1 && argv[0][0] == '-' && argv[0][1] &&
		    !argv[0][2]) {
			switch (argv[0][1]) {
			case 't': f[2] = argv[1]; break;
			case 'i': f[3] = argv[1]; break;
			case 'u': f[4] = argv[1]; break;
			case 'A': f[5] = argv[1]; break;
			case 'a': f[6] = argv[1]; break;
			case 'r': f[7] = argv[1]; break;
			default: usage();
			}
			argc -= 2;
			argv += 2;
		}
		if (argc < 1 || argc > 2)
			usage();
		f[0] = argv[0];
		f[1] = argc > 1 ? argv[1] : "";
		args = join(8, f);
		r = do_call("notify", "show", args);
		free(args);
		return r;
	}
	if (!strcmp(cmd, "listen")) {
		if (rui_bus_connect() < 0)
			return no_bus();
		rui_bus_handler(print_event, NULL);
		if (argc == 0)
			rui_bus_listen("*");
		for (i = 0; i < argc; i++) {
			if (!rui_bus_word_ok(argv[i], 1)) {
				fprintf(stderr, "retro-msg: bad pattern '%s'\n", argv[i]);
				return 64;
			}
			rui_bus_listen(argv[i]);
		}
		return loop();
	}
	if (!strcmp(cmd, "serve")) {
		if (argc != 2)
			usage();
		if (rui_bus_connect() < 0)
			return no_bus();
		serve_cmd = argv[1];
		rui_bus_handler(serve_call, NULL);
		if (rui_bus_claim(argv[0]) < 0) {
			fprintf(stderr, "retro-msg: cannot have the name %s\n",
			    argv[0]);
			return 1;
		}
		return loop();
	}
	usage();
	return 64;
}
