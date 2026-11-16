/*
 * libretroui system probes: CPU and memory (Linux /proc, BSD sysctl(3)).
 * Batteries are handled by the battery modules in modules/.
 */
#include "retroui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
# include <sys/param.h>
# include <sys/sysctl.h>
# define RUI_BSD 1
#endif
#if defined(__OpenBSD__)
# include <uvm/uvmexp.h>
#endif
#if defined(__NetBSD__)
# include <uvm/uvm_extern.h>
#endif

/* Turns two successive (total, idle) tick samples into a busy fraction. */
static int
cpu_delta(unsigned long long total, unsigned long long idle, double *busy)
{
	static unsigned long long ptotal, pidle;
	static int have;
	unsigned long long dt = total - ptotal, di = idle - pidle;
	int ok = have && dt > 0 && di <= dt;

	if (ok)
		*busy = 1.0 - (double)di / (double)dt;
	ptotal = total;
	pidle = idle;
	have = 1;
	if (!ok)
		*busy = 0.0;
	return 1;
}

#if defined(__linux__)

int
rui_sys_cpu(double *busy)
{
	FILE *f = fopen("/proc/stat", "r");
	unsigned long long v[8] = { 0 }, total = 0;
	int n, i;

	if (!f)
		return 0;
	n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
	    &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 4)
		return 0;
	for (i = 0; i < n; i++)
		total += v[i];
	return cpu_delta(total, v[3] + v[4], busy);
}

int
rui_sys_mem(double *used, unsigned long *total_mb)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	unsigned long long total = 0, avail = 0, freek = 0, val;
	int have_avail = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "MemTotal: %llu", &val) == 1)
			total = val;
		else if (sscanf(line, "MemAvailable: %llu", &val) == 1) {
			avail = val;
			have_avail = 1;
		} else if (sscanf(line, "MemFree: %llu", &val) == 1)
			freek = val;
	}
	fclose(f);
	if (!total)
		return 0;
	if (!have_avail)
		avail = freek;
	if (avail > total)
		avail = total;
	*used = (double)(total - avail) / (double)total;
	*total_mb = (unsigned long)(total / 1024);
	return 1;
}

#elif defined(RUI_BSD)

int
rui_sys_cpu(double *busy)
{
	unsigned long long total = 0, idle = 0;
	size_t n, i;
#if defined(__NetBSD__)
	uint64_t cp[16];
	size_t sz = sizeof cp;

	if (sysctlbyname("kern.cp_time", cp, &sz, NULL, 0) < 0)
		return 0;
#elif defined(__OpenBSD__)
	long cp[16];
	size_t sz = sizeof cp;
	int mib[2] = { CTL_KERN, KERN_CPTIME };

	if (sysctl(mib, 2, cp, &sz, NULL, 0) < 0)
		return 0;
#else
	long cp[16];
	size_t sz = sizeof cp;

	if (sysctlbyname("kern.cp_time", cp, &sz, NULL, 0) < 0)
		return 0;
#endif
	n = sz / sizeof cp[0];
	if (n < 2)
		return 0;
	for (i = 0; i < n; i++)
		total += (unsigned long long)cp[i];
	idle = (unsigned long long)cp[n - 1];     /* CP_IDLE is the last state */
	return cpu_delta(total, idle, busy);
}

int
rui_sys_mem(double *used, unsigned long *total_mb)
{
#if defined(__OpenBSD__)
	struct uvmexp u;
	size_t sz = sizeof u;
	int mib[2] = { CTL_VM, VM_UVMEXP };
	unsigned long long total, avail;

	if (sysctl(mib, 2, &u, &sz, NULL, 0) < 0 || u.npages <= 0)
		return 0;
	total = (unsigned long long)u.npages;
	avail = (unsigned long long)u.free + (unsigned long long)u.inactive;
	if (avail > total)
		avail = total;
	*used = (double)(total - avail) / (double)total;
	*total_mb = (unsigned long)(total * (unsigned long long)u.pagesize >> 20);
	return 1;
#elif defined(__NetBSD__)
	struct uvmexp_sysctl u;
	size_t sz = sizeof u;
	unsigned long long total, avail;

	if (sysctlbyname("vm.uvmexp2", &u, &sz, NULL, 0) < 0 || u.npages <= 0)
		return 0;
	total = (unsigned long long)u.npages;
	avail = (unsigned long long)u.free + (unsigned long long)u.inactive;
	if (avail > total)
		avail = total;
	*used = (double)(total - avail) / (double)total;
	*total_mb = (unsigned long)(total * (unsigned long long)u.pagesize >> 20);
	return 1;
#else
	unsigned int pages = 0, freep = 0, inact = 0, cache = 0;
	size_t sz;
	unsigned long long total, avail;
	long pagesz = sysconf(_SC_PAGESIZE);

	sz = sizeof pages;
	if (sysctlbyname("vm.stats.vm.v_page_count", &pages, &sz, NULL, 0) < 0 ||
	    pages == 0)
		return 0;
	sz = sizeof freep;
	if (sysctlbyname("vm.stats.vm.v_free_count", &freep, &sz, NULL, 0) < 0)
		freep = 0;
	sz = sizeof inact;
	if (sysctlbyname("vm.stats.vm.v_inactive_count", &inact, &sz, NULL, 0) < 0)
		inact = 0;
	sz = sizeof cache;
	if (sysctlbyname("vm.stats.vm.v_cache_count", &cache, &sz, NULL, 0) < 0)
		cache = 0;
	total = pages;
	avail = (unsigned long long)freep + inact + cache;
	if (avail > total)
		avail = total;
	*used = (double)(total - avail) / (double)total;
	*total_mb = (unsigned long)(total * (unsigned long long)pagesz >> 20);
	return 1;
#endif
}

#else  /* unknown platform: portable but coarse fallbacks */

int
rui_sys_cpu(double *busy)
{
	double load[1];
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	if (getloadavg(load, 1) != 1)
		return 0;
	if (ncpu < 1)
		ncpu = 1;
	*busy = load[0] / (double)ncpu;
	if (*busy > 1.0)
		*busy = 1.0;
	return 1;
}

int
rui_sys_mem(double *used, unsigned long *total_mb)
{
	(void)used;
	(void)total_mb;
	return 0;
}

#endif
