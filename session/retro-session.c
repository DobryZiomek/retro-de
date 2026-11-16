/*
 * retro-session - starts and supervises the desktop: restores the monitor
 * layout saved by retro-display, then retro-wm, retro-desk, retro-bar, the
 * optional ~/.config/retro-de/autostart shell script and - when enabled in
 * config.h - XDG autostart .desktop entries.
 *
 * Put `exec retro-session` in ~/.xinitrc, or pick "Retro" in a display
 * manager.  The session ends when the window manager exits cleanly (Log
 * Out); crashed components are respawned a few times.  It works with any
 * init system: it is just fork, exec and waitpid.
 *
 * D-Bus is optional: with `dbus = auto` (the default) a private session bus
 * is started only if none exists and dbus-daemon is installed, so programs
 * that want one work; without it everything else still runs.
 */
#include "../lib/retroui.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_RESPAWN 5

typedef struct Comp {
	const char *name;
	pid_t pid;
	int respawns, wanted;
} Comp;

static Comp comps[] = {
	{ "retro-wm", 0, 0, 1 },
	{ "retro-desk", 0, 0, 1 },
	{ "retro-bar", 0, 0, 1 },
};
#define NCOMPS ((int)(sizeof comps / sizeof comps[0]))

static volatile sig_atomic_t got_signal;
static pid_t dbus_pid;                 /* the session bus we started */

static void
dbus_start(void)
{
	int mode = rui_settings()->dbus;
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
	char line[512];
	long pid = 0;
	FILE *p;

	if (mode == RUI_OFF || (addr && *addr))
		return;                            /* disabled, or a bus exists */
	if (!rui_have_cmd("dbus-daemon")) {
		if (mode == RUI_ON)
			fprintf(stderr, "retro-session: dbus = on, but dbus-daemon is "
			    "not installed\n");
		return;
	}
	p = popen("dbus-daemon --session --fork --print-address=1 --print-pid=1 "
	    "2>/dev/null", "r");
	if (!p)
		return;
	if (fgets(line, sizeof line, p)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "unix:", 5) || !strncmp(line, "tcp:", 4))
			setenv("DBUS_SESSION_BUS_ADDRESS", line, 1);
	}
	if (fgets(line, sizeof line, p))
		pid = atol(line);
	pclose(p);
	if (pid <= 0 || !getenv("DBUS_SESSION_BUS_ADDRESS")) {
		fprintf(stderr, "retro-session: could not start a D-Bus session "
		    "bus\n");
		return;
	}
	dbus_pid = (pid_t)pid;
	/* D-Bus-activated services should know our display */
	if (rui_have_cmd("dbus-update-activation-environment") &&
	    system("dbus-update-activation-environment DISPLAY XAUTHORITY "
	    "XDG_CURRENT_DESKTOP >/dev/null 2>&1") != 0)
		fprintf(stderr, "retro-session: dbus-update-activation-environment "
		    "failed\n");
}

static void
dbus_stop(void)
{
	if (dbus_pid > 0)
		kill(dbus_pid, SIGTERM);
	dbus_pid = 0;
}

/* The screen saver is xscreensaver when it is installed (the one NsCDE and
 * most classic desktops use): it brings the savers, the timeouts and the
 * lock, and is set up with its own xscreensaver-settings.  Without it the
 * window manager turns on the X server's plain blanking. */
static int saver_started;

static void
saver_start(void)
{
	if (strcmp(rui_saver_kind(), "xscreensaver"))
		return;
	/* one may be running already (started from ~/.xinitrc) */
	if (rui_have_cmd("xscreensaver-command") &&
	    system("xscreensaver-command -version >/dev/null 2>&1") == 0)
		return;
	rui_spawn("xscreensaver -no-splash >/dev/null 2>&1");
	saver_started = 1;
}

static void
saver_stop(void)
{
	if (saver_started && rui_have_cmd("xscreensaver-command") &&
	    system("xscreensaver-command -exit >/dev/null 2>&1") != 0)
		fprintf(stderr, "retro-session: xscreensaver did not exit\n");
	saver_started = 0;
}

static void
on_signal(int sig)
{
	(void)sig;
	got_signal = 1;
}

static void
msleep(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static pid_t
start(const char *name)
{
	pid_t pid = fork();

	if (pid < 0) {
		fprintf(stderr, "retro-session: fork: %s\n", strerror(errno));
		return 0;
	}
	if (pid == 0) {
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		execlp(name, name, (char *)NULL);
		fprintf(stderr, "retro-session: cannot run %s: %s\n", name,
		    strerror(errno));
		_exit(127);
	}
	return pid;
}

/* Prepend the directory holding this binary to $PATH so the suite finds its
 * siblings even when it is not installed system-wide. */
static void
setup_path(const char *argv0)
{
	char real[PATH_MAX], *slash, *npath;
	const char *path = getenv("PATH");
	size_t n;

	if (!strchr(argv0, '/') || !realpath(argv0, real))
		return;
	slash = strrchr(real, '/');
	if (!slash || slash == real)
		return;
	*slash = '\0';
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	n = strlen(real) + strlen(path) + 2;
	npath = malloc(n);
	if (!npath)
		return;
	snprintf(npath, n, "%s:%s", real, path);
	setenv("PATH", npath, 1);
	free(npath);
	/* run from the build tree: its theme modules and wallpapers count
	 * as the installed ones */
	if ((slash = strrchr(real, '/')) && slash != real) {
		char probe[PATH_MAX + 32];
		struct stat st;

		*slash = '\0';
		snprintf(probe, sizeof probe, "%s/themes", real);
		if (!getenv("RETRO_DE_THEME_PATH") && stat(probe, &st) == 0 &&
		    S_ISDIR(st.st_mode))
			setenv("RETRO_DE_THEME_PATH", probe, 1);
		snprintf(probe, sizeof probe, "%s/wallpapers", real);
		if (!getenv("RETRO_DE_DATA_DIR") && stat(probe, &st) == 0 &&
		    S_ISDIR(st.st_mode))
			setenv("RETRO_DE_DATA_DIR", real, 1);
	}
}

/* the saved monitor layout (retro-display) goes on before anything draws */
static void
restore_monitors(void)
{
	char *path = rui_config_path("monitors.conf");
	struct stat st;
	pid_t pid;

	if (stat(path, &st) < 0) {
		free(path);
		return;
	}
	free(path);
	pid = fork();
	if (pid == 0) {
		execlp("retro-display", "retro-display", "--restore", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

/* Wait (up to ~5 s) until a window manager owns the WM_S<n> selection. */
static void
wait_for_wm(Display *dpy)
{
	char name[32];
	Atom sel;
	int i;

	snprintf(name, sizeof name, "WM_S%d", DefaultScreen(dpy));
	sel = XInternAtom(dpy, name, False);
	for (i = 0; i < 100 && !got_signal; i++) {
		if (XGetSelectionOwner(dpy, sel) != None)
			return;
		if (comps[0].pid && waitpid(comps[0].pid, NULL, WNOHANG) ==
		    comps[0].pid) {
			comps[0].pid = 0;              /* died during startup */
			return;
		}
		msleep(50);
	}
}

static void
run_autostart(void)
{
	char *path = rui_config_path("autostart");
	struct stat st;

	if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		pid_t pid = fork();

		if (pid == 0) {
			pid_t p2 = fork();

			if (p2 == 0) {
				setsid();
				execl("/bin/sh", "sh", path, (char *)NULL);
				_exit(127);
			}
			_exit(0);
		}
		if (pid > 0)
			while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
				;
	}
	free(path);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	Display *dpy;
	int i, done = 0, rc = 0;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			puts("retro-session " RUI_VERSION);
			return 0;
		}
		fprintf(stderr, "usage: retro-session [-v]\n");
		return 2;
	}
	setup_path(argv[0]);
	/* lets applications (and OnlyShowIn= in .desktop files) know us */
	if (!getenv("XDG_CURRENT_DESKTOP"))
		setenv("XDG_CURRENT_DESKTOP", rui_config.desktop_name, 1);
	mkdir(rui_config_dir(), 0755);         /* fine if it already exists */
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "retro-session: cannot open display '%s'\n",
		    XDisplayName(NULL));
		return 1;
	}
	dbus_start();
	restore_monitors();
	saver_start();
	rui_programs_init();                   /* first login: the defaults */
	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == -1) {
		perror("retro-session: pledge");
		return 1;
	}
#endif

	comps[0].pid = start(comps[0].name);
	wait_for_wm(dpy);
	XCloseDisplay(dpy);
	if (!comps[0].pid) {
		fprintf(stderr, "retro-session: window manager failed to start\n");
		dbus_stop();
		return 1;
	}
	for (i = 1; i < NCOMPS; i++)
		comps[i].pid = start(comps[i].name);
	run_autostart();
	if (rui_config.desktop_entries && rui_settings()->xdg_autostart)
		rui_autostart_xdg();

	while (!done && !got_signal) {
		int status = 0;
		pid_t pid = waitpid(-1, &status, 0);

		if (pid < 0) {
			if (errno == EINTR)
				continue;
			break;                             /* no children left */
		}
		for (i = 0; i < NCOMPS; i++) {
			Comp *c = &comps[i];
			int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;

			if (c->pid != pid)
				continue;
			c->pid = 0;
			if (i == 0 && clean) {
				done = 1;                      /* Log Out */
			} else if (clean) {
				c->wanted = 0;                 /* closed on purpose */
			} else if (c->respawns < MAX_RESPAWN) {
				c->respawns++;
				fprintf(stderr, "retro-session: %s died, restarting "
				    "(%d/%d)\n", c->name, c->respawns, MAX_RESPAWN);
				msleep(300);
				c->pid = start(c->name);
			} else {
				fprintf(stderr, "retro-session: %s keeps dying\n", c->name);
				if (i == 0) {
					done = 1;
					rc = 1;
				}
			}
		}
	}

	/* shut the remaining components down, the WM last */
	for (i = NCOMPS - 1; i >= 0; i--)
		if (comps[i].pid > 0)
			kill(comps[i].pid, SIGTERM);
	for (i = 0; i < 60; i++) {
		int left = 0, k;

		for (k = 0; k < NCOMPS; k++) {
			if (comps[k].pid > 0 &&
			    waitpid(comps[k].pid, NULL, WNOHANG) == comps[k].pid)
				comps[k].pid = 0;
			if (comps[k].pid > 0)
				left++;
		}
		if (!left)
			break;
		msleep(50);
	}
	for (i = 0; i < NCOMPS; i++) {
		if (comps[i].pid > 0) {
			kill(comps[i].pid, SIGKILL);
			waitpid(comps[i].pid, NULL, 0);
		}
	}
	saver_stop();
	dbus_stop();
	return rc;
}
