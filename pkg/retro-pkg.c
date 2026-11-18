/*
 * retro-pkg - theme packages for retro-de.
 *
 *   retro-pkg install FILE.rtp|DIR [--apply] [--allow-code]
 *   retro-pkg remove NAME
 *   retro-pkg list
 *   retro-pkg info FILE.rtp|DIR|NAME
 *   retro-pkg apply NAME           switch to the package's look
 *   retro-pkg create DIR [OUT.rtp] make a package from a folder
 *
 * A package (.rtp) is a tar archive, gzip-compressed or not, holding:
 *
 *   package.conf      name, version, author, description, and what
 *                     `apply` sets: theme, style, wallpaper,
 *                     wallpaper_mode, icon_theme, font, pattern
 *   themes/           NAME.theme palettes; NAME.c / NAME.so theme modules
 *                     (code: installed only with --allow-code)
 *   wallpapers/       pictures: PNG, SVG, XPM, PPM
 *   icons/            icons by name (firefox.png ...) and icon themes
 *                     (icons/Papirus/48x48/apps/...)
 *
 * Everything lands in ~/.config/retro-de (themes/, wallpapers/, icons/),
 * where the Style Manager, the desktop and the icon lookups find it; the
 * list of installed files is kept in packages/NAME.files for `remove`.
 * Archive members outside those folders, absolute paths, ".." and links
 * are refused.  No tar, gzip or zlib is needed.
 */
#include "../lib/retroui.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_FILE ((size_t)64 << 20)
#define MAX_FILES 20000

typedef struct Member {
	char *path;                        /* inside the package            */
	unsigned char *data;
	size_t len;
} Member;

typedef struct Pkg {
	Member *m;
	int n, cap;
	/* package.conf */
	char name[64], version[32], author[128], desc[256];
	char keys[16][32], vals[16][256];
	int nkeys;
} Pkg;

static int allow_code;

/* ----------------------------------------------------------------- utils */

static unsigned char *
read_all(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf = NULL;
	size_t cap = 0, n = 0;

	if (!f)
		return NULL;
	for (;;) {
		size_t r;

		if (cap - n < 65536) {
			cap = cap ? cap * 2 : 1 << 20;
			if (cap > ((size_t)512 << 20)) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = rui_realloc(buf, cap);
		}
		r = fread(buf + n, 1, cap - n, f);
		n += r;
		if (r == 0)
			break;
	}
	fclose(f);
	*len = n;
	return buf;
}

static int
mkdirs(const char *path)
{
	char tmp[PATH_MAX], *p;

	rui_strlcpy(tmp, path, sizeof tmp);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* "./a//b/" -> "a/b"; NULL if it climbs out or is absolute */
static char *
clean_path(const char *p)
{
	char *out = rui_malloc(strlen(p) + 1), *o = out;
	const char *s = p;

	if (*p == '/')
		goto bad;
	while (*s) {
		size_t n = strcspn(s, "/");

		if (n == 0 || (n == 1 && s[0] == '.')) {
			;
		} else if (n == 2 && s[0] == '.' && s[1] == '.') {
			goto bad;
		} else {
			if (o != out)
				*o++ = '/';
			memcpy(o, s, n);
			o += n;
		}
		s += n;
		if (*s == '/')
			s++;
	}
	*o = '\0';
	if (!*out)
		goto bad;
	return out;
bad:
	free(out);
	return NULL;
}

static void
add_member(Pkg *pk, const char *path, const unsigned char *data, size_t len)
{
	char *p = clean_path(path);

	if (!p) {
		fprintf(stderr, "retro-pkg: refusing '%s'\n", path);
		return;
	}
	if (pk->n == MAX_FILES) {
		free(p);
		return;
	}
	if (pk->n == pk->cap) {
		pk->cap = pk->cap ? pk->cap * 2 : 64;
		pk->m = rui_realloc(pk->m, (size_t)pk->cap * sizeof *pk->m);
	}
	pk->m[pk->n].path = p;
	pk->m[pk->n].data = rui_malloc(len + 1);
	memcpy(pk->m[pk->n].data, data, len);
	pk->m[pk->n].len = len;
	pk->n++;
}

static void
pkg_free(Pkg *pk)
{
	int i;

	for (i = 0; i < pk->n; i++) {
		free(pk->m[i].path);
		free(pk->m[i].data);
	}
	free(pk->m);
	memset(pk, 0, sizeof *pk);
}

/* ------------------------------------------------------------------- tar */

static unsigned long
octal(const unsigned char *p, size_t n)
{
	unsigned long v = 0;
	size_t i;

	for (i = 0; i < n && (p[i] == ' ' || p[i] == 0); i++)
		;
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
		v = v * 8 + (unsigned long)(p[i] - '0');
	return v;
}

static int
read_tar(Pkg *pk, const unsigned char *t, size_t len)
{
	size_t pos = 0;
	char longname[PATH_MAX] = "";

	while (pos + 512 <= len) {
		const unsigned char *h = t + pos;
		char name[PATH_MAX];
		unsigned long size;
		char type;
		int i, zero = 1;

		for (i = 0; i < 512; i++)
			if (h[i]) {
				zero = 0;
				break;
			}
		if (zero)
			break;                         /* the end */
		size = octal(h + 124, 12);
		type = (char)h[156];
		pos += 512;
		if (size > len - pos) {
			fprintf(stderr, "retro-pkg: truncated archive\n");
			return -1;
		}
		if (longname[0]) {
			rui_strlcpy(name, longname, sizeof name);
			longname[0] = '\0';
		} else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
			snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345,
			    (const char *)h);
		} else {
			snprintf(name, sizeof name, "%.100s", (const char *)h);
		}
		if (type == 'L') {                 /* GNU long name for the next */
			size_t n = size < sizeof longname - 1 ? size : sizeof longname - 1;

			memcpy(longname, t + pos, n);
			longname[n] = '\0';
		} else if ((type == '0' || type == '\0' || type == '7') &&
		    size <= MAX_FILE) {
			add_member(pk, name, t + pos, size);
		} else if (type == '1' || type == '2') {
			fprintf(stderr, "retro-pkg: skipping link '%s'\n", name);
		}
		/* directories, pax headers and the like are skipped */
		pos += (size + 511) / 512 * 512;
	}
	return 0;
}

/* ------------------------------------------------------------- loading */

static void
read_dir(Pkg *pk, const char *base, const char *rel)
{
	char *dir = *rel ? rui_strdupf("%s/%s", base, rel) : rui_strdup(base);
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d))) {
		char *full, *r;
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		full = rui_strdupf("%s/%s", dir, de->d_name);
		r = *rel ? rui_strdupf("%s/%s", rel, de->d_name) :
		    rui_strdup(de->d_name);
		if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
			read_dir(pk, base, r);
		} else if (S_ISREG(st.st_mode) && (size_t)st.st_size <= MAX_FILE) {
			size_t len;
			unsigned char *data = read_all(full, &len);

			if (data)
				add_member(pk, r, data, len);
			free(data);
		}
		free(full);
		free(r);
	}
	if (d)
		closedir(d);
	free(dir);
}

static void
parse_conf(Pkg *pk, const Member *m)
{
	char *text = rui_malloc(m->len + 1), *line, *save = NULL;

	memcpy(text, m->data, m->len);
	text[m->len] = '\0';
	for (line = strtok_r(text, "\n", &save); line;
	    line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '='), *k, *v;

		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		k = rui_rtrim(rui_ltrim(line));
		v = rui_rtrim(rui_ltrim(eq + 1));
		if (!strcmp(k, "name"))
			rui_strlcpy(pk->name, v, sizeof pk->name);
		else if (!strcmp(k, "version"))
			rui_strlcpy(pk->version, v, sizeof pk->version);
		else if (!strcmp(k, "author"))
			rui_strlcpy(pk->author, v, sizeof pk->author);
		else if (!strcmp(k, "description"))
			rui_strlcpy(pk->desc, v, sizeof pk->desc);
		else if ((!strcmp(k, "theme") || !strcmp(k, "style") ||
		    !strcmp(k, "wallpaper") || !strcmp(k, "wallpaper_mode") ||
		    !strcmp(k, "icon_theme") || !strcmp(k, "font") ||
		    !strcmp(k, "desktop_pattern")) && pk->nkeys < 16) {
			rui_strlcpy(pk->keys[pk->nkeys], k, sizeof pk->keys[0]);
			rui_strlcpy(pk->vals[pk->nkeys], v, sizeof pk->vals[0]);
			pk->nkeys++;
		}
	}
	free(text);
}

static int
name_ok(const char *n)
{
	if (!*n || strlen(n) > 60 || n[0] == '.')
		return 0;
	for (; *n; n++)
		if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') ||
		    (*n >= '0' && *n <= '9') || strchr(" ._+-", *n)))
			return 0;
	return 1;
}

/* a file or folder -> package; 0 ok */
static int
load(Pkg *pk, const char *src)
{
	struct stat st;
	int i, top = -1;
	char prefix[PATH_MAX] = "";

	memset(pk, 0, sizeof *pk);
	if (stat(src, &st) < 0) {
		fprintf(stderr, "retro-pkg: %s: %s\n", src, strerror(errno));
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		read_dir(pk, src, "");
	} else {
		size_t len, tlen;
		unsigned char *data = read_all(src, &len), *tar = NULL;
		int r;

		if (!data) {
			fprintf(stderr, "retro-pkg: cannot read %s\n", src);
			return -1;
		}
		if (len > 2 && data[0] == 0x1f && data[1] == 0x8b) {
			tar = rui_gunzip(data, len, &tlen);
			if (!tar) {
				fprintf(stderr, "retro-pkg: %s: bad gzip data\n", src);
				free(data);
				return -1;
			}
			r = read_tar(pk, tar, tlen);
			free(tar);
		} else {
			r = read_tar(pk, data, len);
		}
		free(data);
		if (r < 0)
			return -1;
	}
	/* package.conf at the top, or inside one top folder */
	for (i = 0; i < pk->n; i++) {
		const char *s = strrchr(pk->m[i].path, '/');

		if (!strcmp(s ? s + 1 : pk->m[i].path, "package.conf") &&
		    (!s || !strchr(pk->m[i].path, '/') ||
		    strchr(pk->m[i].path, '/') == s)) {
			if (top < 0 || !s)
				top = i;
		}
	}
	if (top < 0) {
		fprintf(stderr, "retro-pkg: %s has no package.conf\n", src);
		pkg_free(pk);
		return -1;
	}
	{
		const char *s = strrchr(pk->m[top].path, '/');

		if (s)
			snprintf(prefix, sizeof prefix, "%.*s/",
			    (int)(s - pk->m[top].path), pk->m[top].path);
	}
	if (*prefix) {                         /* drop the top folder */
		size_t pl = strlen(prefix);

		for (i = 0; i < pk->n; i++) {
			if (!strncmp(pk->m[i].path, prefix, pl)) {
				memmove(pk->m[i].path, pk->m[i].path + pl,
				    strlen(pk->m[i].path + pl) + 1);
			} else {
				free(pk->m[i].path);
				pk->m[i].path = rui_strdup("");
			}
		}
	}
	for (i = 0; i < pk->n; i++)
		if (!strcmp(pk->m[i].path, "package.conf"))
			parse_conf(pk, &pk->m[i]);
	if (!name_ok(pk->name)) {
		fprintf(stderr, "retro-pkg: package.conf needs a name (letters, "
		    "digits, space . _ + -)\n");
		pkg_free(pk);
		return -1;
	}
	return 0;
}

/* ----------------------------------------------------------- installing */

static int
is_code(const char *path)
{
	size_t n = strlen(path);

	return !strncmp(path, "themes/", 7) && n > 2 &&
	    (!strcmp(path + n - 2, ".c") || !strcmp(path + n - 3, ".so"));
}

static int
wanted(const char *p)
{
	return !strncmp(p, "themes/", 7) || !strncmp(p, "wallpapers/", 11) ||
	    !strncmp(p, "icons/", 6);
}

static char *
list_path(const char *name)
{
	return rui_strdupf("%s/packages/%s.files", rui_config_dir(), name);
}

static void
apply(const Pkg *pk)
{
	char *argv[24];
	int i, k = 0;
	pid_t pid;

	if (!pk->nkeys) {
		printf("%s brings no settings to apply.\n", pk->name);
		return;
	}
	argv[k++] = "retro-settings";
	argv[k++] = "--apply";
	for (i = 0; i < pk->nkeys && k < 22; i++)
		argv[k++] = rui_strdupf("%s=%s", pk->keys[i], pk->vals[i]);
	argv[k] = NULL;
	if ((pid = fork()) == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
	for (i = 2; i < k; i++)
		free(argv[i]);
	printf("Applied %s.\n", pk->name);
}

static int
install(const char *src, int do_apply)
{
	Pkg pk;
	char *lp, *conf_copy;
	FILE *list;
	int i, files = 0, skipped = 0;
	pid_t pid;

	if (load(&pk, src) < 0)
		return 1;
	lp = list_path(pk.name);
	{
		char *dir = rui_strdupf("%s/packages", rui_config_dir());

		mkdirs(dir);
		free(dir);
	}
	if (!(list = fopen(lp, "w"))) {
		fprintf(stderr, "retro-pkg: cannot write %s\n", lp);
		free(lp);
		pkg_free(&pk);
		return 1;
	}
	for (i = 0; i < pk.n; i++) {
		const Member *m = &pk.m[i];
		char *dst, *slash;
		FILE *f;

		if (!*m->path || !strcmp(m->path, "package.conf"))
			continue;
		if (!wanted(m->path)) {
			fprintf(stderr, "retro-pkg: ignoring %s\n", m->path);
			continue;
		}
		if (is_code(m->path) && !allow_code) {
			skipped++;
			continue;
		}
		dst = rui_strdupf("%s/%s", rui_config_dir(), m->path);
		slash = strrchr(dst, '/');
		*slash = '\0';
		mkdirs(dst);
		*slash = '/';
		if (!strcmp(m->path + strlen(m->path) - 2, ".c")) {
			/* a theme module's source: the Style Manager compiles it into
			 * themes/NAME.so (the file name is the theme's name) */
			char tmpd[] = "/tmp/retro-pkg-XXXXXX", *src_c;
			const char *base = strrchr(m->path, '/') + 1;

			free(dst);
			if (!mkdtemp(tmpd))
				continue;
			src_c = rui_strdupf("%s/%s", tmpd, base);
			if ((f = fopen(src_c, "wb"))) {
				fwrite(m->data, 1, m->len, f);
				fclose(f);
				if ((pid = fork()) == 0) {
					execlp("retro-settings", "retro-settings", "--install-theme",
					    src_c, (char *)NULL);
					_exit(127);
				}
				if (pid > 0)
					waitpid(pid, NULL, 0);
				fprintf(list, "themes/%.*s.so\n", (int)(strlen(base) - 2),
				    base);
				files++;
			}
			unlink(src_c);
			rmdir(tmpd);
			free(src_c);
			continue;
		}
		if (!(f = fopen(dst, "wb")) || fwrite(m->data, 1, m->len, f) != m->len) {
			fprintf(stderr, "retro-pkg: cannot write %s\n", dst);
			if (f)
				fclose(f);
			free(dst);
			continue;
		}
		fclose(f);
		fprintf(list, "%s\n", m->path);
		free(dst);
		files++;
	}
	fclose(list);
	/* keep package.conf for list / info / apply */
	conf_copy = rui_strdupf("%s/packages/%s.conf", rui_config_dir(), pk.name);
	for (i = 0; i < pk.n; i++) {
		if (!strcmp(pk.m[i].path, "package.conf")) {
			FILE *f = fopen(conf_copy, "wb");

			if (f) {
				fwrite(pk.m[i].data, 1, pk.m[i].len, f);
				fclose(f);
			}
		}
	}
	free(conf_copy);
	printf("Installed %s %s (%d file%s).\n", pk.name, pk.version, files,
	    files == 1 ? "" : "s");
	if (skipped)
		printf("Skipped %d theme module%s (compiled code): install again with "
		    "--allow-code if you trust its author.\n", skipped,
		    skipped == 1 ? "" : "s");
	if (do_apply)
		apply(&pk);
	else if (pk.nkeys)
		printf("Switch to it with: retro-pkg apply \"%s\"\n", pk.name);
	free(lp);
	pkg_free(&pk);
	return 0;
}

static int
remove_pkg(const char *name)
{
	char *lp, line[PATH_MAX];
	FILE *f;
	int n = 0;

	if (!name_ok(name)) {
		fprintf(stderr, "retro-pkg: bad name\n");
		return 1;
	}
	lp = list_path(name);
	if (!(f = fopen(lp, "r"))) {
		fprintf(stderr, "retro-pkg: %s is not installed\n", name);
		free(lp);
		return 1;
	}
	while (fgets(line, sizeof line, f)) {
		char *p, *full;

		line[strcspn(line, "\n")] = '\0';
		if (!(p = clean_path(line)) || !wanted(p)) {
			free(p);
			continue;
		}
		full = rui_strdupf("%s/%s", rui_config_dir(), p);
		if (unlink(full) == 0)
			n++;
		/* empty icon-theme folders go too */
		for (;;) {
			char *slash = strrchr(full, '/');

			if (!slash)
				break;
			*slash = '\0';
			if (strlen(full) <= strlen(rui_config_dir()) + 12 ||
			    rmdir(full) < 0)
				break;
		}
		free(full);
		free(p);
	}
	fclose(f);
	unlink(lp);
	free(lp);
	lp = rui_strdupf("%s/packages/%s.conf", rui_config_dir(), name);
	unlink(lp);
	free(lp);
	printf("Removed %s (%d file%s).\n", name, n, n == 1 ? "" : "s");
	return 0;
}

static int
list_pkgs(void)
{
	char *dir = rui_strdupf("%s/packages", rui_config_dir());
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0;

	while (d && (de = readdir(d))) {
		size_t l = strlen(de->d_name);
		char *p;
		size_t len;
		unsigned char *data;

		if (l < 6 || strcmp(de->d_name + l - 5, ".conf"))
			continue;
		p = rui_strdupf("%s/%s", dir, de->d_name);
		if ((data = read_all(p, &len))) {
			Pkg pk;
			Member m;

			memset(&pk, 0, sizeof pk);
			m.path = "package.conf";
			m.data = data;
			m.len = len;
			parse_conf(&pk, &m);
			printf("%-24s %-8s %s\n", pk.name, pk.version, pk.desc);
			free(data);
			n++;
		}
		free(p);
	}
	if (d)
		closedir(d);
	if (!n)
		printf("No packages installed (%s).\n", dir);
	free(dir);
	return 0;
}

static int
load_installed(Pkg *pk, const char *name)
{
	char *p = rui_strdupf("%s/packages/%s.conf", rui_config_dir(), name);
	size_t len;
	unsigned char *data = name_ok(name) ? read_all(p, &len) : NULL;
	Member m;

	free(p);
	memset(pk, 0, sizeof *pk);
	if (!data)
		return -1;
	m.path = "package.conf";
	m.data = data;
	m.len = len;
	parse_conf(pk, &m);
	free(data);
	return 0;
}

static int
info(const char *what)
{
	Pkg pk;
	int i, installed = 0;

	if (access(what, F_OK) == 0) {
		if (load(&pk, what) < 0)
			return 1;
	} else if (load_installed(&pk, what) == 0) {
		installed = 1;
	} else {
		fprintf(stderr, "retro-pkg: no package file or installed package "
		    "'%s'\n", what);
		return 1;
	}
	printf("Name:        %s\nVersion:     %s\nAuthor:      %s\n"
	    "Description: %s\n", pk.name, pk.version, pk.author, pk.desc);
	for (i = 0; i < pk.nkeys; i++)
		printf("Sets:        %s = %s\n", pk.keys[i], pk.vals[i]);
	if (!installed) {
		for (i = 0; i < pk.n; i++)
			if (*pk.m[i].path)
				printf("  %-40s %8lu%s\n", pk.m[i].path,
				    (unsigned long)pk.m[i].len, is_code(pk.m[i].path) ?
				    "  (code)" : wanted(pk.m[i].path) ||
				    !strcmp(pk.m[i].path, "package.conf") ? "" :
				    "  (ignored)");
	}
	pkg_free(&pk);
	return 0;
}

/* ------------------------------------------------------------- creating */

static void
tar_header(FILE *f, const char *name, size_t size)
{
	unsigned char h[512];
	unsigned sum = 0;
	int i;

	memset(h, 0, sizeof h);
	snprintf((char *)h, 100, "%s", name);
	snprintf((char *)h + 100, 8, "%07o", 0644);
	snprintf((char *)h + 108, 8, "%07o", 0);
	snprintf((char *)h + 116, 8, "%07o", 0);
	snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
	snprintf((char *)h + 136, 12, "%011lo", 0UL);
	h[156] = '0';
	memcpy(h + 257, "ustar", 6);
	memcpy(h + 263, "00", 2);
	memset(h + 148, ' ', 8);
	for (i = 0; i < 512; i++)
		sum += h[i];
	snprintf((char *)h + 148, 8, "%06o", sum);
	h[155] = ' ';
	fwrite(h, 1, 512, f);
}

static int
create(const char *dir, const char *out)
{
	Pkg pk;
	char *tmpname, *outname;
	FILE *f;
	unsigned char *tar, *gz;
	size_t tlen, glen;
	int i;

	if (load(&pk, dir) < 0)
		return 1;
	outname = out ? rui_strdup(out) : rui_strdupf("%s-%s.rtp", pk.name,
	    *pk.version ? pk.version : "1");
	tmpname = rui_strdupf("%s.tar", outname);
	if (!(f = fopen(tmpname, "wb"))) {
		fprintf(stderr, "retro-pkg: cannot write %s\n", tmpname);
		return 1;
	}
	for (i = 0; i < pk.n; i++) {
		static const unsigned char zero[512];
		char *name;

		if (!*pk.m[i].path)
			continue;
		if (strcmp(pk.m[i].path, "package.conf") && !wanted(pk.m[i].path))
			continue;
		name = rui_strdupf("%s/%s", pk.name, pk.m[i].path);
		if (strlen(name) > 99) {
			fprintf(stderr, "retro-pkg: path too long, skipped: %s\n", name);
			free(name);
			continue;
		}
		tar_header(f, name, pk.m[i].len);
		fwrite(pk.m[i].data, 1, pk.m[i].len, f);
		fwrite(zero, 1, (512 - pk.m[i].len % 512) % 512, f);
		free(name);
	}
	{
		static const unsigned char zero[1024];

		fwrite(zero, 1, sizeof zero, f);
	}
	fclose(f);
	tar = read_all(tmpname, &tlen);
	unlink(tmpname);
	free(tmpname);
	if (!tar)
		return 1;
	gz = rui_gzip_store(tar, tlen, &glen);
	free(tar);
	if (!(f = fopen(outname, "wb")) || fwrite(gz, 1, glen, f) != glen) {
		fprintf(stderr, "retro-pkg: cannot write %s\n", outname);
		return 1;
	}
	fclose(f);
	free(gz);
	printf("Wrote %s\n", outname);
	free(outname);
	pkg_free(&pk);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: retro-pkg install FILE.rtp|DIR [--apply] [--allow-code]\n"
	    "       retro-pkg remove NAME\n"
	    "       retro-pkg list\n"
	    "       retro-pkg info FILE.rtp|DIR|NAME\n"
	    "       retro-pkg apply NAME\n"
	    "       retro-pkg create DIR [OUT.rtp]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *cmd, *arg = NULL, *arg2 = NULL;
	int i, do_apply = 0;

	if (argc < 2)
		usage();
	cmd = argv[1];
	if (!strcmp(cmd, "-v")) {
		puts("retro-pkg " RUI_VERSION);
		return 0;
	}
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--apply"))
			do_apply = 1;
		else if (!strcmp(argv[i], "--allow-code"))
			allow_code = 1;
		else if (!arg)
			arg = argv[i];
		else if (!arg2)
			arg2 = argv[i];
		else
			usage();
	}
	if (!strcmp(cmd, "list"))
		return list_pkgs();
	if (!arg)
		usage();
	if (!strcmp(cmd, "install"))
		return install(arg, do_apply);
	if (!strcmp(cmd, "remove"))
		return remove_pkg(arg);
	if (!strcmp(cmd, "info"))
		return info(arg);
	if (!strcmp(cmd, "create"))
		return create(arg, arg2);
	if (!strcmp(cmd, "apply")) {
		Pkg pk;

		if (load_installed(&pk, arg) < 0) {
			fprintf(stderr, "retro-pkg: %s is not installed\n", arg);
			return 1;
		}
		apply(&pk);
		return 0;
	}
	usage();
	return 2;
}
