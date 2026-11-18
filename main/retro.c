/*
 * retro - every retro-de program in one executable.
 *
 * retro-wm, retro-bar, retro-desk ... are links to this file; it runs the
 * program named by argv[0].  One copy on disk, and the processes of the
 * session share its code in memory.  `retro NAME [ARGS]` works too, and
 * `retro --list` prints the names.
 */
#include <stdio.h>
#include <string.h>

#define PROGRAMS(X) \
	X(retro_wm, "retro-wm") X(retro_bar, "retro-bar") \
	X(retro_desk, "retro-desk") X(retro_run, "retro-run") \
	X(retro_net, "retro-net") X(retro_session, "retro-session") \
	X(retro_settings, "retro-settings") X(retro_bt, "retro-bt") \
	X(retro_display, "retro-display") X(retro_calc, "retro-calc") \
	X(retro_files, "retro-files") X(retro_programs, "retro-programs") \
	X(retro_about, "retro-about") X(retro_bus, "retro-bus") \
	X(retro_msg, "retro-msg") X(retro_pkg, "retro-pkg") \
	X(retro_wallpaper, "retro-wallpaper")

#define DECLARE(fn, name) int fn##_main(int argc, char **argv);
PROGRAMS(DECLARE)

static const struct {
	const char *name;
	int (*main)(int, char **);
} programs[] = {
#define ENTRY(fn, name) { name, fn##_main },
	PROGRAMS(ENTRY)
};

#define NPROGRAMS (sizeof programs / sizeof programs[0])

static int
run(const char *name, int argc, char **argv)
{
	size_t i;

	for (i = 0; i < NPROGRAMS; i++)
		if (!strcmp(name, programs[i].name))
			return programs[i].main(argc, argv);
	return -1;
}

int
main(int argc, char **argv)
{
	const char *base = argc > 0 ? strrchr(argv[0], '/') : NULL;
	size_t i;
	int r;

	base = base ? base + 1 : argc > 0 ? argv[0] : "retro";
	if ((r = run(base, argc, argv)) >= 0)
		return r;
	/* called as "retro": the program is the first argument */
	if (argc > 1 && !strcmp(argv[1], "--list")) {
		for (i = 0; i < NPROGRAMS; i++)
			puts(programs[i].name);
		return 0;
	}
	if (argc > 1 && (r = run(argv[1], argc - 1, argv + 1)) >= 0)
		return r;
	fprintf(stderr, "usage: retro PROGRAM [ARGS...]   (retro --list)\n"
	    "       or run it through a link named after the program\n");
	return 127;
}
