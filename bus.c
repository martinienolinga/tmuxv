/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Martinien Olinga <martinien.olinga@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * THE AGENT BUS, INSIDE THE SERVER.
 *
 * The Claude conversations of the manager talk to each other through a small
 * message bus. It used to be a separate program (a systemd service); it now
 * lives here, in the tmuxv server, and speaks exactly the same HTTP API - so
 * the MCP facade the agents use, and any agent outside tmuxv (another machine,
 * another OS), sees no difference.
 *
 *  - HTTP: evhttp, on the server's own event loop, on @bus-port (4319 by
 *    default) or the first free port after it - several tmuxv servers on one
 *    machine each get their own. Changing @bus-port rebinds on the fly.
 *
 *  - Storage: MariaDB when @bus-db names one, shared by every tmuxv that
 *    points at it - that is how several servers, and agents anywhere, reach
 *    each other. The server NEVER runs a query itself: every database call is
 *    made by one dedicated thread, and answered back through a pipe, so a slow
 *    or dead database can never freeze a single keystroke.
 *
 *  - Without a database (none configured, or it went away), a store in memory
 *    takes over: the conversations of this server keep talking to each other.
 *    When the database comes back, what was written meanwhile is poured into
 *    it and delivery resynchronises - nothing is lost, nothing delivered
 *    twice.
 *
 * The rules (delivery, broadcasts, renames, first fetch) are those of the
 * former service, kept identical on purpose; its tests still apply.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <mysql.h>
#include <errmsg.h>
#undef PROTOCOL_VERSION		/* MariaDB's; tmux has its own */

#include "tmux.h"

#define BUS_VERSION	"tmuxv-bus-1"
#define BUS_MAX_WAIT	60	/* seconds a long-poll may be held */
#define BUS_POLL_MS	300	/* long-poll re-check period */
#define BUS_GHOST	3600	/* unseen for longer: left out of /agents */
#define BUS_PORT_SPAN	32	/* ports tried after @bus-port */
#define BUS_PING_SECS	10	/* seconds between database probes when down */
#define BUS_BODY_MAX	(4 * 1024 * 1024)
#define BUS_PIECE_MAX	(6 * 1024 * 1024)	/* one joined file */
#define BUS_PIECE_BODY	(BUS_PIECE_MAX / 3 * 4 + 64 * 1024)
#define BUS_PIECE_DAYS	30	/* older pieces are dropped */

/* ------------------------------------------------------------------------ */
/* Growable string. The worker thread uses it too: no libevent in there.    */

struct sbuf {
	char	*d;
	size_t	 n;
	size_t	 cap;
};

static void
sb_grow(struct sbuf *b, size_t add)
{
	if (b->n + add + 1 <= b->cap)
		return;
	while (b->n + add + 1 > b->cap)
		b->cap = (b->cap == 0) ? 256 : b->cap * 2;
	b->d = xrealloc(b->d, b->cap);
}

static void
sb_add(struct sbuf *b, const char *s, size_t len)
{
	sb_grow(b, len);
	memcpy(b->d + b->n, s, len);
	b->n += len;
	b->d[b->n] = '\0';
}

static void
sb_str(struct sbuf *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

static void printflike(2, 3)
sb_printf(struct sbuf *b, const char *fmt, ...)
{
	va_list	 ap;
	int	 len;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (len <= 0)
		return;
	sb_grow(b, (size_t)len);
	va_start(ap, fmt);
	vsnprintf(b->d + b->n, (size_t)len + 1, fmt, ap);
	va_end(ap);
	b->n += (size_t)len;
}

/* A JSON string literal (or null). UTF-8 passes through untouched. */
static void
sb_jstr(struct sbuf *b, const char *s)
{
	const u_char	*p;
	char		 esc[8];

	if (s == NULL) {
		sb_str(b, "null");
		return;
	}
	sb_add(b, "\"", 1);
	for (p = (const u_char *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':  sb_add(b, "\\\"", 2); break;
		case '\\': sb_add(b, "\\\\", 2); break;
		case '\n': sb_add(b, "\\n", 2); break;
		case '\r': sb_add(b, "\\r", 2); break;
		case '\t': sb_add(b, "\\t", 2); break;
		case '\b': sb_add(b, "\\b", 2); break;
		case '\f': sb_add(b, "\\f", 2); break;
		default:
			if (*p < 0x20) {
				snprintf(esc, sizeof esc, "\\u%04x", *p);
				sb_str(b, esc);
			} else
				sb_add(b, (const char *)p, 1);
		}
	}
	sb_add(b, "\"", 1);
}

static void
sb_free(struct sbuf *b)
{
	free(b->d);
	memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------------------ */
/* Minimal JSON reader: the request bodies are flat objects of strings,     */
/* numbers and booleans. Anything nested is refused, not guessed at.        */

#define JMAX 16
struct jfield {
	char		*key;
	int		 type;		/* 's', 'n', 'b', 'z' (null) */
	char		*s;
	long long	 n;
	int		 b;
};
struct jobj {
	struct jfield	 f[JMAX];
	int		 nf;
};

static const char *
j_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return (p);
}

static void
j_utf8(struct sbuf *b, u_int cp)
{
	char	o[4];

	if (cp < 0x80) {
		o[0] = (char)cp;
		sb_add(b, o, 1);
	} else if (cp < 0x800) {
		o[0] = (char)(0xc0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3f));
		sb_add(b, o, 2);
	} else if (cp < 0x10000) {
		o[0] = (char)(0xe0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		o[2] = (char)(0x80 | (cp & 0x3f));
		sb_add(b, o, 3);
	} else {
		o[0] = (char)(0xf0 | (cp >> 18));
		o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		o[3] = (char)(0x80 | (cp & 0x3f));
		sb_add(b, o, 4);
	}
}

static int
j_hex4(const char *p, u_int *out)
{
	u_int	v = 0;
	int	i;

	for (i = 0; i < 4; i++) {
		v <<= 4;
		if (p[i] >= '0' && p[i] <= '9')
			v |= (u_int)(p[i] - '0');
		else if (p[i] >= 'a' && p[i] <= 'f')
			v |= (u_int)(p[i] - 'a' + 10);
		else if (p[i] >= 'A' && p[i] <= 'F')
			v |= (u_int)(p[i] - 'A' + 10);
		else
			return (-1);
	}
	*out = v;
	return (0);
}

/* Parse a string at p (on the opening quote); returns the char after it. */
static const char *
j_string(const char *p, char **out)
{
	struct sbuf	b = { 0 };
	u_int		cp, lo;

	if (*p != '"')
		return (NULL);
	p++;
	sb_add(&b, "", 0);
	while (*p != '"') {
		if (*p == '\0')
			goto bad;
		if (*p != '\\') {
			sb_add(&b, p, 1);
			p++;
			continue;
		}
		p++;
		switch (*p) {
		case '"':  sb_add(&b, "\"", 1); break;
		case '\\': sb_add(&b, "\\", 1); break;
		case '/':  sb_add(&b, "/", 1); break;
		case 'n':  sb_add(&b, "\n", 1); break;
		case 'r':  sb_add(&b, "\r", 1); break;
		case 't':  sb_add(&b, "\t", 1); break;
		case 'b':  sb_add(&b, "\b", 1); break;
		case 'f':  sb_add(&b, "\f", 1); break;
		case 'u':
			if (j_hex4(p + 1, &cp) != 0)
				goto bad;
			p += 4;
			if (cp >= 0xd800 && cp <= 0xdbff && p[1] == '\\' &&
			    p[2] == 'u' && j_hex4(p + 3, &lo) == 0 &&
			    lo >= 0xdc00 && lo <= 0xdfff) {
				cp = 0x10000 + ((cp - 0xd800) << 10) +
				    (lo - 0xdc00);
				p += 6;
			}
			j_utf8(&b, cp);
			break;
		default:
			goto bad;
		}
		p++;
	}
	*out = b.d;
	return (p + 1);
bad:
	sb_free(&b);
	return (NULL);
}

static void
j_free(struct jobj *o)
{
	int	i;

	for (i = 0; i < o->nf; i++) {
		free(o->f[i].key);
		free(o->f[i].s);
	}
	o->nf = 0;
}

static int
j_parse(const char *p, struct jobj *o)
{
	struct jfield	*f;
	char		*end;

	memset(o, 0, sizeof *o);
	if (p == NULL)
		return (-1);
	p = j_ws(p);
	if (*p++ != '{')
		return (-1);
	p = j_ws(p);
	if (*p == '}')
		return (0);
	for (;;) {
		if (o->nf == JMAX)
			goto bad;
		f = &o->f[o->nf];
		memset(f, 0, sizeof *f);
		if ((p = j_string(j_ws(p), &f->key)) == NULL)
			goto bad;
		o->nf++;
		p = j_ws(p);
		if (*p++ != ':')
			goto bad;
		p = j_ws(p);
		if (*p == '"') {
			f->type = 's';
			if ((p = j_string(p, &f->s)) == NULL)
				goto bad;
		} else if (strncmp(p, "true", 4) == 0) {
			f->type = 'b'; f->b = 1; p += 4;
		} else if (strncmp(p, "false", 5) == 0) {
			f->type = 'b'; f->b = 0; p += 5;
		} else if (strncmp(p, "null", 4) == 0) {
			f->type = 'z'; p += 4;
		} else if (*p == '-' || isdigit((u_char)*p)) {
			f->type = 'n';
			f->n = strtoll(p, &end, 10);
			p = end;
			while (*p == '.' || *p == 'e' || *p == 'E' ||
			    *p == '+' || *p == '-' || isdigit((u_char)*p))
				p++;
		} else
			goto bad;
		p = j_ws(p);
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}')
			return (0);
		goto bad;
	}
bad:
	j_free(o);
	return (-1);
}

static struct jfield *
j_get(struct jobj *o, const char *key)
{
	int	i;

	for (i = 0; i < o->nf; i++) {
		if (strcmp(o->f[i].key, key) == 0)
			return (&o->f[i]);
	}
	return (NULL);
}

/* A trimmed copy of a string field, or NULL if absent / not a string. */
static char *
j_str(struct jobj *o, const char *key)
{
	struct jfield	*f = j_get(o, key);
	const char	*s, *e;

	if (f == NULL || f->type != 's')
		return (NULL);
	s = f->s;
	while (*s != '\0' && isspace((u_char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((u_char)e[-1]))
		e--;
	return (xstrndup(s, (size_t)(e - s)));
}

static char *
trim_dup(const char *s)
{
	const char	*e;

	if (s == NULL)
		return (xstrdup(""));
	while (*s != '\0' && isspace((u_char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((u_char)e[-1]))
		e--;
	return (xstrndup(s, (size_t)(e - s)));
}

/* ------------------------------------------------------------------------ */
/* Shared rules.                                                            */

/*
 * Broadcast has one canonical address, "*" (the sender's own project), and
 * "**" for the whole bus. Fold the spellings agents naturally reach for.
 */
static char *
bus_normalize(const char *to)
{
	char	*t = trim_dup(to), *l;
	size_t	 i;

	l = xstrdup(t);
	for (i = 0; l[i] != '\0'; i++)
		l[i] = (char)tolower((u_char)l[i]);
	if (strcmp(l, "**") == 0 || strcmp(l, "@everywhere") == 0 ||
	    strcmp(l, "everywhere") == 0) {
		free(t); free(l);
		return (xstrdup("**"));
	}
	if (strcmp(l, "*") == 0 || strcmp(l, "@all") == 0 ||
	    strcmp(l, "all") == 0 || strcmp(l, "@everyone") == 0 ||
	    strcmp(l, "everyone") == 0 || strcmp(l, "#all") == 0 ||
	    strcmp(l, "#everyone") == 0) {
		free(t); free(l);
		return (xstrdup("*"));
	}
	free(l);
	return (t);
}

/* Channels always carry their leading '#'. */
static char *
bus_chan(const char *name)
{
	char	*t = trim_dup(name), *c;

	if (*t == '#')
		return (t);
	xasprintf(&c, "#%s", t);
	free(t);
	return (c);
}

static int
bus_special(const char *to)
{
	return (strcmp(to, "*") == 0 || strcmp(to, "**") == 0 || *to == '#');
}

/* ------------------------------------------------------------------------ */
/* The jobs: one request, whatever store ends up answering it.              */

enum bus_op {
	BUS_HEALTH,
	BUS_AGENTS,
	BUS_REGISTER,
	BUS_RENAME,
	BUS_SEND,
	BUS_ACK,
	BUS_JOIN,
	BUS_LEAVE,
	BUS_CHANNELS,
	BUS_INBOX,
	BUS_PING,
	BUS_SYNC,
	BUS_SNAP_PUT,	/* catalog: one session's cold state */
	BUS_SNAP_PRUNE,	/* catalog: forget sessions closed since */
	BUS_SNAP_LIST,	/* catalog: what can be restored */
	BUS_SNAP_GET,	/* catalog: one session's cold state */
	BUS_PIECE_PUT,	/* a joined file, into the database */
	BUS_PIECE_GET	/* a joined file, from the database */
};

struct bus_sync_msg {
	long long	 ts;
	char		*sender;
	char		*recipient;
	char		*subject;
	char		*body;
	int		 read;
};
struct bus_sync_agent {
	char		*name;
	long long	 last_seen;
	char		*grp;
	int		 managed;
};
struct bus_sync_pair {
	char		*a;
	char		*b;
};

struct bus_job {
	enum bus_op		 op;

	/* Inputs. */
	char			*s1, *s2, *s3, *s4;
	long long		 n1, n2;
	int			 b1, has_b1, has_s2;
	u_char			*blob;		/* a piece's bytes */
	size_t			 bloblen;

	/* SYNC payload (copied: the worker never touches the memory store). */
	struct bus_sync_msg	*msgs;
	u_int			 nmsgs;
	struct bus_sync_agent	*agents;
	u_int			 nagents;
	struct bus_sync_pair	*members, *aliases;
	u_int			 nmembers, naliases;

	/* An internal caller's completion (the catalog), if any. */
	void			(*ucb)(const char *, const char *, void *);
	void			*uarg;

	/* The HTTP request it answers, if any. */
	struct evhttp_request	*req;
	int			 cancelled;
	time_t			 deadline;
	struct event		 retry;
	int			 retry_armed;

	/* Outputs. */
	int			 rc;		/* 0, BUS_DOWN, BUS_ERR */
	int			 code;
	struct sbuf		 out;
	int			 empty;		/* inbox found nothing */
	long long		 maxid;		/* highest message id seen */
	char			 err[256];

	TAILQ_ENTRY(bus_job)	 entry;
};
TAILQ_HEAD(bus_jobs, bus_job);

#define BUS_DOWN	-1	/* the database is unreachable: fall back */
#define BUS_ERR		-2	/* the query itself failed: report it */

static void	bus_dispatch(struct bus_job *);
static void	bus_reply(struct bus_job *);
static void	bus_run_mem(struct bus_job *);
static void	bus_mode_local(const char *);
static void	bus_piece_send(struct evhttp_request *, const char *);
static void	bus_error(struct evhttp_request *, int, const char *,
		    const char *);

/* ------------------------------------------------------------------------ */
/* PIECES: files joined to messages (screenshots, images, logs).            */

/*
 * A piece is kept on disk in a directory of its own, named by its id, under
 * its original name: the extension tells an agent's Read tool what it is, so
 * the agent it reaches is simply given the path. In database mode it goes
 * into the `pieces` table as well, where another tmuxv sharing the database
 * finds it on first request. Set once, before the worker starts.
 */
static char	bus_piece_root[PATH_MAX];

static int
bus_mkdirs(const char *path)
{
	char	tmp[PATH_MAX], *p;

	if (strlcpy(tmp, path, sizeof tmp) >= sizeof tmp)
		return (-1);
	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

static int
bus_piece_id_ok(const char *id)
{
	size_t	i;

	for (i = 0; id[i] != '\0'; i++) {
		if (!isxdigit((u_char)id[i]))
			return (0);
	}
	return (i >= 8 && i <= 40);
}

/* A file name safe on any system the piece may reach. */
static char *
bus_piece_name(const char *in)
{
	const char	*b;
	char		*out;
	size_t		 i, n = 0;

	if (in == NULL)
		in = "";
	if ((b = strrchr(in, '/')) != NULL)
		in = b + 1;
	if ((b = strrchr(in, '\\')) != NULL)
		in = b + 1;
	while (*in == '.')
		in++;
	out = xmalloc(strlen(in) + 8);
	for (i = 0; in[i] != '\0' && n < 120; i++) {
		if (isalnum((u_char)in[i]) || strchr("._-+", in[i]) != NULL)
			out[n++] = in[i];
		else
			out[n++] = '_';
	}
	out[n] = '\0';
	if (n == 0)
		strlcpy(out, "piece", 8);
	return (out);
}

static void
bus_piece_mime(const char *in, char *out, size_t len)
{
	size_t	i;

	strlcpy(out, "application/octet-stream", len);
	if (in == NULL || *in == '\0' || strlen(in) >= len ||
	    strchr(in, '/') == NULL)
		return;
	for (i = 0; in[i] != '\0'; i++) {
		if (!isalnum((u_char)in[i]) && strchr("./+-", in[i]) == NULL)
			return;
	}
	strlcpy(out, in, len);
}

static u_char *
bus_b64_decode(const char *in, size_t *outlen)
{
	static const char	 t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				     "abcdefghijklmnopqrstuvwxyz0123456789+/";
	const char		*k;
	u_char			*out;
	u_int			 v = 0;
	int			 bits = 0;
	size_t			 o = 0;

	out = xmalloc(strlen(in) / 4 * 3 + 4);
	for (; *in != '\0' && *in != '='; in++) {
		if (isspace((u_char)*in))
			continue;
		if ((k = strchr(t, *in)) == NULL) {
			free(out);
			return (NULL);
		}
		v = (v << 6) | (u_int)(k - t);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (u_char)((v >> bits) & 0xff);
		}
	}
	*outlen = o;
	return (out);
}

/* Store a piece (main thread or worker); its path, or NULL. */
static char *
bus_piece_write(const char *id, const char *name, const char *mime,
    const u_char *data, size_t len)
{
	char	 dir[PATH_MAX], tmp[PATH_MAX], *path, *clean;
	FILE	*f;

	if (*bus_piece_root == '\0' || !bus_piece_id_ok(id))
		return (NULL);
	snprintf(dir, sizeof dir, "%s/%s", bus_piece_root, id);
	if (bus_mkdirs(dir) != 0)
		return (NULL);
	snprintf(tmp, sizeof tmp, "%s/.mime", dir);
	if ((f = fopen(tmp, "w")) != NULL) {
		fputs(mime, f);
		fclose(f);
	}
	clean = bus_piece_name(name);
	xasprintf(&path, "%s/%s", dir, clean);
	free(clean);
	snprintf(tmp, sizeof tmp, "%s/.part", dir);
	if ((f = fopen(tmp, "w")) == NULL)
		goto fail;
	if (len != 0 && fwrite(data, 1, len, f) != len) {
		fclose(f);
		unlink(tmp);
		goto fail;
	}
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		goto fail;
	}
	return (path);
fail:
	free(path);
	return (NULL);
}

/* The file of a piece on this machine (malloc'd), or NULL. */
static char *
bus_piece_find(const char *id, char *mime, size_t mimelen)
{
	char		 dir[PATH_MAX], tmp[PATH_MAX], *path = NULL;
	DIR		*d;
	struct dirent	*de;
	struct stat	 sb;
	FILE		*f;

	if (*bus_piece_root == '\0' || !bus_piece_id_ok(id))
		return (NULL);
	snprintf(dir, sizeof dir, "%s/%s", bus_piece_root, id);
	if ((d = opendir(dir)) == NULL)
		return (NULL);
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		xasprintf(&path, "%s/%s", dir, de->d_name);
		if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
			break;
		free(path);
		path = NULL;
	}
	closedir(d);
	if (path != NULL && mime != NULL) {
		strlcpy(mime, "application/octet-stream", mimelen);
		snprintf(tmp, sizeof tmp, "%s/.mime", dir);
		if ((f = fopen(tmp, "r")) != NULL) {
			if (fgets(mime, (int)mimelen, f) == NULL)
				strlcpy(mime, "application/octet-stream", mimelen);
			mime[strcspn(mime, "\r\n")] = '\0';
			fclose(f);
		}
	}
	return (path);
}

/* For the delivery (menu.c): where a piece is on this machine, or NULL. */
const char *
bus_piece_local(const char *id)
{
	static char	 buf[PATH_MAX];
	char		*p;

	if ((p = bus_piece_find(id, NULL, 0)) == NULL)
		return (NULL);
	strlcpy(buf, p, sizeof buf);
	free(p);
	return (buf);
}

/* Where pieces live, and the old ones dropped. At startup, main thread. */
static void
bus_piece_init(void)
{
	const char	*home = find_home();
	char		 dir[PATH_MAX], f[PATH_MAX];
	DIR		*d, *d2;
	struct dirent	*de, *de2;
	struct stat	 sb;
	time_t		 old = time(NULL) - BUS_PIECE_DAYS * 86400;

	if (home == NULL)
		return;
	snprintf(bus_piece_root, sizeof bus_piece_root,
	    "%s/.local/share/tmuxv/pieces", home);
	if (bus_mkdirs(bus_piece_root) != 0) {
		*bus_piece_root = '\0';
		return;
	}
	if ((d = opendir(bus_piece_root)) == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		if (!bus_piece_id_ok(de->d_name))
			continue;
		snprintf(dir, sizeof dir, "%s/%s", bus_piece_root, de->d_name);
		if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode) ||
		    sb.st_mtime >= old)
			continue;
		if ((d2 = opendir(dir)) != NULL) {
			while ((de2 = readdir(d2)) != NULL) {
				if (strcmp(de2->d_name, ".") == 0 ||
				    strcmp(de2->d_name, "..") == 0)
					continue;
				snprintf(f, sizeof f, "%s/%s", dir, de2->d_name);
				unlink(f);
			}
			closedir(d2);
		}
		rmdir(dir);
	}
	closedir(d);
}

static void
bus_job_free(struct bus_job *j)
{
	u_int	i;

	if (j->retry_armed)
		evtimer_del(&j->retry);
	free(j->s1); free(j->s2); free(j->s3); free(j->s4);
	free(j->blob);
	for (i = 0; i < j->nmsgs; i++) {
		free(j->msgs[i].sender); free(j->msgs[i].recipient);
		free(j->msgs[i].subject); free(j->msgs[i].body);
	}
	free(j->msgs);
	for (i = 0; i < j->nagents; i++) {
		free(j->agents[i].name); free(j->agents[i].grp);
	}
	free(j->agents);
	for (i = 0; i < j->nmembers; i++) {
		free(j->members[i].a); free(j->members[i].b);
	}
	free(j->members);
	for (i = 0; i < j->naliases; i++) {
		free(j->aliases[i].a); free(j->aliases[i].b);
	}
	free(j->aliases);
	sb_free(&j->out);
	free(j);
}

static struct bus_job *
bus_job_new(enum bus_op op)
{
	struct bus_job	*j = xcalloc(1, sizeof *j);

	j->op = op;
	j->code = 200;
	return (j);
}

/* ------------------------------------------------------------------------ */
/* State.                                                                   */

static struct event_base	*bus_base;
static struct evhttp		*bus_http;
static struct evhttp_bound_socket *bus_sock;
static int			 bus_port_bound;	/* 0: not listening */
static int			 bus_port_asked;
static char			*bus_bind_addr;
static struct event		 bus_tick_ev;

/* Mode: the database answers (DB), or the memory store does (LOCAL). */
static int			 bus_db_mode;
static char			*bus_db_url;		/* configured, "" = none */
static time_t			 bus_last_ping;
static int			 bus_ping_out;		/* a probe is queued */
static int			 bus_sync_out;		/* a sync is queued */
static long long		 bus_last_db_id;	/* highest id seen there */
static char			 bus_mode_why[256] = "no database configured";

/* Worker thread. */
static pthread_t		 bus_thread;
static pthread_mutex_t		 bus_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t		 bus_cond = PTHREAD_COND_INITIALIZER;
static struct bus_jobs		 bus_todo = TAILQ_HEAD_INITIALIZER(bus_todo);
static struct bus_jobs		 bus_done = TAILQ_HEAD_INITIALIZER(bus_done);
static int			 bus_pipe[2] = { -1, -1 };
static struct event		 bus_pipe_ev;
static char			*bus_worker_url;	/* guarded by bus_lock */
static int			 bus_worker_reset;	/* guarded by bus_lock */
static int			 bus_started;

/* ------------------------------------------------------------------------ */
/* THE MEMORY STORE - answers when there is no database.                    */

struct bus_agent {
	char			*name;
	long long		 last_seen;
	char			*grp;
	int			 managed;
	RB_ENTRY(bus_agent)	 entry;
};
static int
bus_agent_cmp(struct bus_agent *a, struct bus_agent *b)
{
	return (strcmp(a->name, b->name));
}
RB_HEAD(bus_agents, bus_agent);
RB_GENERATE_STATIC(bus_agents, bus_agent, entry, bus_agent_cmp);
static struct bus_agents	mem_agents = RB_INITIALIZER(&mem_agents);

struct bus_msg {
	long long		 id;
	long long		 ts;
	char			*sender;
	char			*recipient;
	char			*subject;
	char			*body;
	int			 read;
	TAILQ_ENTRY(bus_msg)	 entry;
};
TAILQ_HEAD(bus_msgs, bus_msg);
static struct bus_msgs		mem_msgs = TAILQ_HEAD_INITIALIZER(mem_msgs);
static long long		mem_next_id = 1;
static u_int			mem_nmsgs;

struct bus_pair {
	char			*a;	/* channel / old name */
	char			*b;	/* agent / new name */
	TAILQ_ENTRY(bus_pair)	 entry;
};
TAILQ_HEAD(bus_pairs, bus_pair);
static struct bus_pairs		mem_members = TAILQ_HEAD_INITIALIZER(mem_members);
static struct bus_pairs		mem_aliases = TAILQ_HEAD_INITIALIZER(mem_aliases);

static struct bus_agent *
mem_find(const char *name)
{
	struct bus_agent	find;

	find.name = (char *)name;
	return (RB_FIND(bus_agents, &mem_agents, &find));
}

static struct bus_agent *
mem_touch(const char *name)
{
	struct bus_agent	*a;

	if ((a = mem_find(name)) == NULL) {
		a = xcalloc(1, sizeof *a);
		a->name = xstrdup(name);
		a->grp = xstrdup("");
		RB_INSERT(bus_agents, &mem_agents, a);
	}
	a->last_seen = (long long)time(NULL);
	return (a);
}

static void
mem_agent_remove(struct bus_agent *a)
{
	RB_REMOVE(bus_agents, &mem_agents, a);
	free(a->name);
	free(a->grp);
	free(a);
}

static struct bus_pair *
mem_pair_find(struct bus_pairs *l, const char *a, const char *b)
{
	struct bus_pair	*p;

	TAILQ_FOREACH(p, l, entry) {
		if (strcmp(p->a, a) == 0 && (b == NULL || strcmp(p->b, b) == 0))
			return (p);
	}
	return (NULL);
}

static void
mem_pair_add(struct bus_pairs *l, const char *a, const char *b)
{
	struct bus_pair	*p = xcalloc(1, sizeof *p);

	p->a = xstrdup(a);
	p->b = xstrdup(b);
	TAILQ_INSERT_TAIL(l, p, entry);
}

static void
mem_pair_del(struct bus_pairs *l, struct bus_pair *p)
{
	TAILQ_REMOVE(l, p, entry);
	free(p->a);
	free(p->b);
	free(p);
}

/* Follow the renames: "kubuno-2" -> "kubuno-core" -> ... (bounded). */
static char *
mem_resolve(const char *name)
{
	char		*cur = xstrdup(name);
	struct bus_pair	*p;
	int		 i;

	for (i = 0; i < 8; i++) {
		if ((p = mem_pair_find(&mem_aliases, cur, NULL)) == NULL ||
		    strcmp(p->b, cur) == 0)
			break;
		free(cur);
		cur = xstrdup(p->b);
	}
	return (cur);
}

static int
mem_member(const char *channel, const char *agent)
{
	return (mem_pair_find(&mem_members, channel, agent) != NULL);
}

/* The delivery rule, shared by the inbox, the pending count and ack. */
static int
mem_delivered(struct bus_msg *m, const char *agent)
{
	if (strcmp(m->recipient, agent) == 0)
		return (1);
	if (strcmp(m->sender, agent) == 0)
		return (0);
	if (strcmp(m->recipient, "*") == 0)
		return (1);
	if (*m->recipient == '#' && mem_member(m->recipient, agent))
		return (1);
	return (0);
}

static long long
mem_insert(const char *sender, const char *recipient, const char *subject,
    const char *body)
{
	struct bus_msg	*m = xcalloc(1, sizeof *m);

	if (mem_next_id <= bus_last_db_id)
		mem_next_id = bus_last_db_id + 1;
	m->id = mem_next_id++;
	m->ts = (long long)time(NULL);
	m->sender = xstrdup(sender);
	m->recipient = xstrdup(recipient);
	m->subject = (subject != NULL) ? xstrdup(subject) : NULL;
	m->body = xstrdup(body);
	TAILQ_INSERT_TAIL(&mem_msgs, m, entry);
	mem_nmsgs++;
	return (m->id);
}

static void
mem_msg_json(struct sbuf *o, struct bus_msg *m)
{
	sb_str(o, "{\"body\":");
	sb_jstr(o, m->body);
	sb_str(o, ",\"from\":");
	sb_jstr(o, m->sender);
	sb_printf(o, ",\"id\":%lld,\"subject\":", m->id);
	sb_jstr(o, m->subject);
	sb_str(o, ",\"to\":");
	sb_jstr(o, m->recipient);
	sb_printf(o, ",\"ts\":%lld}", m->ts);
}

static void
bus_run_mem(struct bus_job *j)
{
	struct sbuf		*o = &j->out;
	struct bus_agent	*a, *a2;
	struct bus_msg		*m;
	struct bus_pair		*p, *p1;
	char			*from, *to, *ag, *ch;
	long long		 now = (long long)time(NULL), last, cur;
	u_int			 n, first;
	const char		*grp;

	sb_free(o);
	j->rc = 0;
	j->code = 200;
	j->empty = 0;
	switch (j->op) {
	case BUS_HEALTH:
		n = 0;
		RB_FOREACH(a, bus_agents, &mem_agents)
			n++;
		sb_printf(o, "{\"agents\":%u,\"messages\":%u,\"ok\":true,"
		    "\"version\":\"%s\"}", n, mem_nmsgs, BUS_VERSION);
		break;
	case BUS_AGENTS:
		sb_str(o, "{\"agents\":[");
		first = 1;
		RB_FOREACH(a, bus_agents, &mem_agents) {
			if (!j->b1 && a->last_seen <= now - BUS_GHOST)
				continue;
			n = 0;
			TAILQ_FOREACH(m, &mem_msgs, entry) {
				if (!m->read && mem_delivered(m, a->name))
					n++;
			}
			if (!first)
				sb_str(o, ",");
			first = 0;
			sb_str(o, "{\"group\":");
			sb_jstr(o, a->grp);
			sb_printf(o, ",\"last_seen\":%lld,\"managed\":%s,"
			    "\"name\":", a->last_seen,
			    a->managed ? "true" : "false");
			sb_jstr(o, a->name);
			sb_printf(o, ",\"pending\":%u}", n);
		}
		sb_str(o, "]}");
		break;
	case BUS_REGISTER:
		/* Registering a name reclaims it: any old redirect goes. */
		if ((p = mem_pair_find(&mem_aliases, j->s1, NULL)) != NULL)
			mem_pair_del(&mem_aliases, p);
		a = mem_touch(j->s1);
		if (j->has_s2) {
			free(a->grp);
			a->grp = xstrdup(j->s2);
		}
		if (j->has_b1)
			a->managed = j->b1;
		sb_str(o, "{\"name\":");
		sb_jstr(o, j->s1);
		sb_str(o, ",\"ok\":true}");
		break;
	case BUS_RENAME:
		if (strcmp(j->s1, j->s2) == 0) {
			sb_str(o, "{\"ok\":true,\"renamed\":false}");
			break;
		}
		a = mem_find(j->s1);
		a2 = mem_touch(j->s2);
		if (a != NULL) {
			if (*a->grp != '\0') {
				free(a2->grp);
				a2->grp = xstrdup(a->grp);
			}
			if (a->managed)
				a2->managed = 1;
		}
		n = 0;
		TAILQ_FOREACH(m, &mem_msgs, entry) {
			if (!m->read && strcmp(m->recipient, j->s1) == 0) {
				free(m->recipient);
				m->recipient = xstrdup(j->s2);
				n++;
			}
		}
		TAILQ_FOREACH_SAFE(p, &mem_members, entry, p1) {
			if (strcmp(p->b, j->s1) != 0)
				continue;
			if (mem_member(p->a, j->s2))
				mem_pair_del(&mem_members, p);
			else {
				free(p->b);
				p->b = xstrdup(j->s2);
			}
		}
		if (a != NULL)
			mem_agent_remove(a);
		if ((p = mem_pair_find(&mem_aliases, j->s1, NULL)) != NULL) {
			free(p->b);
			p->b = xstrdup(j->s2);
		} else
			mem_pair_add(&mem_aliases, j->s1, j->s2);
		TAILQ_FOREACH_SAFE(p, &mem_aliases, entry, p1) {
			if (strcmp(p->b, j->s1) == 0) {
				free(p->b);
				p->b = xstrdup(j->s2);
			}
			if (strcmp(p->a, p->b) == 0)
				mem_pair_del(&mem_aliases, p);
		}
		sb_str(o, "{\"from\":");
		sb_jstr(o, j->s1);
		sb_printf(o, ",\"messages_moved\":%u,\"ok\":true,"
		    "\"renamed\":true,\"to\":", n);
		sb_jstr(o, j->s2);
		sb_str(o, "}");
		break;
	case BUS_SEND:
		/*
		 * The sender follows its renames too: a session still on its
		 * old name must not resurrect it by writing.
		 */
		from = mem_resolve(j->s1);
		mem_touch(from);
		to = bus_special(j->s2) ? xstrdup(j->s2) : mem_resolve(j->s2);
		if (strcmp(to, "*") == 0 || strcmp(to, "**") == 0) {
			a = mem_find(from);
			grp = (a != NULL) ? a->grp : "";
			last = 0;
			n = 0;
			RB_FOREACH(a2, bus_agents, &mem_agents) {
				if (strcmp(a2->name, from) == 0)
					continue;
				if (strcmp(to, "*") == 0 &&
				    (*grp == '\0' || strcmp(a2->grp, grp) != 0))
					continue;
				last = mem_insert(from, a2->name, j->s3, j->s4);
				n++;
			}
			if (n == 0)
				sb_str(o, "{\"id\":0,\"ok\":true,\"recipients\":0}");
			else {
				sb_printf(o, "{\"id\":%lld,\"ok\":true,"
				    "\"recipients\":%u}", last, n);
			}
		} else {
			last = mem_insert(from, to, j->s3, j->s4);
			sb_printf(o, "{\"id\":%lld,\"ok\":true}", last);
		}
		free(from);
		free(to);
		break;
	case BUS_ACK:
		ag = mem_resolve(j->s1);
		n = 0;
		TAILQ_FOREACH(m, &mem_msgs, entry) {
			if (m->id <= j->n1 && !m->read &&
			    mem_delivered(m, ag)) {
				m->read = 1;
				n++;
			}
		}
		free(ag);
		sb_printf(o, "{\"acked\":%u,\"ok\":true}", n);
		break;
	case BUS_JOIN:
	case BUS_LEAVE:
		ag = mem_resolve(j->s1);
		ch = j->s2;
		if (j->op == BUS_JOIN) {
			mem_touch(ag);
			if (!mem_member(ch, ag))
				mem_pair_add(&mem_members, ch, ag);
		} else if ((p = mem_pair_find(&mem_members, ch, ag)) != NULL)
			mem_pair_del(&mem_members, p);
		free(ag);
		sb_str(o, "{\"channel\":");
		sb_jstr(o, ch);
		sb_str(o, ",\"ok\":true}");
		break;
	case BUS_CHANNELS: {
		/* Channels in order, members in join order. */
		struct bus_pairs	 done = TAILQ_HEAD_INITIALIZER(done);
		struct bus_pair		*q, *best;

		sb_str(o, "{\"channels\":[");
		first = 1;
		for (;;) {
			best = NULL;
			TAILQ_FOREACH(p, &mem_members, entry) {
				if (mem_pair_find(&done, p->a, NULL) != NULL)
					continue;
				if (best == NULL || strcmp(p->a, best->a) < 0)
					best = p;
			}
			if (best == NULL)
				break;
			mem_pair_add(&done, best->a, "");
			if (!first)
				sb_str(o, ",");
			first = 0;
			sb_str(o, "{\"channel\":");
			sb_jstr(o, best->a);
			n = 0;
			TAILQ_FOREACH(q, &mem_members, entry) {
				if (strcmp(q->a, best->a) == 0)
					n++;
			}
			sb_printf(o, ",\"count\":%u,\"members\":[", n);
			n = 0;
			TAILQ_FOREACH(q, &mem_members, entry) {
				if (strcmp(q->a, best->a) != 0)
					continue;
				if (n++ != 0)
					sb_str(o, ",");
				sb_jstr(o, q->b);
			}
			sb_str(o, "]}");
		}
		while ((p = TAILQ_FIRST(&done)) != NULL)
			mem_pair_del(&done, p);
		sb_str(o, "]}");
		break;
	}
	case BUS_INBOX:
		ag = mem_resolve(j->s1);
		mem_touch(ag);
		sb_str(o, "{\"cursor\":");
		cur = j->n1;
		{
			struct sbuf	list = { 0 };

			n = 0;
			TAILQ_FOREACH(m, &mem_msgs, entry) {
				if (!mem_delivered(m, ag))
					continue;
				if (j->b1) {
					if (m->id > cur)
						cur = m->id;	/* real end */
					if (m->read)
						continue;
				} else {
					if (m->id <= j->n1)
						continue;
					if (m->id > cur)
						cur = m->id;
				}
				if (n++ != 0)
					sb_str(&list, ",");
				mem_msg_json(&list, m);
			}
			sb_printf(o, "%lld,\"messages\":[", cur);
			if (list.d != NULL)
				sb_str(o, list.d);
			sb_str(o, "]}");
			sb_free(&list);
			j->empty = (n == 0);
		}
		free(ag);
		break;
	case BUS_PING:
	case BUS_SYNC:
	case BUS_SNAP_PUT:
	case BUS_SNAP_PRUNE:
		break;
	case BUS_SNAP_LIST:
	case BUS_SNAP_GET:
		/* The catalog lives in the database only. */
		j->code = 503;
		strlcpy(j->err, "no database: nothing to restore from",
		    sizeof j->err);
		sb_str(o, "{\"error\":");
		sb_jstr(o, j->err);
		sb_str(o, "}");
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* THE DATABASE STORE - run by the worker thread only.                      */

static MYSQL	*wconn;

static int
db_down_errno(u_int e)
{
	return (e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST ||
	    e == CR_CONN_HOST_ERROR || e == CR_CONNECTION_ERROR ||
	    e == CR_UNKNOWN_HOST || e == CR_SERVER_LOST_EXTENDED ||
	    e == CR_IPSOCK_ERROR || e == CR_SOCKET_CREATE_ERROR);
}

/* A quoted SQL literal (or NULL), escaped for this connection. */
static char *
db_q(MYSQL *c, const char *s)
{
	char	*out;
	size_t	 len;

	if (s == NULL)
		return (xstrdup("NULL"));
	len = strlen(s);
	out = xmalloc(len * 2 + 3);
	out[0] = '\'';
	len = mysql_real_escape_string(c, out + 1, s, (u_long)len);
	out[len + 1] = '\'';
	out[len + 2] = '\0';
	return (out);
}

static int
db_fail(struct bus_job *j, MYSQL *c)
{
	snprintf(j->err, sizeof j->err, "db: %s", mysql_error(c));
	return (db_down_errno(mysql_errno(c)) ? BUS_DOWN : BUS_ERR);
}

static int
db_exec(struct bus_job *j, MYSQL *c, const char *sql)
{
	if (mysql_real_query(c, sql, (u_long)strlen(sql)) != 0)
		return (db_fail(j, c));
	return (0);
}

static int printflike(3, 4)
db_execf(struct bus_job *j, MYSQL *c, const char *fmt, ...)
{
	va_list	 ap;
	char	*sql;
	int	 rc;

	va_start(ap, fmt);
	xvasprintf(&sql, fmt, ap);
	va_end(ap);
	rc = db_exec(j, c, sql);
	free(sql);
	return (rc);
}

static MYSQL_RES *
db_queryf(struct bus_job *j, MYSQL *c, int *rc, const char *fmt, ...)
{
	va_list		 ap;
	char		*sql;
	MYSQL_RES	*r = NULL;

	va_start(ap, fmt);
	xvasprintf(&sql, fmt, ap);
	va_end(ap);
	*rc = db_exec(j, c, sql);
	free(sql);
	if (*rc != 0)
		return (NULL);
	if ((r = mysql_store_result(c)) == NULL && mysql_field_count(c) != 0)
		*rc = db_fail(j, c);
	return (r);
}

/* One number from a query, or dflt. */
static long long
db_num(struct bus_job *j, MYSQL *c, int *rc, const char *sql)
{
	MYSQL_RES	*r;
	MYSQL_ROW	 row;
	long long	 v = 0;

	if ((r = db_queryf(j, c, rc, "%s", sql)) == NULL)
		return (0);
	if ((row = mysql_fetch_row(r)) != NULL && row[0] != NULL)
		v = strtoll(row[0], NULL, 10);
	mysql_free_result(r);
	return (v);
}

/* The delivery predicate, for an already-quoted agent name. */
static char *
db_pred(const char *qa)
{
	char	*s;

	xasprintf(&s, "(messages.recipient = %s OR (messages.recipient = '*' "
	    "AND messages.sender <> %s) OR (messages.recipient IN (SELECT "
	    "channel FROM memberships WHERE agent = %s) AND messages.sender "
	    "<> %s))", qa, qa, qa, qa);
	return (s);
}

static int
db_touch(struct bus_job *j, MYSQL *c, const char *qa)
{
	return (db_execf(j, c, "INSERT INTO agents(name, last_seen) VALUES(%s, "
	    "UNIX_TIMESTAMP()) ON DUPLICATE KEY UPDATE last_seen = "
	    "VALUES(last_seen)", qa));
}

static char *
db_resolve(struct bus_job *j, MYSQL *c, const char *name, int *rc)
{
	char		*cur = xstrdup(name), *q;
	MYSQL_RES	*r;
	MYSQL_ROW	 row;
	int		 i;

	*rc = 0;
	for (i = 0; i < 8; i++) {
		q = db_q(c, cur);
		r = db_queryf(j, c, rc, "SELECT new_name FROM aliases WHERE "
		    "old_name = %s", q);
		free(q);
		if (*rc != 0 || r == NULL)
			break;
		row = mysql_fetch_row(r);
		if (row == NULL || row[0] == NULL || strcmp(row[0], cur) == 0) {
			mysql_free_result(r);
			break;
		}
		free(cur);
		cur = xstrdup(row[0]);
		mysql_free_result(r);
	}
	return (cur);
}

static void
db_msg_json(struct sbuf *o, MYSQL_ROW row)
{
	/* id, ts, sender, recipient, subject, body */
	sb_str(o, "{\"body\":");
	sb_jstr(o, row[5]);
	sb_str(o, ",\"from\":");
	sb_jstr(o, row[2]);
	sb_printf(o, ",\"id\":%s,\"subject\":", row[0]);
	sb_jstr(o, row[4]);
	sb_str(o, ",\"to\":");
	sb_jstr(o, row[3]);
	sb_printf(o, ",\"ts\":%s}", row[1]);
}

static int
db_schema(struct bus_job *j, MYSQL *c)
{
	static const char *create[] = {
		"CREATE TABLE IF NOT EXISTS agents ("
		" name VARCHAR(191) NOT NULL PRIMARY KEY,"
		" last_seen BIGINT NOT NULL,"
		" grp VARCHAR(191) NOT NULL DEFAULT '',"
		" managed TINYINT NOT NULL DEFAULT 0"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE IF NOT EXISTS messages ("
		" id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
		" ts BIGINT NOT NULL,"
		" sender VARCHAR(191) NOT NULL,"
		" recipient VARCHAR(191) NOT NULL,"
		" subject TEXT NULL,"
		" body MEDIUMTEXT NOT NULL,"
		" `read` TINYINT NOT NULL DEFAULT 0,"
		" INDEX idx_messages_recipient (recipient, id),"
		" INDEX idx_messages_unread (`read`, id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE IF NOT EXISTS memberships ("
		" channel VARCHAR(191) NOT NULL,"
		" agent VARCHAR(191) NOT NULL,"
		" PRIMARY KEY (channel, agent)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		/*
		 * The catalog: each tmuxv keeps here the cold state of each of
		 * its sessions, so any tmuxv can bring one back - even once
		 * the server that ran it has stopped.
		 */
		"CREATE TABLE IF NOT EXISTS tmuxv_snapshots ("
		" node VARCHAR(191) NOT NULL,"
		" session VARCHAR(191) NOT NULL,"
		" host VARCHAR(191) NOT NULL DEFAULT '',"
		" updated BIGINT NOT NULL,"
		" windows INT NOT NULL DEFAULT 0,"
		" conversations INT NOT NULL DEFAULT 0,"
		" snapshot MEDIUMTEXT NOT NULL,"
		" PRIMARY KEY (node, session)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE IF NOT EXISTS aliases ("
		" old_name VARCHAR(191) NOT NULL PRIMARY KEY,"
		" new_name VARCHAR(191) NOT NULL,"
		" ts BIGINT NOT NULL"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		/* Files joined to messages, for every tmuxv of the base. */
		"CREATE TABLE IF NOT EXISTS pieces ("
		" id VARCHAR(40) NOT NULL PRIMARY KEY,"
		" ts BIGINT NOT NULL,"
		" name VARCHAR(191) NOT NULL,"
		" mime VARCHAR(100) NOT NULL,"
		" size INT NOT NULL,"
		" data MEDIUMBLOB NOT NULL,"
		" INDEX idx_pieces_ts (ts)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		NULL
	};
	int	i, rc;

	for (i = 0; create[i] != NULL; i++) {
		if ((rc = db_exec(j, c, create[i])) != 0)
			return (rc);
	}
	(void)db_exec(j, c, "ALTER TABLE agents ADD COLUMN IF NOT EXISTS "
	    "grp VARCHAR(191) NOT NULL DEFAULT ''");
	(void)db_exec(j, c, "ALTER TABLE agents ADD COLUMN IF NOT EXISTS "
	    "managed TINYINT NOT NULL DEFAULT 0");
	return (0);
}

/*
 * mysql://[user[:password]@]host[:port]/database[?socket=/path]
 * With no user, the login name - which is what MariaDB's unix_socket
 * authentication expects, and needs no password stored anywhere.
 */
static MYSQL *
db_connect(const char *url, char *err, size_t errlen)
{
	char		*u, *p, *at, *slash, *q, *colon, *sock = NULL;
	char		*user = NULL, *pass = NULL, *host = NULL, *db = NULL;
	u_int		 port = 0, t = 3;
	struct passwd	*pw;
	MYSQL		*c;

	if (strncmp(url, "mysql://", 8) != 0 &&
	    strncmp(url, "mariadb://", 10) != 0) {
		snprintf(err, errlen, "bad database url");
		return (NULL);
	}
	u = xstrdup(strstr(url, "://") + 3);
	if ((q = strchr(u, '?')) != NULL) {
		*q++ = '\0';
		if (strncmp(q, "socket=", 7) == 0)
			sock = q + 7;
	}
	p = u;
	if ((at = strrchr(p, '@')) != NULL) {
		*at = '\0';
		user = p;
		if ((colon = strchr(user, ':')) != NULL) {
			*colon = '\0';
			pass = colon + 1;
		}
		p = at + 1;
	}
	if ((slash = strchr(p, '/')) != NULL) {
		*slash = '\0';
		db = slash + 1;
	}
	host = p;
	if ((colon = strchr(host, ':')) != NULL) {
		*colon = '\0';
		port = (u_int)atoi(colon + 1);
	}
	if (user == NULL || *user == '\0') {
		pw = getpwuid(getuid());
		user = (pw != NULL) ? pw->pw_name : NULL;
	}
	if (*host == '\0')
		host = (char *)"localhost";

	c = mysql_init(NULL);
	mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &t);
	t = 10;
	mysql_options(c, MYSQL_OPT_READ_TIMEOUT, &t);
	mysql_options(c, MYSQL_OPT_WRITE_TIMEOUT, &t);
	mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	if (mysql_real_connect(c, host, user, pass, db, port, sock, 0) ==
	    NULL) {
		snprintf(err, errlen, "%s", mysql_error(c));
		mysql_close(c);
		free(u);
		return (NULL);
	}
	free(u);
	return (c);
}

static int
bus_run_db(struct bus_job *j, MYSQL *c)
{
	struct sbuf	*o = &j->out;
	MYSQL_RES	*r;
	MYSQL_ROW	 row;
	char		*qa, *qb, *qc, *qd, *pred, *from, *to, *ag;
	long long	 v1, v2, last;
	u_int		 n, i;
	int		 rc = 0, first;

	sb_free(o);
	j->code = 200;
	j->empty = 0;
	switch (j->op) {
	case BUS_PING:
		j->maxid = db_num(j, c, &rc,
		    "SELECT COALESCE(MAX(id), 0) FROM messages");
		return (rc);
	case BUS_HEALTH:
		v1 = db_num(j, c, &rc, "SELECT COUNT(*) FROM agents");
		if (rc != 0)
			return (rc);
		v2 = db_num(j, c, &rc, "SELECT COUNT(*) FROM messages");
		if (rc != 0)
			return (rc);
		sb_printf(o, "{\"agents\":%lld,\"messages\":%lld,\"ok\":true,"
		    "\"version\":\"%s\"}", v1, v2, BUS_VERSION);
		return (0);
	case BUS_AGENTS:
		/*
		 * The pending count uses the SAME delivery rule as the inbox,
		 * so a badge never says something the mailbox does not.
		 */
		r = db_queryf(j, c, &rc, "SELECT a.name, a.last_seen, a.grp, "
		    "a.managed, (SELECT COUNT(*) FROM messages WHERE "
		    "messages.`read` = 0 AND (messages.recipient = a.name OR "
		    "(messages.recipient = '*' AND messages.sender <> a.name) "
		    "OR (messages.recipient IN (SELECT channel FROM memberships "
		    "WHERE agent = a.name) AND messages.sender <> a.name))) "
		    "FROM agents a %s ORDER BY a.name", j->b1 ? "" :
		    "WHERE a.last_seen > UNIX_TIMESTAMP() - 3600");
		if (rc != 0)
			return (rc);
		sb_str(o, "{\"agents\":[");
		first = 1;
		while (r != NULL && (row = mysql_fetch_row(r)) != NULL) {
			if (!first)
				sb_str(o, ",");
			first = 0;
			sb_str(o, "{\"group\":");
			sb_jstr(o, row[2] != NULL ? row[2] : "");
			sb_printf(o, ",\"last_seen\":%s,\"managed\":%s,"
			    "\"name\":", row[1], (row[3] != NULL &&
			    atoi(row[3]) != 0) ? "true" : "false");
			sb_jstr(o, row[0]);
			sb_printf(o, ",\"pending\":%s}", row[4]);
		}
		if (r != NULL)
			mysql_free_result(r);
		sb_str(o, "]}");
		return (0);
	case BUS_REGISTER:
		qa = db_q(c, j->s1);
		rc = db_execf(j, c, "DELETE FROM aliases WHERE old_name = %s",
		    qa);
		if (rc == 0)
			rc = db_touch(j, c, qa);
		if (rc == 0 && j->has_s2) {
			qb = db_q(c, j->s2);
			rc = db_execf(j, c, "UPDATE agents SET grp = %s WHERE "
			    "name = %s", qb, qa);
			free(qb);
		}
		if (rc == 0 && j->has_b1) {
			rc = db_execf(j, c, "UPDATE agents SET managed = %d "
			    "WHERE name = %s", j->b1 ? 1 : 0, qa);
		}
		free(qa);
		if (rc != 0)
			return (rc);
		sb_str(o, "{\"name\":");
		sb_jstr(o, j->s1);
		sb_str(o, ",\"ok\":true}");
		return (0);
	case BUS_RENAME:
		if (strcmp(j->s1, j->s2) == 0) {
			sb_str(o, "{\"ok\":true,\"renamed\":false}");
			return (0);
		}
		qa = db_q(c, j->s1);
		qb = db_q(c, j->s2);
		rc = db_execf(j, c, "INSERT INTO agents(name, last_seen, grp, "
		    "managed) SELECT %s, UNIX_TIMESTAMP(), COALESCE((SELECT grp "
		    "FROM agents WHERE name = %s), ''), COALESCE((SELECT managed "
		    "FROM agents WHERE name = %s), 0) ON DUPLICATE KEY UPDATE "
		    "last_seen = VALUES(last_seen), grp = IF(VALUES(grp) = '', "
		    "grp, VALUES(grp)), managed = GREATEST(managed, "
		    "VALUES(managed))", qb, qa, qa);
		v1 = 0;
		if (rc == 0) {
			rc = db_execf(j, c, "UPDATE messages SET recipient = %s "
			    "WHERE recipient = %s AND `read` = 0", qb, qa);
			v1 = (long long)mysql_affected_rows(c);
		}
		if (rc == 0) {
			rc = db_execf(j, c, "UPDATE IGNORE memberships SET "
			    "agent = %s WHERE agent = %s", qb, qa);
		}
		if (rc == 0) {
			rc = db_execf(j, c, "DELETE FROM memberships WHERE "
			    "agent = %s", qa);
		}
		if (rc == 0)
			rc = db_execf(j, c, "DELETE FROM agents WHERE name = %s",
			    qa);
		if (rc == 0) {
			rc = db_execf(j, c, "INSERT INTO aliases(old_name, "
			    "new_name, ts) VALUES(%s, %s, UNIX_TIMESTAMP()) ON "
			    "DUPLICATE KEY UPDATE new_name = VALUES(new_name), "
			    "ts = VALUES(ts)", qa, qb);
		}
		if (rc == 0) {
			rc = db_execf(j, c, "UPDATE aliases SET new_name = %s "
			    "WHERE new_name = %s", qb, qa);
		}
		if (rc == 0)
			(void)db_exec(j, c, "DELETE FROM aliases WHERE "
			    "old_name = new_name");
		free(qa);
		free(qb);
		if (rc != 0)
			return (rc);
		sb_str(o, "{\"from\":");
		sb_jstr(o, j->s1);
		sb_printf(o, ",\"messages_moved\":%lld,\"ok\":true,"
		    "\"renamed\":true,\"to\":", v1);
		sb_jstr(o, j->s2);
		sb_str(o, "}");
		return (0);
	case BUS_SEND:
		from = db_resolve(j, c, j->s1, &rc);
		if (rc != 0) {
			free(from);
			return (rc);
		}
		qa = db_q(c, from);
		if ((rc = db_touch(j, c, qa)) != 0)
			goto send_out;
		if (bus_special(j->s2))
			to = xstrdup(j->s2);
		else if ((to = db_resolve(j, c, j->s2, &rc)), rc != 0) {
			free(to);
			goto send_out;
		}
		qc = db_q(c, j->s3);
		qd = db_q(c, j->s4);
		if (strcmp(to, "*") == 0 || strcmp(to, "**") == 0) {
			if (strcmp(to, "*") == 0) {
				r = db_queryf(j, c, &rc, "SELECT name FROM "
				    "agents WHERE name <> %s AND grp = (SELECT "
				    "grp FROM (SELECT grp FROM agents WHERE "
				    "name = %s) AS g) AND grp <> ''", qa, qa);
			} else {
				r = db_queryf(j, c, &rc, "SELECT name FROM "
				    "agents WHERE name <> %s", qa);
			}
			last = 0;
			n = 0;
			while (rc == 0 && r != NULL &&
			    (row = mysql_fetch_row(r)) != NULL) {
				qb = db_q(c, row[0]);
				rc = db_execf(j, c, "INSERT INTO messages(ts, "
				    "sender, recipient, subject, body) VALUES("
				    "UNIX_TIMESTAMP(), %s, %s, %s, %s)", qa, qb,
				    qc, qd);
				free(qb);
				last = (long long)mysql_insert_id(c);
				n++;
			}
			if (r != NULL)
				mysql_free_result(r);
			if (rc == 0 && n == 0)
				sb_str(o, "{\"id\":0,\"ok\":true,\"recipients\":0}");
			else if (rc == 0) {
				sb_printf(o, "{\"id\":%lld,\"ok\":true,"
				    "\"recipients\":%u}", last, n);
			}
			j->maxid = last;
		} else {
			qb = db_q(c, to);
			rc = db_execf(j, c, "INSERT INTO messages(ts, sender, "
			    "recipient, subject, body) VALUES(UNIX_TIMESTAMP(), "
			    "%s, %s, %s, %s)", qa, qb, qc, qd);
			free(qb);
			if (rc == 0) {
				last = (long long)mysql_insert_id(c);
				sb_printf(o, "{\"id\":%lld,\"ok\":true}", last);
				j->maxid = last;
			}
		}
		free(qc);
		free(qd);
		free(to);
	send_out:
		free(qa);
		free(from);
		return (rc);
	case BUS_ACK:
		ag = db_resolve(j, c, j->s1, &rc);
		if (rc == 0) {
			qa = db_q(c, ag);
			pred = db_pred(qa);
			rc = db_execf(j, c, "UPDATE messages SET `read` = 1 WHERE "
			    "id <= %lld AND `read` = 0 AND %s", j->n1, pred);
			if (rc == 0) {
				sb_printf(o, "{\"acked\":%llu,\"ok\":true}",
				    (unsigned long long)mysql_affected_rows(c));
			}
			free(pred);
			free(qa);
		}
		free(ag);
		return (rc);
	case BUS_JOIN:
	case BUS_LEAVE:
		ag = db_resolve(j, c, j->s1, &rc);
		if (rc == 0) {
			qa = db_q(c, ag);
			qb = db_q(c, j->s2);
			if (j->op == BUS_JOIN) {
				rc = db_touch(j, c, qa);
				if (rc == 0) {
					rc = db_execf(j, c, "INSERT IGNORE INTO "
					    "memberships(channel, agent) VALUES("
					    "%s, %s)", qb, qa);
				}
			} else {
				rc = db_execf(j, c, "DELETE FROM memberships "
				    "WHERE channel = %s AND agent = %s", qb, qa);
			}
			free(qa);
			free(qb);
		}
		free(ag);
		if (rc != 0)
			return (rc);
		sb_str(o, "{\"channel\":");
		sb_jstr(o, j->s2);
		sb_str(o, ",\"ok\":true}");
		return (0);
	case BUS_CHANNELS:
		r = db_queryf(j, c, &rc, "SELECT channel, GROUP_CONCAT(agent), "
		    "COUNT(*) FROM memberships GROUP BY channel ORDER BY channel");
		if (rc != 0)
			return (rc);
		sb_str(o, "{\"channels\":[");
		first = 1;
		while (r != NULL && (row = mysql_fetch_row(r)) != NULL) {
			char	*list = xstrdup(row[1] != NULL ? row[1] : ""), *tok,
				*save = NULL;

			if (!first)
				sb_str(o, ",");
			first = 0;
			sb_str(o, "{\"channel\":");
			sb_jstr(o, row[0]);
			sb_printf(o, ",\"count\":%s,\"members\":[", row[2]);
			i = 0;
			for (tok = strtok_r(list, ",", &save); tok != NULL;
			    tok = strtok_r(NULL, ",", &save)) {
				if (i++ != 0)
					sb_str(o, ",");
				sb_jstr(o, tok);
			}
			sb_str(o, "]}");
			free(list);
		}
		if (r != NULL)
			mysql_free_result(r);
		sb_str(o, "]}");
		return (0);
	case BUS_INBOX:
		ag = db_resolve(j, c, j->s1, &rc);
		if (rc != 0) {
			free(ag);
			return (rc);
		}
		qa = db_q(c, ag);
		pred = db_pred(qa);
		if ((rc = db_touch(j, c, qa)) != 0)
			goto inbox_out;
		if (j->b1) {
			r = db_queryf(j, c, &rc, "SELECT id, ts, sender, "
			    "recipient, subject, body FROM messages WHERE "
			    "messages.`read` = 0 AND %s ORDER BY id", pred);
		} else {
			r = db_queryf(j, c, &rc, "SELECT id, ts, sender, "
			    "recipient, subject, body FROM messages WHERE "
			    "id > %lld AND %s ORDER BY id", j->n1, pred);
		}
		if (rc != 0)
			goto inbox_out;
		{
			struct sbuf	list = { 0 };

			last = j->n1;
			n = 0;
			while (r != NULL && (row = mysql_fetch_row(r)) != NULL) {
				v1 = strtoll(row[0], NULL, 10);
				if (v1 > last)
					last = v1;
				if (n++ != 0)
					sb_str(&list, ",");
				db_msg_json(&list, row);
			}
			if (r != NULL)
				mysql_free_result(r);
			if (j->b1) {
				char	*sql;

				xasprintf(&sql, "SELECT COALESCE(MAX(id), 0) "
				    "FROM messages WHERE %s", pred);
				v2 = db_num(j, c, &rc, sql);
				free(sql);
				if (v2 > last)
					last = v2;
			}
			if (rc == 0) {
				sb_printf(o, "{\"cursor\":%lld,\"messages\":[",
				    last);
				if (list.d != NULL)
					sb_str(o, list.d);
				sb_str(o, "]}");
				j->empty = (n == 0);
				j->maxid = last;
			}
			sb_free(&list);
		}
	inbox_out:
		free(pred);
		free(qa);
		free(ag);
		return (rc);
	case BUS_PIECE_PUT: {
		u_long	el;

		qa = db_q(c, j->s1);
		qb = db_q(c, j->s2);
		qc = db_q(c, j->s3);
		qd = xmalloc(j->bloblen * 2 + 3);
		qd[0] = '\'';
		el = mysql_real_escape_string(c, qd + 1, (const char *)j->blob,
		    (u_long)j->bloblen);
		qd[el + 1] = '\'';
		qd[el + 2] = '\0';
		rc = db_execf(j, c, "INSERT IGNORE INTO pieces(id, ts, name, "
		    "mime, size, data) VALUES(%s, UNIX_TIMESTAMP(), %s, %s, %zu, "
		    "%s)", qa, qb, qc, j->bloblen, qd);
		if (rc == 0) {
			(void)db_execf(j, c, "DELETE FROM pieces WHERE ts < "
			    "UNIX_TIMESTAMP() - %d", BUS_PIECE_DAYS * 86400);
		}
		free(qa); free(qb); free(qc); free(qd);
		return (rc);
	}
	case BUS_PIECE_GET: {
		u_long	*lens;
		char	*path;

		qa = db_q(c, j->s1);
		r = db_queryf(j, c, &rc, "SELECT name, mime, data FROM pieces "
		    "WHERE id = %s", qa);
		free(qa);
		if (rc != 0)
			return (rc);
		if (r == NULL || (row = mysql_fetch_row(r)) == NULL) {
			if (r != NULL)
				mysql_free_result(r);
			j->code = 404;
			return (0);
		}
		lens = mysql_fetch_lengths(r);
		path = bus_piece_write(j->s1, row[0], row[1], (u_char *)row[2],
		    lens[2]);
		mysql_free_result(r);
		j->code = (path != NULL) ? 200 : 500;
		free(path);
		return (0);
	}
	case BUS_SNAP_PUT:
		qa = db_q(c, j->s1);
		qb = db_q(c, j->s2);
		qc = db_q(c, j->s3);
		qd = db_q(c, j->s4);
		rc = db_execf(j, c, "INSERT INTO tmuxv_snapshots(node, session, "
		    "host, updated, windows, conversations, snapshot) VALUES(%s, "
		    "%s, %s, UNIX_TIMESTAMP(), %lld, %lld, %s) ON DUPLICATE KEY "
		    "UPDATE host = VALUES(host), updated = VALUES(updated), "
		    "windows = VALUES(windows), conversations = "
		    "VALUES(conversations), snapshot = VALUES(snapshot)", qa, qb,
		    qc, j->n1, j->n2, qd);
		free(qa); free(qb); free(qc); free(qd);
		return (rc);
	case BUS_SNAP_PRUNE: {
		struct sbuf	 in = { 0 };
		char		*list = xstrdup(j->s2 != NULL ? j->s2 : ""), *tok;
		char		*save = NULL;

		i = 0;
		for (tok = strtok_r(list, "\x1f", &save); tok != NULL;
		    tok = strtok_r(NULL, "\x1f", &save)) {
			qb = db_q(c, tok);
			if (i++ != 0)
				sb_str(&in, ",");
			sb_str(&in, qb);
			free(qb);
		}
		free(list);
		qa = db_q(c, j->s1);
		if (i == 0)
			rc = 0;		/* never wipe a node on an empty list */
		else {
			rc = db_execf(j, c, "DELETE FROM tmuxv_snapshots WHERE "
			    "node = %s AND session NOT IN (%s)", qa, in.d);
		}
		free(qa);
		sb_free(&in);
		return (rc);
	}
	case BUS_SNAP_LIST:
		r = db_queryf(j, c, &rc, "SELECT node, session, host, updated, "
		    "windows, conversations FROM tmuxv_snapshots ORDER BY "
		    "updated DESC");
		if (rc != 0)
			return (rc);
		/*
		 * An internal caller (restore-session) gets plain lines:
		 * node, session, host, updated, windows, conversations.
		 */
		if (j->ucb != NULL) {
			while (r != NULL && (row = mysql_fetch_row(r)) != NULL) {
				sb_printf(o, "%s\t%s\t%s\t%s\t%s\t%s\n", row[0],
				    row[1], row[2], row[3], row[4], row[5]);
			}
			if (r != NULL)
				mysql_free_result(r);
			if (o->d == NULL)
				sb_str(o, "");
			return (0);
		}
		sb_str(o, "{\"sessions\":[");
		first = 1;
		while (r != NULL && (row = mysql_fetch_row(r)) != NULL) {
			if (!first)
				sb_str(o, ",");
			first = 0;
			sb_printf(o, "{\"conversations\":%s,\"host\":", row[5]);
			sb_jstr(o, row[2]);
			sb_str(o, ",\"node\":");
			sb_jstr(o, row[0]);
			sb_str(o, ",\"session\":");
			sb_jstr(o, row[1]);
			sb_printf(o, ",\"updated\":%s,\"windows\":%s}", row[3],
			    row[4]);
		}
		if (r != NULL)
			mysql_free_result(r);
		sb_str(o, "]}");
		return (0);
	case BUS_SNAP_GET:
		qa = db_q(c, j->s1);
		qb = db_q(c, j->s2);
		r = db_queryf(j, c, &rc, "SELECT snapshot FROM tmuxv_snapshots "
		    "WHERE node = %s AND session = %s", qa, qb);
		free(qa);
		free(qb);
		if (rc != 0)
			return (rc);
		if (r != NULL && (row = mysql_fetch_row(r)) != NULL &&
		    row[0] != NULL)
			sb_str(o, row[0]);	/* raw text; JSON-wrapped for HTTP */
		else {
			j->code = 404;
			strlcpy(j->err, "no such session in the catalog",
			    sizeof j->err);
		}
		if (r != NULL)
			mysql_free_result(r);
		return (0);
	case BUS_SYNC:
		/* Pour into the database what was written while it was away. */
		for (i = 0; rc == 0 && i < j->nagents; i++) {
			qa = db_q(c, j->agents[i].name);
			qb = db_q(c, j->agents[i].grp);
			rc = db_execf(j, c, "INSERT INTO agents(name, last_seen, "
			    "grp, managed) VALUES(%s, %lld, %s, %d) ON DUPLICATE "
			    "KEY UPDATE last_seen = GREATEST(last_seen, "
			    "VALUES(last_seen)), grp = IF(VALUES(grp) = '', grp, "
			    "VALUES(grp)), managed = GREATEST(managed, "
			    "VALUES(managed))", qa, j->agents[i].last_seen, qb,
			    j->agents[i].managed);
			free(qa);
			free(qb);
		}
		for (i = 0; rc == 0 && i < j->naliases; i++) {
			qa = db_q(c, j->aliases[i].a);
			qb = db_q(c, j->aliases[i].b);
			rc = db_execf(j, c, "INSERT INTO aliases(old_name, "
			    "new_name, ts) VALUES(%s, %s, UNIX_TIMESTAMP()) ON "
			    "DUPLICATE KEY UPDATE new_name = VALUES(new_name)",
			    qa, qb);
			free(qa);
			free(qb);
		}
		for (i = 0; rc == 0 && i < j->nmembers; i++) {
			qa = db_q(c, j->members[i].a);
			qb = db_q(c, j->members[i].b);
			rc = db_execf(j, c, "INSERT IGNORE INTO memberships("
			    "channel, agent) VALUES(%s, %s)", qa, qb);
			free(qa);
			free(qb);
		}
		for (i = 0; rc == 0 && i < j->nmsgs; i++) {
			qa = db_q(c, j->msgs[i].sender);
			qb = db_q(c, j->msgs[i].recipient);
			qc = db_q(c, j->msgs[i].subject);
			qd = db_q(c, j->msgs[i].body);
			rc = db_execf(j, c, "INSERT INTO messages(ts, sender, "
			    "recipient, subject, body, `read`) VALUES(%lld, %s, "
			    "%s, %s, %s, %d)", j->msgs[i].ts, qa, qb, qc, qd,
			    j->msgs[i].read);
			free(qa); free(qb); free(qc); free(qd);
		}
		if (rc == 0) {
			j->maxid = db_num(j, c, &rc,
			    "SELECT COALESCE(MAX(id), 0) FROM messages");
		}
		return (rc);
	}
	return (0);
}

/* ------------------------------------------------------------------------ */
/* The worker thread.                                                       */

static void *
bus_worker(__unused void *arg)
{
	sigset_t	 set;
	struct bus_job	*j;
	char		*url, err[256];
	int		 reset;

	/* Signals are the main thread's business. */
	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	mysql_thread_init();

	for (;;) {
		pthread_mutex_lock(&bus_lock);
		while (TAILQ_EMPTY(&bus_todo))
			pthread_cond_wait(&bus_cond, &bus_lock);
		j = TAILQ_FIRST(&bus_todo);
		TAILQ_REMOVE(&bus_todo, j, entry);
		url = (bus_worker_url != NULL) ? xstrdup(bus_worker_url) : NULL;
		reset = bus_worker_reset;
		bus_worker_reset = 0;
		pthread_mutex_unlock(&bus_lock);

		if (reset && wconn != NULL) {
			mysql_close(wconn);
			wconn = NULL;
		}
		if (wconn == NULL && url != NULL && *url != '\0') {
			*err = '\0';
			if ((wconn = db_connect(url, err, sizeof err)) != NULL &&
			    db_schema(j, wconn) != 0) {
				mysql_close(wconn);
				wconn = NULL;
			}
			if (wconn == NULL) {
				snprintf(j->err, sizeof j->err, "db: %s",
				    *err != '\0' ? err : "unreachable");
			}
		}
		free(url);

		if (wconn == NULL)
			j->rc = BUS_DOWN;
		else {
			j->rc = bus_run_db(j, wconn);
			if (j->rc == BUS_DOWN) {
				mysql_close(wconn);
				wconn = NULL;
			}
		}

		pthread_mutex_lock(&bus_lock);
		TAILQ_INSERT_TAIL(&bus_done, j, entry);
		pthread_mutex_unlock(&bus_lock);
		if (write(bus_pipe[1], "x", 1) < 0) {
			/* full pipe: the main thread is already woken */
		}
	}
	return (NULL);
}

static void
bus_queue(struct bus_job *j)
{
	pthread_mutex_lock(&bus_lock);
	TAILQ_INSERT_TAIL(&bus_todo, j, entry);
	pthread_cond_signal(&bus_cond);
	pthread_mutex_unlock(&bus_lock);
}

/* ------------------------------------------------------------------------ */
/* Mode switching.                                                          */

static void
bus_mode_local(const char *why)
{
	if (bus_db_mode) {
		log_debug("bus: database lost (%s), answering from memory",
		    why);
	}
	bus_db_mode = 0;
	strlcpy(bus_mode_why, why, sizeof bus_mode_why);
}

/* A snapshot of the memory store, for the sync job. */
static struct bus_job *
bus_sync_job(void)
{
	struct bus_job		*j = bus_job_new(BUS_SYNC);
	struct bus_agent	*a;
	struct bus_msg		*m;
	struct bus_pair		*p;
	u_int			 i;

	RB_FOREACH(a, bus_agents, &mem_agents)
		j->nagents++;
	j->agents = xcalloc(j->nagents + 1, sizeof *j->agents);
	i = 0;
	RB_FOREACH(a, bus_agents, &mem_agents) {
		j->agents[i].name = xstrdup(a->name);
		j->agents[i].grp = xstrdup(a->grp);
		j->agents[i].last_seen = a->last_seen;
		j->agents[i].managed = a->managed;
		i++;
	}
	j->msgs = xcalloc(mem_nmsgs + 1, sizeof *j->msgs);
	TAILQ_FOREACH(m, &mem_msgs, entry) {
		j->msgs[j->nmsgs].ts = m->ts;
		j->msgs[j->nmsgs].sender = xstrdup(m->sender);
		j->msgs[j->nmsgs].recipient = xstrdup(m->recipient);
		j->msgs[j->nmsgs].subject = (m->subject != NULL) ?
		    xstrdup(m->subject) : NULL;
		j->msgs[j->nmsgs].body = xstrdup(m->body);
		j->msgs[j->nmsgs].read = m->read;
		j->nmsgs++;
	}
	TAILQ_FOREACH(p, &mem_members, entry)
		j->nmembers++;
	j->members = xcalloc(j->nmembers + 1, sizeof *j->members);
	i = 0;
	TAILQ_FOREACH(p, &mem_members, entry) {
		j->members[i].a = xstrdup(p->a);
		j->members[i].b = xstrdup(p->b);
		i++;
	}
	TAILQ_FOREACH(p, &mem_aliases, entry)
		j->naliases++;
	j->aliases = xcalloc(j->naliases + 1, sizeof *j->aliases);
	i = 0;
	TAILQ_FOREACH(p, &mem_aliases, entry) {
		j->aliases[i].a = xstrdup(p->a);
		j->aliases[i].b = xstrdup(p->b);
		i++;
	}
	return (j);
}

/* The database took everything back: the memory store starts over. */
static void
bus_mem_clear_msgs(void)
{
	struct bus_msg	*m;

	while ((m = TAILQ_FIRST(&mem_msgs)) != NULL) {
		TAILQ_REMOVE(&mem_msgs, m, entry);
		free(m->sender); free(m->recipient);
		free(m->subject); free(m->body);
		free(m);
	}
	mem_nmsgs = 0;
}

/* ------------------------------------------------------------------------ */
/* Completion, on the main thread.                                          */

static void
bus_retry_cb(__unused int fd, __unused short events, void *arg)
{
	struct bus_job	*j = arg;

	j->retry_armed = 0;
	bus_dispatch(j);
}

static void
bus_finish(struct bus_job *j)
{
	struct timeval	tv = { 0, BUS_POLL_MS * 1000 };

	if (j->maxid > bus_last_db_id && j->rc == 0 && j->op != BUS_SYNC)
		bus_last_db_id = j->maxid;

	switch (j->op) {
	case BUS_PING:
		bus_ping_out = 0;
		if (j->rc == 0) {
			if (j->maxid > bus_last_db_id)
				bus_last_db_id = j->maxid;
			if (!bus_sync_out) {
				bus_sync_out = 1;
				bus_queue(bus_sync_job());
			}
		} else
			strlcpy(bus_mode_why, j->err, sizeof bus_mode_why);
		bus_job_free(j);
		return;
	case BUS_SYNC:
		bus_sync_out = 0;
		if (j->rc == 0) {
			bus_mem_clear_msgs();
			if (j->maxid > bus_last_db_id)
				bus_last_db_id = j->maxid;
			bus_db_mode = 1;
			strlcpy(bus_mode_why, "database", sizeof bus_mode_why);
			log_debug("bus: database back, %u message(s) poured in",
			    j->nmsgs);
			/*
			 * Ids were given by the database: every delivery
			 * cursor restarts with a first fetch (the unread),
			 * so nothing is skipped and nothing arrives twice.
			 */
			claude_bus_resync();
		} else
			strlcpy(bus_mode_why, j->err, sizeof bus_mode_why);
		bus_job_free(j);
		return;
	case BUS_SNAP_PUT:
	case BUS_SNAP_PRUNE:
	case BUS_PIECE_PUT:
		if (j->rc == BUS_DOWN)
			bus_mode_local(j->err);
		bus_job_free(j);
		return;
	case BUS_PIECE_GET:
		if (j->rc == BUS_DOWN)
			bus_mode_local(j->err);
		if (j->req != NULL && !j->cancelled) {
			if (j->rc == 0 && j->code == 200)
				bus_piece_send(j->req, j->s1);
			else
				bus_error(j->req, 404, "unknown piece", j->s1);
		}
		bus_job_free(j);
		return;
	case BUS_SNAP_LIST:
	case BUS_SNAP_GET:
		if (j->rc == BUS_DOWN) {
			bus_mode_local(j->err);
			bus_run_mem(j);		/* "no database" */
		}
		if (j->req == NULL) {
			if (j->ucb != NULL) {
				j->ucb((j->rc == 0 && j->code == 200) ?
				    (j->out.d != NULL ? j->out.d : "") : NULL,
				    (j->rc == 0 && j->code == 200) ? NULL :
				    (*j->err != '\0' ? j->err : "failed"), j->uarg);
			}
			bus_job_free(j);
			return;
		}
		if (j->op == BUS_SNAP_GET && j->code == 200) {
			struct sbuf	w = { 0 };

			sb_str(&w, "{\"node\":");
			sb_jstr(&w, j->s1);
			sb_str(&w, ",\"session\":");
			sb_jstr(&w, j->s2);
			sb_str(&w, ",\"snapshot\":");
			sb_jstr(&w, j->out.d != NULL ? j->out.d : "");
			sb_str(&w, "}");
			sb_free(&j->out);
			j->out = w;
		} else if (j->code != 200 && j->rc == 0 && j->out.d == NULL) {
			sb_str(&j->out, "{\"error\":");
			sb_jstr(&j->out, j->err);
			sb_str(&j->out, "}");
		}
		bus_reply(j);
		return;
	default:
		break;
	}

	if (j->cancelled) {
		bus_job_free(j);
		return;
	}
	if (j->rc == BUS_DOWN) {
		/* The database went away: the memory store answers. */
		bus_mode_local(j->err);
		bus_run_mem(j);
	} else if (j->rc == BUS_ERR) {
		sb_free(&j->out);
		j->code = 500;
		sb_str(&j->out, "{\"error\":");
		sb_jstr(&j->out, j->err);
		sb_str(&j->out, "}");
	}

	/* A long-poll that found nothing waits and looks again. */
	if (j->op == BUS_INBOX && j->empty && j->rc != BUS_ERR &&
	    time(NULL) < j->deadline) {
		evtimer_set(&j->retry, bus_retry_cb, j);
		evtimer_add(&j->retry, &tv);
		j->retry_armed = 1;
		return;
	}
	bus_reply(j);
}

static void
bus_pipe_cb(int fd, __unused short events, __unused void *arg)
{
	struct bus_jobs	 list = TAILQ_HEAD_INITIALIZER(list);
	struct bus_job	*j;
	char		 buf[64];

	while (read(fd, buf, sizeof buf) > 0)
		/* nothing */;
	pthread_mutex_lock(&bus_lock);
	while ((j = TAILQ_FIRST(&bus_done)) != NULL) {
		TAILQ_REMOVE(&bus_done, j, entry);
		TAILQ_INSERT_TAIL(&list, j, entry);
	}
	pthread_mutex_unlock(&bus_lock);
	while ((j = TAILQ_FIRST(&list)) != NULL) {
		TAILQ_REMOVE(&list, j, entry);
		bus_finish(j);
	}
}

/* Answer by the database when there is one, else by memory right away. */
static void
bus_dispatch(struct bus_job *j)
{
	if (bus_db_mode && !bus_sync_out) {
		bus_queue(j);
		return;
	}
	j->rc = 0;
	bus_run_mem(j);
	j->rc = 0;
	bus_finish(j);
}

/* ------------------------------------------------------------------------ */
/* HTTP.                                                                    */

static void
bus_conn_closed(__unused struct evhttp_connection *evcon, void *arg)
{
	struct bus_job	*j = arg;

	/* The client left while we held its request: never touch it again. */
	j->req = NULL;
	j->cancelled = 1;
	if (j->retry_armed) {
		evtimer_del(&j->retry);
		j->retry_armed = 0;
		bus_job_free(j);
	}
}

static void
bus_send_json(struct evhttp_request *req, int code, const char *json)
{
	struct evbuffer	*b = evbuffer_new();

	evhttp_add_header(evhttp_request_get_output_headers(req),
	    "Content-Type", "application/json");
	evbuffer_add(b, json, strlen(json));
	evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", b);
	evbuffer_free(b);
}

static void
bus_reply(struct bus_job *j)
{
	struct evhttp_connection	*evcon;

	if (j->req != NULL) {
		evcon = evhttp_request_get_connection(j->req);
		if (evcon != NULL)
			evhttp_connection_set_closecb(evcon, NULL, NULL);
		bus_send_json(j->req, j->code,
		    j->out.d != NULL ? j->out.d : "{}");
	}
	bus_job_free(j);
}

static void
bus_error(struct evhttp_request *req, int code, const char *msg,
    const char *path)
{
	struct sbuf	o = { 0 };

	sb_str(&o, "{\"error\":");
	sb_jstr(&o, msg);
	if (path != NULL) {
		sb_str(&o, ",\"path\":");
		sb_jstr(&o, path);
	}
	sb_str(&o, "}");
	bus_send_json(req, code, o.d);
	sb_free(&o);
}

/* A piece, as raw bytes; its name and type in the headers. */
static void
bus_piece_send(struct evhttp_request *req, const char *id)
{
	struct evkeyvalq	*h;
	struct evbuffer		*b;
	struct stat		 sb;
	char			 mime[128], *path, *data;
	FILE			*f;
	size_t			 len;

	if ((path = bus_piece_find(id, mime, sizeof mime)) == NULL) {
		bus_error(req, 404, "unknown piece", id);
		return;
	}
	if (stat(path, &sb) != 0 || sb.st_size > BUS_PIECE_MAX ||
	    (f = fopen(path, "r")) == NULL) {
		free(path);
		bus_error(req, 500, "cannot read the piece", id);
		return;
	}
	data = xmalloc((size_t)sb.st_size + 1);
	len = fread(data, 1, (size_t)sb.st_size, f);
	fclose(f);
	h = evhttp_request_get_output_headers(req);
	evhttp_add_header(h, "Content-Type", mime);
	evhttp_add_header(h, "X-Piece-Name", strrchr(path, '/') + 1);
	b = evbuffer_new();
	evbuffer_add(b, data, len);
	evhttp_send_reply(req, 200, "OK", b);
	evbuffer_free(b);
	free(data);
	free(path);
}

/* POST /piece {"name", "mime", "data": base64} -> {"id", ...} */
static void
bus_piece_post(struct evhttp_request *req, struct jobj *o)
{
	struct jfield	*f = j_get(o, "data");
	struct bus_job	*j;
	struct sbuf	 out = { 0 };
	u_char		*data, rnd[16];
	char		 id[40], mime[101], *name, *path, *m;
	size_t		 len;
	u_int		 i;

	if (f == NULL || f->type != 's') {
		bus_error(req, 400, "missing 'data' (the file, base64)", NULL);
		return;
	}
	if ((data = bus_b64_decode(f->s, &len)) == NULL) {
		bus_error(req, 400, "'data' is not base64", NULL);
		return;
	}
	if (len > BUS_PIECE_MAX) {
		free(data);
		bus_error(req, 413, "piece too large (6 MB at most)", NULL);
		return;
	}
	f = j_get(o, "name");
	name = bus_piece_name((f != NULL && f->type == 's') ? f->s : NULL);
	m = j_str(o, "mime");
	bus_piece_mime(m, mime, sizeof mime);
	free(m);
	arc4random_buf(rnd, sizeof rnd);
	for (i = 0; i < sizeof rnd; i++)
		snprintf(id + i * 2, 3, "%02x", rnd[i]);
	if ((path = bus_piece_write(id, name, mime, data, len)) == NULL) {
		free(data);
		free(name);
		bus_error(req, 500, "cannot store the piece", NULL);
		return;
	}
	free(path);
	if (bus_db_mode) {
		j = bus_job_new(BUS_PIECE_PUT);
		j->s1 = xstrdup(id);
		j->s2 = xstrdup(name);
		j->s3 = xstrdup(mime);
		j->blob = data;
		j->bloblen = len;
		data = NULL;
		bus_queue(j);
	}
	free(data);
	sb_str(&out, "{\"id\":");
	sb_jstr(&out, id);
	sb_str(&out, ",\"mime\":");
	sb_jstr(&out, mime);
	sb_str(&out, ",\"name\":");
	sb_jstr(&out, name);
	sb_printf(&out, ",\"ok\":true,\"size\":%zu}", len);
	bus_send_json(req, 200, out.d);
	sb_free(&out);
	free(name);
}

/* GET /piece/<id>: from this machine, else from the database. */
static void
bus_piece_get(struct evhttp_request *req, const char *id)
{
	struct bus_job	*j;
	char		*path;

	if (!bus_piece_id_ok(id)) {
		bus_error(req, 400, "bad piece id", id);
		return;
	}
	if ((path = bus_piece_find(id, NULL, 0)) != NULL || !bus_db_mode) {
		free(path);
		bus_piece_send(req, id);
		return;
	}
	j = bus_job_new(BUS_PIECE_GET);
	j->s1 = xstrdup(id);
	j->req = req;
	bus_queue(j);
}

static void
bus_http_cb(struct evhttp_request *req, __unused void *arg)
{
	struct evhttp_uri	*uri;
	struct evkeyvalq	 params;
	struct evbuffer		*in;
	struct evhttp_connection *evcon;
	struct bus_job		*j = NULL;
	struct jobj		 o;
	struct jfield		*f;
	const char		*path, *query, *v, *tok;
	enum evhttp_cmd_type	 cmd = evhttp_request_get_command(req);
	char			*body = NULL, *t;
	size_t			 len;
	long long		 wait;

	/* Optional shared token (@bus-token). */
	tok = options_get_string(global_s_options, "@bus-token");
	if (tok != NULL && *tok != '\0') {
		v = evhttp_find_header(evhttp_request_get_input_headers(req),
		    "X-Agent-Token");
		if (v == NULL || strcmp(v, tok) != 0) {
			bus_error(req, 401, "invalid or missing X-Agent-Token",
			    NULL);
			return;
		}
	}

	uri = evhttp_uri_parse(evhttp_request_get_uri(req));
	if (uri == NULL) {
		bus_error(req, 400, "bad uri", NULL);
		return;
	}
	path = evhttp_uri_get_path(uri);
	if (path == NULL || *path == '\0')
		path = "/";
	query = evhttp_uri_get_query(uri);
	TAILQ_INIT(&params);
	if (query != NULL)
		evhttp_parse_query_str(query, &params);

	if (cmd == EVHTTP_REQ_POST) {
		in = evhttp_request_get_input_buffer(req);
		len = evbuffer_get_length(in);
		if (len > (strcmp(path, "/piece") == 0 ? BUS_PIECE_BODY :
		    BUS_BODY_MAX)) {
			bus_error(req, 413, "body too large", NULL);
			goto out;
		}
		body = xmalloc(len + 1);
		evbuffer_copyout(in, body, len);
		body[len] = '\0';
	}
	memset(&o, 0, sizeof o);
	if (body != NULL)
		(void)j_parse(body, &o);

	if (cmd == EVHTTP_REQ_GET && strcmp(path, "/health") == 0)
		j = bus_job_new(BUS_HEALTH);
	else if (cmd == EVHTTP_REQ_GET && strcmp(path, "/agents") == 0) {
		j = bus_job_new(BUS_AGENTS);
		v = evhttp_find_header(&params, "all");
		j->b1 = (v != NULL && strcmp(v, "1") == 0);
	} else if (cmd == EVHTTP_REQ_POST && strcmp(path, "/register") == 0) {
		t = j_str(&o, "name");
		if (t == NULL || *t == '\0') {
			free(t);
			bus_error(req, 400, "missing 'name'", NULL);
			goto out;
		}
		j = bus_job_new(BUS_REGISTER);
		j->s1 = t;
		if ((j->s2 = j_str(&o, "group")) != NULL)
			j->has_s2 = 1;
		if ((f = j_get(&o, "managed")) != NULL && f->type == 'b') {
			j->has_b1 = 1;
			j->b1 = f->b;
		}
	} else if (cmd == EVHTTP_REQ_POST && strcmp(path, "/rename") == 0) {
		j = bus_job_new(BUS_RENAME);
		j->s1 = j_str(&o, "from");
		j->s2 = j_str(&o, "to");
		if (j->s1 == NULL || *j->s1 == '\0' || j->s2 == NULL ||
		    *j->s2 == '\0') {
			bus_job_free(j);
			bus_error(req, 400, "'from' and 'to' are required", NULL);
			goto out;
		}
	} else if (cmd == EVHTTP_REQ_POST && strcmp(path, "/send") == 0) {
		j = bus_job_new(BUS_SEND);
		j->s1 = j_str(&o, "from");
		f = j_get(&o, "to");
		j->s2 = bus_normalize((f != NULL && f->type == 's') ? f->s : "");
		f = j_get(&o, "subject");
		j->s3 = (f != NULL && f->type == 's') ? xstrdup(f->s) : NULL;
		f = j_get(&o, "body");
		j->s4 = xstrdup((f != NULL && f->type == 's') ? f->s : "");
		if (j->s1 == NULL || *j->s1 == '\0' || *j->s2 == '\0') {
			bus_job_free(j);
			bus_error(req, 400, "'from' and 'to' are required", NULL);
			goto out;
		}
		if (*j->s4 == '\0' && j->s3 == NULL) {
			bus_job_free(j);
			bus_error(req, 400, "message is empty (need 'body' or "
			    "'subject')", NULL);
			goto out;
		}
	} else if (cmd == EVHTTP_REQ_POST && strcmp(path, "/ack") == 0) {
		t = j_str(&o, "agent");
		if (t == NULL || *t == '\0') {
			free(t);
			bus_error(req, 400, "missing 'agent'", NULL);
			goto out;
		}
		j = bus_job_new(BUS_ACK);
		j->s1 = t;
		if ((f = j_get(&o, "upto")) != NULL && f->type == 'n')
			j->n1 = f->n;
	} else if (cmd == EVHTTP_REQ_POST && (strcmp(path, "/join") == 0 ||
	    strcmp(path, "/leave") == 0)) {
		j = bus_job_new(strcmp(path, "/join") == 0 ? BUS_JOIN :
		    BUS_LEAVE);
		j->s1 = j_str(&o, "agent");
		t = j_str(&o, "channel");
		if (j->s1 == NULL || *j->s1 == '\0' || t == NULL || *t == '\0') {
			free(t);
			bus_job_free(j);
			bus_error(req, 400, "'agent' and 'channel' are required",
			    NULL);
			goto out;
		}
		j->s2 = bus_chan(t);
		free(t);
	} else if (cmd == EVHTTP_REQ_GET && strcmp(path, "/channels") == 0)
		j = bus_job_new(BUS_CHANNELS);
	else if (cmd == EVHTTP_REQ_GET && strcmp(path, "/sessions") == 0)
		j = bus_job_new(BUS_SNAP_LIST);
	else if (cmd == EVHTTP_REQ_GET && strcmp(path, "/snapshot") == 0) {
		j = bus_job_new(BUS_SNAP_GET);
		j->s1 = trim_dup(evhttp_find_header(&params, "node"));
		j->s2 = trim_dup(evhttp_find_header(&params, "session"));
	}
	else if (cmd == EVHTTP_REQ_POST && strcmp(path, "/piece") == 0) {
		bus_piece_post(req, &o);
		goto out;
	} else if (cmd == EVHTTP_REQ_GET && strncmp(path, "/piece/", 7) == 0) {
		bus_piece_get(req, path + 7);
		goto out;
	}
	else if (cmd == EVHTTP_REQ_GET && strcmp(path, "/inbox") == 0) {
		v = evhttp_find_header(&params, "agent");
		t = trim_dup(v);
		if (*t == '\0') {
			free(t);
			bus_error(req, 400, "missing 'agent' query param", NULL);
			goto out;
		}
		j = bus_job_new(BUS_INBOX);
		j->s1 = t;
		v = evhttp_find_header(&params, "since");
		j->n1 = (v != NULL) ? strtoll(v, NULL, 10) : 0;
		v = evhttp_find_header(&params, "unread");
		j->b1 = (v != NULL && strcmp(v, "1") == 0);
		v = evhttp_find_header(&params, "wait");
		wait = (v != NULL) ? strtoll(v, NULL, 10) : 0;
		if (wait < 0)
			wait = 0;
		if (wait > BUS_MAX_WAIT)
			wait = BUS_MAX_WAIT;
		/* The first fetch never waits: it is a snapshot. */
		j->deadline = j->b1 ? 0 : time(NULL) + (time_t)wait;
	} else {
		bus_error(req, 404, "unknown route", path);
		goto out;
	}

	j->req = req;
	if (j->op == BUS_INBOX && j->deadline != 0) {
		evcon = evhttp_request_get_connection(req);
		if (evcon != NULL)
			evhttp_connection_set_closecb(evcon, bus_conn_closed, j);
	}
	bus_dispatch(j);
out:
	j_free(&o);
	free(body);
	evhttp_clear_headers(&params);
	evhttp_uri_free(uri);
}

/* ------------------------------------------------------------------------ */
/* Listening, configuration, the tick.                                      */

static void
bus_unbind(void)
{
	if (bus_http != NULL && bus_sock != NULL)
		evhttp_del_accept_socket(bus_http, bus_sock);
	bus_sock = NULL;
	bus_port_bound = 0;
}

static void
bus_bind(int port)
{
	struct evhttp_bound_socket	*s;
	int				 p;

	bus_unbind();
	if (bus_http == NULL) {
		bus_http = evhttp_new(bus_base);
		evhttp_set_allowed_methods(bus_http,
		    EVHTTP_REQ_GET|EVHTTP_REQ_POST);
		evhttp_set_gencb(bus_http, bus_http_cb, NULL);
		evhttp_set_timeout(bus_http, BUS_MAX_WAIT + 10);
	}
	/* The asked port, or the first free one after it. */
	for (p = port; p < port + BUS_PORT_SPAN && p < 65536; p++) {
		s = evhttp_bind_socket_with_handle(bus_http, bus_bind_addr,
		    (ev_uint16_t)p);
		if (s != NULL) {
			bus_sock = s;
			bus_port_bound = p;
			environ_set(global_environ, "AGENT_BUS_URL", 0,
			    "http://127.0.0.1:%d", p);
			log_debug("bus: listening on %s:%d", bus_bind_addr, p);
			return;
		}
	}
	log_debug("bus: no free port from %d", port);
}

static const char *
bus_opt(const char *name, const char *dflt)
{
	const char	*v;

	if (global_s_options == NULL)
		return (dflt);
	v = options_get_string(global_s_options, name);
	return ((v != NULL) ? v : dflt);
}

static void
bus_tick_cb(__unused int fd, __unused short events, __unused void *arg)
{
	struct timeval	 tv = { 2, 0 };
	const char	*on, *url, *addr;
	int		 port;
	time_t		 now = time(NULL);

	on = bus_opt("@bus", "on");
	if (strcmp(on, "on") != 0) {
		if (bus_port_bound != 0)
			bus_unbind();
		goto again;
	}

	/* The listening side: follow @bus-port and @bus-bind live. */
	port = atoi(bus_opt("@bus-port", "4319"));
	if (port <= 0 || port > 65535)
		port = 4319;
	addr = bus_opt("@bus-bind", "0.0.0.0");
	if (*addr == '\0')
		addr = "0.0.0.0";
	if (port != bus_port_asked || bus_bind_addr == NULL ||
	    strcmp(addr, bus_bind_addr) != 0 || bus_port_bound == 0) {
		free(bus_bind_addr);
		bus_bind_addr = xstrdup(addr);
		bus_port_asked = port;
		bus_bind(port);
	}

	/* The database: follow @bus-db live. */
	url = bus_opt("@bus-db", "");
	if (bus_db_url == NULL || strcmp(url, bus_db_url) != 0) {
		free(bus_db_url);
		bus_db_url = xstrdup(url);
		pthread_mutex_lock(&bus_lock);
		free(bus_worker_url);
		bus_worker_url = xstrdup(url);
		bus_worker_reset = 1;
		pthread_mutex_unlock(&bus_lock);
		bus_mode_local(*url == '\0' ? "no database configured" :
		    "connecting");
		bus_last_ping = 0;
	}
	if (!bus_db_mode && *bus_db_url != '\0' && !bus_ping_out &&
	    !bus_sync_out && now - bus_last_ping >= BUS_PING_SECS) {
		bus_last_ping = now;
		bus_ping_out = 1;
		bus_queue(bus_job_new(BUS_PING));
	}
again:
	evtimer_add(&bus_tick_ev, &tv);
}

/* Start the bus: called once, in the server, after it has forked. */
void
bus_init(struct event_base *base)
{
	struct timeval	tv = { 0, 200000 };

	if (bus_started)
		return;
	bus_started = 1;
	bus_base = base;
	bus_piece_init();	/* before the worker, which reads the root */

	mysql_library_init(0, NULL, NULL);
	if (pipe(bus_pipe) != 0)
		fatal("bus pipe");
	setblocking(bus_pipe[0], 0);
	(void)fcntl(bus_pipe[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(bus_pipe[1], F_SETFD, FD_CLOEXEC);
	event_set(&bus_pipe_ev, bus_pipe[0], EV_READ|EV_PERSIST, bus_pipe_cb,
	    NULL);
	event_add(&bus_pipe_ev, NULL);
	if (pthread_create(&bus_thread, NULL, bus_worker, NULL) != 0)
		fatal("bus thread");

	/* First tick soon: the configuration is read by then. */
	evtimer_set(&bus_tick_ev, bus_tick_cb, NULL);
	evtimer_add(&bus_tick_ev, &tv);
}

/* The catalog, from the rest of tmuxv (upgrade.c). */
void
bus_snapshot_put(const char *node, const char *host, const char *session,
    u_int nwin, u_int nconv, const char *text)
{
	struct bus_job	*j;

	if (!bus_db_mode || bus_sync_out)
		return;
	j = bus_job_new(BUS_SNAP_PUT);
	j->s1 = xstrdup(node);
	j->s2 = xstrdup(session);
	j->s3 = xstrdup(host);
	j->s4 = xstrdup(text);
	j->n1 = nwin;
	j->n2 = nconv;
	bus_queue(j);
}

void
bus_snapshot_prune(const char *node, const char *keep)
{
	struct bus_job	*j;

	if (!bus_db_mode || bus_sync_out)
		return;
	j = bus_job_new(BUS_SNAP_PRUNE);
	j->s1 = xstrdup(node);
	j->s2 = xstrdup(keep != NULL ? keep : "");
	bus_queue(j);
}

void
bus_snapshot_list(void (*cb)(const char *, const char *, void *), void *arg)
{
	struct bus_job	*j = bus_job_new(BUS_SNAP_LIST);

	j->ucb = cb;
	j->uarg = arg;
	bus_dispatch(j);
}

void
bus_snapshot_get(const char *node, const char *session,
    void (*cb)(const char *, const char *, void *), void *arg)
{
	struct bus_job	*j = bus_job_new(BUS_SNAP_GET);

	j->s1 = xstrdup(node);
	j->s2 = xstrdup(session);
	j->ucb = cb;
	j->uarg = arg;
	bus_dispatch(j);
}

int
bus_port(void)
{
	return (bus_port_bound);
}

const char *
bus_mode(void)
{
	if (bus_port_bound == 0)
		return ("off");
	return (bus_db_mode ? "db" : "local");
}

const char *
bus_mode_reason(void)
{
	return (bus_mode_why);
}
