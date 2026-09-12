/* $OpenBSD$ */

/*
 * IDLE PANES TO SWAP.
 *
 * A Claude conversation weighs 400-500 MB and does strictly nothing while you
 * are not looking at it. Under memory pressure the kernel would eventually
 * page it out on its own; this does it EARLIER and, more importantly, CHOOSES
 * who pays - never the pane you are watching.
 *
 * There is no "swap this process out" system call. Two doors exist and only
 * one is open to an ordinary user:
 *
 *   - process_madvise(MADV_PAGEOUT) on another process needs CAP_SYS_NICE.
 *     Tried, refused (EPERM), even on our own children.
 *   - cgroup v2 "memory.reclaim" works with no privilege at all, PROVIDED the
 *     server runs inside a DELEGATED cgroup - which is what
 *     `systemd-run --user --scope -p Delegate=yes` gives. The session scope a
 *     terminal starts in is not delegated, and a process cannot be moved into
 *     the delegated tree afterwards (their common ancestor belongs to root).
 *
 * Two rules of cgroup v2 shape the code below, both learned the hard way:
 *   1. a cgroup holding processes may not enable controllers for its children,
 *      so the server first moves ITSELF into a leaf;
 *   2. memory is charged to the cgroup where a page was ALLOCATED, and moving
 *      a process does not move its pages - so a pane must join its cgroup
 *      BETWEEN fork AND exec, before it allocates anything.
 *
 * Without delegation everything here turns itself off and says nothing.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "tmux.h"

/* Our delegated cgroup, under which one cgroup per pane is created. */
static char		 swap_base[PATH_MAX];
static int		 swap_ready;
static struct event	 swap_ev;
static int		 swap_ev_on;

#define SWAP_INTERVAL 5			/* seconds between checks */
#define SWAP_MAX_ONCE (512 * 1024 * 1024)	/* reclaim cap, per pane, per pass */

/* Write a string to a cgroup file; 0 on success. */
static int
swap_write(const char *path, const char *value)
{
	int	fd;
	ssize_t	n;

	if ((fd = open(path, O_WRONLY)) == -1)
		return (-1);
	n = write(fd, value, strlen(value));
	close(fd);
	return (n < 0 ? -1 : 0);
}

/* Read a number out of a cgroup file ("max" and errors give 0). */
static uint64_t
swap_read_num(const char *path)
{
	FILE		*f;
	char		 buf[64];
	uint64_t	 v = 0;

	if ((f = fopen(path, "r")) == NULL)
		return (0);
	if (fgets(buf, sizeof buf, f) != NULL)
		v = strtoull(buf, NULL, 10);
	fclose(f);
	return (v);
}

/* A process's cgroup path, from /proc/<pid>/cgroup (v2 line is "0::<path>"). */
static int
swap_proc_cgroup(const char *proc, char *out, size_t len)
{
	FILE	*f;
	char	 line[PATH_MAX], *p;

	if ((f = fopen(proc, "r")) == NULL)
		return (0);
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "0::", 3) != 0)
			continue;
		line[strcspn(line, "\n")] = '\0';
		p = line + 3;
		if (snprintf(out, len, "/sys/fs/cgroup%s", p) >= (int)len)
			break;
		fclose(f);
		return (1);
	}
	fclose(f);
	return (0);
}

/* Our own cgroup path. */
static int
swap_own_cgroup(char *out, size_t len)
{
	return (swap_proc_cgroup("/proc/self/cgroup", out, len));
}

/* Is our own cgroup delegated, i.e. may we create children in it? */
static int
swap_delegated(void)
{
	char	base[PATH_MAX], probe[PATH_MAX];

	if (!swap_own_cgroup(base, sizeof base))
		return (0);
	if (snprintf(probe, sizeof probe, "%s/.tmuxv-probe", base) >=
	    (int)sizeof probe)
		return (0);
	if (mkdir(probe, 0755) == -1)
		return (errno == EEXIST);
	rmdir(probe);
	return (1);
}

/*
 * A terminal starts us in a cgroup we do not own, and a running process cannot
 * be moved into a delegated one by hand: the common ancestor belongs to root.
 * But systemd will do it for us - StartTransientUnit accepts a list of PIDs
 * and a Delegate flag, and puts the process in a brand new delegated scope.
 * One D-Bus call, and it returns at once.
 *
 * Re-execing ourselves through `systemd-run --scope` was the first attempt and
 * it was wrong: that command WAITS for the scope to empty, so a detached
 * "new-session -d" never returned.
 *
 * Skipped when it cannot work, or when asked not to (TMUXV_NO_SCOPE=1).
 */
static void
swap_make_scope(void)
{
	char		 unit[64], pid[32], before[PATH_MAX];
	pid_t		 child;
	int		 status;
	const char	*busctl = NULL;
	static const char *where[] = { "/usr/bin/busctl", "/bin/busctl" };
	u_int		 i;

	if (getenv("TMUXV_NO_SCOPE") != NULL)
		return;
	if (getenv("XDG_RUNTIME_DIR") == NULL)
		return;			/* no user manager to ask */
	for (i = 0; i < nitems(where); i++) {
		if (access(where[i], X_OK) == 0) {
			busctl = where[i];
			break;
		}
	}
	if (busctl == NULL)
		return;

	if (!swap_own_cgroup(before, sizeof before))
		return;
	xsnprintf(unit, sizeof unit, "tmuxv-%ld.scope", (long)getpid());
	xsnprintf(pid, sizeof pid, "%ld", (long)getpid());

	switch (child = fork()) {
	case -1:
		return;
	case 0: {
		char *argv[] = { (char *)busctl, "--user", "call",
		    "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
		    "org.freedesktop.systemd1.Manager", "StartTransientUnit",
		    "ssa(sv)a(sa(sv))", unit, "fail", "3",
		    "PIDs", "au", "1", pid,
		    "Delegate", "b", "true",
		    /* Garbage-collect the unit even if it ends badly. */
		    "CollectMode", "s", "inactive-or-failed", "0", NULL };
		int null = open("/dev/null", O_WRONLY);

		if (null != -1) {
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execv(busctl, argv);
		_exit(127);
	}
	default:
		while (waitpid(child, &status, 0) == -1 && errno == EINTR)
			/* nothing */;
		break;
	}

	/*
	 * StartTransientUnit returns a QUEUED JOB: systemd has not necessarily
	 * moved us yet when busctl exits. Wait for the move to actually show up
	 * in our own cgroup - reading it too early was enough to lose the whole
	 * feature, silently.
	 */
	for (i = 0; i < 100; i++) {
		char	now[PATH_MAX];

		if (swap_own_cgroup(now, sizeof now) &&
		    strcmp(now, before) != 0)
			return;
		usleep(20000);		/* 2 seconds at most, in 20 ms steps */
	}
}

/*
 * Set up the cgroup tree, if the kernel and systemd let us. Silent when they
 * do not: this is a bonus, never a requirement.
 */
void
swap_init(void)
{
	char	path[PATH_MAX], leaf[PATH_MAX];
	char	pid[32];

	swap_ready = 0;
	if (!swap_delegated())
		swap_make_scope();	/* ask systemd for a delegated scope */
	if (!swap_own_cgroup(swap_base, sizeof swap_base))
		return;
	/*
	 * A hot upgrade inherits the leaf we already moved into. Climb back out
	 * of every "tmuxv-server" level so the base is always the SAME group,
	 * upgrade or not: otherwise the server sinks one level deeper each
	 * time, and - worse - the panes opened before the upgrade keep their
	 * cgroup under the old base, where nothing would look for them again.
	 */
	for (;;) {
		char	*last = strrchr(swap_base, '/');

		if (last == NULL || last == swap_base ||
		    strcmp(last + 1, "tmuxv-server") != 0)
			break;
		*last = '\0';
	}

	/* Can we create anything here at all? (delegation) */
	if (snprintf(leaf, sizeof leaf, "%s/tmuxv-server", swap_base) >=
	    (int)sizeof leaf)
		return;
	if (mkdir(leaf, 0755) == -1 && errno != EEXIST) {
		log_debug("%s: no delegated cgroup (%s)", __func__,
		    strerror(errno));
		return;
	}

	/*
	 * Rule 1: move ourselves out of the group's root, otherwise enabling a
	 * controller for the children is refused.
	 */
	xsnprintf(pid, sizeof pid, "%ld", (long)getpid());
	xsnprintf(path, sizeof path, "%s/cgroup.procs", leaf);
	if (swap_write(path, pid) != 0) {
		char	 cur[PATH_MAX], *last;

		/*
		 * Refused. The case that matters: an older version left a group
		 * INSIDE our leaf, and a cgroup may not hold processes once its
		 * children have a controller - so we cannot climb back into it.
		 * Rather than lose swap for the whole life of this server, stay
		 * where we are and take our own parent as the base; panes are
		 * grouped next to us just the same.
		 */
		if (!swap_own_cgroup(cur, sizeof cur) ||
		    (last = strrchr(cur, '/')) == NULL || last == cur ||
		    strcmp(last + 1, "tmuxv-server") != 0) {
			log_debug("%s: cannot move into %s", __func__, leaf);
			rmdir(leaf);
			return;
		}
		*last = '\0';
		strlcpy(swap_base, cur, sizeof swap_base);
		log_debug("%s: staying put, base is %s", __func__, swap_base);
	}
	/*
	 * The rule is "no internal processes": the group's root must be EMPTY
	 * before a controller can be handed to its children. We have just left
	 * it, but whoever launched us (the client, the shell of the scope) is
	 * still there - so move those to the leaf too. They are in the scope
	 * created for tmuxv, and cgroup membership changes nothing for them.
	 */
	xsnprintf(path, sizeof path, "%s/cgroup.subtree_control", swap_base);
	if (swap_write(path, "+memory") != 0) {
		char	procs[PATH_MAX], line[64];
		FILE	*f;

		xsnprintf(procs, sizeof procs, "%s/cgroup.procs", swap_base);
		if ((f = fopen(procs, "r")) != NULL) {
			char	dst[PATH_MAX];

			xsnprintf(dst, sizeof dst, "%s/cgroup.procs", leaf);
			while (fgets(line, sizeof line, f) != NULL) {
				line[strcspn(line, "\n")] = '\0';
				if (*line != '\0')
					(void)swap_write(dst, line);
			}
			fclose(f);
		}
		if (swap_write(path, "+memory") != 0) {
			log_debug("%s: no memory controller to delegate",
			    __func__);
			return;
		}
	}
	swap_ready = 1;
	/*
	 * Sweep the empty groups left by panes that are gone (and by the older
	 * naming): a non-empty one refuses to go, so this can never take a
	 * live pane's group away.
	 */
	{
		DIR		*dp;
		struct dirent	*de;
		char		 path[PATH_MAX];

		if ((dp = opendir(swap_base)) != NULL) {
			while ((de = readdir(dp)) != NULL) {
				if (strncmp(de->d_name, "pane-", 5) != 0)
					continue;
				if (snprintf(path, sizeof path, "%s/%s",
				    swap_base, de->d_name) >= (int)sizeof path)
					continue;
				rmdir(path);
			}
			closedir(dp);
		}
	}
	log_debug("%s: idle panes can be paged out (%s)", __func__, swap_base);
}

int
swap_available(void)
{
	return (swap_ready);
}

/*
 * Path of one pane's cgroup, named after the PROCESS, not after the pane: a
 * hot upgrade renumbers the panes but keeps their processes, and a group named
 * after the old number would then be looked up under the new one - i.e. some
 * other pane's group.
 */
static int
swap_pane_path(pid_t pid, char *out, size_t len)
{
	char	proc[64], num[32], *base;
	size_t	n;

	if (!swap_ready || pid <= 0)
		return (0);
	/*
	 * Ask the process where it is really charged. Computing the name is
	 * only the fallback (for a process already gone): a group made by an
	 * older version does not carry the name we would build today, and a
	 * cgroup cannot be renamed without privileges - so guessing would mean
	 * either missing it, or worse, writing into another pane's group.
	 */
	xsnprintf(proc, sizeof proc, "/proc/%ld/cgroup", (long)pid);
	if (swap_proc_cgroup(proc, out, len)) {
		n = strlen(swap_base);
		base = strrchr(out, '/');
		if (strncmp(out, swap_base, n) == 0 && out[n] == '/' &&
		    base != NULL && strncmp(base + 1, "pane-", 5) == 0)
			return (1);
	}
	xsnprintf(num, sizeof num, "%ld", (long)pid);
	if (strlcpy(out, swap_base, len) >= len ||
	    strlcat(out, "/pane-", len) >= len ||
	    strlcat(out, num, len) >= len)
		return (0);
	return (1);
}

/*
 * Create the group and join it, from the CHILD, between fork and exec: the
 * child is the process the group is named after, and charges follow
 * allocations - joining later would leave the memory accounted elsewhere.
 */
void
swap_pane_join(void)
{
	char	path[PATH_MAX], pid[32];

	if (!swap_pane_path(getpid(), path, sizeof path))
		return;
	if (mkdir(path, 0755) == -1 && errno != EEXIST)
		return;
	if (strlcat(path, "/cgroup.procs", sizeof path) >= sizeof path)
		return;
	xsnprintf(pid, sizeof pid, "%ld", (long)getpid());
	(void)swap_write(path, pid);	/* best effort: never block a pane */
}

/* Drop a dead pane's cgroup (fails harmlessly while it still holds tasks). */
void
swap_pane_free(pid_t pid)
{
	char	path[PATH_MAX];

	if (!swap_pane_path(pid, path, sizeof path))
		return;
	rmdir(path);
}


/* Is this pane on screen for somebody right now? */
static int
swap_pane_shown(struct window_pane *wp)
{
	struct client	*c;
	struct session	*s;

	TAILQ_FOREACH(c, &clients, entry) {
		if ((s = c->session) == NULL || s->curw == NULL)
			continue;
		if (s->curw->window != wp->window)
			continue;
		if (window_pane_visible(wp))
			return (1);
	}
	return (0);
}

/* MemAvailable, in MB. */
static u_int
swap_mem_available(void)
{
	FILE	*f;
	char	 line[128];
	u_int	 mb = 0;

	if ((f = fopen("/proc/meminfo", "r")) == NULL)
		return (0);
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "MemAvailable:", 13) == 0) {
			mb = (u_int)(atol(line + 13) / 1024);
			break;
		}
	}
	fclose(f);
	return (mb);
}

static int
swap_opt_num(const char *name, int def)
{
	const char	*v;

	if (options_get(global_s_options, name) == NULL)
		return (def);
	if ((v = options_get_string(global_s_options, name)) == NULL ||
	    *v == '\0')
		return (def);
	return (atoi(v));
}

static int
swap_enabled(void)
{
	const char	*v;

	if (!swap_ready)
		return (0);
	if (options_get(global_s_options, "@swap-idle") == NULL)
		return (1);
	v = options_get_string(global_s_options, "@swap-idle");
	if (v == NULL || *v == '\0' || strcmp(v, "off") == 0 ||
	    strcmp(v, "0") == 0)
		return (0);
	return (1);
}

/*
 * Under the same threshold that already raises the low-memory warning, push
 * the panes nobody is looking at out to swap. The displayed one is never
 * touched: it is the one whose latency would be felt.
 */
static void
swap_check(void)
{
	struct window		*w;
	struct window_pane	*wp;
	char			 path[PATH_MAX], value[32];
	uint64_t		 charged;
	u_int			 avail, warn;

	if (!swap_enabled())
		return;
	warn = (u_int)swap_opt_num("@memory-warn", 512);
	avail = swap_mem_available();
	if (warn == 0 || avail == 0 || avail >= warn)
		return;			/* no pressure: leave it alone */

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->fd == -1 || swap_pane_shown(wp))
				continue;
			if (!swap_pane_path(wp->pid, path, sizeof path))
				continue;
			if (strlcat(path, "/memory.current",
			    sizeof path) >= sizeof path)
				continue;
			charged = swap_read_num(path);
			if (charged < 32 * 1024 * 1024)
				continue;	/* not worth the churn */
			if (charged > SWAP_MAX_ONCE)
				charged = SWAP_MAX_ONCE;

			swap_pane_path(wp->pid, path, sizeof path);
			strlcat(path, "/memory.reclaim", sizeof path);
			xsnprintf(value, sizeof value, "%llu",
			    (unsigned long long)charged);
			if (swap_write(path, value) == 0) {
				log_debug("%s: %%%u paged out %llu MB",
				    __func__, wp->id,
				    (unsigned long long)(charged >> 20));
			}
		}
	}
}

static void
swap_timer(__unused int fd, __unused short events, __unused void *arg)
{
	struct timeval	tv = { SWAP_INTERVAL, 0 };

	swap_check();
	evtimer_add(&swap_ev, &tv);
}

void
swap_start(void)
{
	struct timeval	tv = { SWAP_INTERVAL, 0 };

	if (!swap_ready || swap_ev_on)
		return;
	swap_ev_on = 1;
	evtimer_set(&swap_ev, swap_timer, NULL);
	evtimer_add(&swap_ev, &tv);
}
