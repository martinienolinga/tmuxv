/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <sys/types.h>
#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

static void	 status_message_callback(int, short, void *);
static void	 status_timer_callback(int, short, void *);

static char	*status_prompt_find_history_file(void);
static const char *status_prompt_up_history(u_int *, u_int);
static const char *status_prompt_down_history(u_int *, u_int);
static void	 status_prompt_add_history(const char *, u_int);

static char	*status_prompt_complete(struct client *, const char *, u_int);
static char	*status_prompt_complete_window_menu(struct client *,
		     struct session *, const char *, u_int, char);

struct status_prompt_menu {
	struct client	 *c;
	u_int		  start;
	u_int		  size;
	char		**list;
	char		  flag;
};

static const char	*prompt_type_strings[] = {
	"command",
	"search",
	"target",
	"window-target"
};

/* Status prompt history. */
char		**status_prompt_hlist[PROMPT_NTYPES];
u_int		  status_prompt_hsize[PROMPT_NTYPES];

/* Find the history file to load/save from/to. */
static char *
status_prompt_find_history_file(void)
{
	const char	*home, *history_file;
	char		*path;

	history_file = options_get_string(global_options, "history-file");
	if (*history_file == '\0')
		return (NULL);
	if (*history_file == '/')
		return (xstrdup(history_file));

	if (history_file[0] != '~' || history_file[1] != '/')
		return (NULL);
	if ((home = find_home()) == NULL)
		return (NULL);
	xasprintf(&path, "%s%s", home, history_file + 1);
	return (path);
}

/* Add loaded history item to the appropriate list. */
static void
status_prompt_add_typed_history(char *line)
{
	char			*typestr;
	enum prompt_type	 type = PROMPT_TYPE_INVALID;

	typestr = strsep(&line, ":");
	if (line != NULL)
		type = status_prompt_type(typestr);
	if (type == PROMPT_TYPE_INVALID) {
		/*
		 * Invalid types are not expected, but this provides backward
		 * compatibility with old history files.
		 */
		if (line != NULL)
			*(--line) = ':';
		status_prompt_add_history(typestr, PROMPT_TYPE_COMMAND);
	} else
		status_prompt_add_history(line, type);
}

/* Load status prompt history from file. */
void
status_prompt_load_history(void)
{
	FILE	*f;
	char	*history_file, *line, *tmp;
	size_t	 length;

	if ((history_file = status_prompt_find_history_file()) == NULL)
		return;
	log_debug("loading history from %s", history_file);

	f = fopen(history_file, "r");
	if (f == NULL) {
		log_debug("%s: %s", history_file, strerror(errno));
		free(history_file);
		return;
	}
	free(history_file);

	for (;;) {
		if ((line = fgetln(f, &length)) == NULL)
			break;

		if (length > 0) {
			if (line[length - 1] == '\n') {
				line[length - 1] = '\0';
				status_prompt_add_typed_history(line);
			} else {
				tmp = xmalloc(length + 1);
				memcpy(tmp, line, length);
				tmp[length] = '\0';
				status_prompt_add_typed_history(tmp);
				free(tmp);
			}
		}
	}
	fclose(f);
}

/* Save status prompt history to file. */
void
status_prompt_save_history(void)
{
	FILE	*f;
	u_int	 i, type;
	char	*history_file;

	if ((history_file = status_prompt_find_history_file()) == NULL)
		return;
	log_debug("saving history to %s", history_file);

	f = fopen(history_file, "w");
	if (f == NULL) {
		log_debug("%s: %s", history_file, strerror(errno));
		free(history_file);
		return;
	}
	free(history_file);

	for (type = 0; type < PROMPT_NTYPES; type++) {
		for (i = 0; i < status_prompt_hsize[type]; i++) {
			fputs(prompt_type_strings[type], f);
			fputc(':', f);
			fputs(status_prompt_hlist[type][i], f);
			fputc('\n', f);
		}
	}
	fclose(f);

}

/* Status timer callback. */
static void
status_timer_callback(__unused int fd, __unused short events, void *arg)
{
	struct client	*c = arg;
	struct session	*s = c->session;
	struct timeval	 tv;

	evtimer_del(&c->status.timer);

	if (s == NULL)
		return;

	if (c->message_string == NULL && c->prompt_string == NULL)
		c->flags |= CLIENT_REDRAWSTATUS;

	timerclear(&tv);
	tv.tv_sec = options_get_number(s->options, "status-interval");

	if (tv.tv_sec != 0)
		evtimer_add(&c->status.timer, &tv);
	log_debug("client %p, status interval %d", c, (int)tv.tv_sec);
}

/* Start status timer for client. */
void
status_timer_start(struct client *c)
{
	struct session	*s = c->session;

	if (event_initialized(&c->status.timer))
		evtimer_del(&c->status.timer);
	else
		evtimer_set(&c->status.timer, status_timer_callback, c);

	if (s != NULL && options_get_number(s->options, "status"))
		status_timer_callback(-1, 0, c);
}

/* Start status timer for all clients. */
void
status_timer_start_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry)
		status_timer_start(c);
}

/* Update status cache. */
void
status_update_cache(struct session *s)
{
	s->statuslines = options_get_number(s->options, "status");
	if (s->statuslines == 0)
		s->statusat = -1;
	else if (options_get_number(s->options, "status-position") == 0)
		s->statusat = 0;
	else
		s->statusat = 1;
}

/* Get screen line of status line. -1 means off. */
int
status_at_line(struct client *c)
{
	struct session	*s = c->session;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (-1);
	if (s->statusat != 1)
		return (s->statusat);
	return (c->tty.sy - status_line_size(c));
}

/* Get size of status line for client's session. 0 means off. */
u_int
status_line_size(struct client *c)
{
	struct session	*s = c->session;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (0);
	if (s == NULL)
		return (options_get_number(global_s_options, "status"));
	return (s->statuslines);
}

/*
 * MENU BAR: return the @menu-bar-format option string, or NULL if the menu bar
 * is disabled (option unset or empty).
 */
const char *
menu_bar_format(struct client *c)
{
	struct session		*s = c->session;
	struct options_entry	*o;
	const char		*value;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (NULL);
	if (s == NULL)
		return (NULL);
	o = options_get(s->options, "@menu-bar-format");
	if (o == NULL)
		return (NULL);
	value = options_get_string(s->options, "@menu-bar-format");
	if (value == NULL || *value == '\0')
		return (NULL);
	return (value);
}

/*
 * MENU BAR: the menu definition, compiled into the binary (façon Turbo Vision).
 * Each entry: label, shortcut key shown/pressable, tmux command run on choice.
 * A NULL name is a separator line.
 */
struct menu_bar_entry {
	const char	*name;
	key_code	 key;
	const char	*command;
};
struct menu_bar_def {
	const char			*title;
	char				 mnemonic; /* Alt+this opens it */
	const struct menu_bar_entry	*items;
	u_int				 count;
};

/*
 * MENU BAR: Turbo Vision colour theme (light-grey bar, black text, red
 * mnemonics, green selection, black-on-grey box).
 */
#define MENU_BAR_STYLE		"fill=#c0c0c0,bg=#c0c0c0,fg=#000000"
#define MENU_BAR_MNEMONIC	"#[fg=#cc0000]"
/* Palette moved to tmux.h: menus opened elsewhere (menu.c) share it. */

static const struct menu_bar_entry menu_bar_fichier[] = {
	{ "Nouvelle fenêtre",	'n', "new-window" },
	{ "Nouvelle fenêtre après", 'a', "new-window -a" },
	{ "Renommer la fenêtre", 'r',
	    "command-prompt -I \"#W\" { rename-window \"%%\" }" },
	{ NULL, KEYC_NONE, NULL },
	{ "Fermer la fenêtre",	'x',
	    "confirm-before -p \"Fermer la fenêtre ?\" kill-window" },
	{ NULL, KEYC_NONE, NULL },
	{ "Restaurer une session...", 's', "restore-session -m" },
	{ NULL, KEYC_NONE, NULL },
	{ "Détacher",		'd', "detach-client" },
	{ "Tuer le serveur tmuxv", 'q',
	    "confirm-before -p \"Tuer le serveur tmuxv ?\" kill-server" },
};
static const struct menu_bar_entry menu_bar_edition[] = {
	{ "Mode copie",		'c', "copy-mode" },
	{ "Coller",		'v', "paste-buffer -p" },
	{ "Choisir un tampon",	'b', "display-buffers" },
	{ NULL, KEYC_NONE, NULL },
	{ "Défiler vers le haut", 'u', "copy-mode -u" },
	{ NULL, KEYC_NONE, NULL },
	{ "Effacer l'historique", 'e', "clear-history" },
};
static const struct menu_bar_entry menu_bar_affichage[] = {
	{ "Découper horizontalement", 'h', "split-window -h" },
	{ "Découper verticalement",   'v', "split-window -v" },
	{ NULL, KEYC_NONE, NULL },
	{ "Disposition égale H", '1', "select-layout even-horizontal" },
	{ "Disposition égale V", '2', "select-layout even-vertical" },
	{ "Principale H",	'3', "select-layout main-horizontal" },
	{ "Principale V",	'4', "select-layout main-vertical" },
	{ "Mosaïque",		'5', "select-layout tiled" },
	{ NULL, KEYC_NONE, NULL },
	{ "Zoom panneau",	'z', "resize-pane -Z" },
	{ NULL, KEYC_NONE, NULL },
	/* Same toggles as prefix+B / prefix+F (key-bindings.c). */
	{ "Barre de menus (préfixe B)", 'b',
	  "if -F '#{==:#{@menu-bar},on}' { set -g @menu-bar off } { set -g @menu-bar on }" },
	{ "Mode fenêtré (préfixe F)", 'f',
	  "if -F '#{==:#{@desktop},on}' { set -g @desktop off } { set -g @desktop on }" },
};
static const struct menu_bar_entry menu_bar_panneau[] = {
	{ "Panneau suivant",	'o', "select-pane -t :.+" },
	{ "Panneau précédent",	'p', "select-pane -t :.-" },
	{ NULL, KEYC_NONE, NULL },
	{ "Aller à gauche",	'h', "select-pane -L" },
	{ "Aller en bas",	'j', "select-pane -D" },
	{ "Aller en haut",	'k', "select-pane -U" },
	{ "Aller à droite",	'l', "select-pane -R" },
	{ NULL, KEYC_NONE, NULL },
	{ "Faire pivoter",	'r', "rotate-window" },
	{ "Marquer",		'm', "select-pane -m" },
	{ NULL, KEYC_NONE, NULL },
	{ "Fermer le panneau",	'x',
	    "confirm-before -p \"Fermer le panneau ?\" kill-pane" },
};
static const struct menu_bar_entry menu_bar_fenetre[] = {
	{ "Suivante",		'n', "next-window" },
	{ "Précédente",		'p', "previous-window" },
	{ "Dernière",		'l', "last-window" },
	{ NULL, KEYC_NONE, NULL },
	{ "Déplacer à gauche",	'g', "swap-window -t -1" },
	{ "Déplacer à droite",	'd', "swap-window -t +1" },
	{ NULL, KEYC_NONE, NULL },
	{ "Lister les fenêtres", 'w', "display-windows" },
	{ NULL, KEYC_NONE, NULL },
	{ "Gestionnaire Claude Code", 'c', "claude-manager" },
};
static const struct menu_bar_entry menu_bar_session[] = {
	{ "Nouvelle session",	'n', "new-session" },
	{ "Renommer la session", 'r',
	    "command-prompt -I \"#S\" { rename-session \"%%\" }" },
	{ "Lister les sessions", 's', "display-sessions" },
	{ NULL, KEYC_NONE, NULL },
	{ "Session suivante",	'.', "switch-client -n" },
	{ "Session précédente",	',', "switch-client -p" },
	{ NULL, KEYC_NONE, NULL },
	{ "Détacher",		'd', "detach-client" },
	{ "Tuer la session",	'k',
	    "confirm-before -p \"Tuer la session ?\" kill-session" },
};
static const struct menu_bar_entry menu_bar_parametres[] = {
	{ "Configurer…",	'c', "display-form" },
	{ NULL, KEYC_NONE, NULL },
	{ "Recharger ~/.tmux.conf", 'r', "source-file ~/.tmux.conf" },
};
static const struct menu_bar_entry menu_bar_aide[] = {
	{ "À propos", 'a', "display-about" },
	{ "Raccourcis clavier",	'k', "display-keys" },
	{ "Liste des commandes", 'c', "display-commands" },
};

#define MENU_BAR_ENTRY(x) (x), nitems(x)
static const struct menu_bar_def menu_bar_menus[] = {
	{ "Fichier",   'f', MENU_BAR_ENTRY(menu_bar_fichier) },
	{ "Édition",   'd', MENU_BAR_ENTRY(menu_bar_edition) },
	{ "Affichage", 'a', MENU_BAR_ENTRY(menu_bar_affichage) },
	{ "Panneau",   'p', MENU_BAR_ENTRY(menu_bar_panneau) },
	{ "Fenêtre",   'n', MENU_BAR_ENTRY(menu_bar_fenetre) },
	{ "Session",   's', MENU_BAR_ENTRY(menu_bar_session) },
	{ "Paramètres", 'm', MENU_BAR_ENTRY(menu_bar_parametres) },
	{ "Aide",      'i', MENU_BAR_ENTRY(menu_bar_aide) },
};
#define MENU_BAR_COUNT nitems(menu_bar_menus)

/*
 * DESKTOP: geometry of pane wp's own vertical scrollbar inside a window
 * frame (rx, ry, rw, rh) - every pane has one, like every TVision view: the
 * frame's right column when the pane touches the right edge, otherwise the
 * pane's own LAST column, so the separator column to its right stays a plain
 * draggable resize band (scrollbar, then the band on its right).
 * Returns 0 if the pane is hidden or too short for ▲ + track + ▼.
 */
int
desktop_pane_scrollbar(struct client *c, struct window_pane *wp, u_int rx,
    u_int ry, u_int rw, u_int rh, u_int *bx, u_int *by, u_int *bsize)
{
	u_int	iw = (rw > 2) ? rw - 2 : 0, ih = (rh > 2) ? rh - 2 : 0, size;
	u_int	inset;

	if (!window_pane_visible(wp) || wp->yoff >= ih || wp->xoff >= iw)
		return (0);
	size = wp->sy;
	if (wp->yoff + size > ih)
		size = ih - wp->yoff;
	if (size < 3)
		return (0);
	/* A manager window's panes are shifted right by the list column. */
	inset = claude_inset(c, wp->window);
	if (inset >= iw)
		return (0);
	if (inset + wp->xoff + wp->sx >= iw)
		*bx = rx + rw - 1;			/* frame column */
	else if (wp->sx >= 2)
		*bx = rx + 1 + inset + wp->xoff + wp->sx - 1;	/* last column */
	else
		return (0);				/* too narrow to spare one */
	*by = ry + 1 + wp->yoff;
	*bsize = size;
	return (1);
}

/* MENU BAR: size of the menu bar (0 or 1 line), gated by the @menu-bar option. */
u_int
menu_bar_size(struct client *c)
{
	struct session		*s = c->session;
	struct options_entry	*o;
	const char		*v;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (0);
	if (s == NULL)
		return (0);
	o = options_get(s->options, "@menu-bar");
	if (o == NULL)
		return (0);
	v = options_get_string(s->options, "@menu-bar");
	if (v == NULL || *v == '\0' ||
	    strcmp(v, "off") == 0 || strcmp(v, "0") == 0)
		return (0);
	return (1);
}

/* MENU BAR: build the rendered format string (Turbo Vision theme + ranges). */
char *
menu_bar_build(void)
{
	char		*s, *tmp;
	const char	*title;
	u_int		 i;
	int		 pos;

	xasprintf(&s, "#[%s,bold]", MENU_BAR_STYLE);
	for (i = 0; i < MENU_BAR_COUNT; i++) {
		title = menu_bar_menus[i].title;
		/* Find the mnemonic letter (case-insensitive) to colour red. */
		pos = -1;
		if (menu_bar_menus[i].mnemonic != '\0') {
			const char *p = title;
			while (*p != '\0') {
				if (tolower((u_char)*p) ==
				    tolower((u_char)menu_bar_menus[i].mnemonic)) {
					pos = (int)(p - title);
					break;
				}
				p++;
			}
		}
		if (pos < 0) {
			xasprintf(&tmp, "%s#[range=user|menu_%u] %s #[norange]",
			    s, i, title);
		} else {
			xasprintf(&tmp,
			    "%s#[range=user|menu_%u] %.*s%s%c#[fg=#000000]%s "
			    "#[norange]", s, i, pos, title, MENU_BAR_MNEMONIC,
			    title[pos], title + pos + 1);
		}
		free(s);
		s = tmp;
	}
	return (s);
}

/* MENU BAR: column where the top-level menu at idx starts (from its range). */
static u_int
menu_bar_index_x(struct client *c, u_int idx)
{
	struct style_range	*sr;
	char			 name[16];

	xsnprintf(name, sizeof name, "menu_%u", idx);
	TAILQ_FOREACH(sr, &c->menubar_ranges, entry) {
		if (sr->type == STYLE_RANGE_USER &&
		    strcmp(sr->string, name) == 0)
			return (sr->start);
	}
	return (0);
}

/* MENU BAR: handle Alt+mnemonic to open a menu from the keyboard. */
int
menu_bar_key(struct client *c, key_code key)
{
	u_int	i;
	char	ch;

	if (menu_bar_size(c) == 0 || c->overlay_draw != NULL)
		return (0);
	if (!(key & KEYC_META))
		return (0);
	if ((key & KEYC_MASK_KEY) > 0x7f)
		return (0);
	ch = (char)(key & KEYC_MASK_KEY);
	for (i = 0; i < MENU_BAR_COUNT; i++) {
		if (menu_bar_menus[i].mnemonic != '\0' &&
		    tolower((u_char)ch) ==
		    tolower((u_char)menu_bar_menus[i].mnemonic)) {
			menu_bar_open(c, i, menu_bar_index_x(c, i));
			return (1);
		}
	}
	return (0);
}

/* MENU BAR: open the dropdown for menu index at column px (native, in C). */
void
menu_bar_open(struct client *c, u_int idx, u_int px)
{
	const struct menu_bar_def	*def;
	struct menu			*menu;
	struct menu_item		 it;
	struct cmd_find_state		 fs;
	u_int				 i;

	if (idx >= MENU_BAR_COUNT)
		return;
	def = &menu_bar_menus[idx];

	cmd_find_from_client(&fs, c, 0);
	menu = menu_create(def->title);
	for (i = 0; i < def->count; i++) {
		memset(&it, 0, sizeof it);
		it.name = def->items[i].name;
		it.key = (def->items[i].name != NULL) ?
		    def->items[i].key : KEYC_NONE;
		it.command = def->items[i].command;
		menu_add_item(menu, &it, NULL, c, &fs);
	}
	if (menu->count == 0) {
		menu_free(menu);
		return;
	}
	if (menu_display_menubar(menu, 0, NULL, px, 1, c, BOX_LINES_DEFAULT,
	    MENU_BAR_MENU_STYLE, MENU_BAR_SELECTED_STYLE,
	    MENU_BAR_BORDER_STYLE, &fs, idx) != 0)
		menu_free(menu);
}

/*
 * DESKTOP: Turbo Vision style desktop. When @desktop is on, the window is
 * inset from the screen edges; the surrounding area is painted as a hatched
 * blue desktop and the window content is wrapped in a framed, titled box.
 */
#define DESKTOP_TOP	2	/* reserved rows above content (incl. title) */
#define DESKTOP_BOTTOM	2
#define DESKTOP_LEFT	3	/* reserved cols left of content */
#define DESKTOP_RIGHT	3

int
desktop_inset(struct client *c, u_int *top, u_int *bottom, u_int *left,
    u_int *right)
{
	struct session		*s = c->session;
	struct options_entry	*o;
	const char		*v;

	if (top != NULL)
		*top = *bottom = *left = *right = 0;
	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (0);
	if (s == NULL)
		return (0);
	o = options_get(s->options, "@desktop");
	if (o == NULL)
		return (0);
	v = options_get_string(s->options, "@desktop");
	if (v == NULL || *v == '\0' ||
	    strcmp(v, "off") == 0 || strcmp(v, "0") == 0)
		return (0);
	if (c->tty.sx < DESKTOP_LEFT + DESKTOP_RIGHT + 10 ||
	    c->tty.sy < DESKTOP_TOP + DESKTOP_BOTTOM + 6)
		return (0); /* too small; disable to stay safe */
	if (top != NULL) {
		*top = DESKTOP_TOP;
		*bottom = DESKTOP_BOTTOM;
		*left = DESKTOP_LEFT;
		*right = DESKTOP_RIGHT;
	}
	return (1);
}

/* DESKTOP: is the desktop enabled and is the terminal big enough? */
int
desktop_enabled(struct client *c)
{
	struct session		*s = c->session;
	struct options_entry	*o;
	const char		*v;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (0);
	if (s == NULL)
		return (0);
	o = options_get(s->options, "@desktop");
	if (o == NULL)
		return (0);
	v = options_get_string(s->options, "@desktop");
	if (v == NULL || *v == '\0' ||
	    strcmp(v, "off") == 0 || strcmp(v, "0") == 0)
		return (0);
	if (c->tty.sx < 24 ||
	    c->tty.sy < status_line_size(c) + menu_bar_size(c) + 8)
		return (0);
	return (1);
}

/*
 * DESKTOP: get the floating window rectangle (desktop-area coords). Lazily
 * initialises to a centred window and clamps it inside the desktop area.
 */
int
desktop_get_rect_w(struct client *c, struct window *w, u_int *x, u_int *y,
    u_int *ww, u_int *hh)
{
	u_int	area_w, area_h, off, dx, dy, dw, dh;

	if (!desktop_enabled(c) || w == NULL || w->desktop_ph)
		return (0);
	area_w = c->tty.sx;
	area_h = c->tty.sy - status_line_size(c) - menu_bar_size(c);
	if (area_w < 24 || area_h < 8)
		return (0);

	if (w->desktop_w == 0 || w->desktop_h == 0) {
		w->desktop_w = (area_w * 3) / 4;
		w->desktop_h = (area_h * 3) / 4;
		if (w->desktop_w < 20)
			w->desktop_w = area_w;
		if (w->desktop_h < 6)
			w->desktop_h = area_h;
		off = w->id % 5; /* cascade like Turbo Vision */
		w->desktop_x = (area_w - w->desktop_w) / 2 + off * 2;
		w->desktop_y = (area_h - w->desktop_h) / 2 + off;
	}

	/*
	 * PERSISTENCE: clamp to the current area for DISPLAY only, in locals -
	 * never overwrite the stored (desired) rect. That way the window is
	 * restored verbatim when the space comes back (e.g. detaching, then
	 * re-attaching on a larger terminal after a smaller one). On the same
	 * size, the returned rect is identical to what the user set.
	 */
	dx = w->desktop_x;
	dy = w->desktop_y;
	dw = w->desktop_w;
	dh = w->desktop_h;
	/*
	 * A maximised window fills the desktop area BY DEFINITION, so recompute
	 * it instead of trusting what was stored when it was maximised: the
	 * area grows and shrinks when the menu bar or the status line is
	 * toggled, and a stale size left a strip of desktop showing along the
	 * bottom. The restore rect (desktop_z*) is untouched.
	 */
	if (w->desktop_zoomed) {
		dx = 0;
		dy = 0;
		dw = area_w;
		dh = area_h;
	}
	if (dw > area_w)
		dw = area_w;
	if (dh > area_h)
		dh = area_h;
	if (dx + dw > area_w)
		dx = area_w - dw;
	if (dy + dh > area_h)
		dy = area_h - dh;

	*x = dx;
	*y = dy;
	*ww = dw;
	*hh = dh;
	return (1);
}

/*
 * DESKTOP: maximise/restore a window, the one place that does it. The restore
 * rect (desktop_z*) is saved on the way up and put back on the way down, so a
 * maximised window always comes back exactly where it was - whichever gesture
 * asked for it (the [^] box, a double click on the title bar, a key binding).
 */
void
desktop_toggle_zoom(struct client *c, struct window *w)
{
	u_int	aw, ah;

	if (w == NULL || !desktop_get_area(c, &aw, &ah))
		return;
	if (w->desktop_zoomed) {
		w->desktop_x = w->desktop_zx;
		w->desktop_y = w->desktop_zy;
		w->desktop_w = w->desktop_zw;
		w->desktop_h = w->desktop_zh;
		w->desktop_zoomed = 0;
	} else {
		w->desktop_zx = w->desktop_x;
		w->desktop_zy = w->desktop_y;
		w->desktop_zw = w->desktop_w;
		w->desktop_zh = w->desktop_h;
		w->desktop_x = 0;
		w->desktop_y = 0;
		w->desktop_w = aw;
		w->desktop_h = ah;
		w->desktop_zoomed = 1;
	}
	recalculate_sizes();
	server_redraw_client(c);
}

/* DESKTOP: full desktop area size (cols/rows available for windows). */
int
desktop_get_area(struct client *c, u_int *aw, u_int *ah)
{
	u_int	area_w, area_h;

	if (!desktop_enabled(c))
		return (0);
	area_w = c->tty.sx;
	area_h = c->tty.sy - status_line_size(c) - menu_bar_size(c);
	if (area_w < 24 || area_h < 8)
		return (0);
	*aw = area_w;
	*ah = area_h;
	return (1);
}

int
desktop_get_rect(struct client *c, u_int *x, u_int *y, u_int *w, u_int *h)
{
	if (c->session == NULL || c->session->curw == NULL)
		return (0);
	return (desktop_get_rect_w(c, c->session->curw->window, x, y, w, h));
}

u_int
desktop_top(struct client *c)
{
	u_int	x, y, w, h;

	return (desktop_get_rect(c, &x, &y, &w, &h) ? y + 1 : 0);
}

u_int
desktop_left(struct client *c)
{
	u_int	x, y, w, h;

	return (desktop_get_rect(c, &x, &y, &w, &h) ? x + 1 : 0);
}

u_int
desktop_vert_w(struct client *c, struct window *w)
{
	u_int	x, y, ww, hh, area_h;

	if (!desktop_get_rect_w(c, w, &x, &y, &ww, &hh))
		return (0);
	area_h = c->tty.sy - status_line_size(c) - menu_bar_size(c);
	return (area_h - (hh - 2));
}

u_int
desktop_horiz_w(struct client *c, struct window *w)
{
	u_int	x, y, ww, hh;

	if (!desktop_get_rect_w(c, w, &x, &y, &ww, &hh))
		return (0);
	/* A manager window also gives up its list column + band. */
	return (c->tty.sx - (ww - 2) + claude_inset(c, w));
}

/* CLAUDE: is this window the conversation manager? */
int
claude_manager(struct window *w)
{
	return (w != NULL && w->claude_mgr);
}

/*
 * CLAUDE: width of the conversation list column, clamped so the conversation
 * itself always keeps room. 0 when this is not a manager window (or the
 * window is too narrow to split at all).
 */
u_int
claude_list_width(struct client *c, struct window *w)
{
	u_int	x, y, ww, hh, lw;

	if (!claude_manager(w) || !desktop_get_rect_w(c, w, &x, &y, &ww, &hh))
		return (0);
	if (ww < 2 + 12 + 8)		/* frame + conversation + list */
		return (0);
	lw = (w->claude_listw != 0) ? w->claude_listw : CLAUDE_LISTW;
	if (lw > ww - 2 - 12)
		lw = ww - 2 - 12;
	if (lw < 8)
		lw = 8;
	return (lw);
}

/*
 * DESKTOP: is this window the empty-desktop placeholder? Turbo Vision leaves a
 * bare desktop when the last window is closed; tmux would instead destroy the
 * session (and, with the last session, the server). The placeholder is a window
 * kept alive with a single DEAD pane and drawn as nothing at all.
 */
int
desktop_placeholder(struct window *w)
{
	return (w != NULL && w->desktop_ph);
}

/*
 * DESKTOP: is the windowed mode requested for this session? Unlike
 * desktop_enabled(), this asks the SESSION and not a client, so it also holds
 * while the session is detached - a detached session must not lose its desktop
 * (and itself) just because its last shell exited.
 */
int
desktop_session_on(struct session *s)
{
	struct options_entry	*o;
	const char		*v;

	if (s == NULL)
		return (0);
	if ((o = options_get(s->options, "@desktop")) == NULL)
		return (0);
	v = options_get_string(s->options, "@desktop");
	if (v == NULL || *v == '\0' ||
	    strcmp(v, "off") == 0 || strcmp(v, "0") == 0)
		return (0);
	return (1);
}

/*
 * DESKTOP: should closing this window leave an empty desktop instead of ending
 * the session? Only for the simple, real case: the window belongs to exactly
 * one session and it is that session's last window, with the windowed mode on.
 */
int
desktop_keep_window(struct window *w)
{
	struct session	*s, *found = NULL;

	/* A window with no pane left cannot be kept: there is nothing to hold. */
	if (w == NULL || w->desktop_ph || TAILQ_EMPTY(&w->panes))
		return (0);

	RB_FOREACH(s, sessions, &sessions) {
		if (!session_has(s, w))
			continue;
		if (found != NULL)
			return (0);	/* linked in several sessions */
		found = s;
	}
	if (found == NULL || winlink_count(&found->windows) != 1)
		return (0);
	return (desktop_session_on(found));
}

/*
 * DESKTOP: turn a window into the empty-desktop placeholder. Its panes are
 * reduced to one, whose process is hung up by closing the pty - the pane OBJECT
 * stays (like remain-on-exit), which is what keeps the window, the session and
 * the server alive with nothing displayed.
 */
void
desktop_make_placeholder(struct window *w)
{
	struct window_pane	*wp;

	if (w == NULL || w->desktop_ph)
		return;

	/* Reduce to a single pane, exactly as respawning a window does. */
	if ((wp = TAILQ_FIRST(&w->panes)) != NULL) {
		TAILQ_REMOVE(&w->panes, wp, entry);
		layout_free(w);
		window_destroy_panes(w);
		TAILQ_INSERT_HEAD(&w->panes, wp, entry);
		window_pane_resize(wp, w->sx, w->sy);
		layout_init(w, wp);
		w->active = NULL;
		window_set_active_pane(w, wp, 0);

		window_pane_reset_mode_all(wp);
		if (wp->fd != -1) {
			bufferevent_free(wp->event);
			wp->event = NULL;
			close(wp->fd);	/* SIGHUP to the process group */
			wp->fd = -1;
		}
		wp->base.mode &= ~MODE_CURSOR;	/* no cursor on a bare desktop */
	}

	w->flags &= ~WINDOW_ZOOMED;
	w->desktop_ph = 1;
	free(w->name);
	w->name = xstrdup("Bureau");
	options_set_number(w->options, "automatic-rename", 0);
	server_redraw_window(w);
}

/*
 * CLAUDE: geometry of the scrollable part of the manager list. The header and
 * the "[+ Nouvelle]" button are pinned, everything between them scrolls:
 * conversations, then the "Reprendre" section. Returns the viewport height, the
 * number of items and the scroll range (0 = everything fits).
 */
int
claude_list_geom(struct client *c, struct window *w, u_int *vh, u_int *total,
    u_int *range)
{
	u_int	x, y, ww, hh, ih, nconv, nsess, t;

	if (!claude_manager(w) || !desktop_get_rect_w(c, w, &x, &y, &ww, &hh))
		return (0);
	ih = (hh > 2) ? hh - 2 : 0;
	if (ih < 3)
		return (0);

	nconv = window_count_panes(w);
	nsess = claude_sess_count();
	t = nconv + (nsess != 0 ? 1 + nsess : 0);

	*vh = ih - 2;			/* header and button are pinned */
	*total = t;
	*range = (t > *vh) ? t - *vh : 0;

	/* A shrinking list must not leave the view past the end. */
	if (w->claude_scroll > *range)
		w->claude_scroll = *range;
	return (1);
}

/*
 * CLAUDE: where the list's own scrollbar goes - the LAST column of the list
 * column, alongside the viewport, exactly like a pane's bar takes its last
 * content column. Returns 0 when there is nothing to scroll.
 */
int
claude_list_scrollbar(struct client *c, struct window *w, u_int rx, u_int ry,
    __unused u_int rw, __unused u_int rh, u_int *bx, u_int *by, u_int *bsize)
{
	u_int	lw = claude_list_width(c, w), vh, total, range;

	if (lw < 4 || !claude_list_geom(c, w, &vh, &total, &range))
		return (0);
	if (range == 0 || vh < 3)
		return (0);
	*bx = rx + lw;			/* last column of the list */
	*by = ry + 2;			/* first scrollable row */
	*bsize = vh;
	return (1);
}

/* CLAUDE: scroll the list by delta rows, clamped. */
void
claude_list_scroll(struct window *w, int delta)
{
	struct client	*c;
	u_int		 vh, total, range;
	int		 v;

	if (w == NULL)
		return;
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL && c->session->curw != NULL &&
		    c->session->curw->window == w)
			break;
	}
	if (c == NULL || !claude_list_geom(c, w, &vh, &total, &range))
		return;
	v = (int)w->claude_scroll + delta;
	if (v < 0)
		v = 0;
	if (v > (int)range)
		v = (int)range;
	w->claude_scroll = (u_int)v;
	server_redraw_client(c);
}

/* CLAUDE: thumb dragged to row p of a track of `track` cells. */
void
claude_list_scroll_to(struct window *w, u_int p, u_int track)
{
	struct client	*c;
	u_int		 vh, total, range;

	if (w == NULL)
		return;
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL && c->session->curw != NULL &&
		    c->session->curw->window == w)
			break;
	}
	if (c == NULL || !claude_list_geom(c, w, &vh, &total, &range))
		return;
	w->claude_scroll = scrollbar_value(p, range, track);
	if (w->claude_scroll > range)
		w->claude_scroll = range;
	server_redraw_client(c);
}

/*
 * CLAUDE: what the manager list holds on interior row `lrow` (0 = the row just
 * under the title bar). The drawing and the mouse BOTH go through this: two
 * separate layouts drift apart, and a click then lands one row off.
 *
 * Layout:   0            "Conversations"
 *           1..n         the live conversations
 *           n+1          "Reprendre" (only when there are saved sessions)
 *           n+2..        the saved sessions, most recent first
 *           ih-1         the "[+ Nouvelle]" button, pinned at the bottom
 */
enum claude_row
claude_list_row(struct client *c, struct window *w, u_int lrow, u_int *index)
{
	u_int	x, y, ww, hh, ih, nconv, first, nsess, item, vh, total, range;

	*index = 0;
	if (!claude_manager(w) || !desktop_get_rect_w(c, w, &x, &y, &ww, &hh))
		return (CLAUDE_ROW_NONE);
	ih = (hh > 2) ? hh - 2 : 0;
	if (ih < 3 || lrow >= ih)
		return (CLAUDE_ROW_NONE);
	if (lrow == ih - 1)
		return (CLAUDE_ROW_NEW);
	if (lrow == 0)
		return (CLAUDE_ROW_HEADER);

	/* Row 1 shows item `claude_scroll`, so the view can slide. */
	if (!claude_list_geom(c, w, &vh, &total, &range))
		return (CLAUDE_ROW_NONE);
	item = (lrow - 1) + w->claude_scroll;
	if (item >= total)
		return (CLAUDE_ROW_NONE);

	nconv = window_count_panes(w);
	if (item < nconv) {
		*index = item;
		return (CLAUDE_ROW_CONV);
	}
	if ((nsess = claude_sess_count()) == 0)
		return (CLAUDE_ROW_NONE);
	if (item == nconv)
		return (CLAUDE_ROW_SESSHDR);
	first = nconv + 1;
	if (item > nconv && item - first < nsess) {
		*index = item - first;
		return (CLAUDE_ROW_SESS);
	}
	return (CLAUDE_ROW_NONE);
}

/* CLAUDE: columns taken from the pane area (list column + its band). */
u_int
claude_inset(struct client *c, struct window *w)
{
	u_int	lw = claude_list_width(c, w);

	return ((lw == 0) ? 0 : lw + 1);
}

/*
 * CLAUDE: the conversations a manager hides keep the size of the one it
 * shows. Hidden, a pane is never drawn, so nothing obliges it to keep its
 * cell of the tiled layout - and with thirty conversations that cell was
 * 24x5: Claude Code could not draw its prompt there, so the delivery never
 * found an agent "at its prompt" and mail stayed pending (a console captured
 * there was cut as well).
 */
void
claude_hidden_fit(struct window *w)
{
	struct window_pane	*wp, *act = w->active;

	if (!(w->flags & WINDOW_ZOOMED) || act == NULL ||
	    act->layout_cell == NULL)
		return;
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp != act && wp->layout_cell == NULL)
			window_pane_resize(wp, act->sx, act->sy);
	}
}

/*
 * CLAUDE: show `want` in a zoomed manager window by handing it the zoom cell -
 * no unzoom: that would put every hidden conversation back in its small tiled
 * cell and then back to full size, making all the agents redraw twice for a
 * click. The tiled layout kept aside for the unzoom is left untouched.
 */
static int
claude_zoom_switch(struct window *w, struct window_pane *want)
{
	struct window_pane	*wp, *from = NULL;
	struct layout_cell	*lc;

	if (!(w->flags & WINDOW_ZOOMED))
		return (0);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->layout_cell != NULL) {
			from = wp;
			break;
		}
	}
	if (from == NULL)
		return (0);
	if (from != want) {
		lc = from->layout_cell;
		from->layout_cell = NULL;
		lc->wp = want;
		want->layout_cell = lc;
	}
	if (w->active != want)
		window_set_active_pane(w, want, 1);
	layout_fix_panes(w, NULL);
	claude_hidden_fit(w);
	return (1);
}

/*
 * CLAUDE: a manager window always shows exactly ONE conversation, so keep it
 * zoomed on the active pane. tmux unzooms a window whenever a pane dies or is
 * split, which would otherwise tile every conversation side by side.
 */
int
claude_fix_zoom(struct window *w)
{
	if (!claude_manager(w) || w->active == NULL)
		return (0);
	/*
	 * Never touch a window that nothing references any more: window_zoom()
	 * notifies, a notification takes a reference, and on a dying window
	 * that resurrects it - the notify callback then drops the last
	 * reference a second time and frees it twice (double-free found with
	 * AddressSanitizer on "Fichier > Tuer le serveur" = kill-server).
	 */
	if (w->references == 0)
		return (0);
	if (window_count_panes(w) <= 1)
		return (0);
	/* Already zoomed ON THE ACTIVE pane: only it keeps a layout cell. */
	if ((w->flags & WINDOW_ZOOMED) && w->active->layout_cell != NULL)
		return (0);
	/* Zoomed on some other pane: hand it the zoom cell. */
	if (claude_zoom_switch(w, w->active))
		return (1);
	window_zoom(w->active);
	return (1);			/* caller resizes, outside any loop */
}

/*
 * CLAUDE: show conversation `row` (index in the pane list) - the window is
 * kept zoomed on it, so the others keep running but stay hidden.
 */
void
claude_select_row(struct client *c, struct window *w, u_int row)
{
	struct window_pane	*wp, *want = NULL;
	u_int			 i = 0;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (i++ == row) {
			want = wp;
			break;
		}
	}
	if (want == NULL)
		return;

	/*
	 * Already the conversation on screen: do nothing. Going through
	 * unzoom/zoom/recalculate would resize the pane for nothing, and the
	 * agent running there would be told to redraw (SIGWINCH) each time the
	 * user clicks its name.
	 */
	if (want == w->active &&
	    (window_count_panes(w) <= 1 ||
	    ((w->flags & WINDOW_ZOOMED) && want->layout_cell != NULL))) {
		if (want->claude_unread) {
			claude_mark_read(want);
			server_redraw_client(c);
		}
		log_debug("%s: %%%u already shown", __func__, want->id);
		return;
	}

	if (!claude_zoom_switch(w, want)) {
		if (w->flags & WINDOW_ZOOMED)
			window_unzoom(w);
		window_set_active_pane(w, want, 1);
		if (window_count_panes(w) > 1)
			window_zoom(want);
	}
	claude_mark_read(want);		/* showing it clears its ✉ */
	recalculate_sizes();
	server_redraw_client(c);
}

u_int
desktop_vert(struct client *c)
{
	if (c->session == NULL || c->session->curw == NULL)
		return (0);
	return (desktop_vert_w(c, c->session->curw->window));
}

u_int
desktop_horiz(struct client *c)
{
	if (c->session == NULL || c->session->curw == NULL)
		return (0);
	return (desktop_horiz_w(c, c->session->curw->window));
}

/* MENU BAR: get the clickable range at column x on the menu bar. */
struct style_range *
menu_bar_get_range(struct client *c, u_int x)
{
	struct style_range	*sr;

	TAILQ_FOREACH(sr, &c->menubar_ranges, entry) {
		if (x >= sr->start && x < sr->end)
			return (sr);
	}
	return (NULL);
}

/* Get the prompt line number for client's session. 1 means at the bottom. */
static u_int
status_prompt_line_at(struct client *c)
{
	struct session	*s = c->session;

	if (c->flags & (CLIENT_STATUSOFF|CLIENT_CONTROL))
		return (1);
	return (options_get_number(s->options, "message-line"));
}

/* Get window at window list position. */
struct style_range *
status_get_range(struct client *c, u_int x, u_int y)
{
	struct status_line	*sl = &c->status;
	struct style_range	*sr;

	if (y >= nitems(sl->entries))
		return (NULL);
	TAILQ_FOREACH(sr, &sl->entries[y].ranges, entry) {
		if (x >= sr->start && x < sr->end)
			return (sr);
	}
	return (NULL);
}

/* Free all ranges. */
void
status_free_ranges(struct style_ranges *srs)
{
	struct style_range	*sr, *sr1;

	TAILQ_FOREACH_SAFE(sr, srs, entry, sr1) {
		TAILQ_REMOVE(srs, sr, entry);
		free(sr);
	}
}

/* Save old status line. */
static void
status_push_screen(struct client *c)
{
	struct status_line *sl = &c->status;

	if (sl->active == &sl->screen) {
		sl->active = xmalloc(sizeof *sl->active);
		screen_init(sl->active, c->tty.sx, status_line_size(c), 0);
	}
	sl->references++;
}

/* Restore old status line. */
static void
status_pop_screen(struct client *c)
{
	struct status_line *sl = &c->status;

	if (--sl->references == 0) {
		screen_free(sl->active);
		free(sl->active);
		sl->active = &sl->screen;
	}
}

/* Initialize status line. */
void
status_init(struct client *c)
{
	struct status_line	*sl = &c->status;
	u_int			 i;

	for (i = 0; i < nitems(sl->entries); i++)
		TAILQ_INIT(&sl->entries[i].ranges);

	TAILQ_INIT(&c->menubar_ranges); /* MENU BAR */

	screen_init(&c->desktop_buffer, 1, 1, 0); /* DESKTOP back-buffer */
	c->desktop_buffer_valid = 0;

	screen_init(&sl->screen, c->tty.sx, 1, 0);
	sl->active = &sl->screen;
}

/* Free status line. */
void
status_free(struct client *c)
{
	struct status_line	*sl = &c->status;
	u_int			 i;

	for (i = 0; i < nitems(sl->entries); i++) {
		status_free_ranges(&sl->entries[i].ranges);
		free((void *)sl->entries[i].expanded);
	}

	status_free_ranges(&c->menubar_ranges); /* MENU BAR */
	screen_free(&c->desktop_buffer); /* DESKTOP back-buffer */

	if (event_initialized(&sl->timer))
		evtimer_del(&sl->timer);

	if (sl->active != &sl->screen) {
		screen_free(sl->active);
		free(sl->active);
	}
	screen_free(&sl->screen);
}

/* Draw status line for client. */
int
status_redraw(struct client *c)
{
	struct status_line		*sl = &c->status;
	struct status_line_entry	*sle;
	struct session			*s = c->session;
	struct screen_write_ctx		 ctx;
	struct grid_cell		 gc;
	u_int				 lines, i, n, width = c->tty.sx;
	int				 flags, force = 0, changed = 0, fg, bg;
	struct options_entry		*o;
	union options_value		*ov;
	struct format_tree		*ft;
	char				*expanded;

	log_debug("%s enter", __func__);

	/* Shouldn't get here if not the active screen. */
	if (sl->active != &sl->screen)
		fatalx("not the active screen");

	/* No status line? */
	lines = status_line_size(c);
	if (c->tty.sy == 0 || lines == 0)
		return (1);

	/* Create format tree. */
	flags = FORMAT_STATUS;
	if (c->flags & CLIENT_STATUSFORCE)
		flags |= FORMAT_FORCE;
	ft = format_create(c, NULL, FORMAT_NONE, flags);
	format_defaults(ft, c, NULL, NULL, NULL);

	/* Set up default colour. */
	style_apply(&gc, s->options, "status-style", ft);
	fg = options_get_number(s->options, "status-fg");
	if (!COLOUR_DEFAULT(fg))
		gc.fg = fg;
	bg = options_get_number(s->options, "status-bg");
	if (!COLOUR_DEFAULT(bg))
		gc.bg = bg;
	if (!grid_cells_equal(&gc, &sl->style)) {
		force = 1;
		memcpy(&sl->style, &gc, sizeof sl->style);
	}

	/* Resize the target screen. */
	if (screen_size_x(&sl->screen) != width ||
	    screen_size_y(&sl->screen) != lines) {
		screen_resize(&sl->screen, width, lines, 0);
		changed = force = 1;
	}
	screen_write_start(&ctx, &sl->screen);

	/* Write the status lines. */
	o = options_get(s->options, "status-format");
	if (o == NULL) {
		for (n = 0; n < width * lines; n++)
			screen_write_putc(&ctx, &gc, ' ');
	} else {
		for (i = 0; i < lines; i++) {
			screen_write_cursormove(&ctx, 0, i, 0);

			ov = options_array_get(o, i);
			if (ov == NULL) {
				for (n = 0; n < width; n++)
					screen_write_putc(&ctx, &gc, ' ');
				continue;
			}
			sle = &sl->entries[i];

			expanded = format_expand_time(ft, ov->string);
			if (!force &&
			    sle->expanded != NULL &&
			    strcmp(expanded, sle->expanded) == 0) {
				free(expanded);
				continue;
			}
			changed = 1;

			for (n = 0; n < width; n++)
				screen_write_putc(&ctx, &gc, ' ');
			screen_write_cursormove(&ctx, 0, i, 0);

			status_free_ranges(&sle->ranges);
			format_draw(&ctx, &gc, width, expanded, &sle->ranges,
			    0);

			free(sle->expanded);
			sle->expanded = expanded;
		}
	}
	screen_write_stop(&ctx);

	/* Free the format tree. */
	format_free(ft);

	/* Return if the status line has changed. */
	log_debug("%s exit: force=%d, changed=%d", __func__, force, changed);
	return (force || changed);
}

/* Set a status line message. */
void
status_message_set(struct client *c, int delay, int ignore_styles,
    int ignore_keys, const char *fmt, ...)
{
	struct timeval	 tv;
	va_list		 ap;
	char		*s;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);

	log_debug("%s: %s", __func__, s);

	if (c == NULL) {
		server_add_message("message: %s", s);
		free(s);
		return;
	}

	/*
	 * Turbo Vision desktop: the message goes in a box that closes by
	 * itself after display-time (0 = wait for OK/Escape), the status
	 * bar is left alone.
	 */
	if (c->session != NULL && desktop_enabled(c)) {
		server_add_message("%s message: %s", c->name, s);
		if (delay == -1)
			delay = options_get_number(c->session->options,
			    "display-time");
		message_dialog(c, s, (delay > 0) ? delay : 0);
		free(s);
		return;
	}

	status_message_clear(c);
	status_push_screen(c);
	c->message_string = s;
	server_add_message("%s message: %s", c->name, s);

	/*
	 * With delay -1, the display-time option is used; zero means wait for
	 * key press; more than zero is the actual delay time in milliseconds.
	 */
	if (delay == -1)
		delay = options_get_number(c->session->options, "display-time");
	if (delay > 0) {
		tv.tv_sec = delay / 1000;
		tv.tv_usec = (delay % 1000) * 1000L;

		if (event_initialized(&c->message_timer))
			evtimer_del(&c->message_timer);
		evtimer_set(&c->message_timer, status_message_callback, c);

		evtimer_add(&c->message_timer, &tv);
	}

	if (delay != 0)
		c->message_ignore_keys = ignore_keys;
	c->message_ignore_styles = ignore_styles;

	c->tty.flags |= (TTY_NOCURSOR|TTY_FREEZE);
	c->flags |= CLIENT_REDRAWSTATUS;
}

/* Clear status line message. */
void
status_message_clear(struct client *c)
{
	if (c->message_string == NULL)
		return;

	free(c->message_string);
	c->message_string = NULL;

	if (c->prompt_string == NULL)
		c->tty.flags &= ~(TTY_NOCURSOR|TTY_FREEZE);
	c->flags |= CLIENT_ALLREDRAWFLAGS; /* was frozen and may have changed */

	status_pop_screen(c);
}

/* Clear status line message after timer expires. */
static void
status_message_callback(__unused int fd, __unused short event, void *data)
{
	struct client	*c = data;

	status_message_clear(c);
}

/* Draw client message on status line of present else on last line. */
int
status_message_redraw(struct client *c)
{
	struct status_line	*sl = &c->status;
	struct screen_write_ctx	 ctx;
	struct session		*s = c->session;
	struct screen		 old_screen;
	size_t			 len;
	u_int			 lines, offset, messageline;
	struct grid_cell	 gc;
	struct format_tree	*ft;

	if (c->tty.sx == 0 || c->tty.sy == 0)
		return (0);
	memcpy(&old_screen, sl->active, sizeof old_screen);

	lines = status_line_size(c);
	if (lines <= 1)
		lines = 1;
	screen_init(sl->active, c->tty.sx, lines, 0);

	messageline = status_prompt_line_at(c);
	if (messageline > lines - 1)
		messageline = lines - 1;

	len = screen_write_strlen("%s", c->message_string);
	if (len > c->tty.sx)
		len = c->tty.sx;

	ft = format_create_defaults(NULL, c, NULL, NULL, NULL);
	style_apply(&gc, s->options, "message-style", ft);
	format_free(ft);

	screen_write_start(&ctx, sl->active);
	screen_write_fast_copy(&ctx, &sl->screen, 0, 0, c->tty.sx, lines);
	screen_write_cursormove(&ctx, 0, messageline, 0);
	for (offset = 0; offset < c->tty.sx; offset++)
		screen_write_putc(&ctx, &gc, ' ');
	screen_write_cursormove(&ctx, 0, messageline, 0);
	if (c->message_ignore_styles)
		screen_write_nputs(&ctx, len, &gc, "%s", c->message_string);
	else
		format_draw(&ctx, &gc, c->tty.sx, c->message_string, NULL, 0);
	screen_write_stop(&ctx);

	if (grid_compare(sl->active->grid, old_screen.grid) == 0) {
		screen_free(&old_screen);
		return (0);
	}
	screen_free(&old_screen);
	return (1);
}

/* Enable status line prompt. */
void
status_prompt_set(struct client *c, struct cmd_find_state *fs,
    const char *msg, const char *input, prompt_input_cb inputcb,
    prompt_free_cb freecb, void *data, int flags, enum prompt_type prompt_type)
{
	struct format_tree	*ft;
	char			*tmp;
	int			 boxed;

	if (fs != NULL)
		ft = format_create_from_state(NULL, c, fs);
	else
		ft = format_create_defaults(NULL, c, NULL, NULL, NULL);

	if (input == NULL)
		input = "";
	if (flags & PROMPT_NOFORMAT)
		tmp = xstrdup(input);
	else
		tmp = format_expand_time(ft, input);

	status_message_clear(c);
	status_prompt_clear(c);
	/*
	 * Turbo Vision desktop: the prompt goes in an input box and the
	 * status bar keeps drawing normally, so no temporary status screen
	 * is pushed for it (the push/pop is reference-counted and must stay
	 * balanced with status_prompt_clear()).
	 */
	boxed = ((~flags & PROMPT_SINGLE) && desktop_enabled(c));
	if (!boxed)
		status_push_screen(c);

	c->prompt_string = format_expand_time(ft, msg);

	if (flags & PROMPT_INCREMENTAL) {
		c->prompt_last = xstrdup(tmp);
		c->prompt_buffer = utf8_fromcstr("");
	} else {
		c->prompt_last = NULL;
		c->prompt_buffer = utf8_fromcstr(tmp);
	}
	c->prompt_index = utf8_strlen(c->prompt_buffer);

	c->prompt_inputcb = inputcb;
	c->prompt_freecb = freecb;
	c->prompt_data = data;

	memset(c->prompt_hindex, 0, sizeof c->prompt_hindex);

	c->prompt_flags = flags;
	c->prompt_type = prompt_type;
	c->prompt_mode = PROMPT_ENTRY;

	if (~flags & PROMPT_INCREMENTAL)
		c->tty.flags |= (TTY_NOCURSOR|TTY_FREEZE);
	c->flags |= CLIENT_REDRAWSTATUS;

	if (flags & PROMPT_INCREMENTAL)
		c->prompt_inputcb(c, c->prompt_data, "=", 0);

	/* Show the prompt in the input box; too small -> classic line prompt. */
	if (boxed && c->prompt_string != NULL && !prompt_dialog_open(c))
		status_push_screen(c);

	free(tmp);
	format_free(ft);
}

/* Remove status line prompt. */
void
status_prompt_clear(struct client *c)
{
	int	boxed = c->prompt_dialog;	/* no status screen was pushed */

	if (c->prompt_string == NULL)
		return;

	if (c->prompt_freecb != NULL && c->prompt_data != NULL)
		c->prompt_freecb(c->prompt_data);

	free(c->prompt_last);
	c->prompt_last = NULL;

	free(c->prompt_string);
	c->prompt_string = NULL;
	c->prompt_dialog = 0;

	free(c->prompt_buffer);
	c->prompt_buffer = NULL;

	free(c->prompt_saved);
	c->prompt_saved = NULL;

	c->tty.flags &= ~(TTY_NOCURSOR|TTY_FREEZE);
	c->flags |= CLIENT_ALLREDRAWFLAGS; /* was frozen and may have changed */

	if (!boxed)
		status_pop_screen(c);
}

/* Update status line prompt with a new prompt string. */
void
status_prompt_update(struct client *c, const char *msg, const char *input)
{
	struct format_tree	*ft;
	char			*tmp;

	ft = format_create(c, NULL, FORMAT_NONE, 0);
	format_defaults(ft, c, NULL, NULL, NULL);

	tmp = format_expand_time(ft, input);

	free(c->prompt_string);
	c->prompt_string = format_expand_time(ft, msg);

	free(c->prompt_buffer);
	c->prompt_buffer = utf8_fromcstr(tmp);
	c->prompt_index = utf8_strlen(c->prompt_buffer);

	memset(c->prompt_hindex, 0, sizeof c->prompt_hindex);

	c->flags |= CLIENT_REDRAWSTATUS;

	free(tmp);
	format_free(ft);
}

/* Draw client prompt on status line of present else on last line. */
int
status_prompt_redraw(struct client *c)
{
	struct status_line	*sl = &c->status;
	struct screen_write_ctx	 ctx;
	struct session		*s = c->session;
	struct screen		 old_screen;
	u_int			 i, lines, offset, left, start, width;
	u_int			 pcursor, pwidth, promptline;
	struct grid_cell	 gc, cursorgc;
	struct format_tree	*ft;

	if (c->tty.sx == 0 || c->tty.sy == 0)
		return (0);
	memcpy(&old_screen, sl->active, sizeof old_screen);

	lines = status_line_size(c);
	if (lines <= 1)
		lines = 1;
	screen_init(sl->active, c->tty.sx, lines, 0);

	promptline = status_prompt_line_at(c);
	if (promptline > lines - 1)
		promptline = lines - 1;

	ft = format_create_defaults(NULL, c, NULL, NULL, NULL);
	if (c->prompt_mode == PROMPT_COMMAND)
		style_apply(&gc, s->options, "message-command-style", ft);
	else
		style_apply(&gc, s->options, "message-style", ft);
	format_free(ft);

	memcpy(&cursorgc, &gc, sizeof cursorgc);
	cursorgc.attr ^= GRID_ATTR_REVERSE;

	start = format_width(c->prompt_string);
	if (start > c->tty.sx)
		start = c->tty.sx;

	screen_write_start(&ctx, sl->active);
	screen_write_fast_copy(&ctx, &sl->screen, 0, 0, c->tty.sx, lines);
	screen_write_cursormove(&ctx, 0, promptline, 0);
	for (offset = 0; offset < c->tty.sx; offset++)
		screen_write_putc(&ctx, &gc, ' ');
	screen_write_cursormove(&ctx, 0, promptline, 0);
	format_draw(&ctx, &gc, start, c->prompt_string, NULL, 0);
	screen_write_cursormove(&ctx, start, promptline, 0);

	left = c->tty.sx - start;
	if (left == 0)
		goto finished;

	pcursor = utf8_strwidth(c->prompt_buffer, c->prompt_index);
	pwidth = utf8_strwidth(c->prompt_buffer, -1);
	if (pcursor >= left) {
		/*
		 * The cursor would be outside the screen so start drawing
		 * with it on the right.
		 */
		offset = (pcursor - left) + 1;
		pwidth = left;
	} else
		offset = 0;
	if (pwidth > left)
		pwidth = left;
	c->prompt_cursor = start + c->prompt_index - offset;

	width = 0;
	for (i = 0; c->prompt_buffer[i].size != 0; i++) {
		if (width < offset) {
			width += c->prompt_buffer[i].width;
			continue;
		}
		if (width >= offset + pwidth)
			break;
		width += c->prompt_buffer[i].width;
		if (width > offset + pwidth)
			break;

		if (i != c->prompt_index) {
			utf8_copy(&gc.data, &c->prompt_buffer[i]);
			screen_write_cell(&ctx, &gc);
		} else {
			utf8_copy(&cursorgc.data, &c->prompt_buffer[i]);
			screen_write_cell(&ctx, &cursorgc);
		}
	}
	if (sl->active->cx < screen_size_x(sl->active) && c->prompt_index >= i)
		screen_write_putc(&ctx, &cursorgc, ' ');

finished:
	screen_write_stop(&ctx);

	if (grid_compare(sl->active->grid, old_screen.grid) == 0) {
		screen_free(&old_screen);
		return (0);
	}
	screen_free(&old_screen);
	return (1);
}

/* Is this a separator? */
static int
status_prompt_in_list(const char *ws, const struct utf8_data *ud)
{
	if (ud->size != 1 || ud->width != 1)
		return (0);
	return (strchr(ws, *ud->data) != NULL);
}

/* Is this a space? */
static int
status_prompt_space(const struct utf8_data *ud)
{
	if (ud->size != 1 || ud->width != 1)
		return (0);
	return (*ud->data == ' ');
}

/*
 * Translate key from vi to emacs. Return 0 to drop key, 1 to process the key
 * as an emacs key; return 2 to append to the buffer.
 */
static int
status_prompt_translate_key(struct client *c, key_code key, key_code *new_key)
{
	if (c->prompt_mode == PROMPT_ENTRY) {
		switch (key) {
		case '\001': /* C-a */
		case '\003': /* C-c */
		case '\005': /* C-e */
		case '\007': /* C-g */
		case '\010': /* C-h */
		case '\011': /* Tab */
		case '\013': /* C-k */
		case '\016': /* C-n */
		case '\020': /* C-p */
		case '\024': /* C-t */
		case '\025': /* C-u */
		case '\027': /* C-w */
		case '\031': /* C-y */
		case '\n':
		case '\r':
		case KEYC_LEFT|KEYC_CTRL:
		case KEYC_RIGHT|KEYC_CTRL:
		case KEYC_BSPACE:
		case KEYC_DC:
		case KEYC_DOWN:
		case KEYC_END:
		case KEYC_HOME:
		case KEYC_LEFT:
		case KEYC_RIGHT:
		case KEYC_UP:
			*new_key = key;
			return (1);
		case '\033': /* Escape */
			c->prompt_mode = PROMPT_COMMAND;
			c->flags |= CLIENT_REDRAWSTATUS;
			return (0);
		}
		*new_key = key;
		return (2);
	}

	switch (key) {
	case KEYC_BSPACE:
		*new_key = KEYC_LEFT;
		return (1);
	case 'A':
	case 'I':
	case 'C':
	case 's':
	case 'a':
		c->prompt_mode = PROMPT_ENTRY;
		c->flags |= CLIENT_REDRAWSTATUS;
		break; /* switch mode and... */
	case 'S':
		c->prompt_mode = PROMPT_ENTRY;
		c->flags |= CLIENT_REDRAWSTATUS;
		*new_key = '\025'; /* C-u */
		return (1);
	case 'i':
	case '\033': /* Escape */
		c->prompt_mode = PROMPT_ENTRY;
		c->flags |= CLIENT_REDRAWSTATUS;
		return (0);
	}

	switch (key) {
	case 'A':
	case '$':
		*new_key = KEYC_END;
		return (1);
	case 'I':
	case '0':
	case '^':
		*new_key = KEYC_HOME;
		return (1);
	case 'C':
	case 'D':
		*new_key = '\013'; /* C-k */
		return (1);
	case KEYC_BSPACE:
	case 'X':
		*new_key = KEYC_BSPACE;
		return (1);
	case 'b':
		*new_key = 'b'|KEYC_META;
		return (1);
	case 'B':
		*new_key = 'B'|KEYC_VI;
		return (1);
	case 'd':
		*new_key = '\025'; /* C-u */
		return (1);
	case 'e':
		*new_key = 'e'|KEYC_VI;
		return (1);
	case 'E':
		*new_key = 'E'|KEYC_VI;
		return (1);
	case 'w':
		*new_key = 'w'|KEYC_VI;
		return (1);
	case 'W':
		*new_key = 'W'|KEYC_VI;
		return (1);
	case 'p':
		*new_key = '\031'; /* C-y */
		return (1);
	case 'q':
		*new_key = '\003'; /* C-c */
		return (1);
	case 's':
	case KEYC_DC:
	case 'x':
		*new_key = KEYC_DC;
		return (1);
	case KEYC_DOWN:
	case 'j':
		*new_key = KEYC_DOWN;
		return (1);
	case KEYC_LEFT:
	case 'h':
		*new_key = KEYC_LEFT;
		return (1);
	case 'a':
	case KEYC_RIGHT:
	case 'l':
		*new_key = KEYC_RIGHT;
		return (1);
	case KEYC_UP:
	case 'k':
		*new_key = KEYC_UP;
		return (1);
	case '\010' /* C-h */:
	case '\003' /* C-c */:
	case '\n':
	case '\r':
		return (1);
	}
	return (0);
}

/* Paste into prompt. */
static int
status_prompt_paste(struct client *c)
{
	struct paste_buffer	*pb;
	const char		*bufdata;
	size_t			 size, n, bufsize;
	u_int			 i;
	struct utf8_data	*ud, *udp;
	enum utf8_state		 more;

	size = utf8_strlen(c->prompt_buffer);
	if (c->prompt_saved != NULL) {
		ud = c->prompt_saved;
		n = utf8_strlen(c->prompt_saved);
	} else {
		if ((pb = paste_get_top(NULL)) == NULL)
			return (0);
		bufdata = paste_buffer_data(pb, &bufsize);
		ud = xreallocarray(NULL, bufsize + 1, sizeof *ud);
		udp = ud;
		for (i = 0; i != bufsize; /* nothing */) {
			more = utf8_open(udp, bufdata[i]);
			if (more == UTF8_MORE) {
				while (++i != bufsize && more == UTF8_MORE)
					more = utf8_append(udp, bufdata[i]);
				if (more == UTF8_DONE) {
					udp++;
					continue;
				}
				i -= udp->have;
			}
			if (bufdata[i] <= 31 || bufdata[i] >= 127)
				break;
			utf8_set(udp, bufdata[i]);
			udp++;
			i++;
		}
		udp->size = 0;
		n = udp - ud;
	}
	if (n == 0)
		return (0);

	c->prompt_buffer = xreallocarray(c->prompt_buffer, size + n + 1,
	    sizeof *c->prompt_buffer);
	if (c->prompt_index == size) {
		memcpy(c->prompt_buffer + c->prompt_index, ud,
		    n * sizeof *c->prompt_buffer);
		c->prompt_index += n;
		c->prompt_buffer[c->prompt_index].size = 0;
	} else {
		memmove(c->prompt_buffer + c->prompt_index + n,
		    c->prompt_buffer + c->prompt_index,
		    (size + 1 - c->prompt_index) * sizeof *c->prompt_buffer);
		memcpy(c->prompt_buffer + c->prompt_index, ud,
		    n * sizeof *c->prompt_buffer);
		c->prompt_index += n;
	}

	if (ud != c->prompt_saved)
		free(ud);
	return (1);
}

/* Finish completion. */
static int
status_prompt_replace_complete(struct client *c, const char *s)
{
	char			 word[64], *allocated = NULL;
	size_t			 size, n, off, idx, used;
	struct utf8_data	*first, *last, *ud;

	/* Work out where the cursor currently is. */
	idx = c->prompt_index;
	if (idx != 0)
		idx--;
	size = utf8_strlen(c->prompt_buffer);

	/* Find the word we are in. */
	first = &c->prompt_buffer[idx];
	while (first > c->prompt_buffer && !status_prompt_space(first))
		first--;
	while (first->size != 0 && status_prompt_space(first))
		first++;
	last = &c->prompt_buffer[idx];
	while (last->size != 0 && !status_prompt_space(last))
		last++;
	while (last > c->prompt_buffer && status_prompt_space(last))
		last--;
	if (last->size != 0)
		last++;
	if (last < first)
		return (0);
	if (s == NULL) {
		used = 0;
		for (ud = first; ud < last; ud++) {
			if (used + ud->size >= sizeof word)
				break;
			memcpy(word + used, ud->data, ud->size);
			used += ud->size;
		}
		if (ud != last)
			return (0);
		word[used] = '\0';
	}

	/* Try to complete it. */
	if (s == NULL) {
		allocated = status_prompt_complete(c, word,
		    first - c->prompt_buffer);
		if (allocated == NULL)
			return (0);
		s = allocated;
	}

	/* Trim out word. */
	n = size - (last - c->prompt_buffer) + 1; /* with \0 */
	memmove(first, last, n * sizeof *c->prompt_buffer);
	size -= last - first;

	/* Insert the new word. */
	size += strlen(s);
	off = first - c->prompt_buffer;
	c->prompt_buffer = xreallocarray(c->prompt_buffer, size + 1,
	    sizeof *c->prompt_buffer);
	first = c->prompt_buffer + off;
	memmove(first + strlen(s), first, n * sizeof *c->prompt_buffer);
	for (idx = 0; idx < strlen(s); idx++)
		utf8_set(&first[idx], s[idx]);
	c->prompt_index = (first - c->prompt_buffer) + strlen(s);

	free(allocated);
	return (1);
}

/* Prompt forward to the next beginning of a word. */
static void
status_prompt_forward_word(struct client *c, size_t size, int vi,
    const char *separators)
{
	size_t		 idx = c->prompt_index;
	int		 word_is_separators;

	/* In emacs mode, skip until the first non-whitespace character. */
	if (!vi)
		while (idx != size &&
		    status_prompt_space(&c->prompt_buffer[idx]))
			idx++;

	/* Can't move forward if we're already at the end. */
	if (idx == size) {
		c->prompt_index = idx;
		return;
	}

	/* Determine the current character class (separators or not). */
	word_is_separators = status_prompt_in_list(separators,
	    &c->prompt_buffer[idx]) &&
	    !status_prompt_space(&c->prompt_buffer[idx]);

	/* Skip ahead until the first space or opposite character class. */
	do {
		idx++;
		if (status_prompt_space(&c->prompt_buffer[idx])) {
			/* In vi mode, go to the start of the next word. */
			if (vi)
				while (idx != size &&
				    status_prompt_space(&c->prompt_buffer[idx]))
					idx++;
			break;
		}
	} while (idx != size && word_is_separators == status_prompt_in_list(
	    separators, &c->prompt_buffer[idx]));

	c->prompt_index = idx;
}

/* Prompt forward to the next end of a word. */
static void
status_prompt_end_word(struct client *c, size_t size, const char *separators)
{
	size_t		 idx = c->prompt_index;
	int		 word_is_separators;

	/* Can't move forward if we're already at the end. */
	if (idx == size)
		return;

	/* Find the next word. */
	do {
		idx++;
		if (idx == size) {
			c->prompt_index = idx;
			return;
		}
	} while (status_prompt_space(&c->prompt_buffer[idx]));

	/* Determine the character class (separators or not). */
	word_is_separators = status_prompt_in_list(separators,
	    &c->prompt_buffer[idx]);

	/* Skip ahead until the next space or opposite character class. */
	do {
		idx++;
		if (idx == size)
			break;
	} while (!status_prompt_space(&c->prompt_buffer[idx]) &&
	    word_is_separators == status_prompt_in_list(separators,
	    &c->prompt_buffer[idx]));

	/* Back up to the previous character to stop at the end of the word. */
	c->prompt_index = idx - 1;
}

/* Prompt backward to the previous beginning of a word. */
static void
status_prompt_backward_word(struct client *c, const char *separators)
{
	size_t	idx = c->prompt_index;
	int	word_is_separators;

	/* Find non-whitespace. */
	while (idx != 0) {
		--idx;
		if (!status_prompt_space(&c->prompt_buffer[idx]))
			break;
	}
	word_is_separators = status_prompt_in_list(separators,
	    &c->prompt_buffer[idx]);

	/* Find the character before the beginning of the word. */
	while (idx != 0) {
		--idx;
		if (status_prompt_space(&c->prompt_buffer[idx]) ||
		    word_is_separators != status_prompt_in_list(separators,
		    &c->prompt_buffer[idx])) {
			/* Go back to the word. */
			idx++;
			break;
		}
	}
	c->prompt_index = idx;
}

/* Handle keys in prompt. */
int
status_prompt_key(struct client *c, key_code key)
{
	struct options		*oo = c->session->options;
	char			*s, *cp, prefix = '=';
	const char		*histstr, *separators = NULL, *keystring;
	size_t			 size, idx;
	struct utf8_data	 tmp;
	int			 keys, word_is_separators;

	if (c->prompt_flags & PROMPT_KEY) {
		keystring = key_string_lookup_key(key, 0);
		c->prompt_inputcb(c, c->prompt_data, keystring, 1);
		status_prompt_clear(c);
		return (0);
	}
	size = utf8_strlen(c->prompt_buffer);

	if (c->prompt_flags & PROMPT_NUMERIC) {
		if (key >= '0' && key <= '9')
			goto append_key;
		s = utf8_tocstr(c->prompt_buffer);
		c->prompt_inputcb(c, c->prompt_data, s, 1);
		status_prompt_clear(c);
		free(s);
		return (1);
	}
	key &= ~KEYC_MASK_FLAGS;

	keys = options_get_number(c->session->options, "status-keys");
	if (keys == MODEKEY_VI) {
		switch (status_prompt_translate_key(c, key, &key)) {
		case 1:
			goto process_key;
		case 2:
			goto append_key;
		default:
			return (0);
		}
	}

process_key:
	switch (key) {
	case KEYC_LEFT:
	case '\002': /* C-b */
		if (c->prompt_index > 0) {
			c->prompt_index--;
			break;
		}
		break;
	case KEYC_RIGHT:
	case '\006': /* C-f */
		if (c->prompt_index < size) {
			c->prompt_index++;
			break;
		}
		break;
	case KEYC_HOME:
	case '\001': /* C-a */
		if (c->prompt_index != 0) {
			c->prompt_index = 0;
			break;
		}
		break;
	case KEYC_END:
	case '\005': /* C-e */
		if (c->prompt_index != size) {
			c->prompt_index = size;
			break;
		}
		break;
	case '\011': /* Tab */
		if (status_prompt_replace_complete(c, NULL))
			goto changed;
		break;
	case KEYC_BSPACE:
	case '\010': /* C-h */
		if (c->prompt_index != 0) {
			if (c->prompt_index == size)
				c->prompt_buffer[--c->prompt_index].size = 0;
			else {
				memmove(c->prompt_buffer + c->prompt_index - 1,
				    c->prompt_buffer + c->prompt_index,
				    (size + 1 - c->prompt_index) *
				    sizeof *c->prompt_buffer);
				c->prompt_index--;
			}
			goto changed;
		}
		break;
	case KEYC_DC:
	case '\004': /* C-d */
		if (c->prompt_index != size) {
			memmove(c->prompt_buffer + c->prompt_index,
			    c->prompt_buffer + c->prompt_index + 1,
			    (size + 1 - c->prompt_index) *
			    sizeof *c->prompt_buffer);
			goto changed;
		}
		break;
	case '\025': /* C-u */
		c->prompt_buffer[0].size = 0;
		c->prompt_index = 0;
		goto changed;
	case '\013': /* C-k */
		if (c->prompt_index < size) {
			c->prompt_buffer[c->prompt_index].size = 0;
			goto changed;
		}
		break;
	case '\027': /* C-w */
		separators = options_get_string(oo, "word-separators");
		idx = c->prompt_index;

		/* Find non-whitespace. */
		while (idx != 0) {
			idx--;
			if (!status_prompt_space(&c->prompt_buffer[idx]))
				break;
		}
		word_is_separators = status_prompt_in_list(separators,
		    &c->prompt_buffer[idx]);

		/* Find the character before the beginning of the word. */
		while (idx != 0) {
			idx--;
			if (status_prompt_space(&c->prompt_buffer[idx]) ||
			    word_is_separators != status_prompt_in_list(
			    separators, &c->prompt_buffer[idx])) {
				/* Go back to the word. */
				idx++;
				break;
			}
		}

		free(c->prompt_saved);
		c->prompt_saved = xcalloc(sizeof *c->prompt_buffer,
		    (c->prompt_index - idx) + 1);
		memcpy(c->prompt_saved, c->prompt_buffer + idx,
		    (c->prompt_index - idx) * sizeof *c->prompt_buffer);

		memmove(c->prompt_buffer + idx,
		    c->prompt_buffer + c->prompt_index,
		    (size + 1 - c->prompt_index) *
		    sizeof *c->prompt_buffer);
		memset(c->prompt_buffer + size - (c->prompt_index - idx),
		    '\0', (c->prompt_index - idx) * sizeof *c->prompt_buffer);
		c->prompt_index = idx;

		goto changed;
	case KEYC_RIGHT|KEYC_CTRL:
	case 'f'|KEYC_META:
		separators = options_get_string(oo, "word-separators");
		status_prompt_forward_word(c, size, 0, separators);
		goto changed;
	case 'E'|KEYC_VI:
		status_prompt_end_word(c, size, "");
		goto changed;
	case 'e'|KEYC_VI:
		separators = options_get_string(oo, "word-separators");
		status_prompt_end_word(c, size, separators);
		goto changed;
	case 'W'|KEYC_VI:
		status_prompt_forward_word(c, size, 1, "");
		goto changed;
	case 'w'|KEYC_VI:
		separators = options_get_string(oo, "word-separators");
		status_prompt_forward_word(c, size, 1, separators);
		goto changed;
	case 'B'|KEYC_VI:
		status_prompt_backward_word(c, "");
		goto changed;
	case KEYC_LEFT|KEYC_CTRL:
	case 'b'|KEYC_META:
		separators = options_get_string(oo, "word-separators");
		status_prompt_backward_word(c, separators);
		goto changed;
	case KEYC_UP:
	case '\020': /* C-p */
		histstr = status_prompt_up_history(c->prompt_hindex,
		    c->prompt_type);
		if (histstr == NULL)
			break;
		free(c->prompt_buffer);
		c->prompt_buffer = utf8_fromcstr(histstr);
		c->prompt_index = utf8_strlen(c->prompt_buffer);
		goto changed;
	case KEYC_DOWN:
	case '\016': /* C-n */
		histstr = status_prompt_down_history(c->prompt_hindex,
		    c->prompt_type);
		if (histstr == NULL)
			break;
		free(c->prompt_buffer);
		c->prompt_buffer = utf8_fromcstr(histstr);
		c->prompt_index = utf8_strlen(c->prompt_buffer);
		goto changed;
	case '\031': /* C-y */
		if (status_prompt_paste(c))
			goto changed;
		break;
	case '\024': /* C-t */
		idx = c->prompt_index;
		if (idx < size)
			idx++;
		if (idx >= 2) {
			utf8_copy(&tmp, &c->prompt_buffer[idx - 2]);
			utf8_copy(&c->prompt_buffer[idx - 2],
			    &c->prompt_buffer[idx - 1]);
			utf8_copy(&c->prompt_buffer[idx - 1], &tmp);
			c->prompt_index = idx;
			goto changed;
		}
		break;
	case '\r':
	case '\n':
		s = utf8_tocstr(c->prompt_buffer);
		if (*s != '\0')
			status_prompt_add_history(s, c->prompt_type);
		if (c->prompt_inputcb(c, c->prompt_data, s, 1) == 0)
			status_prompt_clear(c);
		free(s);
		break;
	case '\033': /* Escape */
	case '\003': /* C-c */
	case '\007': /* C-g */
		if (c->prompt_inputcb(c, c->prompt_data, NULL, 1) == 0)
			status_prompt_clear(c);
		break;
	case '\022': /* C-r */
		if (~c->prompt_flags & PROMPT_INCREMENTAL)
			break;
		if (c->prompt_buffer[0].size == 0) {
			prefix = '=';
			free(c->prompt_buffer);
			c->prompt_buffer = utf8_fromcstr(c->prompt_last);
			c->prompt_index = utf8_strlen(c->prompt_buffer);
		} else
			prefix = '-';
		goto changed;
	case '\023': /* C-s */
		if (~c->prompt_flags & PROMPT_INCREMENTAL)
			break;
		if (c->prompt_buffer[0].size == 0) {
			prefix = '=';
			free(c->prompt_buffer);
			c->prompt_buffer = utf8_fromcstr(c->prompt_last);
			c->prompt_index = utf8_strlen(c->prompt_buffer);
		} else
			prefix = '+';
		goto changed;
	default:
		goto append_key;
	}

	c->flags |= CLIENT_REDRAWSTATUS;
	return (0);

append_key:
	if (key <= 0x7f)
		utf8_set(&tmp, key);
	else if (KEYC_IS_UNICODE(key))
		utf8_to_data(key, &tmp);
	else
		return (0);

	c->prompt_buffer = xreallocarray(c->prompt_buffer, size + 2,
	    sizeof *c->prompt_buffer);

	if (c->prompt_index == size) {
		utf8_copy(&c->prompt_buffer[c->prompt_index], &tmp);
		c->prompt_index++;
		c->prompt_buffer[c->prompt_index].size = 0;
	} else {
		memmove(c->prompt_buffer + c->prompt_index + 1,
		    c->prompt_buffer + c->prompt_index,
		    (size + 1 - c->prompt_index) *
		    sizeof *c->prompt_buffer);
		utf8_copy(&c->prompt_buffer[c->prompt_index], &tmp);
		c->prompt_index++;
	}

	if (c->prompt_flags & PROMPT_SINGLE) {
		if (utf8_strlen(c->prompt_buffer) != 1)
			status_prompt_clear(c);
		else {
			s = utf8_tocstr(c->prompt_buffer);
			if (c->prompt_inputcb(c, c->prompt_data, s, 1) == 0)
				status_prompt_clear(c);
			free(s);
		}
	}

changed:
	c->flags |= CLIENT_REDRAWSTATUS;
	if (c->prompt_flags & PROMPT_INCREMENTAL) {
		s = utf8_tocstr(c->prompt_buffer);
		xasprintf(&cp, "%c%s", prefix, s);
		c->prompt_inputcb(c, c->prompt_data, cp, 0);
		free(cp);
		free(s);
	}
	return (0);
}

/* Get previous line from the history. */
static const char *
status_prompt_up_history(u_int *idx, u_int type)
{
	/*
	 * History runs from 0 to size - 1. Index is from 0 to size. Zero is
	 * empty.
	 */

	if (status_prompt_hsize[type] == 0 ||
	    idx[type] == status_prompt_hsize[type])
		return (NULL);
	idx[type]++;
	return (status_prompt_hlist[type][status_prompt_hsize[type] - idx[type]]);
}

/* Get next line from the history. */
static const char *
status_prompt_down_history(u_int *idx, u_int type)
{
	if (status_prompt_hsize[type] == 0 || idx[type] == 0)
		return ("");
	idx[type]--;
	if (idx[type] == 0)
		return ("");
	return (status_prompt_hlist[type][status_prompt_hsize[type] - idx[type]]);
}

/* Add line to the history. */
static void
status_prompt_add_history(const char *line, u_int type)
{
	u_int	i, oldsize, newsize, freecount, hlimit, new = 1;
	size_t	movesize;

	oldsize = status_prompt_hsize[type];
	if (oldsize > 0 &&
	    strcmp(status_prompt_hlist[type][oldsize - 1], line) == 0)
		new = 0;

	hlimit = options_get_number(global_options, "prompt-history-limit");
	if (hlimit > oldsize) {
		if (new == 0)
			return;
		newsize = oldsize + new;
	} else {
		newsize = hlimit;
		freecount = oldsize + new - newsize;
		if (freecount > oldsize)
			freecount = oldsize;
		if (freecount == 0)
			return;
		for (i = 0; i < freecount; i++)
			free(status_prompt_hlist[type][i]);
		movesize = (oldsize - freecount) *
		    sizeof *status_prompt_hlist[type];
		if (movesize > 0) {
			memmove(&status_prompt_hlist[type][0],
			    &status_prompt_hlist[type][freecount], movesize);
		}
	}

	if (newsize == 0) {
		free(status_prompt_hlist[type]);
		status_prompt_hlist[type] = NULL;
	} else if (newsize != oldsize) {
		status_prompt_hlist[type] =
		    xreallocarray(status_prompt_hlist[type], newsize,
			sizeof *status_prompt_hlist[type]);
	}

	if (new == 1 && newsize > 0)
		status_prompt_hlist[type][newsize - 1] = xstrdup(line);
	status_prompt_hsize[type] = newsize;
}

/* Add to completion list. */
static void
status_prompt_add_list(char ***list, u_int *size, const char *s)
{
	u_int	i;

	for (i = 0; i < *size; i++) {
		if (strcmp((*list)[i], s) == 0)
			return;
	}
	*list = xreallocarray(*list, (*size) + 1, sizeof **list);
	(*list)[(*size)++] = xstrdup(s);
}

/* Build completion list. */
static char **
status_prompt_complete_list(u_int *size, const char *s, int at_start)
{
	char					**list = NULL, *tmp;
	const char				**layout, *value, *cp;
	const struct cmd_entry			**cmdent;
	const struct options_table_entry	 *oe;
	size_t					  slen = strlen(s), valuelen;
	struct options_entry			 *o;
	struct options_array_item		 *a;
	const char				 *layouts[] = {
		"even-horizontal", "even-vertical", "main-horizontal",
		"main-vertical", "tiled", NULL
	};

	*size = 0;
	for (cmdent = cmd_table; *cmdent != NULL; cmdent++) {
		if (strncmp((*cmdent)->name, s, slen) == 0)
			status_prompt_add_list(&list, size, (*cmdent)->name);
		if ((*cmdent)->alias != NULL &&
		    strncmp((*cmdent)->alias, s, slen) == 0)
			status_prompt_add_list(&list, size, (*cmdent)->alias);
	}
	o = options_get_only(global_options, "command-alias");
	if (o != NULL) {
		a = options_array_first(o);
		while (a != NULL) {
			value = options_array_item_value(a)->string;
			if ((cp = strchr(value, '=')) == NULL)
				goto next;
			valuelen = cp - value;
			if (slen > valuelen || strncmp(value, s, slen) != 0)
				goto next;

			xasprintf(&tmp, "%.*s", (int)valuelen, value);
			status_prompt_add_list(&list, size, tmp);
			free(tmp);

		next:
			a = options_array_next(a);
		}
	}
	if (at_start)
		return (list);
	for (oe = options_table; oe->name != NULL; oe++) {
		if (strncmp(oe->name, s, slen) == 0)
			status_prompt_add_list(&list, size, oe->name);
	}
	for (layout = layouts; *layout != NULL; layout++) {
		if (strncmp(*layout, s, slen) == 0)
			status_prompt_add_list(&list, size, *layout);
	}
	return (list);
}

/* Find longest prefix. */
static char *
status_prompt_complete_prefix(char **list, u_int size)
{
	char	 *out;
	u_int	  i;
	size_t	  j;

	if (list == NULL || size == 0)
		return (NULL);
	out = xstrdup(list[0]);
	for (i = 1; i < size; i++) {
		j = strlen(list[i]);
		if (j > strlen(out))
			j = strlen(out);
		for (; j > 0; j--) {
			if (out[j - 1] != list[i][j - 1])
				out[j - 1] = '\0';
		}
	}
	return (out);
}

/* Complete word menu callback. */
static void
status_prompt_menu_callback(__unused struct menu *menu, u_int idx, key_code key,
    void *data)
{
	struct status_prompt_menu	*spm = data;
	struct client			*c = spm->c;
	u_int				 i;
	char				*s;

	if (key != KEYC_NONE) {
		idx += spm->start;
		if (spm->flag == '\0')
			s = xstrdup(spm->list[idx]);
		else
			xasprintf(&s, "-%c%s", spm->flag, spm->list[idx]);
		if (c->prompt_type == PROMPT_TYPE_WINDOW_TARGET) {
			free(c->prompt_buffer);
			c->prompt_buffer = utf8_fromcstr(s);
			c->prompt_index = utf8_strlen(c->prompt_buffer);
			c->flags |= CLIENT_REDRAWSTATUS;
		} else if (status_prompt_replace_complete(c, s))
			c->flags |= CLIENT_REDRAWSTATUS;
		free(s);
	}

	for (i = 0; i < spm->size; i++)
		free(spm->list[i]);
	free(spm->list);
}

/* Show complete word menu. */
static int
status_prompt_complete_list_menu(struct client *c, char **list, u_int size,
    u_int offset, char flag)
{
	struct menu			*menu;
	struct menu_item		 item;
	struct status_prompt_menu	*spm;
	u_int				 lines = status_line_size(c), height, i;
	u_int				 py;

	if (size <= 1)
		return (0);
	if (c->tty.sy - lines < 3)
		return (0);

	spm = xmalloc(sizeof *spm);
	spm->c = c;
	spm->size = size;
	spm->list = list;
	spm->flag = flag;

	height = c->tty.sy - lines - 2;
	if (height > 10)
		height = 10;
	if (height > size)
		height = size;
	spm->start = size - height;

	menu = menu_create("");
	for (i = spm->start; i < size; i++) {
		item.name = list[i];
		item.key = '0' + (i - spm->start);
		item.command = NULL;
		menu_add_item(menu, &item, NULL, c, NULL);
	}

	if (options_get_number(c->session->options, "status-position") == 0)
		py = lines;
	else
		py = c->tty.sy - 3 - height;
	offset += utf8_cstrwidth(c->prompt_string);
	if (offset > 2)
		offset -= 2;
	else
		offset = 0;

	if (menu_display(menu, MENU_NOMOUSE|MENU_TAB, 0, NULL, offset, py, c,
	    BOX_LINES_DEFAULT, NULL, NULL, NULL, NULL,
	    status_prompt_menu_callback, spm) != 0) {
		menu_free(menu);
		free(spm);
		return (0);
	}
	return (1);
}

/* Show complete word menu. */
static char *
status_prompt_complete_window_menu(struct client *c, struct session *s,
    const char *word, u_int offset, char flag)
{
	struct menu			 *menu;
	struct menu_item		  item;
	struct status_prompt_menu	 *spm;
	struct winlink			 *wl;
	char				**list = NULL, *tmp;
	u_int				  lines = status_line_size(c), height;
	u_int				  py, size = 0;

	if (c->tty.sy - lines < 3)
		return (NULL);

	spm = xmalloc(sizeof *spm);
	spm->c = c;
	spm->flag = flag;

	height = c->tty.sy - lines - 2;
	if (height > 10)
		height = 10;
	spm->start = 0;

	menu = menu_create("");
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (word != NULL && *word != '\0') {
			xasprintf(&tmp, "%d", wl->idx);
			if (strncmp(tmp, word, strlen(word)) != 0) {
				free(tmp);
				continue;
			}
			free(tmp);
		}

		list = xreallocarray(list, size + 1, sizeof *list);
		if (c->prompt_type == PROMPT_TYPE_WINDOW_TARGET) {
			xasprintf(&tmp, "%d (%s)", wl->idx, wl->window->name);
			xasprintf(&list[size++], "%d", wl->idx);
		} else {
			xasprintf(&tmp, "%s:%d (%s)", s->name, wl->idx,
			    wl->window->name);
			xasprintf(&list[size++], "%s:%d", s->name, wl->idx);
		}
		item.name = tmp;
		item.key = '0' + size - 1;
		item.command = NULL;
		menu_add_item(menu, &item, NULL, c, NULL);
		free(tmp);

		if (size == height)
			break;
	}
	if (size == 0) {
		menu_free(menu);
		return (NULL);
	}
	if (size == 1) {
		menu_free(menu);
		if (flag != '\0') {
			xasprintf(&tmp, "-%c%s", flag, list[0]);
			free(list[0]);
		} else
			tmp = list[0];
		free(list);
		return (tmp);
	}
	if (height > size)
		height = size;

	spm->size = size;
	spm->list = list;

	if (options_get_number(c->session->options, "status-position") == 0)
		py = lines;
	else
		py = c->tty.sy - 3 - height;
	offset += utf8_cstrwidth(c->prompt_string);
	if (offset > 2)
		offset -= 2;
	else
		offset = 0;

	if (menu_display(menu, MENU_NOMOUSE|MENU_TAB, 0, NULL, offset, py, c,
	    BOX_LINES_DEFAULT, NULL, NULL, NULL, NULL,
	    status_prompt_menu_callback, spm) != 0) {
		menu_free(menu);
		free(spm);
		return (NULL);
	}
	return (NULL);
}

/* Sort complete list. */
static int
status_prompt_complete_sort(const void *a, const void *b)
{
	const char	**aa = (const char **)a, **bb = (const char **)b;

	return (strcmp(*aa, *bb));
}

/* Complete a session. */
static char *
status_prompt_complete_session(char ***list, u_int *size, const char *s,
    char flag)
{
	struct session	*loop;
	char		*out, *tmp, n[11];

	RB_FOREACH(loop, sessions, &sessions) {
		if (*s == '\0' || strncmp(loop->name, s, strlen(s)) == 0) {
			*list = xreallocarray(*list, (*size) + 2,
			    sizeof **list);
			xasprintf(&(*list)[(*size)++], "%s:", loop->name);
		} else if (*s == '$') {
			xsnprintf(n, sizeof n, "%u", loop->id);
			if (s[1] == '\0' ||
			    strncmp(n, s + 1, strlen(s) - 1) == 0) {
				*list = xreallocarray(*list, (*size) + 2,
				    sizeof **list);
				xasprintf(&(*list)[(*size)++], "$%s:", n);
			}
		}
	}
	out = status_prompt_complete_prefix(*list, *size);
	if (out != NULL && flag != '\0') {
		xasprintf(&tmp, "-%c%s", flag, out);
		free(out);
		out = tmp;
	}
	return (out);
}

/* Complete word. */
static char *
status_prompt_complete(struct client *c, const char *word, u_int offset)
{
	struct session	 *session;
	const char	 *s, *colon;
	char		**list = NULL, *copy = NULL, *out = NULL;
	char		  flag = '\0';
	u_int		  size = 0, i;

	if (*word == '\0' &&
	    c->prompt_type != PROMPT_TYPE_TARGET &&
	    c->prompt_type != PROMPT_TYPE_WINDOW_TARGET)
		return (NULL);

	if (c->prompt_type != PROMPT_TYPE_TARGET &&
	    c->prompt_type != PROMPT_TYPE_WINDOW_TARGET &&
	    strncmp(word, "-t", 2) != 0 &&
	    strncmp(word, "-s", 2) != 0) {
		list = status_prompt_complete_list(&size, word, offset == 0);
		if (size == 0)
			out = NULL;
		else if (size == 1)
			xasprintf(&out, "%s ", list[0]);
		else
			out = status_prompt_complete_prefix(list, size);
		goto found;
	}

	if (c->prompt_type == PROMPT_TYPE_TARGET ||
	    c->prompt_type == PROMPT_TYPE_WINDOW_TARGET) {
		s = word;
		flag = '\0';
	} else {
		s = word + 2;
		flag = word[1];
		offset += 2;
	}

	/* If this is a window completion, open the window menu. */
	if (c->prompt_type == PROMPT_TYPE_WINDOW_TARGET) {
		out = status_prompt_complete_window_menu(c, c->session, s,
		    offset, '\0');
		goto found;
	}
	colon = strchr(s, ':');

	/* If there is no colon, complete as a session. */
	if (colon == NULL) {
		out = status_prompt_complete_session(&list, &size, s, flag);
		goto found;
	}

	/* If there is a colon but no period, find session and show a menu. */
	if (strchr(colon + 1, '.') == NULL) {
		if (*s == ':')
			session = c->session;
		else {
			copy = xstrdup(s);
			*strchr(copy, ':') = '\0';
			session = session_find(copy);
			free(copy);
			if (session == NULL)
				goto found;
		}
		out = status_prompt_complete_window_menu(c, session, colon + 1,
		    offset, flag);
		if (out == NULL)
			return (NULL);
	}

found:
	if (size != 0) {
		qsort(list, size, sizeof *list, status_prompt_complete_sort);
		for (i = 0; i < size; i++)
			log_debug("complete %u: %s", i, list[i]);
	}

	if (out != NULL && strcmp(word, out) == 0) {
		free(out);
		out = NULL;
	}
	if (out != NULL ||
	    !status_prompt_complete_list_menu(c, list, size, offset, flag)) {
		for (i = 0; i < size; i++)
			free(list[i]);
		free(list);
	}
	return (out);
}

/* Return the type of the prompt as an enum. */
enum prompt_type
status_prompt_type(const char *type)
{
	u_int	i;

	for (i = 0; i < PROMPT_NTYPES; i++) {
		if (strcmp(type, status_prompt_type_string(i)) == 0)
			return (i);
	}
	return (PROMPT_TYPE_INVALID);
}

/* Accessor for prompt_type_strings. */
const char *
status_prompt_type_string(u_int type)
{
	if (type >= PROMPT_NTYPES)
		return ("invalid");
	return (prompt_type_strings[type]);
}
