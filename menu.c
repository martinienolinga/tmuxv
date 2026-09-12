/* $OpenBSD$ */

/*
 * Copyright (c) 2019 Nicholas Marriott <nicholas.marriott@gmail.com>
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
#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"
#include "tmuxv-version.h"

struct menu_data {
	struct cmdq_item	*item;
	int			 flags;

	struct grid_cell	 style;
	struct grid_cell	 border_style;
	struct grid_cell	 selected_style;
	enum box_lines		 border_lines;

	struct cmd_find_state	 fs;
	struct screen		 s;

	u_int			 px;
	u_int			 py;

	struct menu		*menu;
	int			 choice;

	int			 menubar_index; /* MENU BAR: -1 if not a bar menu */
	int			 seen_press;	/* a real press happened on it */

	menu_choice_cb		 cb;
	void			*data;
};

void
menu_add_items(struct menu *menu, const struct menu_item *items,
    struct cmdq_item *qitem, struct client *c, struct cmd_find_state *fs)
{
	const struct menu_item	*loop;

	for (loop = items; loop->name != NULL; loop++)
		menu_add_item(menu, loop, qitem, c, fs);
}

void
menu_add_item(struct menu *menu, const struct menu_item *item,
    struct cmdq_item *qitem, struct client *c, struct cmd_find_state *fs)
{
	struct menu_item	*new_item;
	const char		*key = NULL, *cmd, *suffix = "";
	char			*s, *trimmed, *name;
	u_int			 width, max_width;
	int			 line;
	size_t			 keylen, slen;

	line = (item == NULL || item->name == NULL || *item->name == '\0');
	if (line && menu->count == 0)
		return;
	if (line && menu->items[menu->count - 1].name == NULL)
		return;

	menu->items = xreallocarray(menu->items, menu->count + 1,
	    sizeof *menu->items);
	new_item = &menu->items[menu->count++];
	memset(new_item, 0, sizeof *new_item);

	if (line)
		return;

	if (fs != NULL)
		s = format_single_from_state(qitem, item->name, c, fs);
	else
		s = format_single(qitem, item->name, c, NULL, NULL, NULL);
	if (*s == '\0') { /* no item if empty after format expanded */
		menu->count--;
		return;
	}
	max_width = c->tty.sx - 4;

	slen = strlen(s);
	if (*s != '-' && item->key != KEYC_UNKNOWN && item->key != KEYC_NONE) {
		key = key_string_lookup_key(item->key, 0);
		keylen = strlen(key) + 3; /* 3 = space and two brackets */

		/*
		 * Add the key if it is shorter than a quarter of the available
		 * space or there is space for the entire item text and the
		 * key.
		 */
		if (keylen <= max_width / 4)
			max_width -= keylen;
		else if (keylen >= max_width || slen >= max_width - keylen)
			key = NULL;
	}

	if (slen > max_width) {
		max_width--;
		suffix = ">";
	}
	trimmed = format_trim_right(s, max_width);
	if (key != NULL) {
		xasprintf(&name, "%s%s#[default] #[align=right](%s)",
		    trimmed, suffix, key);
	} else
		xasprintf(&name, "%s%s", trimmed, suffix);
	free(trimmed);

	new_item->name = name;
	free(s);

	cmd = item->command;
	if (cmd != NULL) {
		if (fs != NULL)
			s = format_single_from_state(qitem, cmd, c, fs);
		else
			s = format_single(qitem, cmd, c, NULL, NULL, NULL);
	} else
		s = NULL;
	new_item->command = s;
	new_item->key = item->key;

	width = format_width(new_item->name);
	if (*new_item->name == '-')
		width--;
	if (width > menu->width)
		menu->width = width;
}

struct menu *
menu_create(const char *title)
{
	struct menu	*menu;

	menu = xcalloc(1, sizeof *menu);
	menu->title = xstrdup(title);
	menu->width = format_width(title);

	return (menu);
}

void
menu_free(struct menu *menu)
{
	u_int	i;

	for (i = 0; i < menu->count; i++) {
		free((void *)menu->items[i].name);
		free((void *)menu->items[i].command);
	}
	free(menu->items);

	free((void *)menu->title);
	free(menu);
}

struct screen *
menu_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct menu_data	*md = data;

	*cx = md->px + 2;
	if (md->choice == -1)
		*cy = md->py;
	else
		*cy = md->py + 1 + md->choice;

	return (&md->s);
}

/* Return parts of the input range which are not obstructed by the menu. */
void
menu_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct menu_data	*md = data;
	struct menu		*menu = md->menu;

	server_client_overlay_range(md->px, md->py, menu->width + 4,
	    menu->count + 2, px, py, nx, r);
}

/* Turbo Vision drop shadow (defined further down); TMenuBox sets sfShadow. */
static void	form_draw_shadow(struct client *, u_int, u_int, u_int, u_int);

void
menu_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct menu_data	*md = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &md->s;
	struct menu		*menu = md->menu;
	struct screen_write_ctx	 ctx;
	u_int			 i, px = md->px, py = md->py;
	u_int			 mw = menu->width + 4, mh = menu->count + 2;

	screen_write_start(&ctx, s);
	screen_write_clearscreen(&ctx, 8);

	if (md->border_lines != BOX_LINES_NONE) {
		screen_write_box(&ctx, mw, mh, md->border_lines,
		    &md->border_style, menu->title);
	}

	screen_write_menu(&ctx, menu, md->choice, md->border_lines,
	    &md->style, &md->border_style, &md->selected_style);
	screen_write_stop(&ctx);

	for (i = 0; i < screen_size_y(&md->s); i++) {
		tty_draw_line(tty, s, 0, i, mw, px, py + i,
		    &grid_default_cell, NULL);
	}

	/*
	 * Drop shadow like Turbo Vision's TMenuBox (state |= sfShadow;
	 * shadowSize = {2,1}): darken the cells 2 columns to the right and one
	 * row below, keeping their characters.
	 */
	form_draw_shadow(c, px + mw, py + 1, 2, mh - 1);
	form_draw_shadow(c, px + 2, py + mh, mw, 1);
}

void
menu_free_cb(__unused struct client *c, void *data)
{
	struct menu_data	*md = data;

	if (md->item != NULL)
		cmdq_continue(md->item);

	if (md->cb != NULL)
		md->cb(md->menu, UINT_MAX, KEYC_NONE, md->data);

	screen_free(&md->s);
	menu_free(md->menu);
	free(md);
}

int
menu_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct menu_data		*md = data;
	struct menu			*menu = md->menu;
	struct mouse_event		*m = &event->m;
	u_int				 i;
	int				 count = menu->count, old = md->choice;
	const char			*name = NULL;
	const struct menu_item		*item;
	struct cmdq_state		*state;
	enum cmd_parse_status		 status;
	char				*error;

	if (KEYC_IS_MOUSE(event->key)) {
		/*
		 * MENU BAR / hover: a bare pointer motion (mouse-all / 1003 mode)
		 * carries both the drag and release bits. Treat it as hover:
		 * it highlights but never selects or closes the menu, so menus
		 * are navigable by hovering (Turbo Vision style).
		 */
		int hover = (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b));

		if (md->flags & MENU_NOMOUSE) {
			if (MOUSE_BUTTONS(m->b) != MOUSE_BUTTON_1)
				return (1);
			return (0);
		}
		/*
		 * Turbo Vision: a menu opened BY a mouse press (right-click on
		 * a pane, click on the menu bar) must not be closed by the
		 * release of that very press - it stays until the next click.
		 * So swallow any release arriving before a press of our own,
		 * and only count real presses (not drags) as ours.
		 */
		if (MOUSE_RELEASE(m->b) && !hover) {
			if (!md->seen_press)
				return (0);
		} else if (!hover && !MOUSE_WHEEL(m->b) && !MOUSE_DRAG(m->b))
			md->seen_press = 1;
		if (m->x < md->px ||
		    m->x > md->px + 4 + menu->width ||
		    m->y < md->py + 1 ||
		    m->y > md->py + 1 + count - 1) {
			/*
			 * MENU BAR (Turbo Vision): once a menu is open, moving
			 * onto another top-level entry switches to it; clicking
			 * the entry of the open menu closes it.
			 */
			if (md->menubar_index >= 0 &&
			    m->y < menu_bar_size(c)) {
				struct style_range	*sr;

				sr = menu_bar_get_range(c, m->x);
				if (sr != NULL &&
				    sr->type == STYLE_RANGE_USER &&
				    strncmp(sr->string, "menu_", 5) == 0) {
					int n = atoi(sr->string + 5);
					int pressed = (!hover &&
					    !MOUSE_RELEASE(m->b));

					if (n == md->menubar_index) {
						if (pressed)
							return (1); /* close */
						return (0);
					}
					if (pressed || hover) {
						menu_bar_open(c, (u_int)n,
						    sr->start);
						return (0); /* old menu freed */
					}
					return (0);
				}
			}
			if (~md->flags & MENU_STAYOPEN) {
				if (MOUSE_RELEASE(m->b) && !hover)
					return (1);
			} else {
				if (!MOUSE_RELEASE(m->b) &&
				    !MOUSE_WHEEL(m->b) &&
				    !MOUSE_DRAG(m->b))
					return (1);
			}
			if (md->choice != -1) {
				md->choice = -1;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		if (~md->flags & MENU_STAYOPEN) {
			if (MOUSE_RELEASE(m->b) && !hover)
				goto chosen;
		} else {
			if (!MOUSE_WHEEL(m->b) && !MOUSE_DRAG(m->b))
				goto chosen;
		}
		md->choice = m->y - (md->py + 1);
		if (md->choice != old)
			c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
	for (i = 0; i < (u_int)count; i++) {
		name = menu->items[i].name;
		if (name == NULL || *name == '-')
			continue;
		if (event->key == menu->items[i].key) {
			md->choice = i;
			goto chosen;
		}
	}
	switch (event->key & ~KEYC_MASK_FLAGS) {
	case KEYC_UP:
	case 'k':
		if (old == -1)
			old = 0;
		do {
			if (md->choice == -1 || md->choice == 0)
				md->choice = count - 1;
			else
				md->choice--;
			name = menu->items[md->choice].name;
		} while ((name == NULL || *name == '-') && md->choice != old);
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_BSPACE:
		if (~md->flags & MENU_TAB)
			break;
		return (1);
	case '\011': /* Tab */
		if (~md->flags & MENU_TAB)
			break;
		if (md->choice == count - 1)
			return (1);
		/* FALLTHROUGH */
	case KEYC_DOWN:
	case 'j':
		if (old == -1)
			old = 0;
		do {
			if (md->choice == -1 || md->choice == count - 1)
				md->choice = 0;
			else
				md->choice++;
			name = menu->items[md->choice].name;
		} while ((name == NULL || *name == '-') && md->choice != old);
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_PPAGE:
	case '\002': /* C-b */
		if (md->choice < 6)
			md->choice = 0;
		else {
			i = 5;
			while (i > 0) {
				md->choice--;
				name = menu->items[md->choice].name;
				if (md->choice != 0 &&
				    (name != NULL && *name != '-'))
					i--;
				else if (md->choice == 0)
					break;
			}
		}
		c->flags |= CLIENT_REDRAWOVERLAY;
		break;
	case KEYC_NPAGE:
		if (md->choice > count - 6) {
			md->choice = count - 1;
			name = menu->items[md->choice].name;
		} else {
			i = 5;
			while (i > 0) {
				md->choice++;
				name = menu->items[md->choice].name;
				if (md->choice != count - 1 &&
				    (name != NULL && *name != '-'))
					i++;
				else if (md->choice == count - 1)
					break;
			}
		}
		while (name == NULL || *name == '-') {
			md->choice--;
			name = menu->items[md->choice].name;
		}
		c->flags |= CLIENT_REDRAWOVERLAY;
		break;
	case 'g':
	case KEYC_HOME:
		md->choice = 0;
		name = menu->items[md->choice].name;
		while (name == NULL || *name == '-') {
			md->choice++;
			name = menu->items[md->choice].name;
		}
		c->flags |= CLIENT_REDRAWOVERLAY;
		break;
	case 'G':
	case KEYC_END:
		md->choice = count - 1;
		name = menu->items[md->choice].name;
		while (name == NULL || *name == '-') {
			md->choice--;
			name = menu->items[md->choice].name;
		}
		c->flags |= CLIENT_REDRAWOVERLAY;
		break;
	case '\006': /* C-f */
		break;
	case '\r':
		goto chosen;
	case '\033': /* Escape */
	case '\003': /* C-c */
	case '\007': /* C-g */
	case 'q':
		return (1);
	}
	return (0);

chosen:
	if (md->choice == -1)
		return (1);
	item = &menu->items[md->choice];
	if (item->name == NULL || *item->name == '-') {
		if (md->flags & MENU_STAYOPEN)
			return (0);
		return (1);
	}
	if (md->cb != NULL) {
	    md->cb(md->menu, md->choice, item->key, md->data);
	    md->cb = NULL;
	    return (1);
	}

	if (md->item != NULL)
		event = cmdq_get_event(md->item);
	else
		event = NULL;
	state = cmdq_new_state(&md->fs, event, 0);

	status = cmd_parse_and_append(item->command, NULL, c, state, &error);
	if (status == CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);

	return (1);
}

static void
menu_set_style(struct client *c, struct grid_cell *gc, const char *style,
    const char *option)
{
	struct style	 sytmp;
	struct options	*o = c->session->curw->window->options;

	memcpy(gc, &grid_default_cell, sizeof *gc);
	style_apply(gc, o, option, NULL);
	if (style != NULL) {
		style_set(&sytmp, &grid_default_cell);
		if (style_parse(&sytmp, gc, style) == 0) {
			gc->fg = sytmp.gc.fg;
			gc->bg = sytmp.gc.bg;
		}
	}
	gc->attr = 0;
}

struct menu_data *
menu_prepare(struct menu *menu, int flags, int starting_choice,
    struct cmdq_item *item, u_int px, u_int py, struct client *c,
    enum box_lines lines, const char *style, const char *selected_style,
    const char *border_style, struct cmd_find_state *fs, menu_choice_cb cb,
    void *data)
{
	struct menu_data	*md;
	int			 choice;
	const char		*name;
	struct options		*o = c->session->curw->window->options;

	if (c->tty.sx < menu->width + 4 || c->tty.sy < menu->count + 2)
		return (NULL);
	if (px + menu->width + 4 > c->tty.sx)
		px = c->tty.sx - menu->width - 4;
	if (py + menu->count + 2 > c->tty.sy)
		py = c->tty.sy - menu->count - 2;

	if (lines == BOX_LINES_DEFAULT)
		lines = options_get_number(o, "menu-border-lines");

	md = xcalloc(1, sizeof *md);
	md->item = item;
	md->flags = flags;
	md->border_lines = lines;

	/*
	 * Turbo Vision desktop: menus opened without explicit styles (the
	 * right-click pane menu, display-menu...) use the same grey/green
	 * palette as the menu bar dropdowns instead of the tmux options.
	 */
	if (desktop_enabled(c)) {
		if (style == NULL)
			style = MENU_BAR_MENU_STYLE;
		if (selected_style == NULL)
			selected_style = MENU_BAR_SELECTED_STYLE;
		if (border_style == NULL)
			border_style = MENU_BAR_BORDER_STYLE;
	}
	menu_set_style(c, &md->style, style, "menu-style");
	menu_set_style(c, &md->selected_style, selected_style,
	    "menu-selected-style");
	menu_set_style(c, &md->border_style, border_style, "menu-border-style");

	if (fs != NULL)
		cmd_find_copy_state(&md->fs, fs);
	screen_init(&md->s, menu->width + 4, menu->count + 2, 0);
	if (~md->flags & MENU_NOMOUSE)
		md->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	md->s.mode &= ~MODE_CURSOR;

	md->px = px;
	md->py = py;

	md->menu = menu;
	md->choice = -1;
	md->menubar_index = -1; /* MENU BAR */

	if (md->flags & MENU_NOMOUSE) {
		if (starting_choice >= (int)menu->count) {
			starting_choice = menu->count - 1;
			choice = starting_choice + 1;
			for (;;) {
				name = menu->items[choice - 1].name;
				if (name != NULL && *name != '-') {
					md->choice = choice - 1;
					break;
				}
				if (--choice == 0)
					choice = menu->count;
				if (choice == starting_choice + 1)
					break;
			}
		} else if (starting_choice >= 0) {
			choice = starting_choice;
			for (;;) {
				name = menu->items[choice].name;
				if (name != NULL && *name != '-') {
					md->choice = choice;
					break;
				}
				if (++choice == (int)menu->count)
					choice = 0;
				if (choice == starting_choice)
					break;
			}
		}
	}

	md->cb = cb;
	md->data = data;
	return (md);
}

int
menu_display(struct menu *menu, int flags, int starting_choice,
    struct cmdq_item *item, u_int px, u_int py, struct client *c,
    enum box_lines lines, const char *style, const char *selected_style,
    const char *border_style, struct cmd_find_state *fs, menu_choice_cb cb,
    void *data)
{
	struct menu_data	*md;

	md = menu_prepare(menu, flags, starting_choice, item, px, py, c, lines,
	    style, selected_style, border_style, fs, cb, data);
	if (md == NULL)
		return (-1);
	server_client_set_overlay(c, 0, NULL, menu_mode_cb, menu_draw_cb,
	    menu_key_cb, menu_free_cb, NULL, md);
	return (0);
}

/* MENU BAR: like menu_display but tags the menu with its top-bar index. */
int
menu_display_menubar(struct menu *menu, int flags, struct cmdq_item *item,
    u_int px, u_int py, struct client *c, enum box_lines lines,
    const char *style, const char *selected_style, const char *border_style,
    struct cmd_find_state *fs, u_int menubar_index)
{
	struct menu_data	*md;

	md = menu_prepare(menu, flags, -1, item, px, py, c, lines, style,
	    selected_style, border_style, fs, NULL, NULL);
	if (md == NULL)
		return (-1);
	md->menubar_index = (int)menubar_index;
	server_client_set_overlay(c, 0, NULL, menu_mode_cb, menu_draw_cb,
	    menu_key_cb, menu_free_cb, NULL, md);
	return (0);
}

/*
 * ==========================================================================
 * PARAMÈTRES : dialogue façon Turbo Vision pour configurer tmux.conf.
 * Contrôles inspirés de TVision : cases à cocher, boutons radio, champs de
 * saisie, boutons [ OK ]/[ Annuler ]. À la validation, les options sont
 * appliquées à chaud (set -g/-s, sans redémarrer le serveur) ET persistées
 * dans ~/.tmux.conf (dans un bloc balisé, le reste du fichier est préservé).
 * ==========================================================================
 */

/* Dialog is sized dynamically to the terminal, within these bounds. */
#define FORM_MINW	60
#define FORM_MAXW	118
#define FORM_MINH	16
#define FORM_MAXH	44
#define FORM_NAMEW	30	/* option-name column width */
#define FORM_LIST_Y	3	/* first list row */

/* Thème dialogue Turbo Vision (gris, focus bleu, boutons verts). */
#define FORM_DLG_FG	0x000000
#define FORM_DLG_BG	0xc0c0c0
#define FORM_FOC_FG	0xffffff
#define FORM_FOC_BG	0x0000a8
#define FORM_INP_FG	0x000000
#define FORM_INP_BG	0xffffff
#define FORM_INP_FBG	0x00a8a8
#define FORM_BTN_FG	0x000000
#define FORM_BTN_BG	0xc0c0c0
#define FORM_BFOC_FG	0xffffff
#define FORM_BFOC_BG	0x00a800

#define FORM_DIM_FG	0x5a5a5a
#define FORM_HDR_FG	0x00007a
#define FORM_ERR_FG	0xa80000
#define FORM_HLP_FG	0x00005a

enum form_type { FT_CHECK, FT_INPUT, FT_RADIO, FT_SECTION };

/* Buttons, left to right. */
enum { FB_ADD, FB_DEL, FB_APPLY, FB_OK, FB_CANCEL, FB_N };

/* Sections, in display order. */
enum { FS_USER, FS_SESSION, FS_WINDOW, FS_SERVER, FS_N };
static const char *form_section_title[FS_N] = {
	"Options utilisateur (@...)",
	"Session (set -g)",
	"Fenêtre (set -wg)",
	"Serveur (set -s)"
};

struct form_ctl {
	enum form_type	 type;
	char		 label[80];		/* name, or name[idx] */

	int		 checked;		/* check */
	char		 text[512];		/* input value */
	u_int		 cur;			/* input cursor (byte offset) */
	int		 numeric;
	const char *const *choices;		/* radio: NULL-terminated list */
	int		 sel;

	char		 name[64];		/* option name */
	int		 idx;			/* array item, or -1 */
	char		 orig[512];		/* value when loaded / applied */
	char		 defval[256];		/* built-in default */
	int		 hasdef;
	struct options	*oo;
	const struct options_table_entry *oe;
	const char	*scope;			/* "-s", "-g", "-wg" */
	int		 section;
	int		 isuser;		/* @option */
	int		 isarray;		/* one item of an array option */
	int		 isnew;			/* added in this dialog */
	int		 deleted;		/* remove (@, item) / reset (built-in) */
	int		 applied;		/* scratch, during apply */
};

struct form_data {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	struct form_ctl	*ctl;			/* sections + options, sorted */
	u_int		 nctl, cap;
	u_int		*vis;			/* shown rows (ctl indices) */
	u_int		 nvis;
	int		 fkind;			/* 0 search, 1 row, 2 button */
	u_int		 frow;			/* focused row (index in vis) */
	u_int		 fbtn;			/* focused button */
	u_int		 scroll;		/* first shown row (index in vis) */
	char		 search[128];
	u_int		 scur;
	u_int		 cur_cx, cur_cy;
	int		 pressed;		/* button drawn sunken, or -1 */
	struct event	 flash;
	int		 flash_btn;
	struct event	 sb_timer;		/* TScrollBar auto-repeat */
	int		 sb_mode, sb_dir, sb_on;
	u_int		 sb_ax, sb_ay;
	char		 msg[256];		/* result / error, in the help area */
	int		 msg_err;
	int		 adding;		/* the "new option" box is open */
	int		 afield;		/* 0 name, 1 value */
	char		 aname[128];
	char		 aval[512];
	u_int		 acur[2];
	char		 aerr[160];
};

static int	form_btn_action(struct client *, struct form_data *, u_int);

/* Double-quote a value for a config line, escaping " and \ (and stripping
 * newlines). Prevents any injection into ~/.tmux.conf. */
static void
form_quote(char *dst, size_t dstsize, const char *val)
{
	size_t		 di = 0;
	const char	*p;

	if (dstsize < 3) {
		if (dstsize > 0)
			dst[0] = '\0';
		return;
	}
	dst[di++] = '"';
	for (p = val; *p != '\0' && di + 2 < dstsize - 1; p++) {
		if (*p == '\n' || *p == '\r') {
			dst[di++] = ' ';
			continue;
		}
		if (*p == '"' || *p == '\\')
			dst[di++] = '\\';
		dst[di++] = *p;
	}
	dst[di++] = '"';
	dst[di] = '\0';
}

static void
form_gc(struct grid_cell *gc, int fg, int bg)
{
	memcpy(gc, &grid_default_cell, sizeof *gc);
	gc->fg = colour_join_rgb((fg >> 16) & 0xff, (fg >> 8) & 0xff, fg & 0xff);
	gc->bg = colour_join_rgb((bg >> 16) & 0xff, (bg >> 8) & 0xff, bg & 0xff);
	gc->attr = 0;
}

/*
 * Turbo Vision TScrollBar (tscrlbar.cpp), shared by the desktop windows, the
 * settings form and the text dialogs - one implementation, like the single
 * TScrollBar class. A bar is `size` cells from (x, y) downwards: ▲ on top,
 * ▼ at the bottom and a track of size-2 cells between them. The indicator is
 * ONE ■ cell placed with TScrollBar::getPos() rounding on a ▒ track; when
 * there is nothing to scroll (range == 0) the track is ▓ with no indicator.
 * `value` is the scroll position in [0, range] (TVision minVal..maxVal).
 */
u_int
scrollbar_pos(u_int value, u_int range, u_int track)
{
	if (range == 0 || track <= 1)
		return (0);
	if (value > range)
		value = range;
	/* getPos(): ((value-min) * (size-3) + r/2) / r, and size-3 == track-1. */
	return ((u_int)(((uint64_t)value * (track - 1) + range / 2) / range));
}

/* Inverse of scrollbar_pos, used while dragging the thumb (handleEvent). */
u_int
scrollbar_value(u_int p, u_int range, u_int track)
{
	if (track <= 1)
		return (0);
	if (p > track - 1)
		p = track - 1;
	return ((u_int)(((uint64_t)p * range + (track - 1) / 2) / (track - 1)));
}

static void
scrollbar_glyph(struct grid_cell *gc, u_char b2)
{
	gc->data.data[0] = 0xe2; gc->data.data[1] = 0x96; gc->data.data[2] = b2;
	gc->data.have = gc->data.size = 3;
	gc->data.width = 1;
}

/* TScrollBar::drawPos(getPos()). */
void
scrollbar_draw(struct screen_write_ctx *ctx, u_int x, u_int y, u_int size,
    u_int value, u_int range, int fg, int bg)
{
	struct grid_cell	up, dn, th, tk, nf;
	u_int			track, p, ip;

	if (size < 3)
		return;
	track = size - 2;
	form_gc(&tk, fg, bg);
	memcpy(&up, &tk, sizeof up); memcpy(&dn, &tk, sizeof dn);
	memcpy(&th, &tk, sizeof th); memcpy(&nf, &tk, sizeof nf);
	/* CP437 vChars 1E 1F B1 FE B2 -> ▲ ▼ ▒ ■ ▓ */
	scrollbar_glyph(&up, 0xb2);	/* ▲ U+25B2 */
	scrollbar_glyph(&dn, 0xbc);	/* ▼ U+25BC */
	scrollbar_glyph(&tk, 0x92);	/* ▒ U+2592 page area */
	scrollbar_glyph(&th, 0xa0);	/* ■ U+25A0 indicator */
	scrollbar_glyph(&nf, 0x93);	/* ▓ U+2593 nothing to scroll */

	ip = scrollbar_pos(value, range, track);
	screen_write_cursormove(ctx, x, y, 0);
	screen_write_cell(ctx, &up);
	for (p = 0; p < track; p++) {
		screen_write_cursormove(ctx, x, y + 1 + p, 0);
		if (range == 0)
			screen_write_cell(ctx, &nf);
		else
			screen_write_cell(ctx, (p == ip) ? &th : &tk);
	}
	screen_write_cursormove(ctx, x, y + size - 1, 0);
	screen_write_cell(ctx, &dn);
}

static struct screen *
form_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct form_data	*fd = data;

	*cx = fd->cur_cx;	/* computed in form_draw_cb */
	*cy = fd->cur_cy;
	return (&fd->s);
}

/*
 * Clip the background (panes/desktop) to everything OUTSIDE the dialog so it
 * keeps refreshing around it. The shadow area is intentionally NOT clipped:
 * the background paints it and the overlay repaints the (darkened) shadow on
 * top every frame.
 */
static void
form_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct form_data	*fd = data;

	server_client_overlay_range(fd->px, fd->py, fd->w, fd->h, px, py, nx,
	    r);
}

/* Read the cell currently under (X, Y) on screen, for the drop shadow. */
static void
form_underlying_cell(struct client *c, u_int X, u_int Y, struct grid_cell *gc)
{
	struct session		*s = c->session;
	struct window		*w = (s != NULL) ? s->curw->window : NULL;
	u_int			 mb = menu_bar_size(c);
	u_int			 top = (status_at_line(c) == 0) ?
				     status_line_size(c) : 0;
	u_int			 area_top = mb + top;
	struct window_pane	*wp;

	memcpy(gc, &grid_default_cell, sizeof *gc);
	gc->data.data[0] = ' ';
	gc->data.size = gc->data.have = 1;
	gc->data.width = 1;

	if (desktop_enabled(c) && c->desktop_buffer_valid) {
		if (Y >= area_top) {
			u_int ry = Y - area_top;
			if (X < screen_size_x(&c->desktop_buffer) &&
			    ry < screen_size_y(&c->desktop_buffer))
				grid_get_cell(c->desktop_buffer.grid, X, ry, gc);
		}
		return;
	}
	if (w != NULL && Y >= area_top) {
		u_int px = X, py = Y - area_top;

		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (!window_pane_visible(wp))
				continue;
			if (px >= wp->xoff && px < wp->xoff + wp->sx &&
			    py >= wp->yoff && py < wp->yoff + wp->sy) {
				struct grid *gd = wp->screen->grid;
				grid_get_cell(gd, px - wp->xoff,
				    gd->hsize + (py - wp->yoff), gc);
				break;
			}
		}
	}
}

/* Turbo Vision shadow: keep the character, dim the colours. */
static void
form_darken(struct grid_cell *gc)
{
	gc->fg = colour_join_rgb(0x40, 0x40, 0x40);
	gc->bg = colour_join_rgb(0x00, 0x00, 0x00);
	gc->attr &= ~GRID_ATTR_BRIGHT;
	gc->us = 0;
}

/* Draw one strip (w x h) of drop shadow at screen (atx, aty). */
static void
form_draw_shadow(struct client *c, u_int atx, u_int aty, u_int w, u_int h)
{
	struct tty		*tty = &c->tty;
	struct screen		 sh;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 g;
	u_int			 i, j;

	if (w == 0 || h == 0 || atx >= tty->sx || aty >= tty->sy)
		return;
	if (atx + w > tty->sx)
		w = tty->sx - atx;
	if (aty + h > tty->sy)
		h = tty->sy - aty;
	screen_init(&sh, w, h, 0);
	screen_write_start(&ctx, &sh);
	for (j = 0; j < h; j++) {
		for (i = 0; i < w; i++) {
			form_underlying_cell(c, atx + i, aty + j, &g);
			form_darken(&g);
			screen_write_cursormove(&ctx, i, j, 0);
			screen_write_cell(&ctx, &g);
		}
	}
	screen_write_stop(&ctx);
	for (j = 0; j < h; j++)
		tty_draw_line(tty, &sh, 0, j, w, atx, aty + j,
		    &grid_default_cell, NULL);
	screen_free(&sh);
}

/*
 * Turbo Vision push button (TButton): the title is centred on a coloured face
 * (green when it is the default/focused button, grey otherwise), with NO
 * brackets, and a drop shadow made of ▄ (right, U+2584) and ▀ (bottom, U+2580)
 * in black - exactly the raised look of tvforms/tvdemo. The face occupies the
 * row `row`; the shadow uses `row` (right cell) and `row + 1` (bottom strip).
 * Returns the face width (strlen(label) + 4) for hit-testing.
 */
static u_int
form_draw_button(struct screen_write_ctx *ctx, u_int x, u_int row,
    const char *label, int active, int pressed)
{
	struct grid_cell	 gc, sh;
	char			 face[80];
	u_int			 lw = utf8_cstrwidth(label), bw = lw + 4, i;

	/* Columns, not bytes: an accented label stays centred. */
	snprintf(face, sizeof face, "  %s  ", label);

	form_gc(&gc, active ? FORM_BFOC_FG : FORM_BTN_FG,
	    active ? FORM_BFOC_BG : FORM_BTN_BG);
	if (pressed) {
		/*
		 * Turbo Vision "down" state (TButton::drawState(True)): the face
		 * moves one cell down-right into the shadow, and the drop shadow
		 * disappears - so the button visibly sinks on click.
		 */
		screen_write_cursormove(ctx, x + 1, row + 1, 0);
		screen_write_puts(ctx, &gc, "%s", face);
		return (bw);
	}
	screen_write_cursormove(ctx, x, row, 0);
	screen_write_puts(ctx, &gc, "%s", face);

	form_gc(&sh, 0x000000, FORM_DLG_BG);		/* black shadow on grey */
	sh.data.data[0] = 0xe2; sh.data.data[1] = 0x96; sh.data.data[2] = 0x84;
	sh.data.have = sh.data.size = 3; sh.data.width = 1;	/* ▄ right */
	screen_write_cursormove(ctx, x + bw, row, 0);
	screen_write_cell(ctx, &sh);
	sh.data.data[2] = 0x80;					/* ▀ bottom */
	for (i = 0; i < bw; i++) {
		screen_write_cursormove(ctx, x + 1 + i, row + 1, 0);
		screen_write_cell(ctx, &sh);
	}
	return (bw);
}

static int
form_radio_hit(struct form_ctl *ct, u_int fieldw, u_int lx)
{
	u_int	cx = 0, i;

	for (i = 0; ct->choices[i] != NULL; i++) {
		u_int	tok = 4 + (u_int)strlen(ct->choices[i]) + 1;

		if (cx + tok > fieldw)
			break;
		if (lx >= cx && lx < cx + tok)
			return ((int)i);
		cx += tok;
	}
	return (-1);
}

/* Copy at most `cols` characters of UTF-8 `src` (one column each). */
static u_int
form_clip(char *dst, size_t len, const char *src, u_int cols)
{
	size_t	 i = 0, o = 0, k, n;
	u_int	 w = 0;
	u_char	 b;

	while (src[i] != '\0' && w < cols) {
		b = (u_char)src[i];
		n = (b < 0x80) ? 1 : (b >= 0xf0) ? 4 : (b >= 0xe0) ? 3 :
		    (b >= 0xc0) ? 2 : 1;
		if (o + n + 1 > len)
			break;
		for (k = 0; k < n && src[i] != '\0'; k++)
			dst[o++] = src[i++];
		w++;
	}
	dst[o] = '\0';
	return (w);
}

static void
form_put(struct screen_write_ctx *ctx, struct grid_cell *gc, u_int x, u_int y,
    u_int cols, const char *text)
{
	char	buf[1024];

	form_clip(buf, sizeof buf, text, cols);
	screen_write_cursormove(ctx, x, y, 0);
	screen_write_puts(ctx, gc, "%s", buf);
}

/* Geometry. */
static u_int
form_listrows(struct form_data *fd)
{
	return (fd->h > 10 ? fd->h - 9 : 1);
}

static u_int
form_valx(void)
{
	return (2 + FORM_NAMEW + 1);
}

static u_int
form_fieldw(struct form_data *fd)
{
	u_int	sb = fd->w - 2, v = form_valx();

	return (sb > v + 1 ? sb - v - 1 : 4);
}

static u_int
form_maxscroll(struct form_data *fd)
{
	u_int	L = form_listrows(fd);

	return (fd->nvis > L ? fd->nvis - L : 0);
}

/* The value a control holds right now, as a tmux option string. */
static const char *
form_value(struct form_ctl *ct)
{
	if (ct->type == FT_CHECK)
		return (ct->checked ? "on" : "off");
	if (ct->type == FT_RADIO)
		return (ct->choices[ct->sel]);
	return (ct->text);
}

static int
form_modified(struct form_ctl *ct)
{
	if (ct->type == FT_SECTION)
		return (0);
	return (ct->isnew || ct->deleted ||
	    strcmp(form_value(ct), ct->orig) != 0);
}

static int
form_nondefault(struct form_ctl *ct)
{
	return (ct->hasdef && strcmp(form_value(ct), ct->defval) != 0);
}

static struct form_ctl *
form_add(struct form_data *fd)
{
	struct form_ctl	*ct;

	if (fd->nctl == fd->cap) {
		fd->cap = fd->cap ? fd->cap * 2 : 64;
		fd->ctl = xreallocarray(fd->ctl, fd->cap, sizeof *fd->ctl);
	}
	ct = &fd->ctl[fd->nctl++];
	memset(ct, 0, sizeof *ct);
	ct->idx = -1;
	return (ct);
}

static int
form_ctl_cmp(const void *a0, const void *b0)
{
	const struct form_ctl	*a = a0, *b = b0;
	int			 r;

	if (a->section != b->section)
		return (a->section - b->section);
	if (a->type == FT_SECTION)
		return (-1);
	if (b->type == FT_SECTION)
		return (1);
	if ((r = strcmp(a->name, b->name)) != 0)
		return (r);
	return (a->idx - b->idx);
}

static struct form_ctl *
form_focused_ctl(struct form_data *fd)
{
	if (fd->fkind != 1 || fd->frow >= fd->nvis)
		return (NULL);
	return (&fd->ctl[fd->vis[fd->frow]]);
}

static int
form_is_row(struct form_data *fd, u_int v)
{
	return (v < fd->nvis && fd->ctl[fd->vis[v]].type != FT_SECTION);
}

/* Option rows in total (sections excluded). */
static u_int
form_total(struct form_data *fd)
{
	u_int	i, n = 0;

	for (i = 0; i < fd->nctl; i++) {
		if (fd->ctl[i].type != FT_SECTION)
			n++;
	}
	return (n);
}

static int
form_match(struct form_ctl *ct, const char *q)
{
	if (strcasestr(ct->label, q) != NULL)
		return (1);
	if (strcasestr(form_value(ct), q) != NULL)
		return (1);
	if (ct->oe != NULL && ct->oe->text != NULL &&
	    strcasestr(ct->oe->text, q) != NULL)
		return (1);
	return (0);
}

/* Keep the focused row visible, with its section title when at its top. */
static void
form_scroll_to_focus(struct form_data *fd)
{
	u_int	L = form_listrows(fd), m;

	if (fd->fkind == 1) {
		if (fd->frow < fd->scroll) {
			fd->scroll = fd->frow;
			if (fd->scroll > 0 && !form_is_row(fd, fd->scroll - 1))
				fd->scroll--;
		} else if (fd->frow >= fd->scroll + L)
			fd->scroll = fd->frow - L + 1;
	}
	m = form_maxscroll(fd);
	if (fd->scroll > m)
		fd->scroll = m;
}

/*
 * Rebuild the shown rows. Without a search: every section with its options.
 * With one: a flat list, the exact name first, then every option whose name,
 * value or description contains the text.
 */
static void
form_filter(struct form_data *fd)
{
	struct form_ctl	*cur = form_focused_ctl(fd), *ct;
	char		 keep[80] = "";
	u_int		 i, j, n = 0, pass;

	if (cur != NULL)
		strlcpy(keep, cur->label, sizeof keep);
	fd->vis = xreallocarray(fd->vis, fd->nctl + 1, sizeof *fd->vis);
	if (fd->search[0] == '\0') {
		for (i = 0; i < fd->nctl; i++) {
			if (fd->ctl[i].type == FT_SECTION) {
				j = i + 1;
				if (j >= fd->nctl || fd->ctl[j].type == FT_SECTION)
					continue;	/* empty section */
			}
			fd->vis[n++] = i;
		}
	} else {
		for (pass = 0; pass < 2; pass++) {
			for (i = 0; i < fd->nctl; i++) {
				ct = &fd->ctl[i];
				if (ct->type == FT_SECTION)
					continue;
				if (pass == 0 &&
				    strcasecmp(ct->label, fd->search) == 0)
					fd->vis[n++] = i;
				else if (pass == 1 &&
				    strcasecmp(ct->label, fd->search) != 0 &&
				    form_match(ct, fd->search))
					fd->vis[n++] = i;
			}
		}
	}
	fd->nvis = n;

	/* The same option stays focused when it is still shown. */
	if (fd->fkind == 1) {
		for (i = 0; i < n; i++) {
			if (*keep != '\0' &&
			    strcmp(fd->ctl[fd->vis[i]].label, keep) == 0)
				break;
		}
		if (i < n)
			fd->frow = i;
		else {
			for (i = 0; i < n && !form_is_row(fd, i); i++)
				/* nothing */;
			if (i < n)
				fd->frow = i;
			else
				fd->fkind = 0;
		}
	}
	if (fd->search[0] != '\0' && fd->fkind != 1)
		fd->scroll = 0;
	form_scroll_to_focus(fd);
}

/* First / last option row, from `from` towards `dir`; -1 if none. */
static int
form_next_row(struct form_data *fd, int from, int dir)
{
	int	v;

	for (v = from; v >= 0 && v < (int)fd->nvis; v += dir) {
		if (form_is_row(fd, (u_int)v))
			return (v);
	}
	return (-1);
}

static void
form_focus_row(struct form_data *fd, int v)
{
	if (v < 0)
		return;
	fd->fkind = 1;
	fd->frow = (u_int)v;
	form_scroll_to_focus(fd);
}

/* Tab / Shift-Tab: search -> rows -> buttons -> search. */
static void
form_focus_step(struct form_data *fd, int dir)
{
	int	v;

	if (dir > 0) {
		if (fd->fkind == 0) {
			if ((v = form_next_row(fd, 0, 1)) >= 0)
				form_focus_row(fd, v);
			else {
				fd->fkind = 2;
				fd->fbtn = 0;
			}
		} else if (fd->fkind == 1) {
			if ((v = form_next_row(fd, (int)fd->frow + 1, 1)) >= 0)
				form_focus_row(fd, v);
			else {
				fd->fkind = 2;
				fd->fbtn = 0;
			}
		} else if (fd->fbtn + 1 < FB_N)
			fd->fbtn++;
		else
			fd->fkind = 0;
	} else {
		if (fd->fkind == 0) {
			fd->fkind = 2;
			fd->fbtn = FB_N - 1;
		} else if (fd->fkind == 2) {
			if (fd->fbtn > 0)
				fd->fbtn--;
			else if ((v = form_next_row(fd, (int)fd->nvis - 1,
			    -1)) >= 0)
				form_focus_row(fd, v);
			else
				fd->fkind = 0;
		} else {
			if ((v = form_next_row(fd, (int)fd->frow - 1, -1)) >= 0)
				form_focus_row(fd, v);
			else
				fd->fkind = 0;
		}
	}
}

/* Button labels: "Supprimer" follows what the focused row allows. */
static const char *
form_btn_label(struct form_data *fd, u_int b)
{
	struct form_ctl	*ct;

	switch (b) {
	case FB_ADD:
		return ("Ajouter");
	case FB_DEL:
		ct = form_focused_ctl(fd);
		if (ct != NULL && ct->deleted)
			return ("Rétablir");
		if (ct != NULL && !ct->isuser && !ct->isarray && !ct->isnew)
			return ("Réinitialiser");
		return ("Supprimer");
	case FB_APPLY:
		return ("Appliquer");
	case FB_OK:
		return ("OK");
	}
	return ("Annuler");
}

static void
form_btn_layout(struct form_data *fd, u_int *x, u_int *bw)
{
	u_int	b;

	for (b = 0; b < FB_N; b++)
		bw[b] = utf8_cstrwidth(form_btn_label(fd, b)) + 4;
	x[FB_ADD] = 2;
	x[FB_DEL] = x[FB_ADD] + bw[FB_ADD] + 3;
	x[FB_CANCEL] = fd->w - 2 - (bw[FB_CANCEL] + 1);
	x[FB_OK] = x[FB_CANCEL] - 2 - (bw[FB_OK] + 1);
	x[FB_APPLY] = x[FB_OK] - 2 - (bw[FB_APPLY] + 1);
}

static const char *
form_type_name(struct form_ctl *ct)
{
	if (ct->isuser)
		return ("texte (option utilisateur)");
	if (ct->isarray)
		return ("élément de tableau");
	if (ct->oe == NULL)
		return ("texte");
	switch (ct->oe->type) {
	case OPTIONS_TABLE_STRING:
		return ((ct->oe->flags & OPTIONS_TABLE_IS_STYLE) ? "style" :
		    "texte");
	case OPTIONS_TABLE_NUMBER:
		return ("nombre");
	case OPTIONS_TABLE_KEY:
		return ("touche");
	case OPTIONS_TABLE_COLOUR:
		return ("couleur");
	case OPTIONS_TABLE_FLAG:
		return ("oui / non");
	case OPTIONS_TABLE_CHOICE:
		return ("choix");
	case OPTIONS_TABLE_COMMAND:
		return ("commande");
	}
	return ("texte");
}

/* The "new option" box, drawn over the dialog. */
static void
form_draw_addbox(struct form_data *fd, struct screen_write_ctx *ctx)
{
	struct grid_cell	 dlg, frame, lab, fld, dim;
	u_int			 bw = fd->w - 8 < 72 ? fd->w - 8 : 72, bh = 9;
	u_int			 bx = (fd->w - bw) / 2, by = (fd->h - bh) / 2;
	u_int			 fx = bx + 12, fw = bw - 14, i, j, f, off, k;
	const char		*txt;
	char			 buf[600];

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;
	for (j = 0; j < bh; j++) {
		screen_write_cursormove(ctx, bx, by + j, 0);
		for (i = 0; i < bw; i++)
			screen_write_putc(ctx, &dlg, ' ');
	}
	screen_write_cursormove(ctx, bx, by, 0);
	screen_write_box(ctx, bw, bh, BOX_LINES_DOUBLE, &frame,
	    " Nouvelle option ");
	for (f = 0; f < 2; f++) {
		u_int	 row = by + 2 + f * 2;

		form_gc(&lab, FORM_DLG_FG, FORM_DLG_BG);
		form_put(ctx, &lab, bx + 2, row, 10, f == 0 ? "Nom" : "Valeur");
		txt = (f == 0) ? fd->aname : fd->aval;
		off = (fd->acur[f] >= fw) ? fd->acur[f] - fw + 1 : 0;
		form_gc(&fld, FORM_INP_FG, (fd->afield == (int)f) ?
		    FORM_INP_FBG : FORM_INP_BG);
		for (k = 0; k < fw && k < sizeof buf - 1; k++)
			buf[k] = (off + k < strlen(txt)) ? txt[off + k] : ' ';
		buf[k] = '\0';
		screen_write_cursormove(ctx, fx, row, 0);
		screen_write_puts(ctx, &fld, "%s", buf);
		if (fd->afield == (int)f) {
			fd->cur_cx = fd->px + fx + (fd->acur[f] - off);
			fd->cur_cy = fd->py + row;
		}
	}
	if (fd->aerr[0] != '\0') {
		form_gc(&dim, FORM_ERR_FG, FORM_DLG_BG);
		form_put(ctx, &dim, bx + 2, by + 6, bw - 4, fd->aerr);
	} else {
		form_gc(&dim, FORM_DIM_FG, FORM_DLG_BG);
		form_put(ctx, &dim, bx + 2, by + 6, bw - 4,
		    "@nom = option utilisateur, nom[n] = élément de tableau");
	}
	form_gc(&dim, FORM_DIM_FG, FORM_DLG_BG);
	form_put(ctx, &dim, bx + 2, by + 7, bw - 4,
	    "Entrée : ajouter    Tab : champ suivant    Échap : annuler");
}

static void
form_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct form_data	*fd = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &fd->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, gc, ig, mk, hd;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	struct form_ctl		*fc;
	u_int			 W = fd->w, H = fd->h, i, j;
	u_int			 L = form_listrows(fd), valx = form_valx();
	u_int			 sbcol = W - 2, fieldw = form_fieldw(fd);
	u_int			 bx[FB_N], bw[FB_N], shown = 0;
	u_int			 sx = 15, sw, soff, k;
	char			 buf[1024], line[1024];
	int			 cursor = 0;

	form_scroll_to_focus(fd);
	fd->cur_cx = fd->px;
	fd->cur_cy = fd->py;

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame,
	    " Paramètres tmux ");

	/* Search line. */
	form_gc(&gc, fd->fkind == 0 ? FORM_FOC_FG : FORM_DLG_FG,
	    fd->fkind == 0 ? FORM_FOC_BG : FORM_DLG_BG);
	form_put(&ctx, &gc, 2, 1, 12, "Rechercher :");
	sw = (W > sx + 16) ? W - sx - 16 : 8;
	soff = (fd->scur >= sw) ? fd->scur - sw + 1 : 0;
	form_gc(&ig, FORM_INP_FG, fd->fkind == 0 ? FORM_INP_FBG : FORM_INP_BG);
	for (k = 0; k < sw && k < sizeof buf - 1; k++)
		buf[k] = (soff + k < strlen(fd->search)) ?
		    fd->search[soff + k] : ' ';
	buf[k] = '\0';
	screen_write_cursormove(&ctx, sx, 1, 0);
	screen_write_puts(&ctx, &ig, "%s", buf);
	if (fd->search[0] == '\0' && fd->fkind != 0) {
		form_gc(&ig, FORM_DIM_FG, FORM_INP_BG);
		form_put(&ctx, &ig, sx + 1, 1, sw - 2,
		    "nom, valeur ou description   ( / )");
	}
	if (fd->fkind == 0) {
		fd->cur_cx = fd->px + sx + (fd->scur - soff);
		fd->cur_cy = fd->py + 1;
		cursor = 1;
	}
	for (i = 0; i < fd->nvis; i++) {
		if (form_is_row(fd, i))
			shown++;
	}
	snprintf(line, sizeof line, "%u / %u", shown, form_total(fd));
	form_gc(&gc, FORM_DIM_FG, FORM_DLG_BG);
	form_put(&ctx, &gc, sx + sw + 2, 1, W - (sx + sw + 4), line);

	/* The list. */
	for (j = 0; j < L; j++) {
		u_int		 v = fd->scroll + j, row = FORM_LIST_Y + j;
		struct form_ctl	*ct;
		int		 foc;

		if (v >= fd->nvis)
			break;
		ct = &fd->ctl[fd->vis[v]];
		if (ct->type == FT_SECTION) {
			form_gc(&hd, FORM_HDR_FG, FORM_DLG_BG);
			hd.attr = GRID_ATTR_BRIGHT;
			snprintf(line, sizeof line, "── %s ", ct->label);
			while (strlen(line) + 3 < sizeof line &&
			    utf8_cstrwidth(line) < sbcol - 2)
				strlcat(line, "─", sizeof line);
			form_put(&ctx, &hd, 1, row, sbcol - 2, line);
			continue;
		}
		foc = (fd->fkind == 1 && fd->frow == v);

		/* Marker: + new, - to delete, • changed here, * not default. */
		if (ct->isnew || ct->deleted || form_modified(ct)) {
			form_gc(&mk, FORM_ERR_FG, FORM_DLG_BG);
			mk.attr = GRID_ATTR_BRIGHT;
			form_put(&ctx, &mk, 1, row, 1, ct->isnew ? "+" :
			    ct->deleted ? "-" : "•");
		} else if (form_nondefault(ct)) {
			form_gc(&mk, FORM_DIM_FG, FORM_DLG_BG);
			form_put(&ctx, &mk, 1, row, 1, "*");
		}

		form_gc(&gc, foc ? FORM_FOC_FG : FORM_DLG_FG,
		    foc ? FORM_FOC_BG : FORM_DLG_BG);
		if (form_nondefault(ct) || ct->isuser)
			gc.attr = GRID_ATTR_BRIGHT;
		snprintf(buf, sizeof buf, "%-*.*s", FORM_NAMEW, FORM_NAMEW,
		    ct->label);
		if (strlen(ct->label) > FORM_NAMEW)
			buf[FORM_NAMEW - 1] = '~';
		form_put(&ctx, &gc, 2, row, FORM_NAMEW, buf);

		if (ct->deleted) {
			form_gc(&gc, FORM_DIM_FG, foc ? FORM_INP_FBG :
			    FORM_DLG_BG);
			gc.attr = GRID_ATTR_ITALICS;
			if (ct->isuser || ct->isarray)
				snprintf(line, sizeof line, "(sera supprimée)");
			else
				snprintf(line, sizeof line,
				    "(sera réinitialisée : %s)", ct->defval);
			form_put(&ctx, &gc, valx, row, fieldw, line);
		} else if (ct->type == FT_CHECK) {
			screen_write_cursormove(&ctx, valx, row, 0);
			screen_write_puts(&ctx, &gc, "[%c]",
			    ct->checked ? 'X' : ' ');
		} else if (ct->type == FT_RADIO) {
			struct grid_cell	rg;
			u_int			cx = 0, ri;

			for (ri = 0; ct->choices[ri] != NULL; ri++) {
				u_int	namew = (u_int)strlen(ct->choices[ri]);
				u_int	tok = 4 + namew + 1;
				int	selc = (ri == (u_int)ct->sel);

				if (cx + tok > fieldw)
					break;
				if (selc && foc)
					form_gc(&rg, FORM_INP_FG, FORM_INP_FBG);
				else
					form_gc(&rg, foc ? FORM_FOC_FG :
					    FORM_DLG_FG, foc ? FORM_FOC_BG :
					    FORM_DLG_BG);
				screen_write_cursormove(&ctx, valx + cx, row, 0);
				screen_write_puts(&ctx, &rg, "(%s) %s ",
				    selc ? "\342\200\242" : " ",
				    ct->choices[ri]);
				cx += tok;
			}
		} else {
			size_t	tl = strlen(ct->text);
			u_int	off = (ct->cur >= fieldw) ?
				    ct->cur - fieldw + 1 : 0;

			form_gc(&ig, FORM_INP_FG,
			    foc ? FORM_INP_FBG : FORM_INP_BG);
			for (k = 0; k < fieldw && k < sizeof buf - 1; k++)
				buf[k] = (off + k < tl) ? ct->text[off + k] : ' ';
			buf[k] = '\0';
			screen_write_cursormove(&ctx, valx, row, 0);
			screen_write_puts(&ctx, &ig, "%s", buf);
			if (foc) {
				fd->cur_cx = fd->px + valx + (ct->cur - off);
				fd->cur_cy = fd->py + row;
				cursor = 1;
			}
		}
	}
	if (fd->nvis == 0) {
		form_gc(&gc, FORM_DIM_FG, FORM_DLG_BG);
		form_put(&ctx, &gc, 4, FORM_LIST_Y + 1, W - 8,
		    "Aucune option ne correspond à cette recherche.");
	}
	if (L >= 3)
		scrollbar_draw(&ctx, sbcol, FORM_LIST_Y, L, fd->scroll,
		    form_maxscroll(fd), FORM_DLG_FG, FORM_DLG_BG);

	/* Help: what the focused option is, or the last result. */
	fc = form_focused_ctl(fd);
	form_gc(&gc, FORM_HLP_FG, FORM_DLG_BG);
	if (fc != NULL) {
		const char	*d;
		u_int		 cw = W - 4, n;

		if (fc->isuser)
			d = "Option utilisateur : texte libre, lue par vos "
			    "scripts, plugins et formats (#{@nom}).";
		else if (fc->isarray)
			d = (fc->oe != NULL && fc->oe->text != NULL) ?
			    fc->oe->text : "Élément d'une option tableau.";
		else
			d = (fc->oe != NULL && fc->oe->text != NULL) ?
			    fc->oe->text : "";
		/* Two lines, cut on a space. */
		n = form_clip(line, sizeof line, d, cw);
		if (n == cw && d[strlen(line)] != '\0') {
			char	*sp = strrchr(line, ' ');

			if (sp != NULL && sp > line + cw / 2) {
				*sp = '\0';
				form_put(&ctx, &gc, 2, H - 6, cw, line);
				form_put(&ctx, &gc, 2, H - 5, cw,
				    d + strlen(line) + 1);
			} else {
				form_put(&ctx, &gc, 2, H - 6, cw, line);
				form_put(&ctx, &gc, 2, H - 5, cw,
				    d + strlen(line));
			}
		} else
			form_put(&ctx, &gc, 2, H - 6, cw, line);
	}
	if (fd->msg[0] != '\0') {
		form_gc(&gc, fd->msg_err ? FORM_ERR_FG : FORM_HDR_FG,
		    FORM_DLG_BG);
		gc.attr = GRID_ATTR_BRIGHT;
		form_put(&ctx, &gc, 2, H - 4, W - 4, fd->msg);
	} else if (fc != NULL) {
		snprintf(line, sizeof line, "%s   ·   %s (%s)%s%s%s",
		    form_type_name(fc),
		    fc->section == FS_USER ? "utilisateur" :
		    strcmp(fc->scope, "-s") == 0 ? "serveur" :
		    strcmp(fc->scope, "-wg") == 0 ? "fenêtre" : "session",
		    fc->scope, fc->hasdef ? "   ·   défaut : " : "",
		    fc->hasdef ? (fc->defval[0] != '\0' ? fc->defval :
		    "(vide)") : "", form_modified(fc) ?
		    "   ·   modifiée, non appliquée" : "");
		form_gc(&gc, FORM_DIM_FG, FORM_DLG_BG);
		form_put(&ctx, &gc, 2, H - 4, W - 4, line);
	} else {
		form_gc(&gc, FORM_DIM_FG, FORM_DLG_BG);
		form_put(&ctx, &gc, 2, H - 4, W - 4, "Inser : ajouter    "
		    "Ctrl+Suppr : supprimer / réinitialiser    Entrée : OK    "
		    "Échap : fermer");
	}

	/* Buttons. */
	form_btn_layout(fd, bx, bw);
	for (i = 0; i < FB_N; i++) {
		int	active = (fd->fkind == 2) ? (fd->fbtn == i) :
			    (i == FB_OK);

		form_draw_button(&ctx, bx[i], H - 3, form_btn_label(fd, i),
		    active, fd->pressed == (int)i);
	}

	if (fd->adding) {
		form_draw_addbox(fd, &ctx);
		cursor = 1;
	}
	screen_write_stop(&ctx);

	if (cursor)
		s->mode |= MODE_CURSOR;
	else
		s->mode &= ~MODE_CURSOR;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++) {
		tty_draw_line(tty, s, 0, i, W, fd->px, fd->py + i,
		    &grid_default_cell, NULL);
	}
	form_draw_shadow(c, fd->px + W, fd->py + 1, 2, H - 1);
	form_draw_shadow(c, fd->px + 2, fd->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

static void
form_free_cb(__unused struct client *c, void *data)
{
	struct form_data	*fd = data;

	if (event_initialized(&fd->flash))
		evtimer_del(&fd->flash);
	if (event_initialized(&fd->sb_timer))
		evtimer_del(&fd->sb_timer);
	screen_free(&fd->s);
	free(fd->ctl);
	free(fd->vis);
	free(fd);
}

/*
 * ~/.tmux.conf keeps ONE block written by this dialog. Each save MERGES into
 * it - a line per option, replaced when the option changes again - instead of
 * replacing the whole block with this session's changes only (which silently
 * dropped every setting saved before).
 */
struct form_change {
	char	key[96];
	char	line[1200];
};

static int
form_line_key(const char *l, char *key, size_t len)
{
	const char	*p = l;
	size_t		 n;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "set", 3) != 0 || (p[3] != ' ' && p[3] != '\t'))
		return (0);
	p += 3;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '-')
			break;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
	}
	n = strcspn(p, " \t\n");
	if (n == 0 || n >= len)
		return (0);
	memcpy(key, p, n);
	key[n] = '\0';
	return (1);
}

static void
form_write_config(struct form_change *chg, u_int nchg)
{
	const char	*home = find_home();
	const char	*B = "# >>> tmuxv parametres (genere) >>>";
	const char	*E = "# <<< tmuxv parametres <<<";
	const char	*OB = "# >>> tmux-custom parametres (genere) >>>";
	const char	*OE = "# <<< tmux-custom parametres <<<";
	char		 path[PATH_MAX], tmp[PATH_MAX], key[96];
	char		**pre = NULL, **blk = NULL, **post = NULL, *l = NULL;
	u_int		 npre = 0, nblk = 0, npost = 0, i, k;
	size_t		 ls = 0;
	int		 state = 0, dup;
	FILE		*in, *out;

	if (home == NULL)
		return;
	if ((size_t)snprintf(path, sizeof path, "%s/.tmux.conf", home) >=
	    sizeof path ||
	    (size_t)snprintf(tmp, sizeof tmp, "%s/.tmux.conf.tmp", home) >=
	    sizeof tmp)
		return;

	if ((in = fopen(path, "r")) != NULL) {
		while (getline(&l, &ls, in) != -1) {
			if (state == 0 && (strncmp(l, B, strlen(B)) == 0 ||
			    strncmp(l, OB, strlen(OB)) == 0)) {
				state = 1;
				continue;
			}
			if (state == 1 && (strncmp(l, E, strlen(E)) == 0 ||
			    strncmp(l, OE, strlen(OE)) == 0)) {
				state = 2;
				continue;
			}
			if (state == 0) {
				pre = xreallocarray(pre, npre + 1, sizeof *pre);
				pre[npre++] = xstrdup(l);
			} else if (state == 1) {
				blk = xreallocarray(blk, nblk + 1, sizeof *blk);
				blk[nblk++] = xstrdup(l);
			} else {
				post = xreallocarray(post, npost + 1,
				    sizeof *post);
				post[npost++] = xstrdup(l);
			}
		}
		free(l);
		fclose(in);
	}

	if ((out = fopen(tmp, "w")) == NULL)
		goto done;
	for (i = 0; i < npre; i++)
		fputs(pre[i], out);
	if (state == 0)
		fputs("\n", out);
	fprintf(out, "%s\n", B);
	for (i = 0; i < nblk; i++) {
		dup = 0;
		if (form_line_key(blk[i], key, sizeof key)) {
			for (k = 0; k < nchg; k++) {
				if (strcmp(chg[k].key, key) == 0)
					dup = 1;
			}
		}
		if (!dup)
			fputs(blk[i], out);
	}
	for (k = 0; k < nchg; k++)
		fputs(chg[k].line, out);
	fprintf(out, "%s\n", E);
	for (i = 0; i < npost; i++)
		fputs(post[i], out);
	if (fclose(out) == 0)
		rename(tmp, path);
done:
	for (i = 0; i < npre; i++)
		free(pre[i]);
	for (i = 0; i < nblk; i++)
		free(blk[i]);
	for (i = 0; i < npost; i++)
		free(post[i]);
	free(pre);
	free(blk);
	free(post);
}

/* Reload one row from the option as it is now set. */
static void
form_reload_ctl(struct form_ctl *ct)
{
	struct options_entry	*o = options_get_only(ct->oo, ct->name);
	char			*v;
	u_int			 ci;

	if (o == NULL)
		return;
	v = options_to_string(o, ct->idx, 0);
	if (v == NULL)
		return;
	if (ct->type == FT_CHECK)
		ct->checked = (strcmp(v, "on") == 0 || strcmp(v, "1") == 0);
	else if (ct->type == FT_RADIO) {
		for (ci = 0; ct->choices[ci] != NULL; ci++) {
			if (strcmp(v, ct->choices[ci]) == 0)
				ct->sel = (int)ci;
		}
	} else {
		strlcpy(ct->text, v, sizeof ct->text);
		ct->cur = strlen(ct->text);
	}
	strlcpy(ct->orig, form_value(ct), sizeof ct->orig);
	free(v);
}

/*
 * Apply every change: live, through the same type-checked paths as set-option
 * (options_from_string, options_array_set, options_remove_or_default - no
 * command parsing, so nothing can be injected), then saved to ~/.tmux.conf.
 * A value tmux refuses is reported in the dialog, not silently dropped.
 */
static void
form_apply(struct client *c, struct form_data *fd)
{
	struct form_change	*chg = NULL;
	struct form_ctl		*ct;
	struct options_entry	*o;
	u_int			 i, nchg = 0, bad = 0;
	char			*cause, q[1100];
	const char		*val;

	fd->msg[0] = '\0';
	fd->msg_err = 0;
	for (i = 0; i < fd->nctl; i++) {
		ct = &fd->ctl[i];
		ct->applied = 0;
		if (ct->type == FT_SECTION || !form_modified(ct))
			continue;
		val = form_value(ct);
		cause = NULL;
		if (ct->deleted) {
			if (ct->isnew)
				continue;
			o = options_get_only(ct->oo, ct->name);
			if (o != NULL) {
				if (ct->isarray)
					options_array_set(o, (u_int)ct->idx, NULL,
					    0, NULL);
				else
					options_remove_or_default(o, -1, NULL);
			}
		} else if (ct->isarray) {
			o = options_get_only(ct->oo, ct->name);
			if (o == NULL || options_array_set(o, (u_int)ct->idx,
			    val, 0, &cause) != 0) {
				snprintf(fd->msg, sizeof fd->msg,
				    "%s refusée : %s", ct->label,
				    cause != NULL ? cause : "option absente");
				fd->msg_err = 1;
				free(cause);
				bad++;
				continue;
			}
		} else if (ct->isuser)
			options_set_string(ct->oo, ct->name, 0, "%s", val);
		else if (options_from_string(ct->oo, ct->oe, ct->name, val, 0,
		    &cause) != 0) {
			snprintf(fd->msg, sizeof fd->msg, "%s refusée : %s",
			    ct->label, cause != NULL ? cause : "valeur invalide");
			fd->msg_err = 1;
			free(cause);
			bad++;
			continue;
		}
		options_push_changes(ct->name);
		ct->applied = 1;

		chg = xreallocarray(chg, nchg + 1, sizeof *chg);
		strlcpy(chg[nchg].key, ct->label, sizeof chg[nchg].key);
		if (ct->deleted) {
			snprintf(chg[nchg].line, sizeof chg[nchg].line,
			    "set -u %s %s\n", ct->scope, ct->label);
		} else {
			form_quote(q, sizeof q, val);
			snprintf(chg[nchg].line, sizeof chg[nchg].line,
			    "set %s %s %s\n", ct->scope, ct->label, q);
		}
		nchg++;
	}
	if (nchg != 0)
		form_write_config(chg, nchg);
	free(chg);

	/* The dialog now shows what is set. */
	for (i = fd->nctl; i-- > 0; /* nothing */) {
		ct = &fd->ctl[i];
		if (!ct->applied)
			continue;
		if (ct->deleted && (ct->isuser || ct->isarray)) {
			memmove(&fd->ctl[i], &fd->ctl[i + 1],
			    (fd->nctl - i - 1) * sizeof *fd->ctl);
			fd->nctl--;
			continue;
		}
		ct->deleted = 0;
		ct->isnew = 0;
		form_reload_ctl(ct);
	}
	form_filter(fd);
	if (!bad) {
		if (nchg == 0)
			snprintf(fd->msg, sizeof fd->msg, "Aucun changement.");
		else
			snprintf(fd->msg, sizeof fd->msg, "%u changement(s) "
			    "appliqué(s) et enregistré(s) dans ~/.tmux.conf.",
			    nchg);
	}
	recalculate_sizes();
	server_redraw_client(c);
}

static void
form_toggle_delete(struct form_data *fd)
{
	struct form_ctl	*ct = form_focused_ctl(fd);
	u_int		 i;

	if (ct == NULL) {
		snprintf(fd->msg, sizeof fd->msg,
		    "Choisissez d'abord une option dans la liste.");
		fd->msg_err = 1;
		return;
	}
	if (ct->isnew) {		/* never applied: the row just goes */
		i = fd->vis[fd->frow];
		memmove(&fd->ctl[i], &fd->ctl[i + 1],
		    (fd->nctl - i - 1) * sizeof *fd->ctl);
		fd->nctl--;
		form_filter(fd);
		return;
	}
	ct->deleted = !ct->deleted;
}

static u_int
form_next_index(struct form_data *fd, const char *name)
{
	u_int	i;
	int	max = -1;

	for (i = 0; i < fd->nctl; i++) {
		if (fd->ctl[i].isarray && strcmp(fd->ctl[i].name, name) == 0 &&
		    fd->ctl[i].idx > max)
			max = fd->ctl[i].idx;
	}
	return ((u_int)(max + 1));
}

static void
form_add_open(struct form_data *fd)
{
	struct form_ctl	*ct = form_focused_ctl(fd);

	fd->adding = 1;
	fd->aerr[0] = '\0';
	fd->aval[0] = '\0';
	if (ct != NULL && ct->isarray) {
		snprintf(fd->aname, sizeof fd->aname, "%s[%u]", ct->name,
		    form_next_index(fd, ct->name));
		fd->afield = 1;
	} else {
		strlcpy(fd->aname, "@", sizeof fd->aname);
		fd->afield = 0;
	}
	fd->acur[0] = strlen(fd->aname);
	fd->acur[1] = 0;
}

/* Validate the "new option" box: an @option, or an item of an array. */
static int
form_add_commit(struct form_data *fd)
{
	static const struct {
		const char	*scope;
		int		 section;
	} tabs[] = { { "-g", FS_SESSION }, { "-wg", FS_WINDOW },
		     { "-s", FS_SERVER } };
	struct options			*oos[3] = { global_s_options,
					    global_w_options, global_options };
	struct options			*oo = NULL;
	struct options_entry		*o;
	const struct options_table_entry *oe = NULL;
	struct form_ctl			*ct;
	char				 name[128], *br, *end, label[80];
	const char			*scope = "-g";
	long				 idx = -1;
	int				 section = FS_USER;
	u_int				 i, t;

	strlcpy(name, fd->aname, sizeof name);
	for (i = 0; name[i] == ' '; i++)
		/* nothing */;
	memmove(name, name + i, strlen(name + i) + 1);
	while (*name != '\0' && name[strlen(name) - 1] == ' ')
		name[strlen(name) - 1] = '\0';

	if (name[0] == '@') {
		if (name[1] == '\0' || strpbrk(name, " \t[]\"'#") != NULL) {
			snprintf(fd->aerr, sizeof fd->aerr,
			    "Nom invalide : @ suivi de lettres, chiffres, - ou _.");
			return (-1);
		}
		for (i = 0; i < fd->nctl; i++) {
			ct = &fd->ctl[i];
			if (ct->isuser && strcmp(ct->name, name) == 0 &&
			    !ct->deleted) {
				snprintf(fd->aerr, sizeof fd->aerr, "%s existe "
				    "déjà : modifiez-la dans la liste.", name);
				return (-1);
			}
		}
		oo = global_s_options;
		strlcpy(label, name, sizeof label);
	} else if ((br = strchr(name, '[')) != NULL) {
		*br = '\0';
		idx = strtol(br + 1, &end, 10);
		if (end == br + 1 || *end != ']' || end[1] != '\0' || idx < 0 ||
		    idx > 10000) {
			snprintf(fd->aerr, sizeof fd->aerr,
			    "Index invalide : écrivez nom[n], n entier.");
			return (-1);
		}
		for (t = 0; t < 3; t++) {
			o = options_get_only(oos[t], name);
			if (o != NULL && options_is_array(o)) {
				oo = oos[t];
				oe = options_table_entry(o);
				scope = tabs[t].scope;
				section = tabs[t].section;
				break;
			}
		}
		if (oo == NULL) {
			snprintf(fd->aerr, sizeof fd->aerr,
			    "« %s » n'est pas une option tableau.", name);
			return (-1);
		}
		if (oe != NULL && (oe->flags & OPTIONS_TABLE_IS_HOOK)) {
			snprintf(fd->aerr, sizeof fd->aerr,
			    "Les crochets (hooks) ne se modifient pas ici.");
			return (-1);
		}
		for (i = 0; i < fd->nctl; i++) {
			ct = &fd->ctl[i];
			if (ct->isarray && strcmp(ct->name, name) == 0 &&
			    ct->idx == idx && !ct->deleted) {
				snprintf(fd->aerr, sizeof fd->aerr,
				    "%s[%ld] existe déjà.", name, idx);
				return (-1);
			}
		}
		snprintf(label, sizeof label, "%s[%ld]", name, idx);
	} else {
		snprintf(fd->aerr, sizeof fd->aerr, "Une nouvelle option "
		    "commence par @ (ou désigne un élément : nom[n]).");
		return (-1);
	}

	ct = form_add(fd);
	ct->type = FT_INPUT;
	strlcpy(ct->label, label, sizeof ct->label);
	strlcpy(ct->name, name, sizeof ct->name);
	ct->idx = (int)idx;
	strlcpy(ct->text, fd->aval, sizeof ct->text);
	ct->cur = strlen(ct->text);
	ct->oo = oo;
	ct->oe = oe;
	ct->scope = scope;
	ct->section = section;
	ct->isuser = (idx < 0);
	ct->isarray = (idx >= 0);
	ct->isnew = 1;
	qsort(fd->ctl, fd->nctl, sizeof *fd->ctl, form_ctl_cmp);

	fd->adding = 0;
	fd->search[0] = '\0';
	fd->scur = 0;
	fd->fkind = 0;
	form_filter(fd);
	for (i = 0; i < fd->nvis; i++) {
		if (strcmp(fd->ctl[fd->vis[i]].label, label) == 0) {
			form_focus_row(fd, (int)i);
			break;
		}
	}
	snprintf(fd->msg, sizeof fd->msg, "%s ajoutée : « Appliquer » ou OK "
	    "pour l'enregistrer.", label);
	fd->msg_err = 0;
	return (0);
}

/* Edit a text buffer (search, field of the box). 1 if the key was used. */
static int
form_edit(char *text, size_t size, u_int *cur, key_code key, int numeric)
{
	key_code	base = key & KEYC_MASK_KEY;
	size_t		tl = strlen(text);

	if ((key & KEYC_MASK_MODIFIERS) == 0 && base >= 0x20 && base < 0x7f) {
		if (numeric && (base < '0' || base > '9'))
			return (1);
		if (tl + 1 < size) {
			memmove(text + *cur + 1, text + *cur, tl - *cur + 1);
			text[*cur] = (char)base;
			(*cur)++;
		}
		return (1);
	}
	switch (base) {
	case KEYC_BSPACE:
		if (*cur > 0) {
			memmove(text + *cur - 1, text + *cur, tl - *cur + 1);
			(*cur)--;
		}
		return (1);
	case KEYC_DC:
		if (*cur < tl)
			memmove(text + *cur, text + *cur + 1, tl - *cur);
		return (1);
	case KEYC_LEFT:
		if (*cur > 0)
			(*cur)--;
		return (1);
	case KEYC_RIGHT:
		if (*cur < tl)
			(*cur)++;
		return (1);
	case KEYC_HOME:
		*cur = 0;
		return (1);
	case KEYC_END:
		*cur = tl;
		return (1);
	}
	return (0);
}

static void
form_flash_cb(__unused int fd_, __unused short events, void *arg)
{
	struct form_data	*fd = arg;
	struct client		*c = fd->c;
	int			 b = fd->flash_btn;

	fd->pressed = -1;
	fd->flash_btn = -1;
	if (b >= 0 && form_btn_action(c, fd, (u_int)b))
		server_client_clear_overlay(c);	/* frees fd */
	else
		c->flags |= CLIENT_REDRAWOVERLAY;
}

/* Keyboard press: the button sinks briefly (TButton::press), then acts. */
static void
form_start_flash(struct client *c, struct form_data *fd, u_int b)
{
	struct timeval	tv = { 0, 90000 };

	if (fd->flash_btn >= 0)
		return;
	fd->pressed = (int)b;
	fd->flash_btn = (int)b;
	c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&fd->flash))
		evtimer_del(&fd->flash);
	evtimer_set(&fd->flash, form_flash_cb, fd);
	evtimer_add(&fd->flash, &tv);
}

/* Run a button. 1 when the dialog must close. */
static int
form_btn_action(struct client *c, struct form_data *fd, u_int b)
{
	switch (b) {
	case FB_OK:
		/* A refused value keeps the dialog open, to be seen. */
		form_apply(c, fd);
		return (fd->msg_err ? 0 : 1);
	case FB_CANCEL:
		return (1);
	case FB_APPLY:
		form_apply(c, fd);
		return (0);
	case FB_ADD:
		form_add_open(fd);
		return (0);
	case FB_DEL:
		form_toggle_delete(fd);
		return (0);
	}
	return (0);
}

#define SB_DELAY_MS 440
#define SB_REPEAT_MS 55

static void
form_sb_step(struct form_data *fd, int dir)
{
	u_int	m = form_maxscroll(fd);

	if (dir < 0)
		fd->scroll = (fd->scroll > 0) ? fd->scroll - 1 : 0;
	else
		fd->scroll = (fd->scroll < m) ? fd->scroll + 1 : m;
}

static void
form_sb_timer(__unused int fd_, __unused short events, void *arg)
{
	struct form_data	*fd = arg;
	struct timeval		 tv = { 0, SB_REPEAT_MS * 1000 };

	if (fd->sb_mode != 1)
		return;
	if (fd->sb_on) {
		form_sb_step(fd, fd->sb_dir);
		fd->c->flags |= CLIENT_REDRAWOVERLAY;
	}
	evtimer_add(&fd->sb_timer, &tv);
}

static int
form_mouse(struct client *c, struct form_data *fd, struct mouse_event *m)
{
	struct form_ctl	*ct;
	u_int		 L = form_listrows(fd), valx = form_valx();
	u_int		 sbcol = fd->w - 2, fieldw = form_fieldw(fd);
	u_int		 m_ = form_maxscroll(fd), lx, ly, bx[FB_N], bw[FB_N];
	u_int		 b, top = FORM_LIST_Y, bot = FORM_LIST_Y + L - 1;
	int		 np = -1;

	if (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b))
		return (0);			/* hover */
	if (fd->sb_mode != 0) {
		if (MOUSE_RELEASE(m->b)) {
			fd->sb_mode = 0;
			evtimer_del(&fd->sb_timer);
			return (0);
		}
		if (MOUSE_DRAG(m->b)) {
			if (fd->sb_mode == 1)
				fd->sb_on = (m->x == fd->sb_ax &&
				    m->y == fd->sb_ay);
			else {
				u_int p = (m->y > fd->py + top + 1) ?
				    m->y - (fd->py + top + 1) : 0;

				fd->scroll = scrollbar_value(p, m_, L - 2);
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		fd->sb_mode = 0;
		evtimer_del(&fd->sb_timer);
	}
	if (MOUSE_WHEEL(m->b)) {
		if (MOUSE_BUTTONS(m->b) == MOUSE_WHEEL_UP)
			fd->scroll = (fd->scroll > 3) ? fd->scroll - 3 : 0;
		else
			fd->scroll = (fd->scroll + 3 < m_) ? fd->scroll + 3 : m_;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
	if (m->x < fd->px || m->x >= fd->px + fd->w ||
	    m->y < fd->py || m->y >= fd->py + fd->h) {
		if (MOUSE_RELEASE(m->b) && fd->pressed >= 0) {
			fd->pressed = -1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}
	lx = m->x - fd->px;
	ly = m->y - fd->py;

	if (fd->adding) {		/* only the box's fields, while open */
		u_int	bh = 9, by = (fd->h - bh) / 2;

		if (!MOUSE_RELEASE(m->b) && (ly == by + 2 || ly == by + 4)) {
			fd->afield = (ly == by + 2) ? 0 : 1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}

	/* Scrollbar (TScrollBar: ▲/▼ step and repeat, track grabs). */
	if (lx == sbcol && ly >= top && ly <= bot &&
	    !MOUSE_RELEASE(m->b) && !MOUSE_DRAG(m->b)) {
		struct timeval	tv = { 0, SB_DELAY_MS * 1000 };

		if (ly == top || ly == bot) {
			fd->sb_dir = (ly == top) ? -1 : 1;
			form_sb_step(fd, fd->sb_dir);
			fd->sb_mode = 1;
			fd->sb_on = 1;
			fd->sb_ax = m->x;
			fd->sb_ay = m->y;
			if (event_initialized(&fd->sb_timer))
				evtimer_del(&fd->sb_timer);
			evtimer_set(&fd->sb_timer, form_sb_timer, fd);
			evtimer_add(&fd->sb_timer, &tv);
		} else {
			fd->scroll = scrollbar_value(ly - top - 1, m_, L - 2);
			fd->sb_mode = 2;
		}
		fd->pressed = -1;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}

	/* Buttons: sink on press, act on release over the same button. */
	if (ly == fd->h - 3) {
		form_btn_layout(fd, bx, bw);
		for (b = 0; b < FB_N; b++) {
			if (lx >= bx[b] && lx < bx[b] + bw[b])
				np = (int)b;
		}
		if (!MOUSE_RELEASE(m->b)) {
			if (np >= 0) {
				fd->pressed = np;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		b = (u_int)fd->pressed;
		fd->pressed = -1;
		c->flags |= CLIENT_REDRAWOVERLAY;
		if (np >= 0 && (int)b == np) {
			if (np != FB_DEL) {
				fd->fkind = 2;
				fd->fbtn = (u_int)np;
			}
			return (form_btn_action(c, fd, (u_int)np));
		}
		return (0);
	}
	if (!MOUSE_RELEASE(m->b)) {
		if (fd->pressed >= 0) {
			fd->pressed = -1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}
	fd->pressed = -1;

	if (ly == 1) {			/* the search field */
		fd->fkind = 0;
		if (lx >= 15)
			fd->scur = (lx - 15 < strlen(fd->search)) ?
			    lx - 15 : strlen(fd->search);
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
	if (ly >= top && ly <= bot) {
		u_int	v = fd->scroll + (ly - top);

		if (!form_is_row(fd, v))
			return (0);
		form_focus_row(fd, (int)v);
		ct = &fd->ctl[fd->vis[v]];
		if (ct->deleted)
			;
		else if (ct->type == FT_CHECK) {
			if (lx >= 2 && lx <= valx + 2)
				ct->checked = !ct->checked;
		} else if (ct->type == FT_RADIO) {
			int	rh = -1;

			if (lx >= valx)
				rh = form_radio_hit(ct, fieldw, lx - valx);
			if (rh >= 0)
				ct->sel = rh;
		} else if (lx >= valx && lx < valx + fieldw) {
			u_int	p = lx - valx, tl = strlen(ct->text);
			u_int	off = (ct->cur >= fieldw) ?
				    ct->cur - fieldw + 1 : 0;

			ct->cur = (off + p < tl) ? off + p : tl;
		}
		c->flags |= CLIENT_REDRAWOVERLAY;
	}
	return (0);
}

static int
form_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct form_data	*fd = data;
	struct form_ctl		*ct;
	key_code		 key = event->key, base;
	u_int			 L = form_listrows(fd);
	int			 v, typing;

	if (KEYC_IS_MOUSE(key))
		return (form_mouse(c, fd, &event->m));
	base = key & KEYC_MASK_KEY;
	fd->msg[0] = '\0';
	c->flags |= CLIENT_REDRAWOVERLAY;

	/* The "new option" box has the keyboard while it is open. */
	if (fd->adding) {
		switch (base) {
		case '\033':
		case '\003':
		case '\007':
			fd->adding = 0;
			return (0);
		case '\r':
			form_add_commit(fd);
			return (0);
		case '\011':
		case KEYC_BTAB:
		case KEYC_UP:
		case KEYC_DOWN:
			fd->afield = !fd->afield;
			return (0);
		}
		fd->aerr[0] = '\0';
		if (fd->afield == 0)
			form_edit(fd->aname, sizeof fd->aname, &fd->acur[0],
			    key, 0);
		else
			form_edit(fd->aval, sizeof fd->aval, &fd->acur[1],
			    key, 0);
		return (0);
	}

	ct = form_focused_ctl(fd);
	typing = (ct != NULL && ct->type == FT_INPUT && !ct->deleted);

	switch (base) {
	case '\033':		/* Escape: clear the search first, then close */
		if (fd->search[0] != '\0') {
			fd->search[0] = '\0';
			fd->scur = 0;
			form_filter(fd);
			return (0);
		}
		return (1);
	case '\003':
	case '\007':
		return (1);
	case '\r':		/* Enter = OK, whatever has the focus */
		fd->fkind = 2;
		fd->fbtn = FB_OK;
		form_start_flash(c, fd, FB_OK);
		return (0);
	case '\006':		/* Ctrl-F */
		fd->fkind = 0;
		return (0);
	case KEYC_IC:		/* Insert: new option */
		form_add_open(fd);
		return (0);
	case '\011':
		form_focus_step(fd, 1);
		return (0);
	case KEYC_BTAB:
		form_focus_step(fd, -1);
		return (0);
	}
	if (base == KEYC_DC && (!typing || (key & KEYC_CTRL))) {
		form_toggle_delete(fd);
		return (0);
	}

	/* The search field. */
	if (fd->fkind == 0) {
		if (base == KEYC_DOWN || base == KEYC_NPAGE) {
			if ((v = form_next_row(fd, 0, 1)) >= 0)
				form_focus_row(fd, v);
			else {
				fd->fkind = 2;
				fd->fbtn = FB_OK;
			}
			return (0);
		}
		if (form_edit(fd->search, sizeof fd->search, &fd->scur, key,
		    0))
			form_filter(fd);
		return (0);
	}

	/* The list. */
	if (fd->fkind == 1 && ct != NULL) {
		switch (base) {
		case KEYC_UP:
			if ((v = form_next_row(fd, (int)fd->frow - 1, -1)) >= 0)
				form_focus_row(fd, v);
			else
				fd->fkind = 0;
			return (0);
		case KEYC_DOWN:
			if ((v = form_next_row(fd, (int)fd->frow + 1, 1)) >= 0)
				form_focus_row(fd, v);
			else {
				fd->fkind = 2;
				fd->fbtn = FB_OK;
			}
			return (0);
		case KEYC_NPAGE:
			if (key & KEYC_CTRL)
				v = form_next_row(fd, (int)fd->nvis - 1, -1);
			else if ((v = form_next_row(fd, (int)fd->frow + (int)L,
			    1)) < 0)
				v = form_next_row(fd, (int)fd->nvis - 1, -1);
			form_focus_row(fd, v);
			return (0);
		case KEYC_PPAGE:
			if (key & KEYC_CTRL)
				v = form_next_row(fd, 0, 1);
			else if ((v = form_next_row(fd, (int)fd->frow - (int)L,
			    -1)) < 0)
				v = form_next_row(fd, 0, 1);
			form_focus_row(fd, v);
			return (0);
		}
		if (ct->deleted) {
			if (base == ' ')
				ct->deleted = 0;
			return (0);
		}
		if (ct->type == FT_CHECK) {
			if (base == ' ')
				ct->checked = !ct->checked;
			else if (base == KEYC_LEFT || base == KEYC_RIGHT)
				ct->checked = (base == KEYC_RIGHT);
			else if ((key & KEYC_MASK_MODIFIERS) == 0 &&
			    base > 0x20 && base < 0x7f) {
				fd->fkind = 0;	/* type-ahead: search */
				form_edit(fd->search, sizeof fd->search,
				    &fd->scur, key, 0);
				form_filter(fd);
			}
			return (0);
		}
		if (ct->type == FT_RADIO) {
			if (base == ' ')
				ct->sel = (ct->choices[ct->sel + 1] != NULL) ?
				    ct->sel + 1 : 0;
			else if (base == KEYC_LEFT && ct->sel > 0)
				ct->sel--;
			else if (base == KEYC_RIGHT &&
			    ct->choices[ct->sel + 1] != NULL)
				ct->sel++;
			else if ((key & KEYC_MASK_MODIFIERS) == 0 &&
			    base > 0x20 && base < 0x7f) {
				fd->fkind = 0;
				form_edit(fd->search, sizeof fd->search,
				    &fd->scur, key, 0);
				form_filter(fd);
			}
			return (0);
		}
		form_edit(ct->text, sizeof ct->text, &ct->cur, key,
		    ct->numeric);
		return (0);
	}

	/* The buttons. */
	if (fd->fkind == 2) {
		switch (base) {
		case KEYC_LEFT:
			fd->fbtn = (fd->fbtn + FB_N - 1) % FB_N;
			return (0);
		case KEYC_RIGHT:
			fd->fbtn = (fd->fbtn + 1) % FB_N;
			return (0);
		case KEYC_UP:
			if ((v = form_next_row(fd, (int)fd->nvis - 1, -1)) >= 0)
				form_focus_row(fd, v);
			else
				fd->fkind = 0;
			return (0);
		case ' ':
			form_start_flash(c, fd, fd->fbtn);
			return (0);
		}
		if ((key & KEYC_MASK_MODIFIERS) == 0 && base > 0x20 &&
		    base < 0x7f) {
			fd->fkind = 0;
			form_edit(fd->search, sizeof fd->search, &fd->scur,
			    key, 0);
			form_filter(fd);
		}
	}
	return (0);
}

static void
form_populate_table(struct form_data *fd, struct options *oo,
    const char *scope, int section)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	const struct options_table_entry *oe;
	struct form_ctl			*ct;
	const char			*name;
	char				*v, *d;
	u_int				 ci, idx;

	if (oo == NULL)
		return;
	for (o = options_first(oo); o != NULL; o = options_next(o)) {
		name = options_name(o);
		oe = options_table_entry(o);
		if (oe != NULL && (oe->flags & OPTIONS_TABLE_IS_HOOK))
			continue;	/* hooks: commands, not settings */
		if (options_is_array(o)) {
			for (a = options_array_first(o); a != NULL;
			    a = options_array_next(a)) {
				idx = options_array_item_index(a);
				if ((v = options_to_string(o, (int)idx, 0)) ==
				    NULL)
					continue;
				ct = form_add(fd);
				ct->type = FT_INPUT;
				snprintf(ct->label, sizeof ct->label, "%s[%u]",
				    name, idx);
				strlcpy(ct->name, name, sizeof ct->name);
				ct->idx = (int)idx;
				strlcpy(ct->text, v, sizeof ct->text);
				ct->cur = strlen(ct->text);
				strlcpy(ct->orig, ct->text, sizeof ct->orig);
				ct->oo = oo;
				ct->oe = oe;
				ct->scope = scope;
				ct->section = (*name == '@') ? FS_USER : section;
				ct->isarray = 1;
				free(v);
			}
			continue;
		}
		if ((v = options_to_string(o, -1, 0)) == NULL)
			continue;
		ct = form_add(fd);
		strlcpy(ct->name, name, sizeof ct->name);
		strlcpy(ct->label, name, sizeof ct->label);
		ct->oo = oo;
		ct->oe = oe;
		ct->scope = scope;
		ct->isuser = (*name == '@');
		ct->section = ct->isuser ? FS_USER : section;
		if (oe != NULL && oe->type == OPTIONS_TABLE_FLAG) {
			ct->type = FT_CHECK;
			ct->checked = (strcmp(v, "on") == 0 ||
			    strcmp(v, "1") == 0);
		} else if (oe != NULL && oe->type == OPTIONS_TABLE_CHOICE &&
		    oe->choices != NULL) {
			ct->type = FT_RADIO;
			ct->choices = oe->choices;
			for (ci = 0; oe->choices[ci] != NULL; ci++) {
				if (strcmp(v, oe->choices[ci]) == 0)
					ct->sel = (int)ci;
			}
		} else {
			ct->type = FT_INPUT;
			ct->numeric = (oe != NULL &&
			    oe->type == OPTIONS_TABLE_NUMBER);
			strlcpy(ct->text, v, sizeof ct->text);
			ct->cur = strlen(ct->text);
		}
		strlcpy(ct->orig, form_value(ct), sizeof ct->orig);
		if (oe != NULL) {
			d = options_default_to_string(oe);
			strlcpy(ct->defval, d, sizeof ct->defval);
			ct->hasdef = 1;
			free(d);
		}
		free(v);
	}
}

/*
 * Every global option tmux knows - session, window, server, the user's own
 * @options and each item of the array options - grouped in sections.
 */
static void
form_populate(struct form_data *fd)
{
	struct form_ctl	*ct;
	int		 s;

	for (s = 0; s < FS_N; s++) {
		ct = form_add(fd);
		ct->type = FT_SECTION;
		ct->section = s;
		strlcpy(ct->label, form_section_title[s], sizeof ct->label);
	}
	form_populate_table(fd, global_s_options, "-g", FS_SESSION);
	form_populate_table(fd, global_w_options, "-wg", FS_WINDOW);
	form_populate_table(fd, global_options, "-s", FS_SERVER);
	qsort(fd->ctl, fd->nctl, sizeof *fd->ctl, form_ctl_cmp);
	form_filter(fd);
}

static int
form_size(struct client *c, u_int *w, u_int *h)
{
	*w = (c->tty.sx > 4) ? c->tty.sx - 4 : c->tty.sx;
	if (*w > FORM_MAXW)
		*w = FORM_MAXW;
	*h = (c->tty.sy > 4) ? c->tty.sy - 4 : c->tty.sy;
	if (*h > FORM_MAXH)
		*h = FORM_MAXH;
	return (*w >= FORM_MINW && *h >= FORM_MINH);
}

static void
form_resize_cb(struct client *c, void *data)
{
	struct form_data	*fd = data;
	u_int			 w, h;

	if (!form_size(c, &w, &h)) {
		server_client_clear_overlay(c);	/* frees fd */
		return;
	}
	if (w != fd->w || h != fd->h) {
		screen_resize(&fd->s, w, h, 0);
		fd->w = w;
		fd->h = h;
	}
	form_scroll_to_focus(fd);
	fd->px = (c->tty.sx - w) / 2;
	fd->py = (c->tty.sy - h) / 2;
	c->flags |= CLIENT_REDRAWOVERLAY;
}

void
form_display(struct client *c, __unused struct cmdq_item *item)
{
	struct form_data	*fd;
	u_int			 w, h;

	if (!form_size(c, &w, &h))
		return;

	fd = xcalloc(1, sizeof *fd);
	fd->c = c;
	fd->w = w;
	fd->h = h;
	screen_init(&fd->s, w, h, 0);
	fd->s.mode &= ~MODE_CURSOR;
	fd->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	fd->pressed = -1;
	fd->flash_btn = -1;
	fd->fkind = 0;			/* type to search, right away */

	form_populate(fd);

	fd->px = (c->tty.sx - w) / 2;
	fd->py = (c->tty.sy - h) / 2;
	server_client_set_overlay(c, 0, form_check_cb, form_mode_cb,
	    form_draw_cb, form_key_cb, form_free_cb, form_resize_cb, fd);
}

/* Command: display-form / settings. */
static enum cmd_retval
cmd_settings_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);

	if (c != NULL)
		form_display(c, item);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_settings_entry = {
	.name = "display-form",
	.alias = "settings",

	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_settings_exec
};

/*
 * ==========================================================================
 * Boîte de dialogue TEXTE défilante (Turbo Vision) : titre + texte multi-ligne
 * + scrollbar verticale + bouton OK ombré + ombre portée. Sert pour "À propos",
 * les raccourcis clavier (list-keys) et la liste des commandes (list-commands).
 * ==========================================================================
 */

struct msgbox_data {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	char		*title;
	char	       **lines;
	u_int		 nlines, scroll;
	u_int		 longest;		/* longest line (for refits) */
	int		 ok_pressed;		/* OK button animation */
	struct event	 flash;			/* keyboard press-flash timer */
	int		 flashing;		/* flash in progress */
	struct event	 sb_timer;		/* held-arrow auto-repeat */
	int		 sb_mode;		/* 0 none, 1 arrow held, 2 thumb */
	int		 sb_dir;
	int		 sb_on;
	u_int		 sb_ax, sb_ay;
	/* List mode (TListViewer): selectable rows, double-click/Enter acts. */
	int		 list;
	int		 act_kind;		/* MB_ACT_* below */
	char	       **keys;			/* per line: item key or NULL */
	int		 sel;
	struct timeval	 last_click;
	int		 last_row;
};

static u_int
msgbox_visrows(struct msgbox_data *md)
{
	return (md->h > 5 ? md->h - 4 : 1);
}

static void
msgbox_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct msgbox_data	*md = data;

	server_client_overlay_range(md->px, md->py, md->w, md->h, px, py, nx, r);
}

static struct screen *
msgbox_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct msgbox_data	*md = data;

	*cx = md->px;
	*cy = md->py;
	return (&md->s);
}

static void
msgbox_free_cb(__unused struct client *c, void *data)
{
	struct msgbox_data	*md = data;
	u_int			 i;

	if (event_initialized(&md->flash))
		evtimer_del(&md->flash);
	if (event_initialized(&md->sb_timer))
		evtimer_del(&md->sb_timer);
	for (i = 0; i < md->nlines; i++) {
		free(md->lines[i]);
		if (md->keys != NULL)
			free(md->keys[i]);
	}
	free(md->keys);
	free(md->lines);
	free(md->title);
	screen_free(&md->s);
	free(md);
}

/* Keyboard press-flash for the msgbox OK button, then close (TVision-style). */
static void
msgbox_flash_cb(__unused int fd_, __unused short events, void *arg)
{
	struct msgbox_data	*md = arg;

	server_client_clear_overlay(md->c);	/* frees md via msgbox_free_cb */
}

static void
msgbox_sb_step(struct msgbox_data *md, int dir)
{
	u_int	visrows = msgbox_visrows(md);
	u_int	maxscroll = (md->nlines > visrows) ? md->nlines - visrows : 0;

	if (dir < 0)
		md->scroll = (md->scroll > 0) ? md->scroll - 1 : 0;
	else
		md->scroll = (md->scroll < maxscroll) ?
		    md->scroll + 1 : maxscroll;
}

/* evMouseAuto for the msgbox scrollbar arrows. */
static void
msgbox_sb_timer(__unused int fd_, __unused short events, void *arg)
{
	struct msgbox_data	*md = arg;
	struct timeval		 tv = { 0, SB_REPEAT_MS * 1000 };

	if (md->sb_mode != 1)
		return;
	if (md->sb_on) {
		msgbox_sb_step(md, md->sb_dir);
		md->c->flags |= CLIENT_REDRAWOVERLAY;
	}
	evtimer_add(&md->sb_timer, &tv);
}

static void
msgbox_start_flash(struct client *c, struct msgbox_data *md)
{
	struct timeval	tv = { 0, 90000 };

	if (md->flashing)
		return;
	md->flashing = 1;
	md->ok_pressed = 1;
	c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&md->flash))
		evtimer_del(&md->flash);
	evtimer_set(&md->flash, msgbox_flash_cb, md);
	evtimer_add(&md->flash, &tv);
}

static void
msgbox_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct msgbox_data	*md = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &md->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, gc;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	u_int			 W = md->w, H = md->h, i, j;
	u_int			 visrows = msgbox_visrows(md);
	u_int			 sbcol = W - 2, textw = (sbcol > 3) ? sbcol - 2 : 1;
	u_int			 maxscroll = (md->nlines > visrows) ?
				     md->nlines - visrows : 0;
	u_int			 okx;
	char			 title[128];

	if (md->scroll > maxscroll)
		md->scroll = maxscroll;

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	screen_write_cursormove(&ctx, 0, 0, 0);
	snprintf(title, sizeof title, " %s ", md->title);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame, title);

	/* Visible text lines (scrolled). */
	form_gc(&gc, FORM_DLG_FG, FORM_DLG_BG);
	for (j = 0; j < visrows; j++) {
		u_int	idx = md->scroll + j, k;
		char	lb[1024];
		size_t	tl;

		if (idx >= md->nlines)
			break;
		tl = strlen(md->lines[idx]);
		for (k = 0; k < textw && k < sizeof lb - 1; k++)
			lb[k] = (k < tl) ? md->lines[idx][k] : ' ';
		lb[k] = '\0';
		screen_write_cursormove(&ctx, 2, 1 + j, 0);
		if (md->list && (int)idx == md->sel) {
			/* TListViewer: the selected item on a highlight bar. */
			struct grid_cell	sg;

			form_gc(&sg, FORM_FOC_FG, FORM_FOC_BG);
			screen_write_puts(&ctx, &sg, "%s", lb);
		} else
			screen_write_puts(&ctx, &gc, "%s", lb);
	}

	/* Vertical scrollbar (Turbo Vision TScrollBar). */
	if (visrows >= 3)
		scrollbar_draw(&ctx, sbcol, 1, visrows, md->scroll,
		    (md->nlines > visrows) ? md->nlines - visrows : 0,
		    FORM_DLG_FG, FORM_DLG_BG);

	/* Centred OK button (faithful Turbo Vision TButton, sinks on click). */
	okx = (W > 6) ? (W - 6) / 2 : 1;
	form_draw_button(&ctx, okx, H - 3, "OK", 1, md->ok_pressed);

	screen_write_stop(&ctx);
	s->mode &= ~MODE_CURSOR;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++)
		tty_draw_line(tty, s, 0, i, W, md->px, md->py + i,
		    &grid_default_cell, NULL);
	form_draw_shadow(c, md->px + W, md->py + 1, 2, H - 1);
	form_draw_shadow(c, md->px + 2, md->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

/* Keep the selected list row inside the visible window. */
static void
msgbox_show_sel(struct msgbox_data *md)
{
	u_int	visrows = msgbox_visrows(md);

	if (md->sel < 0)
		return;
	if ((u_int)md->sel < md->scroll)
		md->scroll = md->sel;
	else if ((u_int)md->sel >= md->scroll + visrows)
		md->scroll = md->sel - visrows + 1;
}

#define MB_ACT_SESSION 1		/* key = session name: switch client */
#define MB_ACT_WINDOW 2			/* key = window index: select window */
#define MB_ACT_BUFFER 3			/* key = buffer name: paste it */

/*
 * Activate the selected row (double-click / Enter) according to the list
 * kind. Returns 1 to close the box, 0 to keep it (item gone meanwhile).
 */
static int
msgbox_activate(struct client *c, struct msgbox_data *md)
{
	struct session		*s = c->session;
	struct winlink		*wl;
	struct paste_buffer	*pb;
	struct window_pane	*wp;
	const char		*key, *data;
	size_t			 size;

	if (!md->list || md->sel < 0 || (u_int)md->sel >= md->nlines ||
	    md->keys == NULL || (key = md->keys[md->sel]) == NULL)
		return (0);
	switch (md->act_kind) {
	case MB_ACT_SESSION:
		if ((s = session_find(key)) == NULL)
			return (0);
		if (c->session != s)
			server_client_set_session(c, s);
		break;
	case MB_ACT_WINDOW:
		if (s == NULL)
			return (0);
		wl = winlink_find_by_index(&s->windows, atoi(key));
		if (wl == NULL)
			return (0);
		session_set_current(s, wl);
		server_redraw_session(s);
		break;
	case MB_ACT_BUFFER:
		if (s == NULL || (pb = paste_get_name(key)) == NULL)
			return (0);
		wp = s->curw->window->active;
		data = paste_buffer_data(pb, &size);
		if (wp->screen->mode & MODE_BRACKETPASTE)
			bufferevent_write(wp->event, "\033[200~", 6);
		bufferevent_write(wp->event, data, size);
		if (wp->screen->mode & MODE_BRACKETPASTE)
			bufferevent_write(wp->event, "\033[201~", 6);
		break;
	default:
		return (0);
	}
	recalculate_sizes();
	server_redraw_client(c);
	return (1);
}

static int
msgbox_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct msgbox_data	*md = data;
	struct mouse_event	*m = &event->m;
	key_code		 key = event->key, base;
	u_int			 visrows = msgbox_visrows(md);
	u_int			 sbcol = md->w - 2;
	u_int			 maxscroll = (md->nlines > visrows) ?
				     md->nlines - visrows : 0;

	if (KEYC_IS_MOUSE(key)) {
		int	hover = (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b));
		u_int	lx, ly;

		if (hover)
			return (0);
		if (key == KEYC_DOUBLECLICK)	/* deferred replay: not a press */
			return (0);
		/* Scrollbar interaction in progress (held arrow / thumb). */
		if (md->sb_mode != 0) {
			if (MOUSE_RELEASE(m->b)) {
				md->sb_mode = 0;
				evtimer_del(&md->sb_timer);
				return (0);
			}
			if (MOUSE_DRAG(m->b)) {
				if (md->sb_mode == 1)
					md->sb_on = (m->x == md->sb_ax &&
					    m->y == md->sb_ay);
				else {
					u_int p = (m->y > md->py + 2) ?
					    m->y - (md->py + 2) : 0;

					md->scroll = scrollbar_value(p, maxscroll,
					    visrows - 2);
					c->flags |= CLIENT_REDRAWOVERLAY;
				}
				return (0);
			}
			md->sb_mode = 0;
			evtimer_del(&md->sb_timer);
		}
		if (MOUSE_WHEEL(m->b)) {	/* wheel = 3 * arStep (TScrollBar) */
			if (MOUSE_BUTTONS(m->b) == MOUSE_WHEEL_UP)
				md->scroll = (md->scroll > 3) ? md->scroll - 3 : 0;
			else
				md->scroll = (md->scroll + 3 < maxscroll) ?
				    md->scroll + 3 : maxscroll;
			c->flags |= CLIENT_REDRAWOVERLAY;
			return (0);
		}
		if (m->x < md->px || m->x >= md->px + md->w ||
		    m->y < md->py || m->y >= md->py + md->h) {
			if (MOUSE_RELEASE(m->b) && md->ok_pressed) {
				md->ok_pressed = 0;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		lx = m->x - md->px;
		ly = m->y - md->py;

		/* Scrollbar (TScrollBar): ▲/▼ step 1, track/indicator drags the
		 * thumb along the mouse; acts on press/drag, never on release. */
		if (lx == sbcol && ly >= 1 && ly <= visrows &&
		    !MOUSE_RELEASE(m->b) && !MOUSE_DRAG(m->b)) {
			struct timeval	tv = { 0, SB_DELAY_MS * 1000 };

			if (ly == 1 || ly == visrows) {	/* ▲ / ▼: step, hold */
				md->sb_dir = (ly == 1) ? -1 : 1;
				msgbox_sb_step(md, md->sb_dir);
				md->sb_mode = 1;
				md->sb_on = 1;
				md->sb_ax = m->x;
				md->sb_ay = m->y;
				if (event_initialized(&md->sb_timer))
					evtimer_del(&md->sb_timer);
				evtimer_set(&md->sb_timer, msgbox_sb_timer, md);
				evtimer_add(&md->sb_timer, &tv);
			} else {
				md->scroll = scrollbar_value(ly - 2, maxscroll,
				    visrows - 2);
				md->sb_mode = 2;
			}
			c->flags |= CLIENT_REDRAWOVERLAY;
			return (0);
		}

		/*
		 * List rows (TListViewer): a press selects the row; a second
		 * press on the same row within the click timeout (double-
		 * click) activates it.
		 */
		if (md->list && !MOUSE_RELEASE(m->b) && !MOUSE_DRAG(m->b) &&
		    ly >= 1 && ly <= visrows && lx >= 1 && lx < sbcol) {
			u_int		idx = md->scroll + (ly - 1);
			struct timeval	now, diff;

			if (idx < md->nlines && md->keys != NULL &&
			    md->keys[idx] != NULL) {
				gettimeofday(&now, NULL);
				timersub(&now, &md->last_click, &diff);
				md->sel = (int)idx;
				if ((int)idx == md->last_row && diff.tv_sec == 0 &&
				    diff.tv_usec < KEYC_CLICK_TIMEOUT * 1000L) {
					md->last_row = -1;
					return (msgbox_activate(c, md));
				}
				md->last_row = (int)idx;
				md->last_click = now;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		/* OK button: press (animate) on DOWN, close on RELEASE. */
		{
			u_int	okx = (md->w > 6) ? (md->w - 6) / 2 : 1;
			int	on_ok = (ly == md->h - 3 && lx >= okx &&
				    lx < okx + 6);

			if (!MOUSE_RELEASE(m->b)) {
				if (on_ok) {
					md->ok_pressed = 1;
					c->flags |= CLIENT_REDRAWOVERLAY;
				}
				return (0);
			}
			if (on_ok)
				return (1);
		}
		md->ok_pressed = 0;
		return (0);	/* clicks elsewhere inside do nothing (TVision) */
	}

	base = key & KEYC_MASK_KEY;	/* strip flags AND modifiers (Ctrl-PgDn) */
	if (md->list) {		/* TListViewer keys: move the selection */
		int	n = (int)md->nlines, vr = (int)visrows;

		switch (base) {
		case KEYC_UP:
			if (md->sel > 0)
				md->sel--;
			break;
		case KEYC_DOWN:
			if (md->sel + 1 < n)
				md->sel++;
			break;
		case KEYC_HOME:
			md->sel = 0;
			break;
		case KEYC_END:
			md->sel = n - 1;
			break;
		case KEYC_PPAGE:
			md->sel = (md->sel >= vr) ? md->sel - vr : 0;
			break;
		case KEYC_NPAGE:
			md->sel = (md->sel + vr < n) ? md->sel + vr : n - 1;
			break;
		case '\r':
		case '\n':
			return (msgbox_activate(c, md));
		default:
			goto notlist;
		}
		msgbox_show_sel(md);
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
notlist:
	switch (base) {
	case KEYC_UP:
		md->scroll = (md->scroll > 0) ? md->scroll - 1 : 0;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_DOWN:
		md->scroll = (md->scroll < maxscroll) ? md->scroll + 1 : maxscroll;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_NPAGE:	/* pgStep; Ctrl-PgDn = maxVal (TScrollBar) */
		if (key & KEYC_CTRL)
			md->scroll = maxscroll;
		else
			md->scroll = (md->scroll + visrows <= maxscroll) ?
			    md->scroll + visrows : maxscroll;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_PPAGE:	/* Ctrl-PgUp = minVal */
		if (key & KEYC_CTRL)
			md->scroll = 0;
		else
			md->scroll = (md->scroll >= visrows) ?
			    md->scroll - visrows : 0;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_HOME:
		md->scroll = 0;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_END:
		md->scroll = maxscroll;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case '\r':	/* Enter/Space flash the OK button, then close */
	case ' ':
		msgbox_start_flash(c, md);
		return (0);
	case '\033': /* Escape */
	case '\003': /* C-c */
	case '\007': /* C-g */
	case 'q':
		return (1);	/* close immediately */
	}
	return (0);		/* any other key: nothing (TVision) */
}

/* Dialog size for the current terminal; 0 if the terminal is too small. */
static int
msgbox_size(struct client *c, struct msgbox_data *md, u_int *w, u_int *h)
{
	*w = md->longest + 4;
	if (c->tty.sx > 6 && *w > (u_int)c->tty.sx - 4)
		*w = c->tty.sx - 4;
	if (*w < FORM_MINW)
		*w = FORM_MINW;
	*h = md->nlines + 5;
	if (*h > 34)
		*h = 34;
	if (c->tty.sy > 6 && *h > (u_int)c->tty.sy - 4)
		*h = c->tty.sy - 4;
	if (*h < 8)
		*h = 8;
	return (c->tty.sx >= *w && c->tty.sy >= *h);
}

/*
 * Terminal resized while the dialog is open: like a TVision dialog it stays
 * (refitted and re-centred) instead of tmux's default of dropping the
 * overlay; if the terminal can no longer fit it, it closes.
 */
static void
msgbox_resize_cb(struct client *c, void *data)
{
	struct msgbox_data	*md = data;
	u_int			 w, h, visrows, maxscroll;

	if (!msgbox_size(c, md, &w, &h)) {
		server_client_clear_overlay(c);	/* frees md */
		return;
	}
	if (w != md->w || h != md->h) {
		screen_resize(&md->s, w, h, 0);
		md->w = w;
		md->h = h;
	}
	visrows = msgbox_visrows(md);
	maxscroll = (md->nlines > visrows) ? md->nlines - visrows : 0;
	if (md->scroll > maxscroll)
		md->scroll = maxscroll;
	md->px = (c->tty.sx - w) / 2;
	md->py = (c->tty.sy - h) / 2;
	c->flags |= CLIENT_REDRAWOVERLAY;
}

/*
 * Open a scrollable text dialog. `text` is copied (split into lines).
 * `delay` > 0 closes it automatically after that many milliseconds (used
 * for status messages, like display-time). Returns the box (to turn it into
 * a list) or NULL if it could not open.
 */
static struct msgbox_data *
msgbox_display_delay(struct client *c, const char *title, const char *text,
    int delay)
{
	struct msgbox_data	*md;
	const char		*p, *nl;
	u_int			 w, h, longest = 0;

	md = xcalloc(1, sizeof *md);
	md->c = c;
	md->title = xstrdup(title);

	p = text;
	for (;;) {
		size_t	 linelen;
		char	*line;

		nl = strchr(p, '\n');
		linelen = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
		line = xmalloc(linelen + 1);
		memcpy(line, p, linelen);
		line[linelen] = '\0';
		md->lines = xreallocarray(md->lines, md->nlines + 1,
		    sizeof *md->lines);
		md->lines[md->nlines++] = line;
		if (linelen > longest)
			longest = linelen;
		if (nl == NULL)
			break;
		p = nl + 1;
	}
	/* Drop trailing empty lines (a final "\n" would show as blank rows). */
	while (md->nlines > 1 && md->lines[md->nlines - 1][0] == '\0')
		free(md->lines[--md->nlines]);
	if (md->nlines == 0) {
		md->lines = xreallocarray(md->lines, 1, sizeof *md->lines);
		md->lines[md->nlines++] = xstrdup("");
	}

	md->longest = longest;
	if (!msgbox_size(c, md, &w, &h)) {
		msgbox_free_cb(c, md);
		return (NULL);
	}
	md->sel = -1;
	md->last_row = -1;
	md->w = w;
	md->h = h;
	screen_init(&md->s, w, h, 0);
	md->s.mode &= ~MODE_CURSOR;
	md->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	md->px = (c->tty.sx - w) / 2;
	md->py = (c->tty.sy - h) / 2;
	server_client_set_overlay(c, (delay > 0) ? (u_int)delay : 0,
	    msgbox_check_cb, msgbox_mode_cb, msgbox_draw_cb, msgbox_key_cb,
	    msgbox_free_cb, msgbox_resize_cb, md);
	return (md);
}

static struct msgbox_data *
msgbox_display(struct client *c, const char *title, const char *text)
{
	return (msgbox_display_delay(c, title, text, 0));
}

/*
 * Status message on the desktop (display-message, command errors...): a
 * TVision message box instead of the status line, closing by itself after
 * display-time (delay in ms; 0 = until OK/Escape). Called by
 * status_message_set().
 */
void
message_dialog(struct client *c, const char *text, int delay)
{
	msgbox_display_delay(c, "Message", text, delay);
}

/*
 * ==========================================================================
 * Boîte de CONFIRMATION Turbo Vision (messageBox mfConfirmation | mfYesNo) :
 * texte centré + boutons [Oui] [Non]. Même contrat de rappel que l'invite de
 * la ligne d'état (prompt_input_cb / prompt_free_cb), pour que confirm-before
 * l'utilise en mode bureau : la réponse est le caractère de confirmation
 * (« y » ou la touche -c) pour Oui, NULL pour Non/Échap. Le rappel est
 * TOUJOURS invoqué exactement une fois (sinon la file de commandes qui attend
 * resterait bloquée), y compris si la boîte est fermée autrement.
 * ==========================================================================
 */

struct confirm_data {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	char	       **lines;
	u_int		 nlines;
	int		 focus;			/* 0 = Oui (default), 1 = Non */
	int		 pressed;		/* button being pressed, or -1 */
	struct event	 flash;
	int		 flash_kind;		/* 1 = Oui, 2 = Non */
	char		 yes[2];		/* confirm key, e.g. "y" */
	prompt_input_cb	 cb;
	prompt_free_cb	 freecb;
	void		*data;
	int		 answered;
};

#define CONFIRM_BW 7			/* "Oui"/"Non" face = 3 + 4 */
#define CONFIRM_GAP 4

/* Answer once: run the callback, then close (which frees the data). */
static void
confirm_answer(struct client *c, struct confirm_data *cd, int yes)
{
	if (!cd->answered) {
		cd->answered = 1;
		cd->cb(c, cd->data, yes ? cd->yes : NULL, 1);
	}
	server_client_clear_overlay(c);	/* frees cd via confirm_free_cb */
}

static void
confirm_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct confirm_data	*cd = data;

	server_client_overlay_range(cd->px, cd->py, cd->w, cd->h, px, py, nx, r);
}

static struct screen *
confirm_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct confirm_data	*cd = data;

	*cx = cd->px;
	*cy = cd->py;
	return (&cd->s);
}

static void
confirm_free_cb(struct client *c, void *data)
{
	struct confirm_data	*cd = data;
	u_int			 i;

	if (!cd->answered) {		/* closed without answering = Non */
		cd->answered = 1;
		cd->cb(c, cd->data, NULL, 1);
	}
	if (cd->freecb != NULL)
		cd->freecb(cd->data);
	if (event_initialized(&cd->flash))
		evtimer_del(&cd->flash);
	for (i = 0; i < cd->nlines; i++)
		free(cd->lines[i]);
	free(cd->lines);
	screen_free(&cd->s);
	free(cd);
}

static void
confirm_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct confirm_data	*cd = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &cd->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, gc;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	u_int			 W = cd->w, H = cd->h, i, j, bx;

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame,
	    " Confirmation ");

	/* Centred text lines (messageBox centres its text). */
	form_gc(&gc, FORM_DLG_FG, FORM_DLG_BG);
	for (j = 0; j < cd->nlines; j++) {
		u_int	tl = (u_int)strlen(cd->lines[j]);
		u_int	x = (W > tl + 4) ? (W - tl) / 2 : 2;

		screen_write_cursormove(&ctx, x, 2 + j, 0);
		screen_write_puts(&ctx, &gc, "%.*s", (int)(W - 4),
		    cd->lines[j]);
	}

	/* [Oui] [Non] - TButton faces; the focused one is the default. */
	bx = (W - (2 * CONFIRM_BW + CONFIRM_GAP)) / 2;
	form_draw_button(&ctx, bx, H - 3, "Oui", cd->focus == 0,
	    cd->pressed == 0);
	form_draw_button(&ctx, bx + CONFIRM_BW + CONFIRM_GAP, H - 3, "Non",
	    cd->focus == 1, cd->pressed == 1);

	screen_write_stop(&ctx);
	s->mode &= ~MODE_CURSOR;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++)
		tty_draw_line(tty, s, 0, i, W, cd->px, cd->py + i,
		    &grid_default_cell, NULL);
	form_draw_shadow(c, cd->px + W, cd->py + 1, 2, H - 1);
	form_draw_shadow(c, cd->px + 2, cd->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

/* Keyboard press-flash (TButton::press) then answer. */
static void
confirm_flash_cb(__unused int fd, __unused short events, void *arg)
{
	struct confirm_data	*cd = arg;
	int			 kind = cd->flash_kind;

	cd->pressed = -1;
	cd->flash_kind = 0;
	confirm_answer(cd->c, cd, kind == 1);
}

static void
confirm_start_flash(struct client *c, struct confirm_data *cd, int btn)
{
	struct timeval	tv = { 0, 90000 };

	if (cd->flash_kind != 0)
		return;
	cd->focus = btn;
	cd->pressed = btn;
	cd->flash_kind = (btn == 0) ? 1 : 2;
	c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&cd->flash))
		evtimer_del(&cd->flash);
	evtimer_set(&cd->flash, confirm_flash_cb, cd);
	evtimer_add(&cd->flash, &tv);
}

/* Which button is at dialog-local (lx, ly)? -1 if none. */
static int
confirm_button_at(struct confirm_data *cd, u_int lx, u_int ly)
{
	u_int	bx = (cd->w - (2 * CONFIRM_BW + CONFIRM_GAP)) / 2;

	if (ly != cd->h - 3)
		return (-1);
	if (lx >= bx && lx < bx + CONFIRM_BW)
		return (0);
	if (lx >= bx + CONFIRM_BW + CONFIRM_GAP &&
	    lx < bx + 2 * CONFIRM_BW + CONFIRM_GAP)
		return (1);
	return (-1);
}

static int
confirm_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct confirm_data	*cd = data;
	struct mouse_event	*m = &event->m;
	key_code		 key = event->key, base;

	if (cd->flash_kind != 0)		/* answering: ignore input */
		return (0);
	if (KEYC_IS_MOUSE(key)) {
		int	hover = (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b));
		int	np = -1;

		if (hover || MOUSE_WHEEL(m->b))
			return (0);
		if (m->x >= cd->px && m->x < cd->px + cd->w &&
		    m->y >= cd->py && m->y < cd->py + cd->h)
			np = confirm_button_at(cd, m->x - cd->px,
			    m->y - cd->py);
		if (!MOUSE_RELEASE(m->b)) {	/* press: sink the button */
			if (np >= 0 && !MOUSE_DRAG(m->b)) {
				cd->pressed = np;
				cd->focus = np;
				c->flags |= CLIENT_REDRAWOVERLAY;
			} else if (MOUSE_DRAG(m->b) && cd->pressed >= 0 &&
			    np != cd->pressed) {
				cd->pressed = -1;	/* dragged off */
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		if (cd->pressed >= 0 && np == cd->pressed) {	/* release */
			cd->pressed = -1;
			confirm_answer(c, cd, np == 0);
			return (0);
		}
		if (cd->pressed >= 0) {
			cd->pressed = -1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}

	base = key & KEYC_MASK_KEY;
	switch (base) {
	case '\033': /* Escape */
	case '\003': /* C-c */
	case '\007': /* C-g */
	case 'n': case 'N':
		confirm_start_flash(c, cd, 1);
		return (0);
	case 'y': case 'Y': case 'o': case 'O':
		confirm_start_flash(c, cd, 0);
		return (0);
	case '\r':
	case ' ':
		confirm_start_flash(c, cd, cd->focus);
		return (0);
	case '\011': /* Tab */
	case KEYC_BTAB:
	case KEYC_LEFT:
	case KEYC_RIGHT:
		cd->focus = !cd->focus;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
	if (base == (key_code)cd->yes[0]) {	/* custom -c confirm key */
		confirm_start_flash(c, cd, 0);
		return (0);
	}
	return (0);
}

/*
 * Open the confirmation box. `prompt` is the confirm-before prompt; a
 * trailing "(y/n)"-style hint is dropped since the buttons say it.
 */
void
confirm_dialog(struct client *c, const char *prompt, const char *yes,
    prompt_input_cb cb, prompt_free_cb freecb, void *data)
{
	struct confirm_data	*cd;
	char			*text, *p, *nl;
	size_t			 len;
	u_int			 w, h, longest = 0;

	cd = xcalloc(1, sizeof *cd);
	cd->c = c;
	cd->cb = cb;
	cd->freecb = freecb;
	cd->data = data;
	cd->focus = 0;
	cd->pressed = -1;
	cd->yes[0] = (yes != NULL && yes[0] != '\0') ? yes[0] : 'y';
	cd->yes[1] = '\0';

	text = xstrdup(prompt);
	if ((p = strrchr(text, '(')) != NULL && p[1] != '\0' &&
	    p[2] == '/' && p[3] == 'n' && p[4] == ')')
		*p = '\0';
	len = strlen(text);
	while (len > 0 && text[len - 1] == ' ')
		text[--len] = '\0';

	p = text;
	for (;;) {
		char	*line;

		nl = strchr(p, '\n');
		len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
		line = xmalloc(len + 1);
		memcpy(line, p, len);
		line[len] = '\0';
		cd->lines = xreallocarray(cd->lines, cd->nlines + 1,
		    sizeof *cd->lines);
		cd->lines[cd->nlines++] = line;
		if (len > longest)
			longest = (u_int)len;
		if (nl == NULL)
			break;
		p = nl + 1;
	}
	free(text);

	w = longest + 6;
	if (w < 2 * CONFIRM_BW + CONFIRM_GAP + 6)
		w = 2 * CONFIRM_BW + CONFIRM_GAP + 6;
	if (c->tty.sx > 4 && w > c->tty.sx - 4)
		w = c->tty.sx - 4;
	h = cd->nlines + 6;
	if (c->tty.sx < w || c->tty.sy < h) {
		/* Too small for a box: fall back to the status prompt. */
		u_int	i;

		for (i = 0; i < cd->nlines; i++)
			free(cd->lines[i]);
		free(cd->lines);
		free(cd);
		status_prompt_set(c, NULL, prompt, NULL, cb, freecb, data,
		    PROMPT_SINGLE, PROMPT_TYPE_COMMAND);
		return;
	}
	cd->w = w;
	cd->h = h;
	screen_init(&cd->s, w, h, 0);
	cd->s.mode &= ~MODE_CURSOR;
	cd->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	cd->px = (c->tty.sx - w) / 2;
	cd->py = (c->tty.sy - h) / 2;
	server_client_set_overlay(c, 0, confirm_check_cb, confirm_mode_cb,
	    confirm_draw_cb, confirm_key_cb, confirm_free_cb, NULL, cd);
}

/*
 * ==========================================================================
 * Boîte de SAISIE Turbo Vision (inputBox) pour l'invite de la ligne d'état
 * (command-prompt : rename-window, « : », etc.) en mode bureau. La boîte est
 * une VUE de l'invite existante : elle affiche c->prompt_string (titre) et
 * c->prompt_buffer (TInputLine) et transmet toutes les touches à
 * status_prompt_key() - historique, complétion, %% et invites multiples
 * (status_prompt_update) continuent de fonctionner. OK / Annuler envoient
 * Entrée / Échap. La boîte se ferme dès que l'invite disparaît ; fermée
 * autrement, elle annule l'invite (le rappel est toujours appelé une fois).
 * ==========================================================================
 */

struct prompt_dialog {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	int		 pressed;		/* 0 = OK, 1 = Annuler, -1 none */
	struct event	 flash;
	key_code	 flash_key;		/* key to forward after the flash */
};

#define PD_OKW 6			/* "OK" face */
#define PD_CANW 11			/* "Annuler" face */
#define PD_GAP 4

static void
pd_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct prompt_dialog	*pd = data;

	server_client_overlay_range(pd->px, pd->py, pd->w, pd->h, px, py, nx, r);
}

static struct screen *
pd_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct prompt_dialog	*pd = data;

	*cx = pd->px;
	*cy = pd->py;
	return (&pd->s);
}

static void
pd_free_cb(struct client *c, void *data)
{
	struct prompt_dialog	*pd = data;

	if (event_initialized(&pd->flash))
		evtimer_del(&pd->flash);
	/* Closed while the prompt is still up (menu, resize...): cancel it. */
	if (c->prompt_string != NULL && c->prompt_dialog) {
		c->prompt_dialog = 0;
		if (c->prompt_inputcb(c, c->prompt_data, NULL, 1) == 0)
			status_prompt_clear(c);
	}
	screen_free(&pd->s);
	free(pd);
}

/* Forward a key to the status prompt editor; close the box if it ended. */
static int
pd_forward(struct client *c, struct prompt_dialog *pd, key_code key)
{
	status_prompt_key(c, key);
	if (c->prompt_string == NULL || !c->prompt_dialog)
		return (1);		/* prompt finished: close the box */
	(void)pd;
	c->flags |= CLIENT_REDRAWOVERLAY;
	return (0);
}

static void
pd_draw_cb(struct client *c, void *data, __unused struct screen_redraw_ctx *rctx)
{
	struct prompt_dialog	*pd = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &pd->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, ig, cg;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	u_int			 W = pd->w, H = pd->h, i, j, bx;
	u_int			 fieldw = W - 4, off = 0, idx, n = 0, width = 0;
	u_int			 pcursor, x;
	char			 title[256];
	size_t			 tl;

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	/* Title = the prompt text, e.g. "(rename-window)". */
	snprintf(title, sizeof title, " %s ",
	    (c->prompt_string != NULL) ? c->prompt_string : "");
	tl = strlen(title);
	while (tl > 1 && title[tl - 2] == ' ' && title[tl - 1] == ' ')
		title[--tl] = '\0';
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame, title);

	/*
	 * TInputLine on a cyan bar, scrolled so the cursor is visible, with
	 * ◄/► overflow arrows; the cursor is a reversed cell (the tty cursor
	 * is hidden while a prompt is active, like the status line does).
	 */
	form_gc(&ig, FORM_INP_FG, FORM_INP_FBG);
	screen_write_cursormove(&ctx, 2, 2, 0);
	for (i = 0; i < fieldw; i++)
		screen_write_putc(&ctx, &ig, ' ');
	if (c->prompt_buffer != NULL) {
		pcursor = utf8_strwidth(c->prompt_buffer, c->prompt_index);
		if (pcursor >= fieldw)
			off = pcursor - fieldw + 1;
		x = 2;
		for (idx = 0; c->prompt_buffer[idx].size != 0; idx++) {
			u_int	cw = c->prompt_buffer[idx].width;

			if (width < off) {
				width += cw;
				continue;
			}
			if (x - 2 + cw > fieldw)
				break;
			memcpy(&cg, &ig, sizeof cg);
			memcpy(&cg.data, &c->prompt_buffer[idx], sizeof cg.data);
			if (idx == c->prompt_index)
				cg.attr |= GRID_ATTR_REVERSE;
			screen_write_cursormove(&ctx, x, 2, 0);
			screen_write_cell(&ctx, &cg);
			x += cw;
			width += cw;
			n++;
		}
		if (idx == c->prompt_index && x - 2 < fieldw) {
			/* cursor after the last character */
			memcpy(&cg, &ig, sizeof cg);
			cg.attr |= GRID_ATTR_REVERSE;
			screen_write_cursormove(&ctx, x, 2, 0);
			screen_write_putc(&ctx, &cg, ' ');
		}
		if (off > 0) {
			memcpy(&cg, &ig, sizeof cg);
			cg.data.data[0] = 0xe2; cg.data.data[1] = 0x97;
			cg.data.data[2] = 0x84;	/* ◄ */
			cg.data.have = cg.data.size = 3; cg.data.width = 1;
			screen_write_cursormove(&ctx, 2, 2, 0);
			screen_write_cell(&ctx, &cg);
		}
		if (c->prompt_buffer[idx].size != 0) {
			memcpy(&cg, &ig, sizeof cg);
			cg.data.data[0] = 0xe2; cg.data.data[1] = 0x96;
			cg.data.data[2] = 0xba;	/* ► */
			cg.data.have = cg.data.size = 3; cg.data.width = 1;
			screen_write_cursormove(&ctx, 2 + fieldw - 1, 2, 0);
			screen_write_cell(&ctx, &cg);
		}
	}

	/* [OK] [Annuler] */
	bx = (W - (PD_OKW + PD_GAP + PD_CANW)) / 2;
	form_draw_button(&ctx, bx, H - 3, "OK", 1, pd->pressed == 0);
	form_draw_button(&ctx, bx + PD_OKW + PD_GAP, H - 3, "Annuler", 0,
	    pd->pressed == 1);

	screen_write_stop(&ctx);
	s->mode &= ~MODE_CURSOR;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++)
		tty_draw_line(tty, s, 0, i, W, pd->px, pd->py + i,
		    &grid_default_cell, NULL);
	form_draw_shadow(c, pd->px + W, pd->py + 1, 2, H - 1);
	form_draw_shadow(c, pd->px + 2, pd->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

static void
pd_flash_cb(__unused int fd, __unused short events, void *arg)
{
	struct prompt_dialog	*pd = arg;
	struct client		*c = pd->c;
	key_code		 key = pd->flash_key;

	pd->pressed = -1;
	pd->flash_key = KEYC_NONE;
	if (pd_forward(c, pd, key))
		server_client_clear_overlay(c);	/* frees pd */
}

/* Sink a button for ~90 ms (TButton::press), then forward its key. */
static void
pd_start_flash(struct client *c, struct prompt_dialog *pd, int btn)
{
	struct timeval	tv = { 0, 90000 };

	if (pd->flash_key != KEYC_NONE)
		return;
	pd->pressed = btn;
	pd->flash_key = (btn == 0) ? '\r' : '\033';
	c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&pd->flash))
		evtimer_del(&pd->flash);
	evtimer_set(&pd->flash, pd_flash_cb, pd);
	evtimer_add(&pd->flash, &tv);
}

static int
pd_button_at(struct prompt_dialog *pd, u_int lx, u_int ly)
{
	u_int	bx = (pd->w - (PD_OKW + PD_GAP + PD_CANW)) / 2;

	if (ly != pd->h - 3)
		return (-1);
	if (lx >= bx && lx < bx + PD_OKW)
		return (0);
	if (lx >= bx + PD_OKW + PD_GAP && lx < bx + PD_OKW + PD_GAP + PD_CANW)
		return (1);
	return (-1);
}

static int
pd_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct prompt_dialog	*pd = data;
	struct mouse_event	*m = &event->m;
	key_code		 key = event->key, base;

	if (c->prompt_string == NULL || !c->prompt_dialog)
		return (1);		/* prompt already gone */
	if (pd->flash_key != KEYC_NONE)
		return (0);
	if (KEYC_IS_MOUSE(key)) {
		int	hover = (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b));
		int	np = -1, inside;
		u_int	lx = 0, ly = 0;

		if (hover || MOUSE_WHEEL(m->b))
			return (0);
		inside = (m->x >= pd->px && m->x < pd->px + pd->w &&
		    m->y >= pd->py && m->y < pd->py + pd->h);
		if (inside) {
			lx = m->x - pd->px;
			ly = m->y - pd->py;
			np = pd_button_at(pd, lx, ly);
		}
		if (!MOUSE_RELEASE(m->b)) {
			if (np >= 0 && !MOUSE_DRAG(m->b)) {
				pd->pressed = np;
				c->flags |= CLIENT_REDRAWOVERLAY;
			} else if (MOUSE_DRAG(m->b) && pd->pressed >= 0 &&
			    np != pd->pressed) {
				pd->pressed = -1;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		if (pd->pressed >= 0 && np == pd->pressed) {
			pd->pressed = -1;
			return (pd_forward(c, pd, np == 0 ? '\r' : '\033'));
		}
		if (pd->pressed >= 0) {
			pd->pressed = -1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		/* Click in the input line: move the cursor there. */
		if (inside && ly == 2 && lx >= 2 && lx < pd->w - 2 &&
		    c->prompt_buffer != NULL) {
			u_int	fieldw = pd->w - 4, off = 0, pc, want, i, wd = 0;

			pc = utf8_strwidth(c->prompt_buffer, c->prompt_index);
			if (pc >= fieldw)
				off = pc - fieldw + 1;
			want = off + (lx - 2);
			for (i = 0; c->prompt_buffer[i].size != 0; i++) {
				if (wd + c->prompt_buffer[i].width > want)
					break;
				wd += c->prompt_buffer[i].width;
			}
			c->prompt_index = i;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}

	base = key & KEYC_MASK_KEY;
	switch (base) {
	case '\r':
	case '\n':
		pd_start_flash(c, pd, 0);	/* OK sinks, then Enter */
		return (0);
	case '\033':
		pd_start_flash(c, pd, 1);	/* Annuler sinks, then Escape */
		return (0);
	}
	return (pd_forward(c, pd, key));	/* editing keys, history... */
}

static void
pd_resize_cb(struct client *c, void *data)
{
	struct prompt_dialog	*pd = data;
	u_int			 w = pd->w;

	if (c->tty.sx > 4 && w > c->tty.sx - 4)
		w = c->tty.sx - 4;
	if (w < PD_OKW + PD_GAP + PD_CANW + 6 || c->tty.sy < pd->h) {
		server_client_clear_overlay(c);	/* cancels the prompt */
		return;
	}
	if (w != pd->w) {
		screen_resize(&pd->s, w, pd->h, 0);
		pd->w = w;
	}
	pd->px = (c->tty.sx - w) / 2;
	pd->py = (c->tty.sy - pd->h) / 2;
	c->flags |= CLIENT_REDRAWOVERLAY;
}

/*
 * Called by status_prompt_set() on the desktop: show the prompt in a box.
 * Returns 1 if the box was opened (the caller then does NOT push a status
 * screen: the bar keeps drawing normally under the box), 0 if the terminal
 * is too small (status-line prompt as usual).
 */
int
prompt_dialog_open(struct client *c)
{
	struct prompt_dialog	*pd;
	u_int			 w = 56, h = 7;

	if (c->tty.sx > 4 && w > c->tty.sx - 4)
		w = c->tty.sx - 4;
	if (w < PD_OKW + PD_GAP + PD_CANW + 6 || c->tty.sy < h + 2)
		return (0);

	pd = xcalloc(1, sizeof *pd);
	pd->c = c;
	pd->w = w;
	pd->h = h;
	pd->pressed = -1;
	pd->flash_key = KEYC_NONE;
	screen_init(&pd->s, w, h, 0);
	pd->s.mode &= ~MODE_CURSOR;
	pd->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	pd->px = (c->tty.sx - w) / 2;
	pd->py = (c->tty.sy - h) / 2;
	c->prompt_dialog = 1;
	c->flags |= CLIENT_REDRAWSTATUS;
	server_client_set_overlay(c, 0, pd_check_cb, pd_mode_cb, pd_draw_cb,
	    pd_key_cb, pd_free_cb, pd_resize_cb, pd);
	return (1);
}

/*
 * ==========================================================================
 * Autres « conversations » fenêtrées : listes des FENÊTRES (double-clic =
 * sélectionner), des TAMPONS (double-clic = coller) et journal des MESSAGES.
 * Les touches par défaut w / # / = / ~ / ? / s les utilisent en mode bureau
 * (key-bindings.c), à la place de choose-tree / list-* en plein panneau.
 * ==========================================================================
 */

/* Turn a fresh msgbox into a list whose rows map 1:1 to `keys` (taken). */
static void
msgbox_make_list(struct msgbox_data *md, int kind, char **keys, u_int n,
    int sel)
{
	u_int	i;

	if (md != NULL && n > 0 && n == md->nlines) {
		md->list = 1;
		md->act_kind = kind;
		md->keys = keys;
		md->sel = (sel >= 0 && (u_int)sel < n) ? sel : 0;
		msgbox_show_sel(md);
		return;
	}
	for (i = 0; i < n; i++)
		free(keys[i]);
	free(keys);
}

static enum cmd_retval
cmd_dwindows_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client		*c = cmdq_get_target_client(item);
	struct session		*s;
	struct winlink		*wl;
	struct msgbox_data	*md;
	char			*buf = NULL, **keys = NULL;
	size_t			 sz = 0;
	FILE			*f;
	u_int			 n = 0;
	int			 sel = -1;

	if (c == NULL || (s = c->session) == NULL)
		return (CMD_RETURN_NORMAL);
	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (CMD_RETURN_ERROR);
	RB_FOREACH(wl, winlinks, &s->windows) {
		u_int	np = window_count_panes(wl->window);

		fprintf(f, "%3d: %-24.24s %u panneau%s%s\n", wl->idx,
		    wl->window->name, np, (np > 1) ? "x" : "",
		    (wl == s->curw) ? "   (courante)" : "");
		keys = xreallocarray(keys, n + 1, sizeof *keys);
		xasprintf(&keys[n], "%d", wl->idx);
		if (wl == s->curw)
			sel = (int)n;
		n++;
	}
	fclose(f);
	md = msgbox_display(c, "Fenetres",
	    (buf != NULL && *buf != '\0') ? buf : "(aucune fenetre)");
	free(buf);
	msgbox_make_list(md, MB_ACT_WINDOW, keys, n, sel);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dwindows_entry = {
	.name = "display-windows",
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dwindows_exec
};

static enum cmd_retval
cmd_dbuffers_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client		*c = cmdq_get_target_client(item);
	struct paste_buffer	*pb = NULL;
	struct msgbox_data	*md;
	char			*buf = NULL, **keys = NULL, preview[41];
	const char		*data;
	size_t			 sz = 0, size, i, k;
	FILE			*f;
	u_int			 n = 0;

	if (c == NULL)
		return (CMD_RETURN_NORMAL);
	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (CMD_RETURN_ERROR);
	while ((pb = paste_walk(pb)) != NULL) {
		data = paste_buffer_data(pb, &size);
		for (i = 0, k = 0; i < size && k < sizeof preview - 1; i++) {
			if (data[i] == '\n')
				break;
			preview[k++] = ((u_char)data[i] < 0x20) ?
			    '?' : data[i];
		}
		preview[k] = '\0';
		fprintf(f, "%-10.10s %7zu octets  %s\n", paste_buffer_name(pb),
		    size, preview);
		keys = xreallocarray(keys, n + 1, sizeof *keys);
		keys[n++] = xstrdup(paste_buffer_name(pb));
	}
	fclose(f);
	md = msgbox_display(c, "Tampons (double-clic = coller)",
	    (buf != NULL && *buf != '\0') ? buf : "(aucun tampon)");
	free(buf);
	msgbox_make_list(md, MB_ACT_BUFFER, keys, n, 0);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dbuffers_entry = {
	.name = "display-buffers",
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dbuffers_exec
};

static enum cmd_retval
cmd_dmessages_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client		*c = cmdq_get_target_client(item);
	struct message_entry	*msg;
	char			*buf = NULL, tim[32];
	size_t			 sz = 0;
	FILE			*f;
	struct tm		*tm;

	if (c == NULL)
		return (CMD_RETURN_NORMAL);
	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (CMD_RETURN_ERROR);
	TAILQ_FOREACH_REVERSE(msg, &message_log, message_list, entry) {
		tm = localtime(&msg->msg_time.tv_sec);
		if (tm == NULL || strftime(tim, sizeof tim, "%H:%M:%S", tm) == 0)
			strlcpy(tim, "--:--:--", sizeof tim);
		fprintf(f, "%s  %s\n", tim, msg->msg);
	}
	fclose(f);
	msgbox_display(c, "Messages",
	    (buf != NULL && *buf != '\0') ? buf : "(aucun message)");
	free(buf);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dmessages_entry = {
	.name = "display-log",	/* NOT display-messages: would make
				 * display-message ambiguous (prefix match) */
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dmessages_exec
};

/*
 * ==========================================================================
 * CLAUDE : fenêtre gestionnaire de conversations + sélecteur de répertoire
 * façon Turbo Vision (TFileDialog : saisie du nom, chemin courant, liste des
 * répertoires avec ascenseur, boutons empilés à droite). Le répertoire choisi
 * sert à lancer `claude` dans un nouveau panneau de la fenêtre.
 * ==========================================================================
 */

static char	*claude_lastdir;	/* last directory chosen */

struct dirbox_data {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	char		 path[PATH_MAX];	/* directory being browsed */
	char	       **ents;			/* ".." + subdirectories */
	u_int		 nents, scroll;
	int		 sel;
	int		 focus;			/* 0 list, 1 Choisir, 2 Annuler */
	int		 pressed;		/* button being pressed, or -1 */
	struct event	 flash;
	int		 flash_kind;		/* 1 = Choisir, 2 = Annuler */
	struct timeval	 last_click;
	int		 last_row;
};

#define DB_W 56
#define DB_H 18
#define DB_LISTW (DB_W - 18)		/* list, then the button column */
#define DB_BTNX (DB_W - 14)
#define DB_BW 11

static void	dirbox_load(struct dirbox_data *);

static u_int
dirbox_visrows(struct dirbox_data *db)
{
	return (db->h > 8 ? db->h - 8 : 1);
}

static void
dirbox_free_ents(struct dirbox_data *db)
{
	u_int	i;

	for (i = 0; i < db->nents; i++)
		free(db->ents[i]);
	free(db->ents);
	db->ents = NULL;
	db->nents = 0;
}

/* Read the subdirectories of db->path (plus "..") into db->ents. */
static void
dirbox_load(struct dirbox_data *db)
{
	DIR		*d;
	struct dirent	*e;
	struct stat	 sb;
	char		 full[PATH_MAX];

	dirbox_free_ents(db);
	db->scroll = 0;
	db->sel = 0;
	db->last_row = -1;

	db->ents = xreallocarray(NULL, 1, sizeof *db->ents);
	db->ents[db->nents++] = xstrdup("..");

	if ((d = opendir(db->path)) == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')	/* skip hidden and . / .. */
			continue;
		if ((size_t)snprintf(full, sizeof full, "%s/%s", db->path,
		    e->d_name) >= sizeof full)
			continue;
		if (stat(full, &sb) != 0 || !S_ISDIR(sb.st_mode))
			continue;
		db->ents = xreallocarray(db->ents, db->nents + 1,
		    sizeof *db->ents);
		db->ents[db->nents++] = xstrdup(e->d_name);
	}
	closedir(d);
}

static void
dirbox_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct dirbox_data	*db = data;

	server_client_overlay_range(db->px, db->py, db->w, db->h, px, py, nx, r);
}

static struct screen *
dirbox_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct dirbox_data	*db = data;

	*cx = db->px;
	*cy = db->py;
	return (&db->s);
}

static void
dirbox_free_cb(__unused struct client *c, void *data)
{
	struct dirbox_data	*db = data;

	if (event_initialized(&db->flash))
		evtimer_del(&db->flash);
	dirbox_free_ents(db);
	screen_free(&db->s);
	free(db);
}

static void
dirbox_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct dirbox_data	*db = data;
	struct tty		*tty = &c->tty;
	struct screen		*s = &db->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, gc, ig, sel;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	u_int			 W = db->w, H = db->h, i, j;
	u_int			 visrows = dirbox_visrows(db);
	u_int			 listw = W - 18, maxscroll;
	char			 buf[512];

	maxscroll = (db->nents > visrows) ? db->nents - visrows : 0;
	if (db->scroll > maxscroll)
		db->scroll = maxscroll;

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;
	form_gc(&gc, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&ig, FORM_INP_FG, FORM_INP_FBG);
	form_gc(&sel, FORM_FOC_FG, FORM_FOC_BG);

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame,
	    " Choisir un repertoire ");

	/* "Nom:" label + the path on an input bar (TFileDialog layout). */
	screen_write_cursormove(&ctx, 2, 2, 0);
	screen_write_puts(&ctx, &gc, "Nom:");
	screen_write_cursormove(&ctx, 2, 3, 0);
	snprintf(buf, sizeof buf, "%-*.*s", (int)(W - 4), (int)(W - 4),
	    db->path);
	screen_write_puts(&ctx, &ig, "%s", buf);

	/* "Repertoires:" label + the list. */
	screen_write_cursormove(&ctx, 2, 5, 0);
	screen_write_puts(&ctx, &gc, "Repertoires:");
	for (j = 0; j < visrows; j++) {
		u_int	idx = db->scroll + j;
		char	label[256];

		if (idx >= db->nents)
			break;
		if (strcmp(db->ents[idx], "..") == 0)
			snprintf(label, sizeof label, "..");
		else
			snprintf(label, sizeof label, "[%s]", db->ents[idx]);
		snprintf(buf, sizeof buf, "%-*.*s", (int)listw, (int)listw,
		    label);
		screen_write_cursormove(&ctx, 2, 6 + j, 0);
		screen_write_puts(&ctx, ((int)idx == db->sel) ? &sel : &gc,
		    "%s", buf);
	}
	if (visrows >= 3)
		scrollbar_draw(&ctx, 2 + listw, 6, visrows, db->scroll,
		    maxscroll, FORM_DLG_FG, FORM_DLG_BG);

	/* Buttons stacked on the right, like TFileDialog. */
	form_draw_button(&ctx, DB_BTNX, 6, "Choisir", db->focus != 2,
	    db->pressed == 1);
	form_draw_button(&ctx, DB_BTNX, 9, "Annuler", db->focus == 2,
	    db->pressed == 2);

	screen_write_stop(&ctx);
	s->mode &= ~MODE_CURSOR;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++)
		tty_draw_line(tty, s, 0, i, W, db->px, db->py + i,
		    &grid_default_cell, NULL);
	form_draw_shadow(c, db->px + W, db->py + 1, 2, H - 1);
	form_draw_shadow(c, db->px + 2, db->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

static void	claude_env_opts(struct client *, struct window *, const char *,
		    char *, size_t, char *, size_t);

/*
 * ==========================================================================
 * CLAUDE: RESTORING A PAST CONVERSATION
 *
 * Claude Code keeps one file per conversation in
 * ~/.claude/projects/<directory with every '/' turned into '-'>/<uuid>.jsonl
 * and reopens one with `claude --resume <uuid>`. The manager lists the
 * conversations of its own directory under "Reprendre", most recent first, and
 * a DOUBLE click on a row brings it back as a new conversation.
 *
 * The title is the one Claude shows in its own /resume list: the last
 * "customTitle" the user set, else the last "aiTitle" it generated. Both are
 * rewritten on every exchange, so only the TAIL of the file is read - these
 * files reach tens of megabytes.
 * ==========================================================================
 */

struct claude_sess {
	char	*id;
	char	*title;
	time_t	 mtime;
};

static struct claude_sess	*claude_sess_list;
static u_int			 claude_sess_n;
static char			*claude_sess_dir;	/* project scanned */
static time_t			 claude_sess_scanned;

#define CLAUDE_SESS_MAX		64
#define CLAUDE_SESS_TAIL	(128 * 1024)
#define CLAUDE_SESS_RESCAN	5		/* seconds */

/* Path of the Claude Code project directory holding cwd's conversations. */
static int
claude_project_dir(const char *cwd, char *buf, size_t len)
{
	const char	*home;
	size_t		 i, n;

	if (cwd == NULL || *cwd != '/')
		return (0);
	if ((home = getenv("HOME")) == NULL || *home == '\0')
		return (0);

	n = (size_t)snprintf(buf, len, "%s/.claude/projects/", home);
	if (n >= len)
		return (0);
	for (i = 0; cwd[i] != '\0' && n + 1 < len; i++)
		buf[n++] = (cwd[i] == '/') ? '-' : cwd[i];
	if (n >= len)
		return (0);
	buf[n] = '\0';
	return (1);
}

/* Decode a JSON string (up to the closing quote) into a printable label. */
static void
claude_json_string(const char *p, char *out, size_t len)
{
	size_t	o = 0;
	u_int	cp;

	while (*p != '\0' && *p != '"' && o + 1 < len) {
		if (*p != '\\') {
			/* Control characters would corrupt the drawing. */
			out[o++] = ((u_char)*p < 0x20) ? ' ' : *p;
			p++;
			continue;
		}
		p++;
		switch (*p) {
		case '\0':
			goto done;
		case 'n': case 'r': case 't':
			out[o++] = ' ';
			p++;
			break;
		case 'u':
			if (sscanf(p + 1, "%4x", &cp) != 1) {
				p++;
				break;
			}
			p += 5;
			if (cp < 0x80) {
				out[o++] = (char)cp;
			} else if (cp < 0x800 && o + 2 < len) {
				out[o++] = (char)(0xc0 | (cp >> 6));
				out[o++] = (char)(0x80 | (cp & 0x3f));
			} else if (o + 3 < len) {
				out[o++] = (char)(0xe0 | (cp >> 12));
				out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
				out[o++] = (char)(0x80 | (cp & 0x3f));
			}
			break;
		default:
			out[o++] = *p++;
			break;
		}
	}
done:
	out[o] = '\0';
}

/* Last occurrence of key "<name>":"<value>" in buf. */
static int
claude_json_last(const char *buf, const char *name, char *out, size_t len)
{
	char		 key[64];
	const char	*p, *found = NULL;

	snprintf(key, sizeof key, "\"%s\":\"", name);
	for (p = buf; (p = strstr(p, key)) != NULL; p += strlen(key))
		found = p;
	if (found == NULL)
		return (0);
	claude_json_string(found + strlen(key), out, len);
	return (*out != '\0');
}

/* Title of one conversation file, read from its tail only. */
static void
claude_sess_title(const char *path, char *out, size_t len)
{
	int		 fd;
	off_t		 size, off;
	ssize_t		 n;
	char		*buf;

	*out = '\0';
	if ((fd = open(path, O_RDONLY)) == -1)
		return;
	size = lseek(fd, 0, SEEK_END);
	if (size <= 0) {
		close(fd);
		return;
	}
	off = (size > CLAUDE_SESS_TAIL) ? size - CLAUDE_SESS_TAIL : 0;
	if (lseek(fd, off, SEEK_SET) == -1) {
		close(fd);
		return;
	}
	buf = xmalloc(CLAUDE_SESS_TAIL + 1);
	n = read(fd, buf, CLAUDE_SESS_TAIL);
	close(fd);
	if (n <= 0) {
		free(buf);
		return;
	}
	buf[n] = '\0';

	if (!claude_json_last(buf, "customTitle", out, len) &&
	    !claude_json_last(buf, "aiTitle", out, len))
		claude_json_last(buf, "lastPrompt", out, len);
	free(buf);
}

static int
claude_sess_cmp(const void *a, const void *b)
{
	const struct claude_sess	*sa = a, *sb = b;

	if (sa->mtime < sb->mtime)
		return (1);		/* most recent first */
	if (sa->mtime > sb->mtime)
		return (-1);
	return (0);
}

static void
claude_sess_free(void)
{
	u_int	i;

	for (i = 0; i < claude_sess_n; i++) {
		free(claude_sess_list[i].id);
		free(claude_sess_list[i].title);
	}
	free(claude_sess_list);
	claude_sess_list = NULL;
	claude_sess_n = 0;
}

/*
 * Refresh the list for this directory. Throttled: the manager list is redrawn
 * constantly and this reads the tail of every conversation file.
 */
void
claude_sess_scan(const char *cwd)
{
	char		 dir[PATH_MAX], path[PATH_MAX], title[256];
	DIR		*dp;
	struct dirent	*de;
	struct stat	 sb;
	struct timeval	 tv;
	size_t		 len;
	u_int		 alloc = 0;

	if (!claude_project_dir(cwd, dir, sizeof dir))
		return;
	gettimeofday(&tv, NULL);
	if (claude_sess_dir != NULL && strcmp(claude_sess_dir, dir) == 0 &&
	    tv.tv_sec - claude_sess_scanned < CLAUDE_SESS_RESCAN)
		return;

	claude_sess_free();
	free(claude_sess_dir);
	claude_sess_dir = xstrdup(dir);
	claude_sess_scanned = tv.tv_sec;

	if ((dp = opendir(dir)) == NULL)
		return;
	while ((de = readdir(dp)) != NULL) {
		len = strlen(de->d_name);
		if (len < 7 || strcmp(de->d_name + len - 6, ".jsonl") != 0)
			continue;
		if (snprintf(path, sizeof path, "%s/%s", dir,
		    de->d_name) >= (int)sizeof path)
			continue;
		if (stat(path, &sb) == -1 || !S_ISREG(sb.st_mode) ||
		    sb.st_size == 0)
			continue;
		if (claude_sess_n == alloc) {
			alloc = (alloc == 0) ? 16 : alloc * 2;
			claude_sess_list = xreallocarray(claude_sess_list,
			    alloc, sizeof *claude_sess_list);
		}
		claude_sess_list[claude_sess_n].id = xstrndup(de->d_name,
		    len - 6);
		claude_sess_list[claude_sess_n].mtime = sb.st_mtime;
		claude_sess_list[claude_sess_n].title = NULL;
		claude_sess_n++;
	}
	closedir(dp);

	qsort(claude_sess_list, claude_sess_n, sizeof *claude_sess_list,
	    claude_sess_cmp);
	if (claude_sess_n > CLAUDE_SESS_MAX) {
		u_int	i;

		for (i = CLAUDE_SESS_MAX; i < claude_sess_n; i++) {
			free(claude_sess_list[i].id);
			free(claude_sess_list[i].title);
		}
		claude_sess_n = CLAUDE_SESS_MAX;
	}

	/* Titles only for the ones that are kept. */
	for (alloc = 0; alloc < claude_sess_n; alloc++) {
		snprintf(path, sizeof path, "%s/%s.jsonl", dir,
		    claude_sess_list[alloc].id);
		claude_sess_title(path, title, sizeof title);
		claude_sess_list[alloc].title = xstrdup(title);
	}
}

static int	claude_pane_is_agent(struct window_pane *);

/*
 * The uuid a pane was STARTED on, when it carries one: `claude --resume <uuid>`
 * (or --resume=<uuid>) leaves it on the command line of the foreground process.
 */
static int
claude_pane_resume_uuid(struct window_pane *wp, char *out, size_t len)
{
	char	 path[64], buf[4096], *p, *end, *arg;
	pid_t	 pgrp;
	ssize_t	 n;
	int	 fd;
	size_t	 i;

	*out = '\0';
	if (wp->fd == -1 || (pgrp = tcgetpgrp(wp->fd)) == -1)
		return (0);
	snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pgrp);
	if ((fd = open(path, O_RDONLY)) == -1)
		return (0);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return (0);
	buf[n] = '\0';

	/* Arguments are NUL-separated. */
	arg = NULL;
	for (p = buf, end = buf + n; p < end; p += strlen(p) + 1) {
		/*
		 * --resume <id> (a conversation brought back) or --session-id
		 * <id> (one tmuxv started and named itself): either way the
		 * EXACT conversation, nothing to guess.
		 */
		if (strncmp(p, "--resume=", 9) == 0) {
			arg = p + 9;
			break;
		}
		if (strncmp(p, "--session-id=", 13) == 0) {
			arg = p + 13;
			break;
		}
		if (strcmp(p, "--resume") == 0 ||
		    strcmp(p, "--session-id") == 0) {
			p += strlen(p) + 1;
			if (p < end)
				arg = p;
			break;
		}
	}
	if (arg == NULL || *arg == '\0')
		return (0);
	for (i = 0; arg[i] != '\0'; i++) {
		if (!isxdigit((u_char)arg[i]) && arg[i] != '-')
			return (0);
	}
	strlcpy(out, arg, len);
	return (1);
}

/* The exact Claude session id a pane runs, when its command line says it. */
int
claude_pane_session_uuid(struct window_pane *wp, char *out, size_t len)
{
	return (claude_pane_resume_uuid(wp, out, len));
}

/* The Claude project directory this pane is sitting in. */
static int
claude_pane_projdir(struct window_pane *wp, char *out, size_t len)
{
	char	*cwd;

	cwd = osdep_get_cwd(wp->fd);
	if (cwd == NULL || *cwd != '/')
		cwd = wp->cwd;
	if (cwd == NULL || *cwd != '/')
		return (0);
	return (claude_project_dir(cwd, out, len));
}

/*
 * CLAUDE: drop from the list the conversations that are ALREADY OPEN in this
 * window - "Reprendre" only ever offers what can actually be resumed. A pane
 * started with `claude --resume <uuid>` says which one it holds; a pane started
 * fresh says nothing at all (Claude Code exports no session id, keeps no lock
 * file and does not hold its transcript open), so it is matched the way the
 * crash restore does it: the most recently appended transcript of the
 * directory, one per pane, and only among those touched within the hour - a
 * conversation nobody has written to in an hour is nobody's live session.
 */
static void
claude_sess_hide_running(struct window *w)
{
	struct window_pane	*wp;
	struct window		*loop;
	char			 dir[PATH_MAX], uuid[128];
	int			*busy;
	time_t			 now;
	u_int			 i, keep, best;

	if (w == NULL || claude_sess_n == 0 || claude_sess_dir == NULL)
		return;
	busy = xcalloc(claude_sess_n, sizeof *busy);
	now = time(NULL);

	/* First the panes that name their conversation. */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!claude_pane_is_agent(wp))
			continue;
		if (!claude_pane_projdir(wp, dir, sizeof dir) ||
		    strcmp(dir, claude_sess_dir) != 0)
			continue;
		if (!claude_pane_resume_uuid(wp, uuid, sizeof uuid))
			continue;
		for (i = 0; i < claude_sess_n; i++) {
			if (strcmp(claude_sess_list[i].id, uuid) == 0)
				busy[i] = 1;
		}
	}
	/* Then the others claim the newest transcript still unclaimed. */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!claude_pane_is_agent(wp))
			continue;
		if (!claude_pane_projdir(wp, dir, sizeof dir) ||
		    strcmp(dir, claude_sess_dir) != 0)
			continue;
		if (claude_pane_resume_uuid(wp, uuid, sizeof uuid))
			continue;
		best = claude_sess_n;
		for (i = 0; i < claude_sess_n; i++) {
			if (busy[i] ||
			    now - claude_sess_list[i].mtime > 3600)
				continue;
			if (best == claude_sess_n ||
			    claude_sess_list[i].mtime >
			    claude_sess_list[best].mtime)
				best = i;
		}
		if (best != claude_sess_n)
			busy[best] = 1;
	}

	for (i = 0, keep = 0; i < claude_sess_n; i++) {
		if (busy[i]) {
			free(claude_sess_list[i].id);
			free(claude_sess_list[i].title);
			continue;
		}
		claude_sess_list[keep++] = claude_sess_list[i];
	}
	free(busy);
	if (keep == claude_sess_n)
		return;
	claude_sess_n = keep;
	RB_FOREACH(loop, windows, &windows) {
		if (loop->claude_sel_sess >= (int)claude_sess_n) {
			loop->claude_sel_sess = (claude_sess_n == 0) ? -1 :
			    (int)claude_sess_n - 1;
		}
		if (loop->claude_scroll != 0)
			claude_list_scroll(loop, 0);
	}
}

u_int
claude_sess_count(void)
{
	return (claude_sess_n);
}

const char *
claude_sess_label(u_int i)
{
	if (i >= claude_sess_n)
		return (NULL);
	if (claude_sess_list[i].title != NULL &&
	    *claude_sess_list[i].title != '\0')
		return (claude_sess_list[i].title);
	return (claude_sess_list[i].id);
}

const char *
claude_sess_id(u_int i)
{
	if (i >= claude_sess_n)
		return (NULL);
	return (claude_sess_list[i].id);
}

/*
 * CLAUDE: forget a saved conversation FOR GOOD - the transcript file itself is
 * removed, so `claude --resume` can never bring it back. The entry is dropped
 * from the in-memory list at once (the scan is throttled, the list would
 * otherwise keep showing a conversation that no longer exists) and every
 * window highlighting a row past the end is pulled back into range.
 */
static int
claude_sess_delete(u_int idx)
{
	struct window	*w;
	char		*path;
	u_int		 i;
	int		 fail;

	if (idx >= claude_sess_n || claude_sess_dir == NULL)
		return (0);
	xasprintf(&path, "%s/%s.jsonl", claude_sess_dir,
	    claude_sess_list[idx].id);
	fail = (unlink(path) == -1 && errno != ENOENT);
	free(path);
	if (fail)
		return (0);

	free(claude_sess_list[idx].id);
	free(claude_sess_list[idx].title);
	for (i = idx + 1; i < claude_sess_n; i++)
		claude_sess_list[i - 1] = claude_sess_list[i];
	claude_sess_n--;

	RB_FOREACH(w, windows, &windows) {
		if (w->claude_sel_sess >= (int)claude_sess_n) {
			w->claude_sel_sess = (claude_sess_n == 0) ? -1 :
			    (int)claude_sess_n - 1;
		}
		if (w->claude_scroll != 0)
			claude_list_scroll(w, 0);
	}
	return (1);
}

/*
 * CLAUDE: bring a saved conversation back - a new pane running
 * `claude --resume <uuid>` in the directory the conversation belongs to.
 */
void
claude_resume(struct client *c, u_int idx)
{
	struct session		*s = c->session;
	struct window		*w;
	struct window_pane	*wp;
	struct cmdq_state	*state;
	const char		*id;
	char			*cmd, *error, *cwd, q[PATH_MAX + 16];
	char			 envopt[2560] = "", agent[128];
	size_t			 i;

	if (s == NULL || s->curw == NULL)
		return;
	w = s->curw->window;
	if ((wp = w->active) == NULL)
		return;
	if ((id = claude_sess_id(idx)) == NULL)
		return;
	/*
	 * The id comes from a file name: pass nothing but a plain uuid to the
	 * shell.
	 */
	for (i = 0; id[i] != '\0'; i++) {
		if (!isxdigit((u_char)id[i]) && id[i] != '-')
			return;
	}
	if (i == 0 || i > 64)
		return;

	cwd = osdep_get_cwd(wp->fd);
	if (cwd == NULL || *cwd != '/')
		cwd = wp->cwd;
	if (cwd == NULL || *cwd != '/')
		cwd = (char *)server_client_get_cwd(c, s);

	claude_env_opts(c, w, cwd, envopt, sizeof envopt, agent, sizeof agent);

	/* Same dance as [+ Nouvelle]: never split the zoomed single cell. */
	if (cwd != NULL && *cwd == '/') {
		form_quote(q, sizeof q, cwd);
		xasprintf(&cmd, "select-layout -t %%%u tiled ; "
		    "split-window -h -t %%%u%s -c %s claude --resume %s ; "
		    "select-layout -t %%%u tiled", wp->id, wp->id, envopt, q,
		    id, wp->id);
	} else {
		xasprintf(&cmd, "select-layout -t %%%u tiled ; "
		    "split-window -h -t %%%u%s claude --resume %s ; "
		    "select-layout -t %%%u tiled", wp->id, wp->id, envopt, id,
		    wp->id);
	}
	state = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, c, state, &error) ==
	    CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);
	free(cmd);
}

/*
 * CLAUDE: refresh the saved-conversation list for the manager's own directory.
 * Throttled here rather than in the scan, so the live cwd lookup (which reads
 * /proc) does not run on every redraw either.
 */
void
claude_sess_refresh(struct window *w)
{
	static time_t		 last;
	struct timeval		 tv;
	struct window_pane	*wp;
	char			*cwd, dir[PATH_MAX];

	if (w == NULL || (wp = w->active) == NULL)
		return;
	cwd = osdep_get_cwd(wp->fd);
	if (cwd == NULL || *cwd != '/')
		cwd = wp->cwd;
	if (cwd == NULL || *cwd != '/')
		return;

	/*
	 * "Reprendre" always shows the conversations of the directory the
	 * console is in RIGHT NOW: a `cd` is followed at once, without waiting
	 * for the periodic sweep - the user changed directory and looks at the
	 * list immediately.
	 */
	gettimeofday(&tv, NULL);
	if (claude_project_dir(cwd, dir, sizeof dir) &&
	    (claude_sess_dir == NULL || strcmp(claude_sess_dir, dir) != 0)) {
		claude_sess_scanned = 0;
		last = 0;
	}
	if (tv.tv_sec - last < CLAUDE_SESS_RESCAN)
		return;
	last = tv.tv_sec;

	claude_sess_scan(cwd);
	claude_sess_hide_running(w);
	/*
	 * Self-healing: the mail watcher is armed when a manager is created,
	 * and a manager rebuilt another way (hot upgrade, crash restore) would
	 * otherwise sit there with nobody polling the bus. This costs nothing
	 * when it is already running.
	 */
	claude_bus_start();
}

/*
 * CLAUDE: "[+ Nouvelle]" - open a new conversation (a shell) in the current
 * conversation's working directory, no directory chooser. Splitting unzooms
 * the window, so zoom back on the new pane: the others keep running, hidden.
 */
void
claude_new_shell(struct client *c)
{
	struct session		*s = c->session;
	struct window		*w;
	struct window_pane	*wp;
	struct cmdq_state	*state;
	char			*cmd, *error, *cwd, q[PATH_MAX + 16];
	char			 envopt[2560] = "", agent[128];

	if (s == NULL || s->curw == NULL)
		return;
	w = s->curw->window;
	if ((wp = w->active) == NULL)
		return;

	/* The pane's LIVE directory, not the one it was started in. */
	cwd = osdep_get_cwd(wp->fd);
	if (cwd == NULL || *cwd != '/')
		cwd = wp->cwd;
	if (cwd == NULL || *cwd != '/')
		cwd = (char *)server_client_get_cwd(c, s);

	/*
	 * Identity on the agent bus + the wrapper directory in PATH, so that
	 * simply typing `claude` in this conversation gets the bus tools with
	 * this conversation's own name.
	 */
	claude_env_opts(c, w, cwd, envopt, sizeof envopt, agent, sizeof agent);

	/*
	 * `select-layout` unzooms and rebalances, so the split always happens
	 * on the real (untiled) layout with room to spare - splitting the
	 * zoomed single cell instead halves the space every time and soon
	 * fails with "No space for new pane". No `resize-pane -Z` here: it
	 * TOGGLES, and rapid clicks would race; server_client_loop() restores
	 * the zoom idempotently.
	 */
	if (cwd != NULL && *cwd == '/') {
		form_quote(q, sizeof q, cwd);
		xasprintf(&cmd, "select-layout -t %%%u tiled ; "
		    "split-window -h -t %%%u%s -c %s ; "
		    "select-layout -t %%%u tiled", wp->id, wp->id, envopt, q,
		    wp->id);
	} else {
		xasprintf(&cmd, "select-layout -t %%%u tiled ; "
		    "split-window -h -t %%%u%s ; select-layout -t %%%u tiled",
		    wp->id, wp->id, envopt, wp->id);
	}
	state = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, c, state, &error) ==
	    CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);
	free(cmd);
}

/*
 * ==========================================================================
 * CLAUDE : bus d'agents (claude-agent-server). Chaque conversation reçoit un
 * nom d'agent (AGENT_NAME) à sa création et est enregistrée sur le bus ; les
 * agents s'écrivent entre eux avec les outils existants (MCP send_message /
 * check_inbox, ou curl). tmuxv interroge périodiquement /agents et marque
 * d'un ✉ les conversations dont le compteur « pending » a augmenté.
 * ==========================================================================
 */

const char *
claude_bus_url(void)
{
	static char	 own[64];
	const char	*url;

	/* Our own bus, when this server runs one. */
	if (bus_port() != 0) {
		snprintf(own, sizeof own, "http://127.0.0.1:%d", bus_port());
		return (own);
	}
	url = getenv("AGENT_BUS_URL");
	if (url == NULL || *url == '\0')
		url = "http://localhost:4319";
	return (url);
}

/*
 * The bus changed store (the database came back and took the messages written
 * meanwhile, under new ids): every delivery cursor starts over with a first
 * fetch - which delivers what is still unread, and nothing already read.
 */
void
claude_bus_resync(void)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		wp->claude_cursor_ok = 0;
		wp->claude_pending = 0;
		wp->claude_seen = 0;
	}
}

/*
 * The agent name lives in the conversation's own environment (AGENT_NAME, set
 * when it was spawned), so read it back from the child rather than keeping a
 * second bookkeeping of our own. Cached on the pane.
 */
const char *
claude_agent_name(struct window_pane *wp)
{
	char	 path[64], buf[4096];
	ssize_t	 n;
	int	 fd;
	char	*p, *end;

	if (wp->claude_agent != NULL)
		return (wp->claude_agent);
	if (wp->pid <= 0)
		return (NULL);
	if ((size_t)snprintf(path, sizeof path, "/proc/%ld/environ",
	    (long)wp->pid) >= sizeof path)
		return (NULL);
	if ((fd = open(path, O_RDONLY)) == -1)
		return (NULL);
	n = read(fd, buf, (sizeof buf) - 1);
	close(fd);
	if (n <= 0)
		return (NULL);
	buf[n] = '\0';
	for (p = buf; p < buf + n; p += strlen(p) + 1) {
		if (strncmp(p, "AGENT_NAME=", 11) != 0)
			continue;
		end = p + 11;
		if (*end == '\0')
			break;
		wp->claude_agent = xstrdup(end);
		return (wp->claude_agent);
	}
	return (NULL);
}

/* This conversation is being shown: its mail is no longer new. */
void
claude_mark_read(struct window_pane *wp)
{
	wp->claude_unread = 0;
}

/*
 * Result of "GET /agents": {"agents":[{"last_seen":N,"name":"x","pending":N}]}.
 * Flag every conversation whose pending count grew since the last poll (agents
 * rarely ack, so an absolute count would light up for ever).
 */
static void
claude_bus_parse(const char *json)
{
	struct window		*w;
	struct window_pane	*wp;
	const char		*p, *q, *r;
	char			 name[256];
	size_t			 len;
	u_int			 pending;

	for (p = json; (p = strstr(p, "\"name\":\"")) != NULL; ) {
		p += 8;
		if ((q = strchr(p, '"')) == NULL)
			break;
		len = (size_t)(q - p);
		if (len >= sizeof name)
			len = sizeof name - 1;
		memcpy(name, p, len);
		name[len] = '\0';
		if ((r = strstr(q, "\"pending\":")) == NULL)
			break;
		pending = (u_int)strtoul(r + 10, NULL, 10);
		p = r + 10;

		RB_FOREACH(w, windows, &windows) {
			if (!claude_manager(w))
				continue;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				const char *n = claude_agent_name(wp);

				if (n == NULL || strcmp(n, name) != 0)
					continue;
				log_debug("%s: %s pending=%u (was %u seen=%d)",
				    __func__, name, pending, wp->claude_pending,
				    wp->claude_seen);
				/*
				 * Only a GROWTH means new mail. The first
				 * sample just records the level: a fresh agent
				 * inherits the whole broadcast backlog, which
				 * would otherwise light up every conversation.
				 */
				if (wp->claude_seen &&
				    pending > wp->claude_pending &&
				    wp != w->active)
					wp->claude_unread = 1;
				wp->claude_pending = pending;
				wp->claude_seen = 1;
				if (wp == w->active)
					wp->claude_unread = 0;
				server_redraw_window(w);
			}
		}
	}
}

static void
claude_bus_complete(struct job *job)
{
	struct evbuffer	*evb = job_get_event(job)->input;
	size_t		 len = EVBUFFER_LENGTH(evb);
	char		*buf;

	log_debug("%s: %zu bytes from the agent bus", __func__, len);
	if (len == 0)
		return;
	buf = xmalloc(len + 1);
	memcpy(buf, EVBUFFER_DATA(evb), len);
	buf[len] = '\0';
	claude_bus_parse(buf);
	free(buf);
}

/*
 * Ask the bus who has mail. Called from the server loop; does nothing unless a
 * manager window exists, and never more often than every few seconds.
 */
/*
 * Make `claude` in a manager conversation talk to the bus automatically, with
 * THAT conversation's identity, without writing anything into the user's
 * projects: tmuxv keeps its own MCP declaration in ~/.tmuxv/mcp.json and a
 * small `claude` wrapper in ~/.tmuxv/bin, and puts that directory first in the
 * conversation's PATH. The wrapper adds --mcp-config, which is ADDITIVE, so
 * the user's own MCP servers are untouched; the server is named "tmuxv-bus" so
 * it cannot clash with an existing "agent-bus" entry.
 */
static const char *
claude_mcp_binary(void)
{
	static const char	*paths[] = {
		"/home/martinien/claude-agent-server/target/release/claude-agent-mcp",
		"/usr/local/bin/claude-agent-mcp",
		NULL
	};
	const char		*env = getenv("AGENT_MCP_BIN");
	u_int			 i;

	if (env != NULL && *env != '\0' && access(env, X_OK) == 0)
		return (env);
	for (i = 0; paths[i] != NULL; i++) {
		if (access(paths[i], X_OK) == 0)
			return (paths[i]);
	}
	return (NULL);
}

/*
 * Declare the bus to Claude Code ONCE, at user scope, through its own CLI.
 * Deliberately not a PATH wrapper: the user's ~/.profile puts ~/.local/bin
 * first, so a wrapper directory would always be shadowed. Registering at user
 * scope works whatever the PATH, and the identity stays per conversation
 * because AGENT_NAME is left out of the declaration - each agent inherits the
 * one tmuxv put in its conversation. Undo with:
 *     claude mcp remove --scope user tmuxv-bus
 */
static void
claude_register_mcp(void)
{
	static int	 done;
	const char	*mcp;
	char		*cmd;

	if (done)
		return;
	done = 1;
	if ((mcp = claude_mcp_binary()) == NULL)
		return;
	/* The name comes FIRST, then the options, then -- and the command. */
	xasprintf(&cmd,
	    "claude mcp get tmuxv-bus >/dev/null 2>&1 || "
	    "claude mcp add tmuxv-bus -e AGENT_BUS_URL=%s --scope user -- %s "
	    ">/dev/null 2>&1", claude_bus_url(), mcp);
	job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    JOB_NOWAIT, -1, -1);
	free(cmd);
}

/*
 * Build the "-e AGENT_NAME=... -e PATH=..." options for a new conversation and
 * announce it on the bus. `agent` receives the chosen name.
 */
static void
claude_env_opts(struct client *c, struct window *w, const char *cwd, char *out,
    size_t outlen, char *agent, size_t agentlen)
{
	struct cmdq_state	*state;
	const char		*base;
	char			*reg, *error;
	char			 grp[128];
	size_t			 i;

	*out = '\0';
	base = (cwd != NULL) ? strrchr(cwd, '/') : NULL;
	if (base != NULL && base[1] != '\0')
		base++;
	else
		base = "conv";
	snprintf(agent, agentlen, "%s-%u", base, ++w->claude_seq);
	for (i = 0; agent[i] != '\0'; i++) {	/* keep it addressable */
		if (!isalnum((u_char)agent[i]) && agent[i] != '-' &&
		    agent[i] != '_')
			agent[i] = '-';
	}
	/* Le groupe = le projet, partage par les conversations d'un meme dossier. */
	strlcpy(grp, base, sizeof grp);
	for (i = 0; grp[i] != '\0'; i++) {
		if (!isalnum((u_char)grp[i]) && grp[i] != '-' && grp[i] != '_')
			grp[i] = '-';
	}
	snprintf(out, outlen, " -e AGENT_NAME=%s", agent);
	claude_register_mcp();

	/*
	 * job_run(), never `run-shell`: run-shell shows its output IN THE
	 * ACTIVE PANE (view mode) - the user then sees the curl command line
	 * and a "[0/0]" indicator instead of a clean shell prompt.
	 */
	/*
	 * Le GROUPE est le projet (le dernier element du repertoire) : une
	 * diffusion `*` reste alors entre les conversations du meme projet au
	 * lieu d'arroser tous les agents du bus. `managed` dit au bus que
	 * tmuxv livre lui-meme : l'agent n'a pas a relever sa boite, et ne
	 * recevra donc pas deux fois le meme message.
	 */
	xasprintf(&reg, "curl -s --max-time 2 -X POST %s/register "
	    "-d '{\"name\":\"%s\",\"group\":\"%s\",\"managed\":true}' "
	    ">/dev/null 2>&1", claude_bus_url(), agent, grp);
	job_run(reg, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    JOB_NOWAIT, -1, -1);
	free(reg);
}

/* Is this conversation sitting at a shell prompt (no agent running)? */
static int
claude_pane_is_shell(struct window_pane *wp)
{
	char		*cur;
	const char	*b;
	int		 shell = 0;

	cur = osdep_get_name(wp->fd, wp->tty);
	if (cur != NULL && *cur != '\0') {
		b = strrchr(cur, '/');
		b = (b != NULL) ? b + 1 : cur;
		if (*b == '-')			/* login shell: "-bash" */
			b++;
		shell = (strcmp(b, "bash") == 0 || strcmp(b, "zsh") == 0 ||
		    strcmp(b, "sh") == 0 || strcmp(b, "fish") == 0 ||
		    strcmp(b, "dash") == 0 || strcmp(b, "ksh") == 0);
	}
	free(cur);
	return (shell);
}

/*
 * Is a Claude agent running here? Only then may tmuxv type into the pane:
 * doing it at a shell would EXECUTE the text, and in an editor it would edit.
 * Claude Code shows up as "claude".
 */
static int
claude_pane_is_agent(struct window_pane *wp)
{
	char		*cur;
	const char	*b;
	int		 agent = 0;

	cur = osdep_get_name(wp->fd, wp->tty);
	if (cur != NULL && *cur != '\0') {
		b = strrchr(cur, '/');
		b = (b != NULL) ? b + 1 : cur;
		agent = (strcmp(b, "claude") == 0);
	}
	free(cur);
	return (agent);
}

/* Single-quote for the tmux parser (no format expansion inside). */
static void
claude_squote(char *dst, size_t dstsize, const char *s)
{
	size_t	di = 0;

	if (dstsize < 3)
		return;
	dst[di++] = '\'';
	for (; *s != '\0' && di + 6 < dstsize; s++) {
		if (*s == '\'') {		/* ' -> '\'' */
			dst[di++] = '\''; dst[di++] = '\\';
			dst[di++] = '\''; dst[di++] = '\'';
		} else
			dst[di++] = *s;
	}
	dst[di++] = '\'';
	dst[di] = '\0';
}

/* Copy a JSON string body, decoding escapes and flattening newlines. */
static void
claude_json_unescape(char *dst, size_t dstsize, const char *s, const char *end)
{
	size_t	di = 0;
	u_int	cp;

	while (s < end && di + 5 < dstsize) {
		if (*s != '\\') {
			dst[di++] = *s++;
			continue;
		}
		s++;
		if (s >= end)
			break;
		switch (*s) {
		case 'n': case 'r': case 't':
			dst[di++] = ' ';
			s++;
			break;
		case 'u':
			if (end - s < 5) {
				s = end;
				break;
			}
			cp = (u_int)strtoul((char[]){ s[1], s[2], s[3], s[4],
			    '\0' }, NULL, 16);
			s += 5;
			if (cp < 0x80)
				dst[di++] = (char)cp;
			else if (cp < 0x800) {
				dst[di++] = (char)(0xc0 | (cp >> 6));
				dst[di++] = (char)(0x80 | (cp & 0x3f));
			} else {
				dst[di++] = (char)(0xe0 | (cp >> 12));
				dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3f));
				dst[di++] = (char)(0x80 | (cp & 0x3f));
			}
			break;
		default:
			dst[di++] = *s++;
			break;
		}
	}
	dst[di] = '\0';
}

struct claude_fetch {
	u_int	wp_id;
};

/*
 * Is the agent actually waiting at its input prompt? Claude Code shows other
 * screens first (the "do you trust this folder?" question, its startup
 * banner), and typing then would answer THOSE instead. Look for its prompt
 * marker on the last lines: "> " or "❯".
 */
/*
 * A choice menu - a permission request ("Do you want to proceed?" / "❯ 1.
 * Yes"), the trust screen, a login: Enter there ANSWERS it. Typing a message
 * into one would say "yes" in the user's place.
 */
static int
claude_line_is_menu(const char *line)
{
	static const char	*words[] = { "Do you want to", "Esc to cancel",
				    "Enter to confirm", "Esc to exit",
				    "trust the files", "don't ask again", NULL };
	const char		*p, *q;
	u_int			 i;

	for (i = 0; words[i] != NULL; i++) {
		if (strstr(line, words[i]) != NULL)
			return (1);
	}
	for (p = line; (p = strstr(p, "\342\235\257")) != NULL; p += 3) {
		q = p + 3;
		while (*q == ' ')
			q++;
		if (isdigit((u_char)*q) && q[1] == '.')
			return (1);		/* "❯ 1." */
	}
	return (0);
}

static int
claude_pane_ready(struct window_pane *wp)
{
	/*
	 * The CURRENT screen: Claude Code is a full-screen TUI and draws on
	 * the alternate screen, so wp->base still holds the old shell content
	 * and would never show its prompt.
	 */
	struct screen		*s = wp->screen;
	struct grid		*gd = s->grid;
	struct grid_cell	 gc;
	u_int			 sx = screen_size_x(s);
	u_int			 sy = screen_size_y(s);
	u_int			 x, y;
	char			 line[2048];
	const char		*p;
	size_t			 n;
	int			 menu_below = 0;

	/*
	 * From the BOTTOM up, the first line carrying a prompt is the live
	 * one (those above are the history). Where it sits does not matter: a
	 * short or freshly cleared conversation draws it near the top. If it is
	 * the cursor of a choice menu ("❯ 1. Yes"), or a menu shows below it,
	 * the agent is answering a question: never type into that.
	 */
	for (y = sy; y-- > 0; /* nothing */) {
		n = 0;
		for (x = 0; x < sx; x++) {
			grid_get_cell(gd, x, gd->hsize + y, &gc);
			if (gc.flags & GRID_FLAG_PADDING)
				continue;
			if (gc.data.size != 0 && n + gc.data.size < sizeof line) {
				memcpy(line + n, gc.data.data, gc.data.size);
				n += gc.data.size;
			}
		}
		line[n] = '\0';
		if (claude_line_is_menu(line)) {
			if (strstr(line, "\342\235\257") != NULL)
				return (0);		/* "❯ 1." */
			menu_below = 1;
			continue;
		}
		p = line;
		while (*p == ' ' || strncmp(p, "\342\224\202", 3) == 0)
			p += (*p == ' ') ? 1 : 3;	/* spaces, "│" */
		if (strstr(line, "\342\235\257") != NULL ||	/* ❯ */
		    strncmp(p, "> ", 2) == 0)
			return (!menu_below);
	}
	return (0);
}

struct claude_enter {
	struct event	 ev;
	u_int		 wp_id;
};

/*
 * Send the Enter that validates an injected line, a moment later: Claude Code
 * reads the text and an immediately following CR as ONE burst and keeps the
 * CR as a line break inside its prompt, leaving the message typed but never
 * sent. A short pause makes it a real submission.
 */
static void
claude_enter_cb(__unused int fd, __unused short events, void *arg)
{
	struct claude_enter	*ce = arg;
	struct window_pane	*wp;
	struct cmdq_state	*st;
	char			*cmd, *error;

	if ((wp = window_pane_find_by_id(ce->wp_id)) != NULL) {
		xasprintf(&cmd, "send-keys -t %%%u Enter", wp->id);
		st = cmdq_new_state(NULL, NULL, 0);
		if (cmd_parse_and_append(cmd, NULL, NULL, st, &error) ==
		    CMD_PARSE_ERROR)
			free(error);
		cmdq_free_state(st);
		free(cmd);
	}
	evtimer_del(&ce->ev);
	free(ce);
}

/* Type one line into a conversation and validate it (agents only). */
static void
claude_inject(struct window_pane *wp, const char *line)
{
	struct cmdq_state	*st;
	struct claude_enter	*ce;
	struct timeval		 tv = { 0, 600000 };
	char			*cmd, *error;
	char			 quoted[4900];

	claude_squote(quoted, sizeof quoted, line);
	xasprintf(&cmd, "send-keys -t %%%u -l -- %s", wp->id, quoted);
	st = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, NULL, st, &error) ==
	    CMD_PARSE_ERROR) {
		log_debug("%s: %s", __func__, error);
		free(error);
	}
	cmdq_free_state(st);
	free(cmd);

	ce = xmalloc(sizeof *ce);
	ce->wp_id = wp->id;
	evtimer_set(&ce->ev, claude_enter_cb, ce);
	evtimer_add(&ce->ev, &tv);
}

/*
 * A message may carry files: send_message puts a [piece:ID] line for each.
 * The agent is given the path of the file on this machine instead, which it
 * can Read as is (images included); a piece not here yet is left to the
 * get_attachment tool.
 */
static void
claude_pieces_local(char *body, size_t size)
{
	char		 out[4096], id[48];
	const char	*p = body, *e, *path;
	size_t		 o = 0, n;

	if (strstr(body, "[piece:") == NULL)
		return;
	while (*p != '\0' && o < sizeof out - 1) {
		if (strncmp(p, "[piece:", 7) == 0 &&
		    (e = strchr(p + 7, ']')) != NULL &&
		    (n = (size_t)(e - (p + 7))) != 0 && n < sizeof id) {
			memcpy(id, p + 7, n);
			id[n] = '\0';
			if ((path = bus_piece_local(id)) != NULL) {
				o += snprintf(out + o, sizeof out - o,
				    "[fichier joint : %s]", path);
			} else {
				o += snprintf(out + o, sizeof out - o,
				    "[fichier joint %s : ouvre-le avec l'outil "
				    "get_attachment]", id);
			}
			if (o >= sizeof out)
				o = sizeof out - 1;
			p = e + 1;
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
	strlcpy(body, out, size);
}

/*
 * Inbox answer for one conversation: hand every new message to the agent
 * running there by typing it in, then acknowledge it on the bus. This is what
 * makes reception automatic and continuous - no polling recipe for the agent
 * to run, nothing for the user to launch.
 */
static void
claude_inbox_complete(struct job *job)
{
	struct claude_fetch	*cf = job_get_data(job);
	struct evbuffer		*evb = job_get_event(job)->input;
	struct window_pane	*wp;
	size_t			 len = EVBUFFER_LENGTH(evb);
	char			*buf, *cmd, *p, *q;
	char			 body[2048], from[128], line[2400];
	u_int			 cursor = 0, id, last = 0;
	int			 delivered = 0;

	if ((wp = window_pane_find_by_id(cf->wp_id)) == NULL)
		return;
	wp->claude_fetching = 0;
	if (len == 0)
		return;
	buf = xmalloc(len + 1);
	memcpy(buf, EVBUFFER_DATA(evb), len);
	buf[len] = '\0';

	if ((p = strstr(buf, "\"cursor\":")) != NULL)
		cursor = (u_int)strtoul(p + 9, NULL, 10);

	/*
	 * The first answer carries only what was never read (unread=1), so it
	 * is delivered like any other: the history already read is never
	 * replayed, but mail that waited while nobody was delivering - after a
	 * restart, a rename - is no longer silently skipped.
	 */
	if (!wp->claude_cursor_ok) {
		wp->claude_cursor = cursor;
		wp->claude_cursor_ok = 1;
	}

	for (p = buf; (p = strstr(p, "\"body\":\"")) != NULL; ) {
		p += 8;
		for (q = p; *q != '\0'; q++) {	/* end of the JSON string */
			if (*q == '\\' && q[1] != '\0') {
				q++;
				continue;
			}
			if (*q == '"')
				break;
		}
		if (*q == '\0')
			break;
		claude_json_unescape(body, sizeof body, p, q);
		claude_pieces_local(body, sizeof body);
		p = q + 1;

		*from = '\0';
		if ((q = strstr(p, "\"from\":\"")) != NULL) {
			q += 8;
			claude_json_unescape(from, sizeof from, q,
			    strchr(q, '"') ? strchr(q, '"') : q);
		}
		id = 0;
		if ((q = strstr(p, "\"id\":")) != NULL)
			id = (u_int)strtoul(q + 5, NULL, 10);
		if (id > last)
			last = id;

		snprintf(line, sizeof line, "[bus] message de %s : %s",
		    (*from != '\0') ? from : "?", body);
		claude_inject(wp, line);
		delivered++;
	}

	if (cursor > wp->claude_cursor)
		wp->claude_cursor = cursor;
	if (delivered != 0) {
		const char	*agent = claude_agent_name(wp);

		log_debug("%s: %%%u delivered %d message(s)", __func__, wp->id,
		    delivered);
		if (agent != NULL && last != 0) {
			xasprintf(&cmd, "curl -s --max-time 2 -X POST %s/ack "
			    "-d '{\"agent\":\"%s\",\"upto\":%u}' >/dev/null",
			    claude_bus_url(), agent, last);
			job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL,
			    NULL, NULL, JOB_NOWAIT, -1, -1);
			free(cmd);
		}
		wp->claude_unread = 0;
		wp->claude_pending = 0;
	}
	free(buf);
}

static void
claude_inbox_free(void *data)
{
	free(data);
}

/*
 * Fetch the inbox of every conversation that currently runs an agent (not a
 * shell: typing into a shell would execute the text).
 */
static void
claude_deliver(void)
{
	struct window		*w;
	struct window_pane	*wp;
	struct claude_fetch	*cf;
	const char		*agent;
	char			*cmd, line[1400];

	RB_FOREACH(w, windows, &windows) {
		char	roster[1024] = "";
		int	changed;

		if (!claude_manager(w) || w->references == 0)
			continue;

		/* Who is present, so every agent can address the others. */
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if ((agent = claude_agent_name(wp)) == NULL)
				continue;
			if (*roster != '\0')
				strlcat(roster, ", ", sizeof roster);
			strlcat(roster, agent, sizeof roster);
		}
		changed = (w->claude_roster == NULL ||
		    strcmp(w->claude_roster, roster) != 0);
		if (changed) {
			free(w->claude_roster);
			w->claude_roster = xstrdup(roster);
		}

		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->fd == -1)
				continue;
			if ((agent = claude_agent_name(wp)) == NULL)
				continue;
			if (!claude_pane_is_agent(wp)) {
				/* Shell or another program: keep the ✉. */
				wp->claude_cursor_ok = 0;
				wp->claude_briefed = 0;
				continue;
			}
			if (!claude_pane_ready(wp)) {
				/*
				 * Busy or on a startup screen: leave it alone,
				 * the mail stays pending (and marked ✉) and we
				 * try again on the next tick.
				 */
				log_debug("%s: %%%u not at its prompt yet",
				    __func__, wp->id);
				continue;
			}
			/*
			 * NO automatic announcement: an agent used to be told
			 * who it was and who else was there as soon as it
			 * started. The user asked to trigger that themselves,
			 * with a chance to edit the wording - see the
			 * "Annoncer" entry of the conversation menu and the
			 * claude-announce command. Incoming mail is still
			 * delivered on its own.
			 */
			if (wp->claude_fetching)
				continue;
			/*
			 * Only ask for the mailbox when there IS mail. /agents,
			 * polled on the same tick, already says how much is
			 * waiting for each agent - so an idle manager costs one
			 * curl every three seconds instead of one PER
			 * CONVERSATION. The first fetch still happens
			 * unconditionally: it is what sets the cursor to the
			 * current end, so the backlog is never replayed.
			 */
			if (wp->claude_cursor_ok && wp->claude_pending == 0)
				continue;
			cf = xmalloc(sizeof *cf);
			cf->wp_id = wp->id;
			wp->claude_fetching = 1;
			/*
			 * First fetch: what has never been read (mail that
			 * arrived while nobody was delivering - a restart, a
			 * rename) plus the real end of the mailbox as cursor.
			 * After that, only what is newer than the cursor.
			 */
			if (!wp->claude_cursor_ok) {
				xasprintf(&cmd, "curl -s --max-time 3 "
				    "'%s/inbox?agent=%s&since=0&unread=1'",
				    claude_bus_url(), agent);
			} else {
				xasprintf(&cmd, "curl -s --max-time 3 "
				    "'%s/inbox?agent=%s&since=%u'",
				    claude_bus_url(), agent, wp->claude_cursor);
			}
			job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL,
			    claude_inbox_complete, claude_inbox_free, cf,
			    JOB_NOWAIT, -1, -1);
			free(cmd);
		}
	}
}

static struct event	 claude_bus_ev;
static int		 claude_bus_ev_on;

/* Is there still a manager window to poll for? */
static int
claude_bus_wanted(void)
{
	struct window	*w;

	RB_FOREACH(w, windows, &windows) {
		if (claude_manager(w) && w->references != 0)
			return (1);
	}
	return (0);
}

/*
 * The server loop only runs when something happens, so an idle desktop would
 * never notice new mail. Drive the poll from a timer instead, re-armed while a
 * manager window exists.
 */
/*
 * ==========================================================================
 * MEMORY: what each conversation costs, and a warning when the machine runs
 * short. One Claude Code process weighs 400-500 MB on its own; with a dozen
 * conversations the AGENTS saturate the machine, not tmuxv. Showing the figure
 * next to each conversation is what lets the user close the right one.
 * ==========================================================================
 */

/* RSS of one process, in KB (0 if unknown). */
static u_int
claude_proc_rss(pid_t pid)
{
	char	path[64], line[128];
	FILE	*f;
	u_int	kb = 0;

	snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
	if ((f = fopen(path, "r")) == NULL)
		return (0);
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			kb = (u_int)atoi(line + 6);
			break;
		}
	}
	fclose(f);
	return (kb);
}

/* The pane's foreground process plus its direct children (MCP servers...). */
static u_int
claude_pane_mem(struct window_pane *wp)
{
	pid_t	pgrp;
	char	path[80], buf[1024], *p, *end;
	FILE	*f;
	u_int	kb;
	long	child;

	if (wp->fd == -1 || (pgrp = tcgetpgrp(wp->fd)) == -1)
		return (0);
	kb = claude_proc_rss(pgrp);
	snprintf(path, sizeof path, "/proc/%ld/task/%ld/children", (long)pgrp,
	    (long)pgrp);
	if ((f = fopen(path, "r")) != NULL) {
		if (fgets(buf, sizeof buf, f) != NULL) {
			p = buf;
			while ((child = strtol(p, &end, 10)) > 0 && end != p) {
				kb += claude_proc_rss((pid_t)child);
				p = end;
			}
		}
		fclose(f);
	}
	return (kb);
}

/* Machine-wide MemAvailable, in MB (0 if unknown). */
static u_int
claude_mem_available(void)
{
	char	line[128];
	FILE	*f;
	u_int	mb = 0;

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

u_int
claude_mem_total(struct window *w)
{
	struct window_pane	*wp;
	u_int			 kb = 0;

	TAILQ_FOREACH(wp, &w->panes, entry)
		kb += wp->claude_mem;
	return (kb);
}

/*
 * Sample every manager conversation (on the 3 s bus tick, so a handful of
 * /proc reads at most), and raise the warning ONCE when the machine drops
 * under @memory-warn MB available - repeated at most every minute.
 */
void
claude_mem_sample(void)
{
	static time_t		 warned;
	struct window		*w;
	struct window_pane	*wp;
	struct client		*c;
	struct timeval		 tv;
	const char		*v;
	u_int			 avail, warn = 512, mb, changed;
	int			 low;

	if ((v = options_get_string(global_s_options, "@memory-warn")) != NULL &&
	    *v != '\0')
		warn = (u_int)atoi(v);
	avail = claude_mem_available();
	low = (warn != 0 && avail != 0 && avail < warn);
	gettimeofday(&tv, NULL);

	RB_FOREACH(w, windows, &windows) {
		if (!claude_manager(w) || w->references == 0)
			continue;
		changed = 0;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			mb = claude_pane_mem(wp);
			if (mb / 1024 != wp->claude_mem / 1024)
				changed = 1;
			wp->claude_mem = mb;
		}
		if (w->claude_lowmem != low) {
			w->claude_lowmem = low;
			changed = 1;
		}
		if (changed)
			server_redraw_window(w);
		/*
		 * The warning is thrown at whoever is LOOKING at the manager,
		 * and the once-a-minute limit only starts counting once it has
		 * actually been shown: a manager window nobody is watching (a
		 * second session, a restored one) used to eat the limit and the
		 * warning never reached the screen.
		 */
		if (low && tv.tv_sec - warned >= 60) {
			TAILQ_FOREACH(c, &clients, entry) {
				if (c->session == NULL ||
				    c->session->curw == NULL ||
				    c->session->curw->window != w)
					continue;
				warned = tv.tv_sec;
				/*
				 * delay 0: the warning STAYS until the user
				 * presses a key - display-time (0.75 s by
				 * default) would blink it away before anyone
				 * reads it.
				 */
				status_message_set(c, 0, 1, 0,
				    "Memoire faible : %u Mo disponibles - le "
				    "gestionnaire en utilise %u, fermez une "
				    "conversation", avail,
				    claude_mem_total(w) / 1024);
			}
		}
	}
}

static void
claude_bus_timer(__unused int fd, __unused short events, __unused void *arg)
{
	struct timeval	tv = { 3, 0 };

	claude_mem_sample();	/* MEMORY: per-conversation figures + warning */
	claude_bus_poll();	/* who has mail waiting (the ✉) */
	claude_deliver();	/* hand it to the agents, automatically */
	if (claude_bus_wanted())
		evtimer_add(&claude_bus_ev, &tv);
	else
		claude_bus_ev_on = 0;
}

void
claude_bus_start(void)
{
	struct timeval	tv = { 3, 0 };

	if (claude_bus_ev_on)
		return;
	/* Nobody to watch for: the timer would stop again on its first tick. */
	if (!claude_bus_wanted())
		return;
	if (event_initialized(&claude_bus_ev))
		evtimer_del(&claude_bus_ev);
	evtimer_set(&claude_bus_ev, claude_bus_timer, NULL);
	evtimer_add(&claude_bus_ev, &tv);
	claude_bus_ev_on = 1;
}

/*
 * The agents this tmuxv hosts are present even when idle: say so to the bus
 * now and then, or after an hour of silence they vanish from list_agents and
 * the others stop writing to them.
 */
#define CLAUDE_KEEPALIVE	600

static void	claude_json_escape(char *, size_t, const char *);

static void
claude_bus_keepalive(void)
{
	struct window		*w;
	struct window_pane	*wp;
	struct evbuffer		*eb = evbuffer_new();
	const char		*agent;
	char			 name[256], json[400], q[900], *cmd;
	u_int			 n = 0;

	RB_FOREACH(w, windows, &windows) {
		if (!claude_manager(w) || w->references == 0)
			continue;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if ((agent = claude_agent_name(wp)) == NULL ||
			    !claude_pane_is_agent(wp))
				continue;
			claude_json_escape(name, sizeof name, agent);
			snprintf(json, sizeof json,
			    "{\"name\":\"%s\",\"managed\":true}", name);
			claude_squote(q, sizeof q, json);
			evbuffer_add_printf(eb, "%scurl -s --max-time 3 -X POST "
			    "%s/register -d %s >/dev/null", n++ ? "; " : "",
			    claude_bus_url(), q);
		}
	}
	if (n != 0) {
		cmd = xmalloc(EVBUFFER_LENGTH(eb) + 1);
		memcpy(cmd, EVBUFFER_DATA(eb), EVBUFFER_LENGTH(eb));
		cmd[EVBUFFER_LENGTH(eb)] = '\0';
		job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		    JOB_NOWAIT, -1, -1);
		free(cmd);
	}
	evbuffer_free(eb);
}

void
claude_bus_poll(void)
{
	static time_t	 last, kept;
	struct window	*w;
	struct client	*c = NULL, *loop;
	time_t		 now = time(NULL);
	char		*cmd;
	int		 any = 0;

	if (now - last < 3)
		return;
	RB_FOREACH(w, windows, &windows) {
		if (claude_manager(w) && w->references != 0) {
			any = 1;
			break;
		}
	}
	if (!any)
		return;
	last = now;
	TAILQ_FOREACH(loop, &clients, entry) {
		if (loop->session != NULL) {
			c = loop;
			break;
		}
	}
	/*
	 * all=1: the pending count of EVERY agent, also those idle for over
	 * an hour, which /agents leaves out - an agent asleep for an hour no
	 * longer had its mail counted, so it was never fetched for it.
	 */
	xasprintf(&cmd, "curl -s --max-time 2 '%s/agents?all=1'",
	    claude_bus_url());
	job_run(cmd, 0, NULL, NULL, NULL, server_client_get_cwd(c, NULL), NULL,
	    claude_bus_complete, NULL, NULL, JOB_NOWAIT, -1, -1);
	free(cmd);
	if (now - kept >= CLAUDE_KEEPALIVE) {
		kept = now;
		claude_bus_keepalive();
	}
}

/* Escape a string for a JSON body. */
static void
claude_json_escape(char *dst, size_t dstsize, const char *s)
{
	size_t	di = 0;

	for (; *s != '\0' && di + 7 < dstsize; s++) {
		if (*s == '"' || *s == '\\') {
			dst[di++] = '\\';
			dst[di++] = *s;
		} else if ((u_char)*s < 0x20)
			dst[di++] = ' ';
		else
			dst[di++] = *s;
	}
	dst[di] = '\0';
}

/*
 * CLAUDE: right-click on a conversation - a Turbo Vision drop-down acting on
 * that conversation (like Claude Desktop's context menu). Every entry is a
 * plain tmux command targeting the pane, so they all go through the normal
 * queue (and the rename/close prompts appear in the TVision dialogs).
 */
void
claude_row_menu(struct client *c, struct window *w, u_int row, u_int px,
    u_int py)
{
	struct window_pane	*wp, *target = NULL;
	struct menu		*menu;
	struct menu_item	 it;
	struct cmd_find_state	 fs;
	u_int			 i = 0;
	char			*cwd, *open, *rename, *newhere, *copy, *close;
	char			*send = NULL, *copyname = NULL, *announce = NULL;
	char			*select, *range;
	const char		*agent;
	char			 qd[PATH_MAX + 16], qt[512], title[256];
	char			 fromj[128], toj[128];

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (i++ == row) {
			target = wp;
			break;
		}
	}
	if (target == NULL)
		return;

	cwd = osdep_get_cwd(target->fd);
	if (cwd == NULL || *cwd != '/')
		cwd = target->cwd;
	if (cwd == NULL || *cwd != '/')
		cwd = (char *)"";
	form_quote(qd, sizeof qd, cwd);
	strlcpy(title, (target->base.title != NULL) ? target->base.title : "",
	    sizeof title);
	form_quote(qt, sizeof qt, title);

	xasprintf(&open, "select-pane -t %%%u", target->id);
	/*
	 * The pane id must NOT appear inside a command-prompt template: %1..%9
	 * are the template's own placeholders (cmd_template_replace), so "-t %1"
	 * came back as "-t <what the user typed>" - and there is no way to
	 * escape a literal %1. The pane is selected BEFORE the prompt instead,
	 * and the command then targets the current pane.
	 */
	xasprintf(&rename, "select-pane -t %%%u ; command-prompt -I %s "
	    "-p \"Nom de la conversation\" { claude-rename \"%%%%\" }",
	    target->id, qt);
	xasprintf(&newhere, "select-layout -t %%%u tiled ; "
	    "split-window -h -t %%%u -c %s ; select-layout -t %%%u tiled",
	    target->id, target->id, qd, target->id);
	xasprintf(&copy, "set-buffer -- %s", qd);
	/* Agent bus: write to this conversation, or copy its bus address. */
	agent = claude_agent_name(target);
	if (agent != NULL) {
		char	from[128];

		claude_json_escape(fromj, sizeof fromj,
		    (w->active != NULL && claude_agent_name(w->active) != NULL) ?
		    claude_agent_name(w->active) : "tmuxv");
		claude_json_escape(toj, sizeof toj, agent);
		snprintf(from, sizeof from, "%s", fromj);
		xasprintf(&send, "command-prompt -p \"Message a %s\" "
		    "{ run-shell -b \"curl -s --max-time 3 -X POST %s/send "
		    "-d '{\\\"from\\\":\\\"%s\\\",\\\"to\\\":\\\"%s\\\","
		    "\\\"subject\\\":\\\"tmuxv\\\",\\\"body\\\":\\\"%%%%\\\"}' "
		    ">/dev/null\" }", agent, claude_bus_url(), from, toj);
		xasprintf(&copyname, "set-buffer -- \"%s\"", agent);

		/*
		 * "Annoncer": the wording is only a PROPOSAL - it lands in an
		 * editable prompt, and nothing is sent until the user
		 * validates. The box is a MULTI-LINE one (an announcement can be
		 * long), so the command opens it itself rather than going
		 * through the one-line command-prompt.
		 */
		xasprintf(&announce, "claude-announce -t %%%u", target->id);
	}
	xasprintf(&close, "confirm-before -p \"Fermer cette conversation ?\" "
	    "\"kill-pane -t %%%u\"", target->id);
	xasprintf(&select, "claude-mark -t %%%u", target->id);
	xasprintf(&range, "claude-mark -r -t %%%u", target->id);

	cmd_find_from_client(&fs, c, 0);
	menu = menu_create("");
#define ADD(n, k, cmd) do {						\
	memset(&it, 0, sizeof it);					\
	it.name = (n); it.key = (k); it.command = (cmd);		\
	menu_add_item(menu, &it, NULL, c, &fs);				\
} while (0)
	ADD("Ouvrir", 'o', open);
	ADD("Renommer", 'r', rename);
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Nouvelle ici", 'n', newhere);
	ADD("Copier le chemin", 'c', copy);
	ADD(NULL, KEYC_NONE, NULL);
	ADD(target->claude_marked ? "Retirer de la selection" :
	    "Ajouter a la selection", 's', select);
	ADD("Selectionner jusqu'ici", 'j', range);
	ADD("Tout selectionner", 't', "claude-mark -a");	/* 'a' = Annoncer */
	if (send != NULL) {
		ADD(NULL, KEYC_NONE, NULL);
		ADD("Annoncer...", 'a', announce);
		ADD("Envoyer un message", 'm', send);
		ADD("Copier le nom d'agent", 'g', copyname);
	}
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Fermer", 'x', close);
#undef ADD

	if (menu->count == 0 ||
	    menu_display(menu, 0, -1, NULL, px, py, c, BOX_LINES_DEFAULT, NULL,
	    NULL, NULL, &fs, NULL, NULL) != 0)
		menu_free(menu);

	free(open); free(rename); free(newhere); free(copy); free(close);
	free(send); free(copyname); free(announce); free(select); free(range);
}

/*
 * CLAUDE: context menu of a saved conversation ("Reprendre" section). Deleting
 * is irreversible - the transcript file goes - so it asks first.
 */
void
claude_sess_menu(struct client *c, u_int idx, u_int px, u_int py)
{
	struct menu		*menu;
	struct menu_item	 it;
	struct cmd_find_state	 fs;
	const char		*id, *label;
	char			*resume, *forget, *copy, *select, *range;
	char			 qi[128], ql[512];

	if ((id = claude_sess_id(idx)) == NULL)
		return;
	if ((label = claude_sess_label(idx)) == NULL)
		label = id;
	form_quote(qi, sizeof qi, id);
	form_quote(ql, sizeof ql, label);

	xasprintf(&resume, "claude-session -r %s", qi);
	xasprintf(&forget, "confirm-before -p \"Supprimer definitivement "
	    "cette conversation ?\" \"claude-session -d %s\"", id);
	xasprintf(&copy, "set-buffer -- %s", ql);
	xasprintf(&select, "claude-mark -S %s", qi);
	xasprintf(&range, "claude-mark -r -S %s", qi);

	cmd_find_from_client(&fs, c, 0);
	menu = menu_create("");
#define ADD(n, k, cmd) do {						\
	memset(&it, 0, sizeof it);					\
	it.name = (n); it.key = (k); it.command = (cmd);		\
	menu_add_item(menu, &it, NULL, c, &fs);				\
} while (0)
	ADD("Reprendre", 'r', resume);
	ADD("Copier le titre", 'c', copy);
	ADD(NULL, KEYC_NONE, NULL);
	ADD(claude_sess_marked(idx) ? "Retirer de la selection" :
	    "Ajouter a la selection", 's', select);
	ADD("Selectionner jusqu'ici", 'j', range);
	ADD("Tout selectionner", 'a', "claude-mark -a -S");
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Supprimer definitivement", 'x', forget);
#undef ADD

	if (menu->count == 0 ||
	    menu_display(menu, 0, -1, NULL, px, py, c, BOX_LINES_DEFAULT, NULL,
	    NULL, NULL, &fs, NULL, NULL) != 0)
		menu_free(menu);

	free(resume); free(forget); free(copy); free(select); free(range);
}

/*
 * CLAUDE: multi-selection in the list. Conversations carry their mark on the
 * pane (it goes away with it); saved conversations are marked BY UUID, since
 * their list is rescanned, re-sorted and filtered all the time. One section is
 * selected at a time: marking in one clears the other.
 */
static char	**claude_sess_marks;
static u_int	  claude_sess_nmarks;

static int
claude_sess_mark_find(const char *id)
{
	u_int	i;

	for (i = 0; i < claude_sess_nmarks; i++) {
		if (strcmp(claude_sess_marks[i], id) == 0)
			return ((int)i);
	}
	return (-1);
}

int
claude_sess_marked(u_int idx)
{
	const char	*id = claude_sess_id(idx);

	return (id != NULL && claude_sess_mark_find(id) != -1);
}

static void
claude_sess_mark_set(const char *id, int on)
{
	int	i = claude_sess_mark_find(id);

	if (on && i == -1) {
		claude_sess_marks = xreallocarray(claude_sess_marks,
		    claude_sess_nmarks + 1, sizeof *claude_sess_marks);
		claude_sess_marks[claude_sess_nmarks++] = xstrdup(id);
	} else if (!on && i != -1) {
		free(claude_sess_marks[i]);
		claude_sess_marks[i] = claude_sess_marks[--claude_sess_nmarks];
	}
}

static void
claude_sess_mark_clear(void)
{
	u_int	i;

	for (i = 0; i < claude_sess_nmarks; i++)
		free(claude_sess_marks[i]);
	free(claude_sess_marks);
	claude_sess_marks = NULL;
	claude_sess_nmarks = 0;
}

/* Marked rows of the list as it is shown now. */
static u_int
claude_sess_nmarked(void)
{
	u_int	i, n = 0;

	for (i = 0; i < claude_sess_count(); i++)
		n += claude_sess_marked(i);
	return (n);
}

static u_int
claude_conv_nmarked(struct window *w)
{
	struct window_pane	*wp;
	u_int			 n = 0;

	TAILQ_FOREACH(wp, &w->panes, entry)
		n += (wp->claude_marked != 0);
	return (n);
}

static void
claude_conv_mark_clear(struct window *w)
{
	struct window_pane	*wp;

	TAILQ_FOREACH(wp, &w->panes, entry)
		wp->claude_marked = 0;
}

static struct window_pane *
claude_conv_at(struct window *w, u_int row)
{
	struct window_pane	*wp;
	u_int			 i = 0;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (i++ == row)
			return (wp);
	}
	return (NULL);
}

static void
claude_list_redraw(struct window *w)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL && c->session->curw != NULL &&
		    c->session->curw->window == w)
			server_redraw_client(c);
	}
}

/* Toggle one conversation; the first mark also takes the one on screen. */
static void
claude_conv_toggle(struct window *w, struct window_pane *wp)
{
	claude_sess_mark_clear();
	if (claude_conv_nmarked(w) == 0 && w->active != NULL &&
	    w->active != wp)
		w->active->claude_marked = 1;
	wp->claude_marked = !wp->claude_marked;
	w->claude_anchor = wp->id + 1;
}

/* Mark from the anchor (else the one on screen) down or up to `wp`. */
static void
claude_conv_range(struct window *w, struct window_pane *wp)
{
	struct window_pane	*loop, *from = NULL;
	int			 in = 0;

	claude_sess_mark_clear();
	if (w->claude_anchor != 0)
		from = window_pane_find_by_id(w->claude_anchor - 1);
	if (from == NULL || from->window != w)
		from = w->active;
	if (from == NULL)
		from = wp;
	TAILQ_FOREACH(loop, &w->panes, entry) {
		if (loop == from || loop == wp) {
			loop->claude_marked = 1;
			if (from != wp)
				in = !in;
			continue;
		}
		loop->claude_marked = in;
	}
	w->claude_anchor = from->id + 1;
}

static void
claude_sess_toggle(struct window *w, u_int idx)
{
	const char	*id;

	claude_conv_mark_clear(w);
	if ((id = claude_sess_id(idx)) == NULL)
		return;
	if (claude_sess_nmarked() == 0 && w->claude_sel_sess >= 0 &&
	    (u_int)w->claude_sel_sess != idx &&
	    claude_sess_id(w->claude_sel_sess) != NULL)
		claude_sess_mark_set(claude_sess_id(w->claude_sel_sess), 1);
	claude_sess_mark_set(id, !claude_sess_marked(idx));
	w->claude_sel_sess = (int)idx;
	w->claude_anchor_sess = (int)idx;
}

static void
claude_sess_range(struct window *w, u_int idx)
{
	u_int	i, a, lo, hi;

	claude_conv_mark_clear(w);
	if (w->claude_anchor_sess >= 0 &&
	    (u_int)w->claude_anchor_sess < claude_sess_count())
		a = w->claude_anchor_sess;
	else if (w->claude_sel_sess >= 0 &&
	    (u_int)w->claude_sel_sess < claude_sess_count())
		a = w->claude_sel_sess;
	else
		a = idx;
	lo = (a < idx) ? a : idx;
	hi = (a < idx) ? idx : a;
	for (i = 0; i < claude_sess_count(); i++)
		claude_sess_mark_set(claude_sess_id(i), i >= lo && i <= hi);
	w->claude_sel_sess = (int)idx;
	w->claude_anchor_sess = (int)a;
}

/* Right-click on a conversation that is part of a multi-selection. */
static void
claude_conv_multi_menu(struct client *c, struct window *w, u_int px, u_int py)
{
	struct menu		*menu;
	struct menu_item	 it;
	struct cmd_find_state	 fs;
	u_int			 n = claude_conv_nmarked(w);
	char			 title[64], *msg, *type, *close;

	snprintf(title, sizeof title, " %u conversations ", n);
	xasprintf(&msg, "command-prompt -p \"Message aux %u agents\" "
	    "{ claude-marked -m \"%%%%\" }", n);
	xasprintf(&type, "command-prompt -p \"Texte a taper dans les %u\" "
	    "{ claude-marked -i \"%%%%\" }", n);
	xasprintf(&close, "confirm-before -p \"Fermer ces %u conversations ?\" "
	    "\"claude-marked -k\"", n);

	cmd_find_from_client(&fs, c, 0);
	menu = menu_create(title);
#define ADD(n, k, cmd) do {						\
	memset(&it, 0, sizeof it);					\
	it.name = (n); it.key = (k); it.command = (cmd);		\
	menu_add_item(menu, &it, NULL, c, &fs);				\
} while (0)
	ADD("Envoyer un message a toutes...", 'm', msg);
	ADD("Taper un texte dans chacune...", 't', type);
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Copier les noms d'agents", 'g', "claude-marked -c");
	ADD("Copier les chemins", 'c', "claude-marked -p");
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Tout selectionner", 'a', "claude-mark -a");
	ADD("Tout deselectionner", 'u', "claude-mark -u");
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Fermer la selection", 'x', close);
#undef ADD
	if (menu_display(menu, 0, -1, NULL, px, py, c, BOX_LINES_DEFAULT, NULL,
	    NULL, NULL, &fs, NULL, NULL) != 0)
		menu_free(menu);
	free(msg); free(type); free(close);
	(void)w;
}

/* Right-click on a saved conversation that is part of a multi-selection. */
static void
claude_sess_multi_menu(struct client *c, u_int px, u_int py)
{
	struct menu		*menu;
	struct menu_item	 it;
	struct cmd_find_state	 fs;
	u_int			 n = claude_sess_nmarked();
	char			 title[64], *resume, *forget;

	snprintf(title, sizeof title, " %u conversations ", n);
	xasprintf(&resume, "claude-marked -R");
	xasprintf(&forget, "confirm-before -p \"Supprimer definitivement "
	    "ces %u conversations ?\" \"claude-marked -D\"", n);

	cmd_find_from_client(&fs, c, 0);
	menu = menu_create(title);
#define ADD(n, k, cmd) do {						\
	memset(&it, 0, sizeof it);					\
	it.name = (n); it.key = (k); it.command = (cmd);		\
	menu_add_item(menu, &it, NULL, c, &fs);				\
} while (0)
	ADD("Reprendre la selection", 'r', resume);
	ADD("Copier les titres", 'c', "claude-marked -T");
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Tout selectionner", 'a', "claude-mark -a -S");
	ADD("Tout deselectionner", 'u', "claude-mark -u");
	ADD(NULL, KEYC_NONE, NULL);
	ADD("Supprimer definitivement la selection", 'x', forget);
#undef ADD
	if (menu_display(menu, 0, -1, NULL, px, py, c, BOX_LINES_DEFAULT, NULL,
	    NULL, NULL, &fs, NULL, NULL) != 0)
		menu_free(menu);
	free(resume); free(forget);
}

/*
 * CLAUDE: a click on a conversation or a saved conversation of the list, with
 * its modifiers. Plain click: show it (and drop the selection). Ctrl+click:
 * add it to / take it out of the selection. Shift+click - or Alt+click, since
 * most terminals keep Shift+click for their own text selection - selects the
 * whole range from the last one clicked. Right-click on a row of a selection
 * of several opens the menu of common actions.
 */
void
claude_list_click(struct client *c, struct window *w, enum claude_row kind,
    u_int idx, int dbl, u_int b, u_int px, u_int py)
{
	struct window_pane	*wp;
	u_int			 button = MOUSE_BUTTONS(b);
	int			 ctrl = (b & MOUSE_MASK_CTRL) != 0;
	int			 range = (b & (MOUSE_MASK_SHIFT|MOUSE_MASK_META)) != 0;

	if (kind == CLAUDE_ROW_CONV) {
		if (dbl || (wp = claude_conv_at(w, idx)) == NULL)
			return;
		if (button == MOUSE_BUTTON_1 && (ctrl || range)) {
			if (ctrl)
				claude_conv_toggle(w, wp);
			else
				claude_conv_range(w, wp);
			claude_list_redraw(w);
			return;
		}
		if (button == MOUSE_BUTTON_3 && wp->claude_marked &&
		    claude_conv_nmarked(w) >= 2) {
			claude_conv_multi_menu(c, w, px, py);
			return;
		}
		if (button == MOUSE_BUTTON_1) {
			claude_conv_mark_clear(w);
			claude_sess_mark_clear();
			w->claude_anchor = wp->id + 1;
			claude_list_redraw(w);
		}
		claude_select_row(c, w, idx);
		if (button == MOUSE_BUTTON_3)
			claude_row_menu(c, w, idx, px, py);
		return;
	}
	if (kind != CLAUDE_ROW_SESS || claude_sess_id(idx) == NULL)
		return;
	if (dbl) {
		claude_sess_mark_clear();
		w->claude_sel_sess = (int)idx;
		claude_list_redraw(w);
		claude_resume(c, idx);
		return;
	}
	if (button == MOUSE_BUTTON_1 && (ctrl || range)) {
		if (ctrl)
			claude_sess_toggle(w, idx);
		else
			claude_sess_range(w, idx);
		claude_list_redraw(w);
		return;
	}
	if (button == MOUSE_BUTTON_3 && claude_sess_marked(idx) &&
	    claude_sess_nmarked() >= 2) {
		claude_sess_multi_menu(c, px, py);
		return;
	}
	if (button == MOUSE_BUTTON_1) {
		claude_sess_mark_clear();
		claude_conv_mark_clear(w);
		w->claude_anchor_sess = (int)idx;
	}
	w->claude_sel_sess = (int)idx;
	claude_list_redraw(w);
	if (button == MOUSE_BUTTON_3)
		claude_sess_menu(c, idx, px, py);
}

/*
 * `claude-mark` - the selection, from the keyboard, the menus or a script:
 *   -t pane   add/remove a conversation (with -r: the range up to it)
 *   -S uuid   add/remove a saved conversation (with -r: the range)
 *   -a        select every conversation (with -S: every saved one)
 *   -u        drop the selection
 */
static enum cmd_retval
cmd_claude_mark_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w = target->w;
	struct window_pane	*wp;
	const char		*id = args_get(args, 'S');
	u_int			 i;

	if (w == NULL || !claude_manager(w)) {
		cmdq_error(item, "pas une fenetre du gestionnaire Claude");
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'u')) {
		claude_conv_mark_clear(w);
		claude_sess_mark_clear();
	} else if (args_has(args, 'a')) {
		if (args_has(args, 'S')) {
			claude_conv_mark_clear(w);
			for (i = 0; i < claude_sess_count(); i++)
				claude_sess_mark_set(claude_sess_id(i), 1);
		} else {
			claude_sess_mark_clear();
			TAILQ_FOREACH(wp, &w->panes, entry)
				wp->claude_marked = 1;
		}
	} else if (id != NULL) {
		for (i = 0; i < claude_sess_count(); i++) {
			if (strcmp(claude_sess_id(i), id) == 0)
				break;
		}
		if (i == claude_sess_count()) {
			cmdq_error(item, "conversation introuvable : %s", id);
			return (CMD_RETURN_ERROR);
		}
		if (args_has(args, 'r'))
			claude_sess_range(w, i);
		else {
			claude_conv_mark_clear(w);
			claude_sess_mark_set(id, !claude_sess_marked(i));
			w->claude_anchor_sess = (int)i;
		}
	} else if (args_has(args, 't') && target->wp != NULL) {
		if (args_has(args, 'r'))
			claude_conv_range(w, target->wp);
		else {
			claude_sess_mark_clear();
			target->wp->claude_marked = !target->wp->claude_marked;
			w->claude_anchor = target->wp->id + 1;
		}
	}
	claude_list_redraw(w);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_claude_mark_entry = {
	.name = "claude-mark",
	.alias = NULL,

	.args = { "arS:t:u", 0, 0, NULL },
	.usage = "[-aru] [-S uuid] [-t target-pane]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_claude_mark_exec
};

/*
 * Report on the status line of an attached client, or on the command's own
 * output when it comes from a script (no session there to display on).
 */
static void printflike(3, 4)
claude_say(struct cmdq_item *item, struct client *c, const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	if (c != NULL && c->session != NULL)
		status_message_set(c, -1, 1, 0, "%s", msg);
	else
		cmdq_print(item, "%s", msg);
	free(msg);
}

/* Queue a command line for `c`, as if typed. */
static void
claude_queue(struct client *c, const char *cmd)
{
	struct cmdq_state	*state = cmdq_new_state(NULL, NULL, 0);
	char			*error;

	if (cmd_parse_and_append(cmd, NULL, c, state, &error) ==
	    CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);
}

/*
 * `claude-marked` - act on the selection:
 *   -m text  bus message to every selected agent
 *   -i text  type the text into every selected conversation's prompt
 *   -c / -p  copy the agent names / the directories (paste buffer)
 *   -k       close the selected conversations
 *   -R / -D  resume / delete for good the selected saved conversations
 *   -T       copy the titles of the selected saved conversations
 * Without a selection, nothing happens: a stale menu never acts on
 * something the user did not pick.
 */
static enum cmd_retval
cmd_claude_marked_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct client		*c = cmdq_get_client(item), *tc;
	struct window		*w = target->w;
	struct window_pane	*wp;
	struct evbuffer		*out;
	const char		*text, *agent;
	char			*cmd, *s, **ids = NULL, body[4096], json[5200];
	char			 q[5400], who[128];
	u_int			 i, n = 0, done = 0, skipped = 0;

	if (w == NULL || !claude_manager(w)) {
		cmdq_error(item, "pas une fenetre du gestionnaire Claude");
		return (CMD_RETURN_ERROR);
	}
	/* Who acts: the caller if attached, else a client on the session. */
	tc = (c != NULL && c->session != NULL) ? c :
	    cmd_find_best_client(target->s);

	/* Saved conversations: collect the uuids first, the list moves. */
	if (args_has(args, 'R') || args_has(args, 'D') || args_has(args, 'T')) {
		for (i = 0; i < claude_sess_count(); i++) {
			if (!claude_sess_marked(i))
				continue;
			ids = xreallocarray(ids, n + 1, sizeof *ids);
			ids[n++] = xstrdup(claude_sess_id(i));
		}
		if (n == 0) {
			cmdq_error(item, "aucune conversation selectionnee");
			return (CMD_RETURN_ERROR);
		}
		out = evbuffer_new();
		for (i = 0; i < n; i++) {
			u_int	j;

			for (j = 0; j < claude_sess_count(); j++) {
				if (strcmp(claude_sess_id(j), ids[i]) == 0)
					break;
			}
			if (j == claude_sess_count())
				continue;
			if (args_has(args, 'T')) {
				evbuffer_add_printf(out, "%s%s", done ? "\n" : "",
				    claude_sess_label(j));
				done++;
			} else if (args_has(args, 'D'))
				done += claude_sess_delete(j);
			else if (tc != NULL) {
				claude_resume(tc, j);
				done++;
			}
		}
		if (args_has(args, 'T') && done != 0) {
			s = xmalloc(EVBUFFER_LENGTH(out) + 1);
			memcpy(s, EVBUFFER_DATA(out), EVBUFFER_LENGTH(out));
			s[EVBUFFER_LENGTH(out)] = '\0';
			paste_add(NULL, s, EVBUFFER_LENGTH(out));
		} else {
			claude_sess_mark_clear();
			w->claude_anchor_sess = -1;
		}
		evbuffer_free(out);
		for (i = 0; i < n; i++)
			free(ids[i]);
		free(ids);
		if (c != NULL) {
			claude_say(item, c, "%u conversation%s %s",
			    done, done > 1 ? "s" : "",
			    args_has(args, 'T') ? "copiee(s)" :
			    args_has(args, 'D') ? "supprimee(s)" :
			    "reprise(s)");
		}
		claude_list_redraw(w);
		return (CMD_RETURN_NORMAL);
	}

	if (claude_conv_nmarked(w) == 0) {
		cmdq_error(item, "aucune conversation selectionnee");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'k')) {
		/* By id, through the queue: the pane list changes as they go. */
		out = evbuffer_new();
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->claude_marked) {
				evbuffer_add_printf(out, "%skill-pane -t %%%u",
				    done++ ? " ; " : "", wp->id);
			}
		}
		cmd = xmalloc(EVBUFFER_LENGTH(out) + 1);
		memcpy(cmd, EVBUFFER_DATA(out), EVBUFFER_LENGTH(out));
		cmd[EVBUFFER_LENGTH(out)] = '\0';
		evbuffer_free(out);
		claude_conv_mark_clear(w);
		w->claude_anchor = 0;
		claude_queue(tc, cmd);
		free(cmd);
		claude_list_redraw(w);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'c') || args_has(args, 'p')) {
		out = evbuffer_new();
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (!wp->claude_marked)
				continue;
			if (args_has(args, 'c')) {
				if ((agent = claude_agent_name(wp)) == NULL) {
					skipped++;
					continue;
				}
				evbuffer_add_printf(out, "%s%s",
				    done++ ? " " : "", agent);
			} else {
				s = osdep_get_cwd(wp->fd);
				if (s == NULL || *s != '/')
					s = wp->cwd;
				if (s == NULL || *s != '/') {
					skipped++;
					continue;
				}
				evbuffer_add_printf(out, "%s%s",
				    done++ ? "\n" : "", s);
			}
		}
		if (done != 0) {
			s = xmalloc(EVBUFFER_LENGTH(out) + 1);
			memcpy(s, EVBUFFER_DATA(out), EVBUFFER_LENGTH(out));
			s[EVBUFFER_LENGTH(out)] = '\0';
			paste_add(NULL, s, EVBUFFER_LENGTH(out));
		}
		evbuffer_free(out);
		if (c != NULL) {
			claude_say(item, c, "%u copie(s)%s", done,
			    skipped ? ", sans nom d'agent ignorees" : "");
		}
		return (CMD_RETURN_NORMAL);
	}

	text = args_get(args, 'm');
	if (text == NULL)
		text = args_get(args, 'i');
	if (text == NULL || *text == '\0') {
		cmdq_error(item, "rien a envoyer");
		return (CMD_RETURN_ERROR);
	}
	claude_json_escape(body, sizeof body, text);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!wp->claude_marked)
			continue;
		if (args_has(args, 'i')) {
			if (!claude_pane_is_agent(wp)) {
				skipped++;
				continue;
			}
			claude_inject(wp, text);
			done++;
			continue;
		}
		if ((agent = claude_agent_name(wp)) == NULL) {
			skipped++;
			continue;
		}
		claude_json_escape(who, sizeof who, agent);
		snprintf(json, sizeof json, "{\"from\":\"tmuxv\",\"to\":\"%s\","
		    "\"subject\":\"tmuxv\",\"body\":\"%s\"}", who, body);
		claude_squote(q, sizeof q, json);
		xasprintf(&cmd, "curl -s --max-time 3 -X POST "
		    "-H 'Content-Type: application/json' %s/send -d %s",
		    claude_bus_url(), q);
		job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		    JOB_NOWAIT, -1, -1);
		free(cmd);
		done++;
	}
	if (c != NULL) {
		claude_say(item, c, "%s a %u conversation%s%s",
		    args_has(args, 'i') ? "Texte tape" : "Message envoye",
		    done, done > 1 ? "s" : "",
		    skipped ? " (sans agent : ignorees)" : "");
	}
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_claude_marked_entry = {
	.name = "claude-marked",
	.alias = NULL,

	.args = { "cDi:km:pRTt:", 0, 0, NULL },
	.usage = "[-cDkpRT] [-i text] [-m text] [-t target-pane]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_claude_marked_exec
};

/* Start `claude` in `dir`, in a new pane of the manager window. */
static void
claude_start(struct client *c, const char *dir)
{
	struct session		*s = c->session;
	struct window		*w;
	struct cmdq_state	*state;
	char			*cmd, *error, q[PATH_MAX + 16], uuid[40];
	u_char			 r[16];

	if (s == NULL || s->curw == NULL)
		return;
	w = s->curw->window;
	free(claude_lastdir);
	claude_lastdir = xstrdup(dir);

	/*
	 * tmuxv names the conversation itself (a random RFC 4122 v4 uuid):
	 * from then on it knows EXACTLY which transcript belongs to this pane
	 * - to restore it after a crash, on this server or another - instead
	 * of guessing from the newest file in a directory shared by many.
	 */
	arc4random_buf(r, sizeof r);
	r[6] = (r[6] & 0x0f) | 0x40;
	r[8] = (r[8] & 0x3f) | 0x80;
	snprintf(uuid, sizeof uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
	    "%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4],
	    r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14],
	    r[15]);

	/*
	 * Split (this unzooms the window), run claude there, then zoom back on
	 * the new pane: the other conversations keep running, hidden.
	 */
	form_quote(q, sizeof q, dir);
	xasprintf(&cmd, "select-layout -t %%%u tiled ; "
	    "split-window -h -t %%%u -c %s claude --session-id %s ; "
	    "select-layout -t %%%u tiled",
	    w->active->id, w->active->id, q, uuid, w->active->id);
	state = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, c, state, &error) ==
	    CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);
	free(cmd);
}

static void
dirbox_flash_cb(__unused int fd, __unused short events, void *arg)
{
	struct dirbox_data	*db = arg;
	struct client		*c = db->c;
	int			 kind = db->flash_kind;
	char			 path[PATH_MAX];

	db->pressed = -1;
	db->flash_kind = 0;
	strlcpy(path, db->path, sizeof path);
	server_client_clear_overlay(c);		/* frees db */
	if (kind == 1)
		claude_start(c, path);
}

static void
dirbox_start_flash(struct client *c, struct dirbox_data *db, int btn)
{
	struct timeval	tv = { 0, 90000 };

	if (db->flash_kind != 0)
		return;
	db->pressed = btn;
	db->flash_kind = btn;
	c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&db->flash))
		evtimer_del(&db->flash);
	evtimer_set(&db->flash, dirbox_flash_cb, db);
	evtimer_add(&db->flash, &tv);
}

/* Enter the selected directory. */
static void
dirbox_enter(struct client *c, struct dirbox_data *db)
{
	char	 next[PATH_MAX], *slash;

	if (db->sel < 0 || (u_int)db->sel >= db->nents)
		return;
	if (strcmp(db->ents[db->sel], "..") == 0) {
		if (strcmp(db->path, "/") == 0)
			return;
		strlcpy(next, db->path, sizeof next);
		if ((slash = strrchr(next, '/')) != NULL) {
			if (slash == next)
				next[1] = '\0';
			else
				*slash = '\0';
		}
	} else if ((size_t)snprintf(next, sizeof next, "%s%s%s", db->path,
	    (strcmp(db->path, "/") == 0) ? "" : "/",
	    db->ents[db->sel]) >= sizeof next)
		return;
	strlcpy(db->path, next, sizeof db->path);
	dirbox_load(db);
	c->flags |= CLIENT_REDRAWOVERLAY;
}

static int
dirbox_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct dirbox_data	*db = data;
	struct mouse_event	*m = &event->m;
	key_code		 key = event->key, base;
	u_int			 visrows = dirbox_visrows(db);
	u_int			 listw = db->w - 18;
	u_int			 maxscroll = (db->nents > visrows) ?
				     db->nents - visrows : 0;

	if (db->flash_kind != 0)
		return (0);
	if (KEYC_IS_MOUSE(key)) {
		int	hover = (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b));
		u_int	lx, ly;
		int	np = -1;

		if (hover || key == KEYC_DOUBLECLICK)
			return (0);
		if (MOUSE_WHEEL(m->b)) {
			if (MOUSE_BUTTONS(m->b) == MOUSE_WHEEL_UP)
				db->scroll = (db->scroll > 3) ? db->scroll - 3 : 0;
			else
				db->scroll = (db->scroll + 3 < maxscroll) ?
				    db->scroll + 3 : maxscroll;
			c->flags |= CLIENT_REDRAWOVERLAY;
			return (0);
		}
		if (m->x < db->px || m->x >= db->px + db->w ||
		    m->y < db->py || m->y >= db->py + db->h) {
			if (MOUSE_RELEASE(m->b) && db->pressed >= 0) {
				db->pressed = -1;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		lx = m->x - db->px;
		ly = m->y - db->py;

		if (lx >= DB_BTNX && lx < DB_BTNX + DB_BW) {
			if (ly >= 6 && ly < 8)
				np = 1;
			else if (ly >= 9 && ly < 11)
				np = 2;
		}
		if (!MOUSE_RELEASE(m->b)) {
			if (np > 0 && !MOUSE_DRAG(m->b)) {
				db->pressed = np;
				db->focus = (np == 1) ? 1 : 2;
				c->flags |= CLIENT_REDRAWOVERLAY;
				return (0);
			}
			/* Scrollbar column. */
			if (lx == 2 + listw && ly >= 6 && ly < 6 + visrows &&
			    !MOUSE_DRAG(m->b)) {
				if (ly == 6)
					db->scroll = (db->scroll > 0) ?
					    db->scroll - 1 : 0;
				else if (ly == 6 + visrows - 1)
					db->scroll = (db->scroll < maxscroll) ?
					    db->scroll + 1 : maxscroll;
				else
					db->scroll = scrollbar_value(ly - 7,
					    maxscroll, visrows - 2);
				c->flags |= CLIENT_REDRAWOVERLAY;
				return (0);
			}
			/* List rows: click selects, double-click enters. */
			if (lx >= 2 && lx < 2 + listw && ly >= 6 &&
			    ly < 6 + visrows && !MOUSE_DRAG(m->b)) {
				u_int		idx = db->scroll + (ly - 6);
				struct timeval	now, diff;

				if (idx < db->nents) {
					gettimeofday(&now, NULL);
					timersub(&now, &db->last_click, &diff);
					db->sel = (int)idx;
					db->focus = 0;
					if ((int)idx == db->last_row &&
					    diff.tv_sec == 0 &&
					    diff.tv_usec <
					    KEYC_CLICK_TIMEOUT * 1000L) {
						db->last_row = -1;
						dirbox_enter(c, db);
						return (0);
					}
					db->last_row = (int)idx;
					db->last_click = now;
					c->flags |= CLIENT_REDRAWOVERLAY;
				}
				return (0);
			}
			return (0);
		}
		if (db->pressed > 0 && np == db->pressed) {
			int	btn = db->pressed;

			db->pressed = -1;
			dirbox_start_flash(c, db, btn);
			return (0);
		}
		if (db->pressed >= 0) {
			db->pressed = -1;
			c->flags |= CLIENT_REDRAWOVERLAY;
		}
		return (0);
	}

	base = key & KEYC_MASK_KEY;
	switch (base) {
	case '\033':
	case '\003':
	case '\007':
		return (1);
	case '\r':
	case '\n':
		if (db->focus == 2)
			return (1);
		if (db->focus == 1)
			dirbox_start_flash(c, db, 1);
		else
			dirbox_enter(c, db);	/* Enter on the list: descend */
		return (0);
	case '\011':	/* Tab cycles list -> Choisir -> Annuler */
	case KEYC_BTAB:
		db->focus = (db->focus + 1) % 3;
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	case KEYC_UP:
		if (db->sel > 0)
			db->sel--;
		break;
	case KEYC_DOWN:
		if (db->sel + 1 < (int)db->nents)
			db->sel++;
		break;
	case KEYC_HOME:
		db->sel = 0;
		break;
	case KEYC_END:
		db->sel = (int)db->nents - 1;
		break;
	case KEYC_PPAGE:
		db->sel = (db->sel >= (int)visrows) ?
		    db->sel - (int)visrows : 0;
		break;
	case KEYC_NPAGE:
		db->sel = (db->sel + (int)visrows < (int)db->nents) ?
		    db->sel + (int)visrows : (int)db->nents - 1;
		break;
	default:
		return (0);
	}
	if (db->sel >= 0) {			/* keep the selection visible */
		if ((u_int)db->sel < db->scroll)
			db->scroll = db->sel;
		else if ((u_int)db->sel >= db->scroll + visrows)
			db->scroll = db->sel - visrows + 1;
	}
	db->focus = 0;
	c->flags |= CLIENT_REDRAWOVERLAY;
	return (0);
}

/*
 * CLAUDE: open the directory chooser, starting from the last chosen
 * directory, else the current pane's directory.
 */
void
claude_new_dialog(struct client *c)
{
	struct dirbox_data	*db;
	const char		*start = NULL;
	u_int			 w = DB_W, h = DB_H;

	if (c->tty.sx < w + 4 || c->tty.sy < h + 2)
		return;
	if (claude_lastdir != NULL)
		start = claude_lastdir;
	else if (c->session != NULL && c->session->curw != NULL &&
	    c->session->curw->window->active->cwd != NULL)
		start = c->session->curw->window->active->cwd;
	if (start == NULL || *start != '/')
		start = server_client_get_cwd(c, c->session);
	if (start == NULL || *start != '/')
		start = "/";

	db = xcalloc(1, sizeof *db);
	db->c = c;
	db->w = w;
	db->h = h;
	db->pressed = -1;
	db->focus = 0;
	db->last_row = -1;
	strlcpy(db->path, start, sizeof db->path);
	dirbox_load(db);

	screen_init(&db->s, w, h, 0);
	db->s.mode &= ~MODE_CURSOR;
	db->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	db->px = (c->tty.sx - w) / 2;
	db->py = (c->tty.sy - h) / 2;
	server_client_set_overlay(c, 0, dirbox_check_cb, dirbox_mode_cb,
	    dirbox_draw_cb, dirbox_key_cb, dirbox_free_cb, NULL, db);
}

/*
 * Command: claude-manager - open the conversation manager window. It is an
 * ordinary desktop window (frame, title, move/resize, scrollbar) flagged as
 * the manager, so it inherits everything the console windows have.
 */
static enum cmd_retval
cmd_claude_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client		*c = cmdq_get_target_client(item);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct spawn_context	 sc = { 0 };
	struct winlink		*new_wl;
	struct cmdq_state	*state;
	char			*cause = NULL, *reg, *error;
	char			 agent[128], grp[128];

	if (c == NULL || target->s == NULL)
		return (CMD_RETURN_NORMAL);

	sc.item = item;
	sc.s = target->s;
	sc.tc = c;
	sc.name = "Claude Code";
	sc.idx = -1;
	sc.environ = environ_create();

	/*
	 * The window's first conversation gets a bus identity too, so it can be
	 * written to like the ones created later with "[+ Nouvelle]".
	 */
	{
		const char	*cwd = server_client_get_cwd(c, target->s);
		const char	*base;
		size_t		 i;

		base = (cwd != NULL) ? strrchr(cwd, '/') : NULL;
		if (base != NULL && base[1] != '\0')
			base++;
		else
			base = "conv";
		snprintf(agent, sizeof agent, "%s-1", base);
		for (i = 0; agent[i] != '\0'; i++) {
			if (!isalnum((u_char)agent[i]) && agent[i] != '-' &&
			    agent[i] != '_')
				agent[i] = '-';
		}
		strlcpy(grp, base, sizeof grp);
		for (i = 0; grp[i] != '\0'; i++) {
			if (!isalnum((u_char)grp[i]) && grp[i] != '-' &&
			    grp[i] != '_')
				grp[i] = '-';
		}
		environ_set(sc.environ, "AGENT_NAME", 0, "%s", agent);
		claude_register_mcp();	/* declare the bus to Claude Code */
	}

	new_wl = spawn_window(&sc, &cause);
	environ_free(sc.environ);
	if (new_wl == NULL) {
		cmdq_error(item, "%s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	new_wl->window->claude_mgr = 1;
	new_wl->window->claude_seq = 1;		/* "-1" is taken */
	claude_bus_start();			/* start watching for mail */
	/* Announce it on the agent bus. */
	/*
	 * job_run(), never `run-shell`: run-shell shows its output IN THE
	 * ACTIVE PANE (view mode) - the user then sees the curl command line
	 * and a "[0/0]" indicator instead of a clean shell prompt.
	 */
	/*
	 * Le GROUPE est le projet (le dernier element du repertoire) : une
	 * diffusion `*` reste alors entre les conversations du meme projet au
	 * lieu d'arroser tous les agents du bus. `managed` dit au bus que
	 * tmuxv livre lui-meme : l'agent n'a pas a relever sa boite, et ne
	 * recevra donc pas deux fois le meme message.
	 */
	xasprintf(&reg, "curl -s --max-time 2 -X POST %s/register "
	    "-d '{\"name\":\"%s\",\"group\":\"%s\",\"managed\":true}' "
	    ">/dev/null 2>&1", claude_bus_url(), agent, grp);
	job_run(reg, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    JOB_NOWAIT, -1, -1);
	free(reg);
	recalculate_sizes();
	server_redraw_client(c);
	return (CMD_RETURN_NORMAL);
}

/*
 * Command: claude-new-dir - start `claude` in a directory picked with the
 * Turbo Vision chooser. Not on the "[+ Nouvelle]" button (which just opens a
 * shell in the current directory) but kept available.
 */
static enum cmd_retval
cmd_claude_dir_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);

	if (c != NULL)
		claude_new_dialog(c);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_claude_dir_entry = {
	.name = "claude-new-dir",
	.alias = NULL,

	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_claude_dir_exec
};

/*
 * Command: claude-rename - rename a conversation. The displayed name (the pane
 * title) AND its address on the agent bus move together, and the new address is
 * announced on the bus. A claude ALREADY running keeps the identity it was
 * started with (its MCP server read it at startup); the next one started in
 * this conversation picks the new one up through the wrapper.
 */
static enum cmd_retval
cmd_claude_rename_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	const char		*name = args_string(args, 0);
	char			 agent[128], *cmd;
	size_t			 i;

	if (wp == NULL || name == NULL || *name == '\0')
		return (CMD_RETURN_NORMAL);

	screen_set_title(&wp->base, name);	/* what the list shows */

	strlcpy(agent, name, sizeof agent);
	for (i = 0; agent[i] != '\0'; i++) {
		if (!isalnum((u_char)agent[i]) && agent[i] != '-' &&
		    agent[i] != '_')
			agent[i] = '-';
	}
	if (*agent != '\0') {
		char		*shellcmd, *error, *cur;
		const char	*old = claude_agent_name(wp);
		int		 at_prompt = 0;

		/*
		 * Prevenir le bus que c'est un RENOMMAGE, pas une nouvelle
		 * inscription : le courrier non lu suit, les salons suivent,
		 * l'ancienne adresse cesse d'exister et les pairs restes
		 * dessus sont rediriges. Sans cela l'ancien nom continuait de
		 * vivre - et la moindre session encore sur ce nom le faisait
		 * reapparaitre indefiniment.
		 */
		if (old != NULL && *old != '\0' && strcmp(old, agent) != 0) {
			char	*mv;

			xasprintf(&mv, "curl -s --max-time 2 -X POST "
			    "%s/rename -d '{\"from\":\"%s\",\"to\":\"%s\"}' "
			    ">/dev/null 2>&1", claude_bus_url(), old, agent);
			job_run(mv, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
			    NULL, JOB_NOWAIT, -1, -1);
			free(mv);
		}

		free(wp->claude_agent);
		wp->claude_agent = xstrdup(agent);
		wp->claude_seen = 0;		/* address changed: resample */
		wp->claude_pending = 0;
		/*
		 * A new mailbox: the cursor belonged to the old one, and the
		 * unread mail the bus moved over keeps its (older) ids - start
		 * again with a first fetch, which delivers exactly that.
		 */
		wp->claude_cursor_ok = 0;
		/*
		 * Le GROUPE (le projet) et `managed` doivent repartir avec le
		 * nouveau nom : sans eux, une conversation renommee sortait du
		 * groupe - une diffusion `*` ne l'atteignait plus - et le bus
		 * ne savait plus que tmuxv lui livre son courrier.
		 */
		{
			char		 grp[128];
			const char	*base;
			char		*pcwd;
			size_t		 k;

			pcwd = osdep_get_cwd(wp->fd);
			if (pcwd == NULL || *pcwd != '/')
				pcwd = wp->cwd;
			base = (pcwd != NULL) ? strrchr(pcwd, '/') : NULL;
			if (base != NULL && base[1] != '\0')
				base++;
			else
				base = "conv";
			strlcpy(grp, base, sizeof grp);
			for (k = 0; grp[k] != '\0'; k++) {
				if (!isalnum((u_char)grp[k]) &&
				    grp[k] != '-' && grp[k] != '_')
					grp[k] = '-';
			}
			xasprintf(&cmd, "curl -s --max-time 2 -X POST "
			    "%s/register -d '{\"name\":\"%s\","
			    "\"group\":\"%s\",\"managed\":true}' "
			    ">/dev/null 2>&1", claude_bus_url(), agent, grp);
		}
		job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		    JOB_NOWAIT, -1, -1);
		free(cmd);

		/*
		 * Make the NEXT agent started here use the new address: update
		 * the live shell's environment. Only when the conversation is
		 * sitting at a shell prompt - never type into a running
		 * program (an agent already running keeps its own identity
		 * anyway, its MCP server read it at startup).
		 */
		cur = osdep_get_name(wp->fd, wp->tty);
		if (cur != NULL && *cur != '\0') {
			const char	*b = strrchr(cur, '/');

			b = (b != NULL) ? b + 1 : cur;
			if (*b == '-')		/* login shell: argv[0]="-bash" */
				b++;
			at_prompt = (strcmp(b, "bash") == 0 ||
			    strcmp(b, "zsh") == 0 || strcmp(b, "sh") == 0 ||
			    strcmp(b, "fish") == 0 || strcmp(b, "dash") == 0 ||
			    strcmp(b, "ksh") == 0);
		}
		free(cur);
		log_debug("%s: %%%u at_prompt=%d agent=%s", __func__, wp->id,
		    at_prompt, agent);
		if (at_prompt) {
			struct cmdq_state	*st;

			xasprintf(&shellcmd,
			    "send-keys -t %%%u \" export AGENT_NAME=%s\" Enter",
			    wp->id, agent);
			st = cmdq_new_state(NULL, NULL, 0);
			if (cmd_parse_and_append(shellcmd, NULL,
			    cmdq_get_client(item), st, &error) ==
			    CMD_PARSE_ERROR) {
				log_debug("%s: %s", __func__, error);
				free(error);
			}
			cmdq_free_state(st);
			free(shellcmd);
		}
	}
	server_redraw_window(wp->window);
	return (CMD_RETURN_NORMAL);
}

/*
 * ==========================================================================
 * CLAUDE: MULTI-LINE TEXT BOX (Turbo Vision TMemo) for the announcement.
 *
 * An announcement can be long, so it gets a real text area instead of a
 * one-line input: the text is word-wrapped over several rows, the cursor moves
 * with the arrows, Enter starts a new line, and Tab reaches the buttons - which
 * is how a TMemo behaves in a TDialog (Enter belongs to the text; the buttons
 * are reached with Tab).
 * ==========================================================================
 */

struct memo_dialog {
	struct client	*c;
	struct screen	 s;
	u_int		 px, py, w, h;
	u_int		 wp_id;			/* conversation to announce to */

	char		*text;
	size_t		 len, alloc;
	size_t		 cursor;		/* byte offset in text */
	u_int		 scroll;		/* first visible wrapped row */

	int		 focus;			/* 0 text, 1 OK, 2 Annuler */
	int		 pressed;		/* button being flashed */
	struct event	 flash;
};

#define MEMO_ROWS 8			/* visible text rows */

/* Wrapped layout: where each display row starts, in byte offsets. */
struct memo_wrap {
	size_t	start[256];
	size_t	end[256];
	u_int	n;
};

/*
 * Break the text into display rows of at most `width` columns, on spaces when
 * possible and always on a newline. Byte offsets, so the cursor maps back.
 */
static void
memo_layout(struct memo_dialog *md, u_int width, struct memo_wrap *wr)
{
	size_t	i = 0, start = 0, lastsp = 0;
	u_int	col = 0;

	wr->n = 0;
	if (width == 0)
		width = 1;
	while (i <= md->len && wr->n < nitems(wr->start) - 1) {
		if (i == md->len) {
			wr->start[wr->n] = start;
			wr->end[wr->n] = i;
			wr->n++;
			break;
		}
		if (md->text[i] == '\n') {
			wr->start[wr->n] = start;
			wr->end[wr->n] = i;
			wr->n++;
			i++;
			start = i;
			col = 0;
			lastsp = 0;
			continue;
		}
		if (md->text[i] == ' ')
			lastsp = i;
		/* Continuation bytes do not take a column of their own. */
		if (((u_char)md->text[i] & 0xc0) != 0x80)
			col++;
		i++;
		if (col > width) {
			size_t	brk = (lastsp > start) ? lastsp : i - 1;

			wr->start[wr->n] = start;
			wr->end[wr->n] = brk;
			wr->n++;
			start = (lastsp > start) ? brk + 1 : brk;
			i = start;
			col = 0;
			lastsp = 0;
		}
	}
	if (wr->n == 0) {
		wr->start[0] = wr->end[0] = 0;
		wr->n = 1;
	}
}

/* Which display row holds the cursor. */
static u_int
memo_cursor_row(struct memo_dialog *md, struct memo_wrap *wr)
{
	u_int	i;

	for (i = 0; i < wr->n; i++) {
		if (md->cursor >= wr->start[i] && md->cursor <= wr->end[i])
			return (i);
	}
	return (wr->n > 0 ? wr->n - 1 : 0);
}

static void
memo_insert(struct memo_dialog *md, const char *data, size_t n)
{
	if (md->len + n + 1 > md->alloc) {
		md->alloc = (md->len + n + 1) * 2;
		md->text = xrealloc(md->text, md->alloc);
	}
	memmove(md->text + md->cursor + n, md->text + md->cursor,
	    md->len - md->cursor + 1);
	memcpy(md->text + md->cursor, data, n);
	md->len += n;
	md->cursor += n;
}

/* Delete the whole UTF-8 character before the cursor. */
static void
memo_backspace(struct memo_dialog *md)
{
	size_t	n = 1;

	if (md->cursor == 0)
		return;
	while (md->cursor - n > 0 &&
	    ((u_char)md->text[md->cursor - n] & 0xc0) == 0x80)
		n++;
	memmove(md->text + md->cursor - n, md->text + md->cursor,
	    md->len - md->cursor + 1);
	md->len -= n;
	md->cursor -= n;
}

static void
memo_delete(struct memo_dialog *md)
{
	size_t	n = 1;

	if (md->cursor >= md->len)
		return;
	while (md->cursor + n < md->len &&
	    ((u_char)md->text[md->cursor + n] & 0xc0) == 0x80)
		n++;
	memmove(md->text + md->cursor, md->text + md->cursor + n,
	    md->len - md->cursor - n + 1);
	md->len -= n;
}

static void
memo_left(struct memo_dialog *md)
{
	if (md->cursor == 0)
		return;
	md->cursor--;
	while (md->cursor > 0 &&
	    ((u_char)md->text[md->cursor] & 0xc0) == 0x80)
		md->cursor--;
}

static void
memo_right(struct memo_dialog *md)
{
	if (md->cursor >= md->len)
		return;
	md->cursor++;
	while (md->cursor < md->len &&
	    ((u_char)md->text[md->cursor] & 0xc0) == 0x80)
		md->cursor++;
}

static void
memo_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct memo_dialog	*md = data;

	server_client_overlay_range(md->px, md->py, md->w, md->h, px, py, nx, r);
}

static struct screen *
memo_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct memo_dialog	*md = data;

	*cx = md->px;
	*cy = md->py;
	return (&md->s);
}

static void
memo_free_cb(__unused struct client *c, void *data)
{
	struct memo_dialog	*md = data;

	if (event_initialized(&md->flash))
		evtimer_del(&md->flash);
	screen_free(&md->s);
	free(md->text);
	free(md);
}

static void
memo_draw_cb(struct client *c, void *data, __unused struct screen_redraw_ctx *rctx)
{
	struct memo_dialog	*md = data;
	struct screen		*s = &md->s;
	struct screen_write_ctx	 ctx;
	struct grid_cell	 dlg, frame, ig, cg, hint;
	struct memo_wrap	 wr;
	overlay_check_cb	 saved_check;
	void			*saved_data;
	u_int			 W = md->w, H = md->h, i, j;
	u_int			 fieldw = W - 4, crow;
	char			 buf[512];

	form_gc(&dlg, FORM_DLG_FG, FORM_DLG_BG);
	form_gc(&frame, FORM_DLG_FG, FORM_DLG_BG);
	frame.attr = GRID_ATTR_BRIGHT;
	form_gc(&ig, FORM_INP_FG, FORM_INP_FBG);
	memcpy(&cg, &ig, sizeof cg);
	cg.attr |= GRID_ATTR_REVERSE;
	form_gc(&hint, 0x404040, FORM_DLG_BG);

	screen_write_start(&ctx, s);
	for (j = 0; j < H; j++) {
		screen_write_cursormove(&ctx, 0, j, 0);
		for (i = 0; i < W; i++)
			screen_write_putc(&ctx, &dlg, ' ');
	}
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_box(&ctx, W, H, BOX_LINES_DOUBLE, &frame, " Annonce ");

	memo_layout(md, fieldw, &wr);
	crow = memo_cursor_row(md, &wr);
	if (crow < md->scroll)
		md->scroll = crow;
	if (crow >= md->scroll + MEMO_ROWS)
		md->scroll = crow - MEMO_ROWS + 1;
	if (wr.n <= MEMO_ROWS)
		md->scroll = 0;

	/* The text area: a cyan field, like every input in these dialogs. */
	for (j = 0; j < MEMO_ROWS; j++) {
		u_int	row = md->scroll + j;
		size_t	p, e;
		u_int	x = 2;

		screen_write_cursormove(&ctx, 2, 2 + j, 0);
		for (i = 0; i < fieldw; i++)
			screen_write_putc(&ctx, &ig, ' ');
		if (row >= wr.n)
			continue;
		p = wr.start[row];
		e = wr.end[row];
		while (p < e && x < 2 + fieldw) {
			size_t	n = 1;
			char	ch[8];

			while (p + n < e &&
			    ((u_char)md->text[p + n] & 0xc0) == 0x80)
				n++;
			if (n >= sizeof ch)
				n = sizeof ch - 1;
			memcpy(ch, md->text + p, n);
			ch[n] = '\0';
			screen_write_cursormove(&ctx, x, 2 + j, 0);
			screen_write_puts(&ctx,
			    (md->focus == 0 && md->cursor == p) ? &cg : &ig,
			    "%s", ch);
			p += n;
			x++;
		}
		/* Cursor sitting at the very end of a row. */
		if (md->focus == 0 && md->cursor == e && x < 2 + fieldw &&
		    (row == wr.n - 1 || md->cursor != wr.start[row + 1])) {
			screen_write_cursormove(&ctx, x, 2 + j, 0);
			screen_write_puts(&ctx, &cg, " ");
		}
	}

	/* How to use it - a TMemo keeps Enter for the text, Tab for the buttons. */
	snprintf(buf, sizeof buf, "Entree = nouvelle ligne, Tab = boutons, "
	    "Echap = annuler%s",
	    (wr.n > MEMO_ROWS) ? "   (defile)" : "");
	screen_write_cursormove(&ctx, 2, 2 + MEMO_ROWS, 0);
	screen_write_puts(&ctx, &hint, "%.*s", (int)fieldw, buf);

	form_draw_button(&ctx, (W / 2) - 12, H - 3, "OK", md->focus == 1,
	    md->pressed == 1);
	form_draw_button(&ctx, (W / 2) + 2, H - 3, "Annuler", md->focus == 2,
	    md->pressed == 2);
	screen_write_stop(&ctx);

	/*
	 * Blit to the terminal, like every other dialog here: an overlay draw
	 * callback that only writes into its own screen shows NOTHING - the
	 * box still takes the keys, which makes the omission easy to miss.
	 * overlay_check is turned off while blitting so the box may paint its
	 * own rectangle.
	 */
	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	for (i = 0; i < H; i++) {
		tty_draw_line(&c->tty, s, 0, i, W, md->px, md->py + i,
		    &grid_default_cell, NULL);
	}
	form_draw_shadow(c, md->px + W, md->py + 1, 2, H - 1);
	form_draw_shadow(c, md->px + 2, md->py + H, W, 1);
	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

/*
 * Hand the announcement to the agent as a real PASTE, not as typed keys: a
 * typed newline is "send" for Claude Code, so a multi-line message would be
 * cut into as many messages. Bracketed paste keeps it whole, and the Enter
 * that follows (delayed, like every injection here) sends it once.
 */
static void
memo_send(struct memo_dialog *md)
{
	struct window_pane	*wp = window_pane_find_by_id(md->wp_id);
	struct claude_enter	*ce;
	struct timeval		 tv = { 0, 600000 };
	int			 bracket;

	if (wp == NULL || wp->fd == -1 || md->len == 0)
		return;
	bracket = (wp->screen->mode & MODE_BRACKETPASTE);
	if (bracket)
		bufferevent_write(wp->event, "\033[200~", 6);
	bufferevent_write(wp->event, md->text, md->len);
	if (bracket)
		bufferevent_write(wp->event, "\033[201~", 6);

	ce = xmalloc(sizeof *ce);
	ce->wp_id = wp->id;
	evtimer_set(&ce->ev, claude_enter_cb, ce);
	evtimer_add(&ce->ev, &tv);
}

static void
memo_flash_cb(__unused int fd, __unused short events, void *data)
{
	struct memo_dialog	*md = data;
	struct client		*c = md->c;
	int			 which = md->pressed;

	md->pressed = 0;
	if (which == 1)
		memo_send(md);
	server_client_clear_overlay(c);
}

/* Press a button: show it sunk for a moment, then act (TVision TButton). */
static void
memo_press(struct memo_dialog *md, int which)
{
	struct timeval	tv = { 0, 100000 };

	md->pressed = which;
	md->c->flags |= CLIENT_REDRAWOVERLAY;
	if (event_initialized(&md->flash))
		evtimer_del(&md->flash);
	evtimer_set(&md->flash, memo_flash_cb, md);
	evtimer_add(&md->flash, &tv);
}

static int
memo_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct memo_dialog	*md = data;
	struct mouse_event	*m = &event->m;
	key_code		 key = event->key;
	struct memo_wrap	 wr;
	u_int			 fieldw = md->w - 4, crow, i;
	u_int			 okx, cancelx;

	if (md->pressed != 0)
		return (0);			/* a button is flashing */

	if (KEYC_IS_MOUSE(key)) {
		u_int	lx, ly;

		if (MOUSE_DRAG(m->b) && MOUSE_RELEASE(m->b))
			return (0);		/* hover */
		if (key == KEYC_DOUBLECLICK)
			return (0);
		if (m->x < md->px || m->x >= md->px + md->w ||
		    m->y < md->py || m->y >= md->py + md->h) {
			if (MOUSE_RELEASE(m->b))
				return (0);
			return (1);		/* click outside closes */
		}
		lx = m->x - md->px;
		ly = m->y - md->py;
		if (MOUSE_RELEASE(m->b))
			return (0);
		okx = (md->w / 2) - 12;
		cancelx = (md->w / 2) + 2;
		if (ly == md->h - 3) {
			if (lx >= okx && lx < okx + 6) {
				md->focus = 1;
				memo_press(md, 1);
				return (0);
			}
			if (lx >= cancelx && lx < cancelx + 11) {
				md->focus = 2;
				memo_press(md, 2);
				return (0);
			}
		}
		/* Click in the text: put the cursor there. */
		if (ly >= 2 && ly < 2 + MEMO_ROWS && lx >= 2 &&
		    lx < 2 + fieldw) {
			u_int	row;

			memo_layout(md, fieldw, &wr);
			row = md->scroll + (ly - 2);
			if (row < wr.n) {
				size_t	p = wr.start[row];
				u_int	col = 0;

				while (p < wr.end[row] && col < lx - 2) {
					p++;
					while (p < wr.end[row] &&
					    ((u_char)md->text[p] & 0xc0) == 0x80)
						p++;
					col++;
				}
				md->cursor = p;
				md->focus = 0;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
		}
		return (0);
	}

	switch (key & KEYC_MASK_KEY) {
	case '\033':				/* Escape: cancel */
		return (1);
	case '\t':
		md->focus = (md->focus + 1) % 3;
		break;
	case KEYC_BTAB:
		md->focus = (md->focus + 2) % 3;
		break;
	case '\r':
	case '\n':
		if (md->focus == 1) {
			memo_press(md, 1);
			return (0);
		}
		if (md->focus == 2)
			return (1);
		memo_insert(md, "\n", 1);
		break;
	case KEYC_LEFT:
		memo_left(md);
		md->focus = 0;
		break;
	case KEYC_RIGHT:
		memo_right(md);
		md->focus = 0;
		break;
	case KEYC_UP:
	case KEYC_DOWN: {
		u_int	col = 0;
		size_t	p;

		memo_layout(md, fieldw, &wr);
		crow = memo_cursor_row(md, &wr);
		for (p = wr.start[crow]; p < md->cursor; p++) {
			if (((u_char)md->text[p] & 0xc0) != 0x80)
				col++;
		}
		if ((key & KEYC_MASK_KEY) == KEYC_UP) {
			if (crow == 0)
				break;
			crow--;
		} else {
			if (crow + 1 >= wr.n)
				break;
			crow++;
		}
		p = wr.start[crow];
		for (i = 0; i < col && p < wr.end[crow]; i++) {
			p++;
			while (p < wr.end[crow] &&
			    ((u_char)md->text[p] & 0xc0) == 0x80)
				p++;
		}
		md->cursor = p;
		md->focus = 0;
		break;
	}
	case KEYC_HOME:
		memo_layout(md, fieldw, &wr);
		md->cursor = wr.start[memo_cursor_row(md, &wr)];
		break;
	case KEYC_END:
		memo_layout(md, fieldw, &wr);
		md->cursor = wr.end[memo_cursor_row(md, &wr)];
		break;
	case KEYC_BSPACE:
	case '\b':
	case 0x7f:
		memo_backspace(md);
		md->focus = 0;
		break;
	case KEYC_DC:
		memo_delete(md);
		md->focus = 0;
		break;
	default:
		if (key & (KEYC_CTRL|KEYC_META))
			break;
		{
			key_code		k = key & KEYC_MASK_KEY;
			struct utf8_data	ud;

			/* Same decoding as the status prompt (status.c). */
			if (k >= 0x20 && k <= 0x7f)
				utf8_set(&ud, k);
			else if (KEYC_IS_UNICODE(key))
				utf8_to_data(key, &ud);
			else
				break;
			memo_insert(md, (char *)ud.data, ud.size);
			md->focus = 0;
		}
		break;
	}
	c->flags |= CLIENT_REDRAWOVERLAY;
	return (0);
}

/*
 * CLAUDE: the wording PROPOSED in the box - who this conversation is and who
 * else is around. Only a starting point: the user edits it before sending.
 */
void
claude_announce_default(struct window_pane *wp, char *buf, size_t len)
{
	struct window		*w = wp->window;
	struct window_pane	*loop;
	const char		*me = claude_agent_name(wp), *a;
	char			 roster[1024] = "";

	TAILQ_FOREACH(loop, &w->panes, entry) {
		if (loop == wp || (a = claude_agent_name(loop)) == NULL)
			continue;
		if (*roster != '\0')
			strlcat(roster, ", ", sizeof roster);
		strlcat(roster, a, sizeof roster);
	}
	snprintf(buf, len,
	    "[tmuxv] Tu es l'agent \"%s\". Agents presents : %s. "
	    "Ecris-leur avec send_message (to: leur nom, ou \"*\" pour tous) ; "
	    "leurs messages arriveront ici tout seuls, tu n'as rien a lancer "
	    "ni a surveiller.",
	    (me != NULL) ? me : "?",
	    (*roster != '\0') ? roster : "aucun autre pour le moment");
}

/* CLAUDE: open the announcement text box for conversation `wp`. */
void
claude_announce_box(struct client *c, struct window_pane *wp, const char *init)
{
	struct memo_dialog	*md;
	u_int			 w, h;

	w = 64;
	if (w > c->tty.sx - 4)
		w = c->tty.sx - 4;
	h = MEMO_ROWS + 6;
	if (c->tty.sx < w + 2 || c->tty.sy < h + 2)
		return;

	md = xcalloc(1, sizeof *md);
	md->c = c;
	md->wp_id = wp->id;
	md->len = (init != NULL) ? strlen(init) : 0;
	md->alloc = md->len + 256;
	md->text = xmalloc(md->alloc);
	if (init != NULL)
		memcpy(md->text, init, md->len);
	md->text[md->len] = '\0';
	md->cursor = md->len;
	md->w = w;
	md->h = h;
	screen_init(&md->s, w, h, 0);
	md->s.mode &= ~MODE_CURSOR;
	md->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	md->px = (c->tty.sx - w) / 2;
	md->py = (c->tty.sy - h) / 2;
	server_client_set_overlay(c, 0, memo_check_cb, memo_mode_cb,
	    memo_draw_cb, memo_key_cb, memo_free_cb, NULL, md);
}

/*
 * CLAUDE: "claude-announce" - inject an announcement into a conversation. The
 * user triggers it (conversation menu -> Annoncer) and edits the wording in the
 * prompt first; nothing is ever announced on its own.
 */
static enum cmd_retval
cmd_claude_announce_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct client		*c = cmdq_get_client(item);
	const char		*text = args_string(args, 0);

	if (wp == NULL)
		return (CMD_RETURN_NORMAL);
	if (!claude_pane_is_agent(wp)) {
		cmdq_error(item, "cette conversation ne fait pas tourner "
		    "d'agent claude");
		return (CMD_RETURN_ERROR);
	}
	/*
	 * No text given: open the multi-line box, prefilled with a proposal.
	 * With a text: send it as is (scriptable, and what the tests use).
	 */
	if (text == NULL || *text == '\0') {
		char	def[900];

		if (c == NULL)
			return (CMD_RETURN_NORMAL);
		claude_announce_default(wp, def, sizeof def);
		claude_announce_box(c, wp, def);
		return (CMD_RETURN_NORMAL);
	}
	claude_inject(wp, text);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval	cmd_claude_session_exec(struct cmd *,
			    struct cmdq_item *);

/*
 * `claude-session -r <uuid>` brings a saved conversation back (what a double
 * click on the "Reprendre" list does) and `claude-session -d <uuid>` deletes
 * its transcript for good. The uuid is looked up in the scanned list, so a
 * stale menu entry fails cleanly instead of unlinking whatever was passed.
 */
const struct cmd_entry cmd_claude_session_entry = {
	.name = "claude-session",
	.alias = NULL,

	.args = { "dr", 1, 1, NULL },
	.usage = "[-d|-r] uuid",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_claude_session_exec
};

static enum cmd_retval
cmd_claude_session_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	struct client	*c = cmdq_get_client(item);
	const char	*id = args_string(args, 0);
	u_int		 i, n = claude_sess_count();

	if (id == NULL || *id == '\0')
		return (CMD_RETURN_ERROR);
	for (i = 0; i < n; i++) {
		if (strcmp(claude_sess_id(i), id) == 0)
			break;
	}
	if (i == n) {
		cmdq_error(item, "conversation introuvable : %s", id);
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'd')) {
		if (!claude_sess_delete(i)) {
			cmdq_error(item, "suppression impossible : %s", id);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}
	if (c != NULL)
		claude_resume(c, i);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_claude_announce_entry = {
	.name = "claude-announce",
	.alias = NULL,

	.args = { "t:", 0, 1, NULL },
	.usage = "[-t target-pane] [text]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_claude_announce_exec
};

const struct cmd_entry cmd_claude_rename_entry = {
	.name = "claude-rename",
	.alias = NULL,

	.args = { "t:", 1, 1, NULL },
	.usage = "[-t target-pane] name",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_claude_rename_exec
};

const struct cmd_entry cmd_claude_entry = {
	.name = "claude-manager",
	.alias = NULL,

	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_claude_exec
};

/* --- text builders --- */

/*
 * The version of the PACKAGE (packaging/src/control), compiled in: the About
 * box of a running server shows what it really runs, hot upgrades included.
 * The tag lets packaging/build.sh check a binary before packaging it.
 */
const char	tmuxv_version_tag[] = "@(#)tmuxv " TMUXV_VERSION " @";

static char *
about_text(void)
{
	static const char	*months[] = { "Jan", "janvier", "Feb", "fevrier",
				    "Mar", "mars", "Apr", "avril", "May", "mai",
				    "Jun", "juin", "Jul", "juillet", "Aug", "aout",
				    "Sep", "septembre", "Oct", "octobre", "Nov",
				    "novembre", "Dec", "decembre" };
	const char		*month = __DATE__;
	char			 built[64], *s;
	u_int			 i;

	/* __DATE__ is "Sep 12 2026": the same date, the French way. */
	for (i = 0; i < nitems(months); i += 2) {
		if (strncmp(__DATE__, months[i], 3) == 0) {
			month = months[i + 1];
			break;
		}
	}
	if (month != __DATE__) {
		snprintf(built, sizeof built, "%d %s %s a %.5s",
		    atoi(__DATE__ + 4), month, __DATE__ + 7, __TIME__);
	} else
		snprintf(built, sizeof built, "%s %.5s", __DATE__, __TIME__);

	xasprintf(&s,
	    "\n"
	    "tmuxv " TMUXV_VERSION "\n"
	    "Compile le %s\n"
	    "\n"
	    "Interface facon Turbo Vision, incrustee dans tmux %s\n"
	    "\n"
	    "  . Barre de menu (souris + clavier)\n"
	    "  . Fenetres flottantes (deplacer / redimensionner)\n"
	    "  . Scrollbars, ombres portees, dialogues\n"
	    "  . Mode bureau + parametres dynamiques\n"
	    "\n"
	    "Usage personnel.\n",
	    built, getversion());
	return (s);
}

static char *
commands_text(void)
{
	char	*buf = NULL;
	size_t	 sz = 0;
	FILE	*f;
	u_int	 i;

	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (xstrdup("(erreur)"));
	for (i = 0; cmd_table[i] != NULL; i++) {
		const struct cmd_entry	*e = cmd_table[i];

		if (e->alias != NULL)
			fprintf(f, "%s (%s)\n", e->name, e->alias);
		else
			fprintf(f, "%s\n", e->name);
		if (e->usage != NULL && *e->usage != '\0')
			fprintf(f, "    %s\n", e->usage);
	}
	fclose(f);
	return (buf);
}

static char *
keys_text(void)
{
	char			*buf = NULL;
	size_t			 sz = 0;
	FILE			*f;
	struct key_table	*table;
	struct key_binding	*bd;

	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (xstrdup("(erreur)"));
	for (table = key_bindings_first_table(); table != NULL;
	    table = key_bindings_next_table(table)) {
		int	any = 0;

		for (bd = key_bindings_first(table); bd != NULL;
		    bd = key_bindings_next(table, bd)) {
			const char	*keyname;
			char		*cmdstr;

			keyname = key_string_lookup_key(bd->key, 0);
			cmdstr = cmd_list_print(bd->cmdlist, 0);
			fprintf(f, "%-11s %-14s %s\n", table->name,
			    keyname != NULL ? keyname : "?",
			    cmdstr != NULL ? cmdstr : "");
			free(cmdstr);
			any = 1;
		}
		if (any)
			fprintf(f, "\n");
	}
	fclose(f);
	return (buf);
}

/* --- commands: display-about / display-keys / display-commands --- */

static enum cmd_retval
cmd_about_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);
	char		*t;

	if (c != NULL) {
		t = about_text();
		msgbox_display(c, "A propos", t);
		free(t);
	}
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_about_entry = {
	.name = "display-about",
	.alias = "about",
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_about_exec
};

static enum cmd_retval
cmd_dkeys_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);
	char		*t;

	if (c != NULL) {
		t = keys_text();
		msgbox_display(c, "Raccourcis clavier", t);
		free(t);
	}
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dkeys_entry = {
	.name = "display-keys",
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dkeys_exec
};

static enum cmd_retval
cmd_dcommands_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);
	char		*t;

	if (c != NULL) {
		t = commands_text();
		msgbox_display(c, "Liste des commandes", t);
		free(t);
	}
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dcommands_entry = {
	.name = "display-commands",
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dcommands_exec
};

static char *
sessions_text(void)
{
	char		*buf = NULL;
	size_t		 sz = 0;
	FILE		*f;
	struct session	*s;

	if ((f = open_memstream(&buf, &sz)) == NULL)
		return (xstrdup("(erreur)"));
	RB_FOREACH(s, sessions, &sessions) {
		u_int	n = winlink_count(&s->windows);

		fprintf(f, "%-20s %u fenetre%s%s\n", s->name, n,
		    n > 1 ? "s" : "", s->attached ? "   (attachee)" : "");
	}
	fclose(f);
	if (buf == NULL || *buf == '\0') {
		free(buf);
		return (xstrdup("(aucune session)"));
	}
	return (buf);
}

static enum cmd_retval
cmd_dsessions_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);
	char		*t;

	if (c != NULL) {
		struct msgbox_data	*md;

		t = sessions_text();
		md = msgbox_display(c, "Sessions", t);
		free(t);
		if (md != NULL && !RB_EMPTY(&sessions)) {
			/*
			 * TListViewer: one selectable row per session (same
			 * order as sessions_text); the current one selected.
			 * Double-click / Enter switches to it; OK/Escape close.
			 */
			struct session	*s;
			u_int		 i = 0;

			md->list = 1;
			md->act_kind = MB_ACT_SESSION;
			md->keys = xcalloc(md->nlines, sizeof *md->keys);
			RB_FOREACH(s, sessions, &sessions) {
				if (i >= md->nlines)
					break;
				md->keys[i] = xstrdup(s->name);
				if (s == c->session)
					md->sel = (int)i;
				i++;
			}
			if (md->sel < 0)
				md->sel = 0;
			msgbox_show_sel(md);
		}
	}
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_dsessions_entry = {
	.name = "display-sessions",
	.alias = NULL,
	.args = { "c:", 0, 0, NULL },
	.usage = "[-c target-client]",
	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_dsessions_exec
};
