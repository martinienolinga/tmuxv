/* $OpenBSD$ */

/*
 * HOT UPGRADE: replace the tmuxv binary WITHOUT killing what it runs.
 *
 * The shells and agents in the panes are children of the server, tied to it
 * only by the pseudo-terminal master file descriptors it holds. execve()
 * replaces the code of a process while keeping its pid and its open file
 * descriptors - so the server can serialise its state, exec the new binary,
 * and the new code wakes up owning the very same ptys, the same children (still
 * its own children, so SIGCHLD keeps working), the same listening socket and
 * the same client connections. Nothing is respawned; nothing is lost.
 *
 * This is the same trick nginx and haproxy use to upgrade without dropping
 * connections. The state travels through a small tab-separated file whose
 * first line carries a version, because it is the OLD binary that writes it
 * and the NEW one that reads it.
 *
 * What is NOT carried over yet: the pane grids (scrollback and visible
 * screen). Full-screen programs are asked to repaint themselves instead - see
 * upgrade_load(). Everything else - sessions, windows, layouts, working
 * directories, the desktop rectangles, the Claude manager - is restored.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

static void	upgrade_save_options(FILE *, struct options *, char);

/*
 * Path the server was started from. /proc/self/exe is NOT usable here: once
 * the package has replaced the file, it still points at the old (deleted)
 * inode - and the old inode is exactly what we are trying to leave behind.
 */
char		*server_binary_path;

/* Set by main() when this process IS the freshly exec'd server. */
const char	*server_upgrade_file;

/* Listening socket inherited from the previous binary (-1 = none). */
int		 server_upgrade_socket = -1;

/*
 * Copy of the PREVIOUS binary, kept aside so a failed upgrade can go back to
 * it. The file it came from may already have been replaced on disk - that is
 * the whole point of upgrading - so a path is not enough, we need a copy.
 */
const char	*server_upgrade_fallback;

/* State file of the upgrade in progress, for the fallback handler. */
static char	 upgrade_state_path[PATH_MAX];

#define UPGRADE_VERSION 1

/*
 * CRASH RECOVERY: the same state, written periodically to ~/.tmuxv-state, but
 * COLD - the descriptors of a dead server mean nothing, so panes carry their
 * working directory instead and come back as shells. The file is deleted on a
 * clean shutdown, so finding one at startup IS the crash detection: no
 * timestamp heuristics, unlike tmux-continuum which has to guess from
 * #{start_time} within ten seconds.
 */
static struct event	 upgrade_save_ev;
static int		 upgrade_save_on;

/*
 * User options (@...) are STRINGS, always: options_get_number() on one is a
 * fatal error, which is how the first attempt killed the server at startup.
 */
static int
upgrade_opt_num(const char *name, int def)
{
	const char	*v;

	if (options_get(global_s_options, name) == NULL)
		return (def);
	if ((v = options_get_string(global_s_options, name)) == NULL ||
	    *v == '\0')
		return (def);
	return (atoi(v));
}

int
upgrade_enabled(void)
{
	const char	*v;

	if (options_get(global_s_options, "@restore") == NULL)
		return (1);			/* on by default */
	v = options_get_string(global_s_options, "@restore");
	if (v == NULL || *v == '\0' || strcmp(v, "off") == 0 ||
	    strcmp(v, "0") == 0)
		return (0);
	return (1);
}

/*
 * Client flags that describe WHAT THE CLIENT IS, as opposed to what it is
 * doing right now. Losing CLIENT_UTF8 is what turns the desktop hatching into
 * stripes and "Édition" into "_dition": every multi-byte character gets drawn
 * one byte at a time.
 */
#define UPGRADE_CLIENT_FLAGS (CLIENT_UTF8|CLIENT_LOGIN|CLIENT_CONTROL| \
	CLIENT_NOSTARTSERVER|CLIENT_DEFAULTSOCKET)

/* ~/.tmuxv-state, or NULL if there is no home to put it in. */
static const char *
upgrade_state_file(void)
{
	static char	path[PATH_MAX];
	const char	*home;

	if (*path != '\0')
		return (path);
	if ((home = getenv("HOME")) == NULL || *home == '\0')
		return (NULL);
	xsnprintf(path, sizeof path, "%s/.tmuxv-state", home);
	return (path);
}

/* A tab or a newline in a title would split the record: keep them out. */
static void
upgrade_clean(char *dst, size_t len, const char *src)
{
	size_t	i;

	if (src == NULL)
		src = "";
	strlcpy(dst, src, len);
	for (i = 0; dst[i] != '\0'; i++) {
		if (dst[i] == '\t' || dst[i] == '\n' || dst[i] == '\r')
			dst[i] = ' ';
	}
}

/* Split a line in place; returns the number of fields found. */
static u_int
upgrade_split(char *line, char **fields, u_int max)
{
	u_int	n = 0;
	char	*p = line;

	while (n < max) {
		fields[n++] = p;
		if ((p = strchr(p, '\t')) == NULL)
			break;
		*p++ = '\0';
	}
	return (n);
}

/*
 * Write everything the new binary needs. Windows first (they own the panes and
 * their file descriptors), then sessions, then the links between them, then
 * the clients.
 */
static int
upgrade_save(const char *path, char **cause)
{
	FILE			*f;
	struct window		*w;
	struct window_pane	*wp;
	struct session		*s;
	struct winlink		*wl;
	struct client		*c;
	char			*layout;
	char			 name[256], title[256], cwd[PATH_MAX];
	char			 tty[PATH_MAX], term[128], caps[4096], *p;
	char			 agent[256];
	u_int			 i;

	if ((f = fopen(path, "w")) == NULL) {
		xasprintf(cause, "%s: %s", path, strerror(errno));
		return (-1);
	}
	fprintf(f, "tmuxv-upgrade\t%d\n", UPGRADE_VERSION);
	fprintf(f, "socket\t%d\t%s\n", server_upgrade_socket_fd(),
	    socket_path != NULL ? socket_path : "");

	RB_FOREACH(w, windows, &windows) {
		/*
		 * A zoomed window's live layout has a single cell: dump the
		 * SAVED one, which describes every pane, and re-zoom on the
		 * other side.
		 */
		layout = layout_dump(w->saved_layout_root != NULL ?
		    w->saved_layout_root : w->layout_root);
		if (layout == NULL)
			continue;
		upgrade_clean(name, sizeof name, w->name);
		fprintf(f, "window\t@%u\t%u\t%u\t%u\t%u\t%s\t%s\t%d\t"
		    "%u\t%u\t%u\t%u\t%d\t%d\t%u\n",
		    w->id, w->sx, w->sy, w->xpixel, w->ypixel, name, layout,
		    (w->flags & WINDOW_ZOOMED) ? 1 : 0,
		    w->desktop_x, w->desktop_y, w->desktop_w, w->desktop_h,
		    w->desktop_zoomed, w->claude_mgr, w->claude_listw);
		free(layout);

		TAILQ_FOREACH(wp, &w->panes, entry) {
			upgrade_clean(title, sizeof title, wp->base.title);
			upgrade_clean(cwd, sizeof cwd, wp->cwd);
			upgrade_clean(tty, sizeof tty, wp->tty);
			/*
			 * The bus address a renamed conversation answers to.
			 * It lives in memory only (the environment of the
			 * running agent still holds the name it was born with),
			 * so without this the pane would come back listening on
			 * its old address and its mail would never arrive.
			 */
			upgrade_clean(agent, sizeof agent,
			    wp->claude_agent);
			fprintf(f, "pane\t@%u\t%d\t%ld\t%d\t%s\t%s\t%s\t%s\n",
			    w->id, wp->fd, (long)wp->pid,
			    (wp == w->active) ? 1 : 0, tty, cwd, title, agent);
		}
	}

	RB_FOREACH(s, sessions, &sessions) {
		upgrade_clean(name, sizeof name, s->name);
		upgrade_clean(cwd, sizeof cwd, s->cwd);
		fprintf(f, "session\t$%u\t%s\t%s\n", s->id, name, cwd);
		RB_FOREACH(wl, winlinks, &s->windows) {
			fprintf(f, "link\t$%u\t%d\t@%u\t%d\n", s->id, wl->idx,
			    wl->window->id, (s->curw == wl) ? 1 : 0);
		}
	}

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->peer == NULL || (~c->flags & CLIENT_IDENTIFIED))
			continue;
		if (c->flags & (CLIENT_CONTROL|CLIENT_DEAD|CLIENT_EXIT))
			continue;
		if (c->fd == -1)
			continue;	/* only plain attached terminals */
		upgrade_clean(tty, sizeof tty, c->ttyname);
		upgrade_clean(term, sizeof term, c->term_name);
		/*
		 * The terminfo capabilities the client sent at identify time
		 * must travel too: without them tty_open() cannot build the
		 * terminal and the screen stays frozen. They are joined with
		 * \001, which cannot appear in a capability string.
		 */
		caps[0] = '\0';
		for (i = 0; i < c->term_ncaps; i++) {
			if (i != 0)
				strlcat(caps, "\001", sizeof caps);
			strlcat(caps, c->term_caps[i], sizeof caps);
		}
		for (p = caps; *p != '\0'; p++) {
			if (*p == '\t' || *p == '\n')
				*p = ' ';
		}
		fprintf(f, "client\t%d\t%d\t$%u\t%s\t%s\t%u\t%u\t%d\t%s\t%s"
		    "\t%llu\n",
		    proc_peer_fd(c->peer), c->fd,
		    (c->session != NULL) ? c->session->id : 0,
		    tty, term, c->tty.sx, c->tty.sy, c->term_features,
		    (c->term_type != NULL) ? c->term_type : "", caps,
		    (unsigned long long)(c->flags & UPGRADE_CLIENT_FLAGS));
	}

	/*
	 * Every global option, exactly as it is now - the configuration's
	 * values AND whatever was changed while the server ran (a port, a
	 * database, a theme tweak). The new process starts from the defaults,
	 * so without this a hot upgrade silently dropped them all.
	 */
	upgrade_save_options(f, global_options, 's');
	upgrade_save_options(f, global_s_options, 'g');
	upgrade_save_options(f, global_w_options, 'w');

	if (ferror(f) || fclose(f) != 0) {
		xasprintf(cause, "%s: write failed", path);
		return (-1);
	}
	return (0);
}

/* Option values go through the state file as hex: no tab, no newline. */
static char *
upgrade_hex_encode(const char *s)
{
	static const char	 digits[] = "0123456789abcdef";
	size_t			 len = strlen(s), i;
	char			*out = xmalloc(len * 2 + 1);

	for (i = 0; i < len; i++) {
		out[i * 2] = digits[((u_char)s[i]) >> 4];
		out[i * 2 + 1] = digits[((u_char)s[i]) & 0xf];
	}
	out[len * 2] = '\0';
	return (out);
}

static char *
upgrade_hex_decode(const char *s)
{
	size_t	 len = strlen(s) / 2, i;
	char	*out = xmalloc(len + 1);
	u_int	 v;

	for (i = 0; i < len; i++) {
		if (sscanf(s + i * 2, "%2x", &v) != 1)
			v = '?';
		out[i] = (char)v;
	}
	out[len] = '\0';
	return (out);
}

/* "option <scope> <name> <index|-1|clear> <hex value>" */
static void
upgrade_save_options(FILE *f, struct options *oo, char scope)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	char				*v, *hex;
	u_int				 idx;

	for (o = options_first(oo); o != NULL; o = options_next(o)) {
		if (options_is_array(o)) {
			/* Emptied first, so a shortened array stays short. */
			fprintf(f, "option\t%c\t%s\tclear\t\n", scope,
			    options_name(o));
			for (a = options_array_first(o); a != NULL;
			    a = options_array_next(a)) {
				idx = options_array_item_index(a);
				v = options_to_string(o, (int)idx, 0);
				hex = upgrade_hex_encode(v);
				fprintf(f, "option\t%c\t%s\t%u\t%s\n", scope,
				    options_name(o), idx, hex);
				free(hex);
				free(v);
			}
			continue;
		}
		v = options_to_string(o, -1, 0);
		hex = upgrade_hex_encode(v);
		fprintf(f, "option\t%c\t%s\t-1\t%s\n", scope, options_name(o),
		    hex);
		free(hex);
		free(v);
	}
}

struct upgrade_opt {
	char			 scope;
	char			*name;
	int			 idx;		/* -2 clear, -1 scalar, >= 0 item */
	char			*value;
	TAILQ_ENTRY(upgrade_opt) entry;
};
TAILQ_HEAD(upgrade_opts, upgrade_opt);
static struct upgrade_opts	upgrade_opts = TAILQ_HEAD_INITIALIZER(upgrade_opts);

/*
 * Runs AFTER the configuration has been read again: the values from before the
 * upgrade win, so a change made while the server ran is not undone by the
 * file it was never written to.
 */
static enum cmd_retval
upgrade_options_apply(__unused struct cmdq_item *item, __unused void *data)
{
	struct upgrade_opt	*uo;
	struct options		*oo;
	struct options_entry	*o;
	char			*cause;
	struct client		*c;

	while ((uo = TAILQ_FIRST(&upgrade_opts)) != NULL) {
		TAILQ_REMOVE(&upgrade_opts, uo, entry);
		oo = (uo->scope == 's') ? global_options :
		    (uo->scope == 'w') ? global_w_options : global_s_options;
		o = options_get_only(oo, uo->name);
		cause = NULL;
		if (uo->idx == -2) {
			if (o != NULL && options_is_array(o))
				options_array_clear(o);
		} else if (uo->idx >= 0) {
			if (o == NULL && *uo->name == '@')
				o = options_set_string(oo, uo->name, 0, "%s", "");
			if (o != NULL && options_is_array(o)) {
				options_array_set(o, (u_int)uo->idx, uo->value, 0,
				    &cause);
			}
		} else if (*uo->name == '@') {
			/*
			 * A user option is a plain string, and may not exist
			 * yet in this process (set while the server ran, never
			 * in the configuration): options_from_string() would
			 * read its old value first and die on it. Set it the
			 * way "set -g @x value" does.
			 */
			options_set_string(oo, uo->name, 0, "%s", uo->value);
		} else if (o != NULL) {
			if (options_from_string(oo, options_table_entry(o),
			    uo->name, uo->value, 0, &cause) != 0) {
				log_debug("%s: %s: %s", __func__, uo->name,
				    cause != NULL ? cause : "?");
			}
		}
		free(cause);
		options_push_changes(uo->name);
		free(uo->name);
		free(uo->value);
		free(uo);
	}
	TAILQ_FOREACH(c, &clients, entry)
		server_redraw_client(c);
	return (CMD_RETURN_NORMAL);
}

/* Descriptors must survive execve(): clear close-on-exec on everything kept. */
static void
upgrade_keep_fd(int fd)
{
	int	flags;

	if (fd < 0)
		return;
	if ((flags = fcntl(fd, F_GETFD)) != -1)
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void
upgrade_keep_all(void)
{
	struct window		*w;
	struct window_pane	*wp;
	struct client		*c;

	upgrade_keep_fd(server_upgrade_socket_fd());
	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry)
			upgrade_keep_fd(wp->fd);
	}
	TAILQ_FOREACH(c, &clients, entry) {
		upgrade_keep_fd(c->fd);
		if (c->peer != NULL)
			upgrade_keep_fd(proc_peer_fd(c->peer));
	}
}

/*
 * Everything owed to the clients must reach the wire BEFORE the exec: the new
 * binary inherits the sockets, not the buffers that sit in front of them.
 */
static void
upgrade_flush(void)
{
	struct client	*c;
	u_int		 i;
	int		 retval = 0;

	/*
	 * A command client (the one that typed "upgrade-server", and any other
	 * waiting on a reply) has no terminal and expects an answer we will
	 * never send once we have exec'd. Dismiss them now, or they hang.
	 */
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->peer == NULL || c->fd != -1)
			continue;
		proc_send(c->peer, MSG_EXIT, -1, &retval, sizeof retval);
	}

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->fd != -1 && c->tty.out != NULL) {
			for (i = 0; i < 100; i++) {
				if (EVBUFFER_LENGTH(c->tty.out) == 0)
					break;
				if (evbuffer_write(c->tty.out, c->fd) <= 0)
					break;
			}
		}
		if (c->peer != NULL)
			proc_flush_peer(c->peer);
	}
}

/*
 * The listening socket's number is needed BEFORE the state file is replayed -
 * server_start() would otherwise create a second socket on the same path and
 * fight the one we inherited. So peek at that one line early.
 */
int
upgrade_peek_socket(const char *path)
{
	FILE	*f;
	char	 line[1024];
	int	 fd = -1;

	if ((f = fopen(path, "r")) == NULL)
		return (-1);
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "socket\t", 7) == 0) {
			fd = atoi(line + 7);
			break;
		}
	}
	fclose(f);
	if (fd >= 0 && fcntl(fd, F_GETFD) == -1)
		fd = -1;		/* not actually inherited */
	return (fd);
}

/*
 * CLAUDE: which conversation is this pane running? Claude Code tells nobody:
 * its command line is just "claude", it exports no session id, keeps no lock
 * file and does not even hold its transcript open (checked). The one
 * observable trace is the file it keeps APPENDING to - so the active session
 * of a pane is the most recently written .jsonl of its project directory.
 * Files already claimed by another pane are skipped, which is what separates
 * two conversations sitting in the same directory.
 *
 * A heuristic, then, but the only one available - and a wrong guess costs at
 * most a conversation reopened in place of another, never a lost one.
 */
static int
upgrade_claude_session(const char *cwd, char **claimed, u_int nclaimed,
    char *out, size_t len)
{
	char		 dir[PATH_MAX], path[PATH_MAX], best[128] = "";
	const char	*home;
	DIR		*dp;
	struct dirent	*de;
	struct stat	 sb;
	time_t		 newest = 0, now = time(NULL);
	size_t		 l, i;
	u_int		 j;

	*out = '\0';
	if (cwd == NULL || *cwd != '/')
		return (0);
	if ((home = getenv("HOME")) == NULL || *home == '\0')
		return (0);

	l = (size_t)snprintf(dir, sizeof dir, "%s/.claude/projects/", home);
	for (i = 0; cwd[i] != '\0' && l + 1 < sizeof dir; i++)
		dir[l++] = (cwd[i] == '/') ? '-' : cwd[i];
	dir[l] = '\0';

	if ((dp = opendir(dir)) == NULL)
		return (0);
	while ((de = readdir(dp)) != NULL) {
		l = strlen(de->d_name);
		if (l < 7 || strcmp(de->d_name + l - 6, ".jsonl") != 0)
			continue;
		if (snprintf(path, sizeof path, "%s/%s", dir,
		    de->d_name) >= (int)sizeof path)
			continue;
		if (stat(path, &sb) == -1 || !S_ISREG(sb.st_mode))
			continue;
		/* A transcript untouched for an hour is not a live session. */
		if (now - sb.st_mtime > 3600)
			continue;
		for (j = 0; j < nclaimed; j++) {
			if (claimed[j] != NULL &&
			    strncmp(claimed[j], de->d_name, l - 6) == 0)
				break;
		}
		if (j != nclaimed)
			continue;		/* another pane has it */
		if (sb.st_mtime > newest) {
			newest = sb.st_mtime;
			strlcpy(best, de->d_name, sizeof best);
			best[strlen(best) - 6] = '\0';	/* drop .jsonl */
		}
	}
	closedir(dp);
	if (*best == '\0')
		return (0);
	/* It becomes a command argument: nothing but a plain uuid. */
	for (i = 0; best[i] != '\0'; i++) {
		if (!isxdigit((u_char)best[i]) && best[i] != '-')
			return (0);
	}
	strlcpy(out, best, len);
	return (1);
}

/* Is this Claude conversation already open in a pane of this server? */
static int
upgrade_uuid_running(const char *uuid)
{
	struct window_pane	*wp;
	char			 cur[128];

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (claude_pane_session_uuid(wp, cur, sizeof cur) &&
		    strcmp(cur, uuid) == 0)
			return (1);
	}
	return (0);
}

/* Is this pane running Claude Code? (claude_pane_is_agent is menu.c's own) */
static int
upgrade_is_claude(struct window_pane *wp)
{
	char		*cur;
	const char	*b;
	int		 yes = 0;

	if (wp->fd == -1)
		return (0);
	cur = osdep_get_name(wp->fd, wp->tty);
	if (cur != NULL && *cur != '\0') {
		b = strrchr(cur, '/');
		b = (b != NULL) ? b + 1 : cur;
		yes = (strcmp(b, "claude") == 0);
	}
	free(cur);
	return (yes);
}

/*
 * Write the cold state: what it takes to REBUILD the arrangement, not to
 * inherit it. Panes are described by their working directory only - a crash
 * recovery that re-ran whatever was in each pane would be a footgun (the
 * tmux-resurrect documentation gives the example of a "sudo mkfs" coming back
 * on its own), so they return as shells.
 */
/*
 * The cold state, for the whole server (only == NULL) or a single session:
 * what the state file holds, and what the database catalog keeps per session
 * so that a session can be brought back on ANY tmuxv - even after the server
 * that ran it is gone.
 */
static void
upgrade_cold_write(FILE *f, struct session *only, u_int *nwin, u_int *nconv)
{
	struct window		*w;
	struct window_pane	*wp;
	struct session		*s;
	struct winlink		*wl;
	char			*layout;
	char			 name[256], cwd[PATH_MAX], *live;
	char			 uuid[128], agent[256];
	char			**claimed = NULL, **used = NULL;
	u_int			 nclaimed = 0, nused = 0, ci;

	if (nwin != NULL)
		*nwin = 0;
	if (nconv != NULL)
		*nconv = 0;

	/*
	 * First the EXACT ids (tmuxv started the conversation with --session-id,
	 * or brought it back with --resume): the guess for the other panes must
	 * never take one of those.
	 */
	RB_FOREACH(w, windows, &windows) {
		if (only != NULL && !session_has(only, w))
			continue;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (upgrade_is_claude(wp) &&
			    claude_pane_session_uuid(wp, uuid, sizeof uuid)) {
				claimed = xreallocarray(claimed, nclaimed + 1,
				    sizeof *claimed);
				claimed[nclaimed++] = xstrdup(uuid);
			}
		}
	}

	fprintf(f, "tmuxv-upgrade\t%d\n", UPGRADE_VERSION);
	fprintf(f, "cold\t1\n");
	RB_FOREACH(w, windows, &windows) {
		if (only != NULL && !session_has(only, w))
			continue;
		layout = layout_dump(w->saved_layout_root != NULL ?
		    w->saved_layout_root : w->layout_root);
		if (layout == NULL)
			continue;
		if (nwin != NULL)
			(*nwin)++;
		upgrade_clean(name, sizeof name, w->name);
		fprintf(f, "window\t@%u\t%u\t%u\t%u\t%u\t%s\t%s\t%d\t"
		    "%u\t%u\t%u\t%u\t%d\t%d\t%u\n",
		    w->id, w->sx, w->sy, w->xpixel, w->ypixel, name, layout,
		    (w->flags & WINDOW_ZOOMED) ? 1 : 0,
		    w->desktop_x, w->desktop_y, w->desktop_w, w->desktop_h,
		    w->desktop_zoomed, w->claude_mgr, w->claude_listw);
		free(layout);
		TAILQ_FOREACH(wp, &w->panes, entry) {
			/* The LIVE directory, not the one it started in. */
			live = (wp->fd != -1) ? osdep_get_cwd(wp->fd) : NULL;
			upgrade_clean(cwd, sizeof cwd,
			    (live != NULL && *live == '/') ? live : wp->cwd);
			/*
			 * A Claude conversation is the one thing worth bringing
			 * back running: it is resumable by id, so nothing is
			 * re-executed blindly. Never the same id twice: two
			 * processes appending to one transcript corrupt it.
			 */
			*uuid = '\0';
			if (upgrade_is_claude(wp)) {
				if (claude_pane_session_uuid(wp, uuid,
				    sizeof uuid)) {
					for (ci = 0; ci < nused; ci++) {
						if (strcmp(used[ci], uuid) == 0)
							break;
					}
					if (ci != nused)
						*uuid = '\0';
				} else if (upgrade_claude_session(cwd, claimed,
				    nclaimed, uuid, sizeof uuid)) {
					claimed = xreallocarray(claimed,
					    nclaimed + 1, sizeof *claimed);
					claimed[nclaimed++] = xstrdup(uuid);
				} else
					*uuid = '\0';
				if (*uuid != '\0') {
					used = xreallocarray(used, nused + 1,
					    sizeof *used);
					used[nused++] = xstrdup(uuid);
					if (nconv != NULL)
						(*nconv)++;
				}
			}
			upgrade_clean(agent, sizeof agent, wp->claude_agent);
			fprintf(f, "pane\t@%u\t-1\t0\t%d\t\t%s\t\t%s\t%s\n",
			    w->id, (wp == w->active) ? 1 : 0, cwd, uuid, agent);
		}
	}
	RB_FOREACH(s, sessions, &sessions) {
		if (only != NULL && s != only)
			continue;
		upgrade_clean(name, sizeof name, s->name);
		upgrade_clean(cwd, sizeof cwd, s->cwd);
		fprintf(f, "session\t$%u\t%s\t%s\n", s->id, name, cwd);
		RB_FOREACH(wl, winlinks, &s->windows) {
			fprintf(f, "link\t$%u\t%d\t@%u\t%d\n", s->id,
			    wl->idx, wl->window->id, (s->curw == wl) ? 1 : 0);
		}
	}
	for (ci = 0; ci < nclaimed; ci++)
		free(claimed[ci]);
	free(claimed);
	for (ci = 0; ci < nused; ci++)
		free(used[ci]);
	free(used);
}

/* The cold state as text (xmalloc'd), for the database catalog. */
char *
upgrade_cold_text(struct session *only, u_int *nwin, u_int *nconv)
{
	char	*buf = NULL;
	size_t	 len = 0;
	FILE	*f;

	if ((f = open_memstream(&buf, &len)) == NULL)
		return (NULL);
	upgrade_cold_write(f, only, nwin, nconv);
	if (fclose(f) != 0) {
		free(buf);
		return (NULL);
	}
	return (buf);
}

/* This server, as the catalog knows it: host and socket. */
const char *
upgrade_node_name(void)
{
	static char	node[PATH_MAX + 300];
	char		host[256];

	if (*node == '\0') {
		if (gethostname(host, sizeof host) != 0)
			strlcpy(host, "localhost", sizeof host);
		host[sizeof host - 1] = '\0';
		snprintf(node, sizeof node, "%s:%s", host,
		    socket_path != NULL ? socket_path : "");
	}
	return (node);
}

static const char *
upgrade_host(void)
{
	static char	host[256];

	if (*host == '\0' && gethostname(host, sizeof host) != 0)
		strlcpy(host, "localhost", sizeof host);
	return (host);
}

/*
 * Every snapshot also goes to the database, session by session, when the bus
 * has one: that is what lets another tmuxv bring a session back after this
 * server has stopped. Sessions closed meanwhile leave the catalog.
 */
static void
upgrade_push_catalog(void)
{
	struct session	*s;
	char		*text, *keep = NULL, *k;
	u_int		 nw, nc, n = 0;
	const char	*node;

	if (strcmp(bus_mode(), "db") != 0)
		return;
	node = upgrade_node_name();
	RB_FOREACH(s, sessions, &sessions) {
		if ((text = upgrade_cold_text(s, &nw, &nc)) == NULL)
			continue;
		bus_snapshot_put(node, upgrade_host(), s->name, nw, nc, text);
		free(text);
		if (keep == NULL)
			keep = xstrdup(s->name);
		else {
			xasprintf(&k, "%s\x1f%s", keep, s->name);
			free(keep);
			keep = k;
		}
		n++;
	}
	if (n != 0)
		bus_snapshot_prune(node, keep);
	free(keep);
}

static void
upgrade_save_cold(void)
{
	FILE		*f;
	const char	*path = upgrade_state_file();
	char		 tmp[PATH_MAX];

	if (path == NULL)
		return;
	xsnprintf(tmp, sizeof tmp, "%s.new", path);
	if ((f = fopen(tmp, "w")) == NULL)
		return;
	upgrade_cold_write(f, NULL, NULL, NULL);
	if (ferror(f) || fclose(f) != 0) {
		unlink(tmp);
		return;
	}
	/* Atomic: a crash must never leave a half-written state behind. */
	if (rename(tmp, path) != 0)
		unlink(tmp);
}

static void
upgrade_save_timer(__unused int fd, __unused short events, __unused void *arg)
{
	struct timeval	tv;
	int		interval;

	interval = upgrade_opt_num("@restore-interval", 60);
	if (interval <= 0) {
		upgrade_save_on = 0;
		return;
	}
	upgrade_save_cold();
	upgrade_push_catalog();
	tv.tv_sec = interval;
	tv.tv_usec = 0;
	evtimer_add(&upgrade_save_ev, &tv);
}

/* Start snapshotting. Called once the server is up. */
void
upgrade_save_start(void)
{
	/*
	 * Short first delay: at this point of the startup the configuration
	 * has not run yet, so @restore-interval is not readable - the timer
	 * callback picks it up for every snapshot after this one.
	 */
	struct timeval	tv = { 3, 0 };

	if (upgrade_save_on)
		return;
	upgrade_save_on = 1;
	evtimer_set(&upgrade_save_ev, upgrade_save_timer, NULL);
	evtimer_add(&upgrade_save_ev, &tv);
}

/*
 * A clean shutdown removes the state file - so a file still lying there at
 * startup means the previous server did NOT exit cleanly.
 */
void
upgrade_save_clean(void)
{
	const char	*path = upgrade_state_file();

	if (upgrade_save_on && path != NULL)
		unlink(path);
	upgrade_save_on = 0;
}

/*
 * Stopped from outside (logout, shutdown): stop snapshotting - the panes are
 * about to die, a new snapshot would record their absence - and leave the
 * last state file where it is, so the next start restores it.
 */
void
upgrade_save_keep(void)
{
	if (upgrade_save_on)
		evtimer_del(&upgrade_save_ev);
	upgrade_save_on = 0;
}

/* Double-quote a path for a generated command (no injection from a cwd). */
static void
upgrade_quote(char *dst, size_t len, const char *val)
{
	size_t	di = 0;

	if (len < 3)
		return;
	dst[di++] = '"';
	while (*val != '\0' && di + 2 < len) {
		if (*val == '"' || *val == '\\' || *val == '$' || *val == '`')
			dst[di++] = '\\';
		if (di + 2 >= len)
			break;
		if (*val != '\n' && *val != '\r')
			dst[di++] = *val;
		val++;
	}
	dst[di++] = '"';
	dst[di] = '\0';
}

/*
 * Rebuild from a COLD state, by generating the tmux commands that recreate the
 * arrangement. Going through the command queue avoids needing a cmdq_item to
 * call spawn_pane(), and every step is one the user could have typed.
 */
struct upgrade_cold_win {
	char	*layout;
	char	*name;
	int	 zoomed;
	u_int	 dx, dy, dw, dh;
	int	 dzoom, mgr;
	u_int	 listw;
	u_int	 old_id;
	int	 idx;			/* index once linked in a session */
	char	*session;
	char   **cwds;			/* one working directory per pane */
	char   **uuids;			/* Claude conversation, or NULL */
	char   **agents;		/* bus address of a renamed pane */
	u_int	 npanes;
	int	 done;			/* handed to the finishing callback */
};

/* Applied after the commands have run: fields that are not tmux options. */
static enum cmd_retval
upgrade_cold_finish(__unused struct cmdq_item *item, void *data)
{
	struct upgrade_cold_win	*cw = data;
	struct session		*s;
	struct winlink		*wl;
	u_int			 i;

	if ((s = session_find(cw->session)) != NULL &&
	    (wl = winlink_find_by_index(&s->windows, cw->idx)) != NULL) {
		struct window	*w = wl->window;

		free(w->name);
		w->name = xstrdup(cw->name);
		options_set_number(w->options, "automatic-rename", 0);
		w->desktop_x = cw->dx;
		w->desktop_y = cw->dy;
		w->desktop_w = cw->dw;
		w->desktop_h = cw->dh;
		w->desktop_zoomed = cw->dzoom;
		w->claude_mgr = cw->mgr;
		w->claude_listw = cw->listw;
		if (cw->zoomed && w->active != NULL)
			window_zoom(w->active);
		/* Give each pane back the address its mail is sent to. */
		{
			struct window_pane	*wp;
			u_int			 k = 0;

			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (k >= cw->npanes)
					break;
				if (cw->agents[k] != NULL) {
					free(wp->claude_agent);
					wp->claude_agent =
					    xstrdup(cw->agents[k]);
				}
				k++;
			}
		}
		server_redraw_window(w);
	}
	for (i = 0; i < cw->npanes; i++) {
		free(cw->cwds[i]);
		free(cw->uuids[i]);
		free(cw->agents[i]);
	}
	free(cw->cwds);
	free(cw->uuids);
	free(cw->agents);
	free(cw->layout);
	free(cw->name);
	free(cw->session);
	free(cw);
	return (CMD_RETURN_NORMAL);
}

static void
upgrade_run(const char *cmd)
{

	struct cmdq_state	*state;
	char			*error;

	state = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, NULL, state, &error) ==
	    CMD_PARSE_ERROR) {
		log_debug("%s: %s: %s", __func__, cmd, error);
		free(error);
	}
	cmdq_free_state(state);
}

static int
upgrade_restore_path(const char *path, int ondemand, char **renamed)
{
	FILE			*f;
	char			 line[8192], *fields[20], *cmd, q[PATH_MAX + 16];
	u_int			 n, i, j, k;
	int			 cold = 0, restored = 0, base;
	struct upgrade_cold_win	**wins = NULL, *cw = NULL;
	u_int			  nwins = 0;
	struct { char *name; char *cwd; } *sess = NULL;
	u_int			  nsess = 0;
	char			 *cursess = NULL;

	if (path == NULL || (f = fopen(path, "r")) == NULL)
		return (0);
	if (fgets(line, sizeof line, f) == NULL ||
	    strncmp(line, "tmuxv-upgrade\t", 14) != 0 ||
	    atoi(line + 14) != UPGRADE_VERSION) {
		fclose(f);
		return (0);
	}

	/*
	 * FIRST PASS: read everything. The file lists windows BEFORE the links
	 * that say which session they belong to, so nothing can be created
	 * while reading - that was the mistake that restored a session with a
	 * single empty window.
	 */
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		n = upgrade_split(line, fields, nitems(fields));
		if (n == 0)
			continue;
		if (strcmp(fields[0], "cold") == 0) {
			cold = 1;
			continue;
		}
		if (!cold)
			break;			/* a live state: not for us */

		if (strcmp(fields[0], "window") == 0 && n >= 16) {
			cw = xcalloc(1, sizeof *cw);
			cw->old_id = (u_int)strtoul(fields[1] + 1, NULL, 10);
			cw->name = xstrdup(fields[6]);
			cw->layout = xstrdup(fields[7]);
			cw->zoomed = atoi(fields[8]);
			cw->dx = (u_int)atoi(fields[9]);
			cw->dy = (u_int)atoi(fields[10]);
			cw->dw = (u_int)atoi(fields[11]);
			cw->dh = (u_int)atoi(fields[12]);
			cw->dzoom = atoi(fields[13]);
			cw->mgr = atoi(fields[14]);
			cw->listw = (u_int)atoi(fields[15]);
			cw->idx = -1;
			wins = xreallocarray(wins, nwins + 1, sizeof *wins);
			wins[nwins++] = cw;
			continue;
		}
		if (strcmp(fields[0], "pane") == 0 && n >= 7 && nwins != 0) {
			cw = wins[nwins - 1];
			cw->cwds = xreallocarray(cw->cwds, cw->npanes + 1,
			    sizeof *cw->cwds);
			cw->uuids = xreallocarray(cw->uuids, cw->npanes + 1,
			    sizeof *cw->uuids);
			cw->agents = xreallocarray(cw->agents, cw->npanes + 1,
			    sizeof *cw->agents);
			cw->cwds[cw->npanes] = xstrdup(fields[6]);
			/*
			 * Field 8: the Claude conversation to resume. Eight,
			 * not seven - the record keeps the live format's title
			 * column between the directory and the uuid.
			 */
			cw->uuids[cw->npanes] = (n > 8 && *fields[8] != '\0') ?
			    xstrdup(fields[8]) : NULL;
			/* Field 9: the bus address a renamed conversation
			 * answers to (the environment of the process that will
			 * be started carries only its birth name). */
			cw->agents[cw->npanes] = (n > 9 && *fields[9] != '\0') ?
			    xstrdup(fields[9]) : NULL;
			cw->npanes++;
			continue;
		}
		if (strcmp(fields[0], "session") == 0 && n >= 4) {
			sess = xreallocarray(sess, nsess + 1, sizeof *sess);
			sess[nsess].name = xstrdup(fields[2]);
			sess[nsess].cwd = xstrdup(fields[3]);
			nsess++;
			free(cursess);
			cursess = xstrdup(fields[2]);
			continue;
		}
		if (strcmp(fields[0], "link") == 0 && n >= 5 &&
		    cursess != NULL) {
			for (i = 0; i < nwins; i++) {
				if (wins[i]->old_id !=
				    (u_int)strtoul(fields[3] + 1, NULL, 10))
					continue;
				free(wins[i]->session);
				wins[i]->session = xstrdup(cursess);
				wins[i]->idx = atoi(fields[2]);
				break;
			}
			continue;
		}
	}
	fclose(f);

	/*
	 * On demand (restore-session), into a server that is already running:
	 * a session of the same name gets another one ("t-2"), rather than
	 * being skipped or merged.
	 */
	if (ondemand) {
		for (i = 0; i < nsess; i++) {
			char	*nn;
			u_int	 suffix = 2;

			if (session_find(sess[i].name) == NULL)
				continue;
			for (;;) {
				xasprintf(&nn, "%s-%u", sess[i].name, suffix++);
				if (session_find(nn) == NULL)
					break;
				free(nn);
			}
			for (j = 0; j < nwins; j++) {
				if (wins[j]->session != NULL &&
				    strcmp(wins[j]->session, sess[i].name) == 0) {
					free(wins[j]->session);
					wins[j]->session = xstrdup(nn);
				}
			}
			free(sess[i].name);
			sess[i].name = nn;
		}
		if (renamed != NULL && nsess != 0)
			*renamed = xstrdup(sess[0].name);
	}
	/*
	 * A conversation already open HERE is not opened a second time: two
	 * processes appending to one transcript would corrupt it. That pane
	 * comes back as a shell in the conversation's directory.
	 */
	for (j = 0; j < nwins; j++) {
		for (k = 0; k < wins[j]->npanes; k++) {
			if (wins[j]->uuids[k] != NULL &&
			    upgrade_uuid_running(wins[j]->uuids[k])) {
				free(wins[j]->uuids[k]);
				wins[j]->uuids[k] = NULL;
			}
		}
	}

	/* SECOND PASS: sessions, then their windows, then panes, then layout. */
	base = options_get_number(global_s_options, "base-index");
	for (i = 0; i < nsess; i++) {
		if (session_find(sess[i].name) != NULL)
			continue;		/* already there: leave it be */
		upgrade_quote(q, sizeof q, sess[i].cwd);
		xasprintf(&cmd, "new-session -d -s %s -c %s", sess[i].name, q);
		upgrade_run(cmd);
		free(cmd);
		restored = 1;

		for (j = 0; j < nwins; j++) {
			cw = wins[j];
			if (cw->session == NULL ||
			    strcmp(cw->session, sess[i].name) != 0)
				continue;
			upgrade_quote(q, sizeof q,
			    (cw->npanes != 0) ? cw->cwds[0] : sess[i].cwd);
			/*
			 * -k so the window new-session just created is
			 * replaced when it sits on a wanted index. A pane that
			 * ran a Claude conversation comes back ON it
			 * (claude --resume): that is not re-running an
			 * arbitrary command, it is reopening a document.
			 */
			if (cw->npanes != 0 && cw->uuids[0] != NULL) {
				xasprintf(&cmd, "new-window -d -k -t %s:%d "
				    "-c %s claude --resume %s", sess[i].name,
				    cw->idx, q, cw->uuids[0]);
			} else {
				xasprintf(&cmd, "new-window -d -k -t %s:%d "
				    "-c %s", sess[i].name, cw->idx, q);
			}
			upgrade_run(cmd);
			free(cmd);
			for (k = 1; k < cw->npanes; k++) {
				upgrade_quote(q, sizeof q, cw->cwds[k]);
				if (cw->uuids[k] != NULL) {
					xasprintf(&cmd, "split-window -d -t "
					    "%s:%d -c %s claude --resume %s",
					    sess[i].name, cw->idx, q,
					    cw->uuids[k]);
				} else {
					xasprintf(&cmd, "split-window -d -t "
					    "%s:%d -c %s", sess[i].name,
					    cw->idx, q);
				}
				upgrade_run(cmd);
				free(cmd);
			}
			xasprintf(&cmd, "select-layout -t %s:%d %s",
			    sess[i].name, cw->idx, cw->layout);
			upgrade_run(cmd);
			free(cmd);
			cmdq_append(NULL,
			    cmdq_get_callback(upgrade_cold_finish, cw));
			cw->done = 1;
		}
		/*
		 * The window new-session created may sit on an index nobody
		 * asked for: drop it, but only if it was not reused.
		 */
		for (j = 0; j < nwins; j++) {
			if (wins[j]->session != NULL &&
			    strcmp(wins[j]->session, sess[i].name) == 0 &&
			    wins[j]->idx == base)
				break;
		}
		if (j == nwins) {
			xasprintf(&cmd, "kill-window -t %s:%d", sess[i].name,
			    base);
			upgrade_run(cmd);
			free(cmd);
		}
	}

	for (i = 0; i < nwins; i++) {
		if (wins[i]->done)
			continue;		/* freed by the callback */
		for (j = 0; j < wins[i]->npanes; j++) {
			free(wins[i]->cwds[j]);
			free(wins[i]->uuids[j]);
			free(wins[i]->agents[j]);
		}
		free(wins[i]->cwds);
		free(wins[i]->uuids);
		free(wins[i]->agents);
		free(wins[i]->layout);
		free(wins[i]->name);
		free(wins[i]->session);
		free(wins[i]);
	}
	free(wins);
	for (i = 0; i < nsess; i++) {
		free(sess[i].name);
		free(sess[i].cwd);
	}
	free(sess);
	free(cursess);
	if (!ondemand)
		unlink(path);
	return (restored);
}

/* Crash recovery at startup: the state file of THIS server. */
int
upgrade_restore_cold(void)
{
	return (upgrade_restore_path(upgrade_state_file(), 0, NULL));
}

/* Bring back a session from a cold state text (the database catalog). */
int
upgrade_restore_text(const char *text, char **newname, char **cause)
{
	char	 path[] = "/tmp/tmuxv-restore-XXXXXX";
	int	 fd, rc;
	size_t	 len = strlen(text);

	*cause = NULL;
	if ((fd = mkstemp(path)) == -1) {
		xasprintf(cause, "%s", strerror(errno));
		return (-1);
	}
	if (write(fd, text, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		xasprintf(cause, "cannot write %s", path);
		return (-1);
	}
	close(fd);
	rc = upgrade_restore_path(path, 1, newname);
	unlink(path);
	if (rc <= 0) {
		xasprintf(cause, "nothing to restore in that snapshot");
		return (-1);
	}
	return (0);
}

/* ------------------------------------------------------------------------ */
/* restore-session: bring back a session from the catalog, on this server.  */

static enum cmd_retval	cmd_restore_session_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_restore_session_entry = {
	.name = "restore-session",
	.alias = NULL,

	.args = { "flmn:s:", 0, 0, NULL },
	.usage = "[-flm] [-n node] [-s session]",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_restore_session_exec
};

struct restore_ctx {
	struct cmdq_item	*item;
	struct client		*c;
	int			 list;
	int			 menu;
	int			 force;
	char			*node;
	char			*session;
};

/* A snapshot refreshed this recently means its server still runs. */
#define RESTORE_ALIVE 180

static void
restore_ctx_free(struct restore_ctx *ctx)
{
	free(ctx->node);
	free(ctx->session);
	free(ctx);
}

static void
restore_age(char *buf, size_t len, long long secs)
{
	if (secs < 120)
		snprintf(buf, len, "%lld s", secs);
	else if (secs < 7200)
		snprintf(buf, len, "%lld min", secs / 60);
	else if (secs < 172800)
		snprintf(buf, len, "%lld h", secs / 3600);
	else
		snprintf(buf, len, "%lld j", secs / 86400);
}

static void
restore_get_cb(const char *text, const char *err, void *arg)
{
	struct restore_ctx	*ctx = arg;
	char			*newname = NULL, *cause = NULL;

	if (text == NULL)
		cmdq_error(ctx->item, "restore-session: %s", err);
	else if (upgrade_restore_text(text, &newname, &cause) != 0) {
		cmdq_error(ctx->item, "restore-session: %s", cause);
		free(cause);
	} else {
		cmdq_print(ctx->item, "session restauree : %s (depuis %s)",
		    newname != NULL ? newname : ctx->session, ctx->node);
		free(newname);
	}
	cmdq_continue(ctx->item);
	restore_ctx_free(ctx);
}

static void
restore_list_cb(const char *tsv, const char *err, void *arg)
{
	struct restore_ctx	*ctx = arg;
	char			*copy, *line, *save = NULL, *f[6], *p, age[32];
	char			*cmd, qn[PATH_MAX + 320], qs[512];
	u_int			 n, rows = 0;
	long long		 updated, now = (long long)time(NULL);
	int			 found = 0;
	struct menu		*menu = NULL;
	struct menu_item	 it;
	struct cmd_find_state	 fs;

	if (tsv == NULL) {
		cmdq_error(ctx->item, "restore-session: %s", err);
		cmdq_continue(ctx->item);
		restore_ctx_free(ctx);
		return;
	}
	if (ctx->menu) {
		if (ctx->c == NULL || ctx->c->session == NULL) {
			cmdq_error(ctx->item, "restore-session: no client");
			cmdq_continue(ctx->item);
			restore_ctx_free(ctx);
			return;
		}
		cmd_find_from_client(&fs, ctx->c, 0);
		menu = menu_create("Restaurer une session");
	}

	copy = xstrdup(tsv);
	for (line = strtok_r(copy, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		for (n = 0, p = line; n < 6; n++) {
			f[n] = p;
			if ((p = strchr(p, '\t')) == NULL) {
				n++;
				break;
			}
			*p++ = '\0';
		}
		if (n < 6)
			continue;
		rows++;
		updated = strtoll(f[3], NULL, 10);
		restore_age(age, sizeof age, now - updated);
		if (ctx->list) {
			cmdq_print(ctx->item, "%s\t%s\t%s fenetre(s), %s "
			    "conversation(s), mise a jour il y a %s", f[1], f[0],
			    f[4], f[5], age);
			continue;
		}
		if (ctx->menu) {
			char	*label;

			upgrade_quote(qn, sizeof qn, f[0]);
			upgrade_quote(qs, sizeof qs, f[1]);
			xasprintf(&label, "%s  -  %s, %s conv., il y a %s",
			    f[1], f[2], f[5], age);
			xasprintf(&cmd, "restore-session -f -n %s -s %s", qn,
			    qs);
			memset(&it, 0, sizeof it);
			it.name = label;
			it.key = KEYC_NONE;
			it.command = cmd;
			menu_add_item(menu, &it, NULL, ctx->c, &fs);
			free(label);
			free(cmd);
			continue;
		}
		if (strcmp(f[0], ctx->node) != 0 ||
		    strcmp(f[1], ctx->session) != 0)
			continue;
		found = 1;
		if (!ctx->force && now - updated < RESTORE_ALIVE) {
			cmdq_error(ctx->item, "restore-session: %s a mis a jour "
			    "cette session il y a %s - ce serveur tourne sans doute "
			    "encore, et ses conversations seraient ouvertes deux "
			    "fois. -f pour restaurer quand meme.", f[0], age);
			found = -1;
		}
	}
	free(copy);

	if (ctx->list) {
		if (rows == 0)
			cmdq_print(ctx->item, "catalogue vide");
	} else if (ctx->menu) {
		if (menu->count == 0) {
			menu_free(menu);
			cmdq_error(ctx->item, "restore-session: catalogue vide");
		} else if (menu_display(menu, 0, -1, NULL,
		    ctx->c->tty.sx > 60 ? ctx->c->tty.sx / 2 - 30 : 0,
		    ctx->c->tty.sy > 10 ? ctx->c->tty.sy / 3 : 0, ctx->c,
		    BOX_LINES_DEFAULT, NULL, NULL, NULL, &fs, NULL, NULL) != 0)
			menu_free(menu);
	} else if (found == 1) {
		bus_snapshot_get(ctx->node, ctx->session, restore_get_cb, ctx);
		return;		/* still waiting */
	} else if (found == 0) {
		cmdq_error(ctx->item, "restore-session: %s n'est pas dans le "
		    "catalogue pour %s", ctx->session, ctx->node);
	}
	cmdq_continue(ctx->item);
	restore_ctx_free(ctx);
}

static enum cmd_retval
cmd_restore_session_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct restore_ctx	*ctx;

	ctx = xcalloc(1, sizeof *ctx);
	ctx->item = item;
	ctx->c = cmdq_get_client(item);
	ctx->list = args_has(args, 'l');
	ctx->menu = args_has(args, 'm');
	ctx->force = args_has(args, 'f');
	if (args_has(args, 'n'))
		ctx->node = xstrdup(args_get(args, 'n'));
	if (args_has(args, 's'))
		ctx->session = xstrdup(args_get(args, 's'));
	if (!ctx->list && !ctx->menu &&
	    (ctx->node == NULL || ctx->session == NULL)) {
		restore_ctx_free(ctx);
		cmdq_error(item, "restore-session: -l, -m, or -n node -s session");
		return (CMD_RETURN_ERROR);
	}
	bus_snapshot_list(restore_list_cb, ctx);
	return (CMD_RETURN_WAIT);
}

/* Copy the running binary aside, so a failed upgrade has something to go to. */
static int
upgrade_copy_self(char *out, size_t len)
{
	char	buf[65536];
	int	in = -1, dst = -1;
	ssize_t	n;

	if (server_binary_path == NULL)
		return (-1);
	xsnprintf(out, len, "%s/tmuxv-fallback-%ld", _PATH_TMP,
	    (long)getpid());
	if ((in = open(server_binary_path, O_RDONLY)) == -1)
		return (-1);
	dst = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0700);
	if (dst == -1) {
		close(in);
		return (-1);
	}
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (write(dst, buf, n) != n) {
			close(in);
			close(dst);
			unlink(out);
			return (-1);
		}
	}
	close(in);
	close(dst);
	if (n < 0) {
		unlink(out);
		return (-1);
	}
	return (0);
}

/*
 * Go back to the previous binary. Called from the crash handler as well, so
 * nothing here may allocate: execv() itself is async-signal-safe.
 */
void
upgrade_fallback(void)
{
	char	*argv[8];
	u_int	 i = 0;

	if (server_upgrade_fallback == NULL || *upgrade_state_path == '\0')
		return;
	argv[i++] = (char *)server_upgrade_fallback;
	argv[i++] = (char *)"-R";
	argv[i++] = upgrade_state_path;
	/* No -F: the fallback must not fall back again, or it would loop. */
	if (socket_path != NULL) {
		argv[i++] = (char *)"-S";
		argv[i++] = (char *)socket_path;
	}
	argv[i] = NULL;
	execv(server_upgrade_fallback, argv);
}

static void
upgrade_guard_handler(int sig)
{
	upgrade_fallback();		/* returns only if it failed */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* Arm (or disarm) the net that catches a new binary dying while rebuilding. */
static void
upgrade_guard(int on)
{
	static const int	sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL,
				    SIGABRT };
	u_int			i;

	for (i = 0; i < nitems(sigs); i++)
		signal(sigs[i], on ? upgrade_guard_handler : SIG_DFL);
	/*
	 * Disarmed: back to the crash log, not to the default action - with
	 * SIG_DFL, every crash after a hot upgrade died without a trace.
	 */
	if (!on)
		server_set_crash_handler();
}

/*
 * Save, then become the new binary. On success this never returns; on failure
 * the old server simply carries on, which is the whole point of doing the
 * exec last.
 */
int
upgrade_exec(const char *binary, char **cause)
{
	char		 path[PATH_MAX], fallback[PATH_MAX];
	char		**argv;
	u_int		 i = 0, k;
	struct stat	 sb;

	if (binary == NULL || *binary == '\0') {
		xasprintf(cause, "no binary to upgrade to");
		return (-1);
	}
	if (stat(binary, &sb) != 0 || !S_ISREG(sb.st_mode) ||
	    access(binary, X_OK) != 0) {
		xasprintf(cause, "%s: not an executable file", binary);
		return (-1);
	}

	xsnprintf(path, sizeof path, "%s/tmuxv-upgrade-%ld.state",
	    _PATH_TMP, (long)getpid());
	if (upgrade_save(path, cause) != 0)
		return (-1);

	upgrade_keep_all();
	upgrade_flush();
	log_debug("%s: exec %s with state %s", __func__, binary, path);

	if (upgrade_copy_self(fallback, sizeof fallback) != 0)
		*fallback = '\0';	/* no net, but the upgrade may still work */

	argv = xcalloc(12 + 2 * cfg_nfiles, sizeof *argv);
	argv[i++] = (char *)binary;
	/*
	 * The same configuration files as the server being replaced, when it
	 * was given some (-f): the new process reads them again. Without -f
	 * it finds the default ones by itself, the same way.
	 */
	if (!cfg_quiet) {
		for (k = 0; k < cfg_nfiles; k++) {
			argv[i++] = (char *)"-f";
			argv[i++] = cfg_files[k];
		}
	}
	if (getenv("TMUXV_UPGRADE_DEBUG") != NULL)
		argv[i++] = (char *)"-vv";	/* journal du nouveau serveur */
	argv[i++] = (char *)"-R";
	argv[i++] = path;
	if (*fallback != '\0') {
		argv[i++] = (char *)"-F";
		argv[i++] = fallback;
	}
	if (socket_path != NULL) {
		argv[i++] = (char *)"-S";
		argv[i++] = (char *)socket_path;
	}
	argv[i] = NULL;

	execv(binary, argv);

	/* Still here: nothing was lost, tell the user why. */
	xasprintf(cause, "%s: exec failed: %s", binary, strerror(errno));
	free(argv);
	unlink(path);
	return (-1);
}

/* Rebuild one window and its panes around the inherited descriptors. */
struct upgrade_win {
	u_int			 old_id;
	struct window		*w;
	int			 zoomed;
	char			*layout;
	struct window_pane	*active;
	struct window_pane	*last;		/* insertion point */
	RB_ENTRY(upgrade_win)	 entry;
};
static int
upgrade_win_cmp(struct upgrade_win *a, struct upgrade_win *b)
{
	if (a->old_id < b->old_id)
		return (-1);
	if (a->old_id > b->old_id)
		return (1);
	return (0);
}
RB_HEAD(upgrade_wins, upgrade_win);
RB_GENERATE_STATIC(upgrade_wins, upgrade_win, entry, upgrade_win_cmp);

struct upgrade_sess {
	u_int			 old_id;
	struct session		*s;
	RB_ENTRY(upgrade_sess)	 entry;
};
static int
upgrade_sess_cmp(struct upgrade_sess *a, struct upgrade_sess *b)
{
	if (a->old_id < b->old_id)
		return (-1);
	if (a->old_id > b->old_id)
		return (1);
	return (0);
}
RB_HEAD(upgrade_sesss, upgrade_sess);
RB_GENERATE_STATIC(upgrade_sesss, upgrade_sess, entry, upgrade_sess_cmp);

static struct upgrade_win *
upgrade_find_win(struct upgrade_wins *wins, const char *s)
{
	struct upgrade_win	find;

	if (*s == '@')
		s++;
	find.old_id = (u_int)strtoul(s, NULL, 10);
	return (RB_FIND(upgrade_wins, wins, &find));
}

static struct event	upgrade_repaint_ev;

/* One row less on every pane's terminal: half of the repaint nudge. */
static void
upgrade_shrink_panes(void)
{
	struct window		*w;
	struct window_pane	*wp;
	struct winsize		 ws;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->fd == -1)
				continue;
			if (ioctl(wp->fd, TIOCGWINSZ, &ws) == -1)
				continue;
			if (ws.ws_row < 2)
				continue;
			ws.ws_row--;
			(void)ioctl(wp->fd, TIOCSWINSZ, &ws);
		}
	}
}

/* ... and the real size back, which is the change the program acts on. */
static void
upgrade_repaint_done(__unused int fd, __unused short events,
    __unused void *arg)
{
	struct window		*w;
	struct window_pane	*wp;
	struct winsize		 ws;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->fd == -1)
				continue;
			memset(&ws, 0, sizeof ws);
			ws.ws_col = wp->sx;
			ws.ws_row = wp->sy;
			(void)ioctl(wp->fd, TIOCSWINSZ, &ws);
			if (wp->pid > 0)
				killpg(wp->pid, SIGWINCH);
		}
	}
}

static struct upgrade_sess *
upgrade_find_sess(struct upgrade_sesss *sesss, const char *s)
{
	struct upgrade_sess	find;

	if (*s == '$')
		s++;
	find.old_id = (u_int)strtoul(s, NULL, 10);
	return (RB_FIND(upgrade_sesss, sesss, &find));
}

void
upgrade_load(const char *path)
{
	FILE			*f;
	char			 line[8192], *fields[20], *cause = NULL;
	u_int			 n;
	struct upgrade_wins	 wins;
	struct upgrade_sesss	 sesss;
	struct upgrade_win	*uw, *uw1;
	struct upgrade_sess	*us, *us1;
	struct window_pane	*wp;
	struct winlink		*wl;
	struct client		*c;

	RB_INIT(&wins);
	RB_INIT(&sesss);

	/*
	 * From here to the end we are rebuilding the whole server around
	 * inherited descriptors. If the new code dies doing it, the previous
	 * binary takes over with the same state file - the sessions survive a
	 * bad version.
	 */
	strlcpy(upgrade_state_path, path, sizeof upgrade_state_path);
	if (server_upgrade_fallback != NULL)
		upgrade_guard(1);

	/* A deliberate crash, to test that net (tests/upgradefail.sh). */
	if (getenv("TMUXV_UPGRADE_CRASH") != NULL)
		raise(SIGSEGV);

	if ((f = fopen(path, "r")) == NULL) {
		log_debug("%s: %s: %s", __func__, path, strerror(errno));
		return;
	}
	if (fgets(line, sizeof line, f) == NULL ||
	    strncmp(line, "tmuxv-upgrade\t", 14) != 0 ||
	    atoi(line + 14) != UPGRADE_VERSION) {
		log_debug("%s: %s: not a state file of this version", __func__,
		    path);
		fclose(f);
		return;
	}

	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		n = upgrade_split(line, fields, nitems(fields));
		if (n == 0)
			continue;
		if (strcmp(fields[0], "option") == 0 && n >= 4) {
			struct upgrade_opt	*uo = xcalloc(1, sizeof *uo);

			uo->scope = *fields[1];
			uo->name = xstrdup(fields[2]);
			if (strcmp(fields[3], "clear") == 0)
				uo->idx = -2;
			else
				uo->idx = atoi(fields[3]);
			uo->value = upgrade_hex_decode(n >= 5 ? fields[4] : "");
			TAILQ_INSERT_TAIL(&upgrade_opts, uo, entry);
			continue;
		}

		if (strcmp(fields[0], "window") == 0 && n >= 16) {
			struct window	*w;

			w = window_create((u_int)atoi(fields[2]),
			    (u_int)atoi(fields[3]), (u_int)atoi(fields[4]),
			    (u_int)atoi(fields[5]));
			free(w->name);
			w->name = xstrdup(fields[6]);
			options_set_number(w->options, "automatic-rename", 0);
			w->desktop_x = (u_int)atoi(fields[9]);
			w->desktop_y = (u_int)atoi(fields[10]);
			w->desktop_w = (u_int)atoi(fields[11]);
			w->desktop_h = (u_int)atoi(fields[12]);
			w->desktop_zoomed = atoi(fields[13]);
			w->claude_mgr = atoi(fields[14]);
			w->claude_listw = (u_int)atoi(fields[15]);

			uw = xcalloc(1, sizeof *uw);
			uw->old_id = (u_int)strtoul(fields[1] + 1, NULL, 10);
			uw->w = w;
			uw->layout = xstrdup(fields[7]);
			uw->zoomed = atoi(fields[8]);
			RB_INSERT(upgrade_wins, &wins, uw);
			continue;
		}

		if (strcmp(fields[0], "pane") == 0 && n >= 7) {
			if ((uw = upgrade_find_win(&wins, fields[1])) == NULL)
				continue;
			/*
			 * Insert AFTER the previous pane: passing NULL makes
			 * window_add_pane() fall back on w->active, which is
			 * still NULL here - and it dereferences it.
			 */
			/*
			 * history-limit is a SESSION option and no session
			 * exists yet at this point of the replay: take the
			 * global one, which is what a new pane would get.
			 */
			wp = window_add_pane(uw->w, uw->last,
			    options_get_number(global_s_options,
			    "history-limit"), 0);
			if (uw->last == NULL) {
				/*
				 * layout_parse() frees the window's current
				 * layout first, and layout_free_cell() does
				 * not expect NULL: give the window a real one
				 * as soon as it has a pane.
				 */
				layout_init(uw->w, wp);
				window_set_active_pane(uw->w, wp, 0);
			}
			uw->last = wp;
			/*
			 * THE point of the whole exercise: the pane keeps the
			 * pty it already had, so its program never noticed.
			 */
			wp->fd = atoi(fields[2]);
			wp->pid = (pid_t)atol(fields[3]);
			/*
			 * #{pane_current_command} gives up when the pane has no
			 * shell recorded - it is only the LAST resort, the real
			 * name still comes from the process itself, but without
			 * it the status line and automatic-rename come back
			 * empty after an upgrade.
			 */
			if (wp->shell == NULL) {
				wp->shell = xstrdup(options_get_string(
				    global_s_options, "default-shell"));
			}
			if (*fields[5] != '\0')
				strlcpy(wp->tty, fields[5], sizeof wp->tty);
			free(wp->cwd);
			wp->cwd = xstrdup(fields[6]);
			if (n > 7 && *fields[7] != '\0')
				screen_set_title(&wp->base, fields[7]);
			if (n > 8 && *fields[8] != '\0') {
				free(wp->claude_agent);
				wp->claude_agent = xstrdup(fields[8]);
			}
			if (wp->fd != -1)
				window_pane_set_event(wp);
			if (atoi(fields[4]))
				uw->active = wp;
			continue;
		}

		if (strcmp(fields[0], "session") == 0 && n >= 4) {
			struct session	*s;
			struct options	*oo;

			oo = options_create(global_s_options);
			s = session_create(NULL, fields[2], fields[3],
			    environ_create(), oo, NULL);
			us = xcalloc(1, sizeof *us);
			us->old_id = (u_int)strtoul(fields[1] + 1, NULL, 10);
			us->s = s;
			RB_INSERT(upgrade_sesss, &sesss, us);
			continue;
		}

		if (strcmp(fields[0], "link") == 0 && n >= 5) {
			if ((us = upgrade_find_sess(&sesss, fields[1])) == NULL)
				continue;
			if ((uw = upgrade_find_win(&wins, fields[3])) == NULL)
				continue;
			wl = session_attach(us->s, uw->w, atoi(fields[2]),
			    &cause);
			if (wl == NULL) {
				log_debug("%s: attach failed: %s", __func__,
				    cause != NULL ? cause : "?");
				free(cause);
				cause = NULL;
				continue;
			}
			if (atoi(fields[4]))
				session_set_current(us->s, wl);
			continue;
		}

		if (strcmp(fields[0], "client") == 0 && n >= 7) {
			int	peerfd = atoi(fields[1]);
			int	ttyfd = atoi(fields[2]);

			if (peerfd < 0)
				continue;
			c = server_client_create(peerfd);
			c->fd = ttyfd;
			c->ttyname = xstrdup(fields[4]);
			c->term_name = xstrdup(fields[5]);
			c->name = xstrdup(fields[4]);
			c->flags |= CLIENT_IDENTIFIED;
			if ((us = upgrade_find_sess(&sesss, fields[3])) != NULL)
				c->session = us->s;
			/*
			 * A rebuilt client never sends MSG_IDENTIFY_CWD. It is
			 * the configuration client when the configuration is
			 * read again below, and a `run` there (tpm...) took its
			 * NULL cwd and crashed the server.
			 */
			if (c->cwd == NULL) {
				const char	*home = find_home();

				if (c->session != NULL && c->session->cwd != NULL)
					c->cwd = xstrdup(c->session->cwd);
				else
					c->cwd = xstrdup(home != NULL ? home : "/");
			}
			if (n > 11) {
				c->flags |= (strtoull(fields[11], NULL, 10) &
				    UPGRADE_CLIENT_FLAGS);
			}
			if (n > 8)
				c->term_features = atoi(fields[8]);
			if (n > 9 && *fields[9] != '\0')
				c->term_type = xstrdup(fields[9]);
			if (n > 10 && *fields[10] != '\0') {
				char	*cp = fields[10], *end;

				/* Rebuild the capability array, \001 separated. */
				for (;;) {
					end = strchr(cp, '\001');
					if (end != NULL)
						*end = '\0';
					if (*cp != '\0') {
						c->term_caps = xreallocarray(
						    c->term_caps,
						    c->term_ncaps + 1,
						    sizeof *c->term_caps);
						c->term_caps[c->term_ncaps++] =
						    xstrdup(cp);
					}
					if (end == NULL)
						break;
					cp = end + 1;
				}
			}
			if (ttyfd != -1 && tty_init(&c->tty, c) == 0) {
				c->tty.sx = (u_int)atoi(fields[6]);
				if (n > 7)
					c->tty.sy = (u_int)atoi(fields[7]);
				c->flags |= CLIENT_TERMINAL;
				if (tty_open(&c->tty, &cause) != 0) {
					log_debug("%s: tty_open: %s", __func__,
					    cause != NULL ? cause : "?");
					free(cause);
					cause = NULL;
				}
			}
			continue;
		}
	}
	fclose(f);

	/* Layouts last: every pane of a window must exist first. */
	RB_FOREACH_SAFE(uw, upgrade_wins, &wins, uw1) {
		if (layout_parse(uw->w, uw->layout, &cause) != 0) {
			log_debug("%s: layout: %s", __func__,
			    cause != NULL ? cause : "?");
			free(cause);
			cause = NULL;
		}
		if (uw->active != NULL)
			window_set_active_pane(uw->w, uw->active, 0);
		if (uw->zoomed && uw->w->active != NULL)
			window_zoom(uw->w->active);
		free(uw->layout);
		RB_REMOVE(upgrade_wins, &wins, uw);
		free(uw);
	}
	RB_FOREACH_SAFE(us, upgrade_sesss, &sesss, us1) {
		RB_REMOVE(upgrade_sesss, &sesss, us);
		free(us);
	}

	recalculate_sizes();

	/*
	 * The grids did not travel, so what a pane shows is whatever its
	 * program last painted - which the new server never saw. Full-screen
	 * programs (Claude Code, vim, less) redraw on a size change, so give
	 * every one of them a resize round trip; a shell just gets its prompt
	 * back on the next key.
	 */
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL && (c->flags & CLIENT_TERMINAL)) {
			tty_resize(&c->tty);
			server_redraw_client(c);
		}
	}
	/*
	 * Make the full-screen programs repaint. The new server inherits the
	 * ptys but NOT what was drawn on them, so every TUI (Claude Code, an
	 * editor, top...) would sit on a BLANK pane until it printed something
	 * of its own - which an idle agent never does. A bare SIGWINCH is not
	 * enough either: a program that compares the size it reads with the one
	 * it had ignores it (checked). So the terminal is made one row shorter
	 * now and given its real size back a moment later - a size that really
	 * CHANGED, twice, is what forces a redraw.
	 */
	upgrade_shrink_panes();
	{
		struct timeval	tv = { 0, 150000 };

		evtimer_set(&upgrade_repaint_ev, upgrade_repaint_done, NULL);
		evtimer_add(&upgrade_repaint_ev, &tv);
	}
	/*
	 * The mail watcher is armed when a manager window is CREATED - a window
	 * rebuilt here never went through that command, so without this the
	 * conversations would come back with nobody polling the bus, and no
	 * message would ever be delivered again.
	 */
	claude_bus_start();
	/*
	 * Read the configuration again, as a fresh start does. It is only ever
	 * loaded when the FIRST client identifies itself - and the clients
	 * rebuilt here never go through that - so without this every hot
	 * upgrade silently dropped the user's settings (prefix, mouse, theme,
	 * plugins, @options), leaving tmuxv's defaults.
	 */
	start_cfg();
	cmdq_append(NULL, cmdq_get_callback(upgrade_options_apply, NULL));

	/* Rebuilt: the net comes down and the state file can go. */
	upgrade_guard(0);
	if (server_upgrade_fallback != NULL) {
		unlink(server_upgrade_fallback);
		server_upgrade_fallback = NULL;
	}
	unlink(path);
	*upgrade_state_path = '\0';
	log_debug("%s: state restored", __func__);
}

/*
 * "upgrade-server [path]" - load a new binary in place. Never automatic: the
 * user picks the moment, because a botched exec would take the sessions with
 * it (and a good one still loses what the panes had on screen).
 */
static enum cmd_retval
cmd_upgrade_server_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	const char	*binary = args_string(args, 0);
	char		*cause = NULL;

	if (binary == NULL || *binary == '\0')
		binary = server_binary_path;
	if (binary == NULL) {
		cmdq_error(item, "je ne sais pas de quel binaire je viens : "
		    "donnez-en le chemin");
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Everything the clients are owed must be on the wire BEFORE the exec:
	 * the new binary inherits the sockets, not our pending buffers.
	 */
	if (upgrade_exec(binary, &cause) != 0) {
		cmdq_error(item, "%s", cause != NULL ? cause : "echec");
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);	/* not reached: execv() succeeded */
}

const struct cmd_entry cmd_upgrade_server_entry = {
	.name = "upgrade-server",
	.alias = NULL,

	.args = { "", 0, 1, NULL },
	.usage = "[binary]",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_upgrade_server_exec
};
