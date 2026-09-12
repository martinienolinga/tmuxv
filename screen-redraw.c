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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

static void	screen_redraw_draw_borders(struct screen_redraw_ctx *);
static void	screen_redraw_draw_panes(struct screen_redraw_ctx *);
static void	screen_redraw_draw_status(struct screen_redraw_ctx *);
static void	screen_redraw_draw_menubar(struct screen_redraw_ctx *);
static void	screen_redraw_draw_desktop(struct screen_redraw_ctx *, int);
static void	screen_redraw_claude_list(struct screen_write_ctx *,
		    struct client *, struct window *, u_int, u_int, u_int,
		    u_int);
static void	screen_redraw_draw_pane(struct screen_redraw_ctx *,
		    struct window_pane *);
static void	screen_redraw_set_context(struct client *,
		    struct screen_redraw_ctx *);

#define START_ISOLATE "\342\201\246"
#define END_ISOLATE   "\342\201\251"

/* Border in relation to a pane. */
enum screen_redraw_border_type {
	SCREEN_REDRAW_OUTSIDE,
	SCREEN_REDRAW_INSIDE,
	SCREEN_REDRAW_BORDER_LEFT,
	SCREEN_REDRAW_BORDER_RIGHT,
	SCREEN_REDRAW_BORDER_TOP,
	SCREEN_REDRAW_BORDER_BOTTOM
};
#define BORDER_MARKERS "  +,.-"

/* Get cell border character. */
static void
screen_redraw_border_set(struct window *w, struct window_pane *wp,
    enum pane_lines pane_lines, int cell_type, struct grid_cell *gc)
{
	u_int	idx;

	if (cell_type == CELL_OUTSIDE && w->fill_character != NULL) {
		utf8_copy(&gc->data, &w->fill_character[0]);
		return;
	}

	switch (pane_lines) {
	case PANE_LINES_NUMBER:
		if (cell_type == CELL_OUTSIDE) {
			gc->attr |= GRID_ATTR_CHARSET;
			utf8_set(&gc->data, CELL_BORDERS[CELL_OUTSIDE]);
			break;
		}
		gc->attr &= ~GRID_ATTR_CHARSET;
		if (wp != NULL && window_pane_index(wp, &idx) == 0)
			utf8_set(&gc->data, '0' + (idx % 10));
		else
			utf8_set(&gc->data, '*');
		break;
	case PANE_LINES_DOUBLE:
		gc->attr &= ~GRID_ATTR_CHARSET;
		utf8_copy(&gc->data, tty_acs_double_borders(cell_type));
		break;
	case PANE_LINES_HEAVY:
		gc->attr &= ~GRID_ATTR_CHARSET;
		utf8_copy(&gc->data, tty_acs_heavy_borders(cell_type));
		break;
	case PANE_LINES_SIMPLE:
		gc->attr &= ~GRID_ATTR_CHARSET;
		utf8_set(&gc->data, SIMPLE_BORDERS[cell_type]);
		break;
	default:
		gc->attr |= GRID_ATTR_CHARSET;
		utf8_set(&gc->data, CELL_BORDERS[cell_type]);
		break;
	}
}

/* Return if window has only two panes. */
static int
screen_redraw_two_panes(struct window *w, int direction)
{
	struct window_pane	*wp;

	wp = TAILQ_NEXT(TAILQ_FIRST(&w->panes), entry);
	if (wp == NULL)
		return (0); /* one pane */
	if (TAILQ_NEXT(wp, entry) != NULL)
		return (0); /* more than two panes */
	if (direction == 0 && wp->xoff == 0)
		return (0);
	if (direction == 1 && wp->yoff == 0)
		return (0);
	return (1);
}

/* Check if cell is on the border of a pane. */
static enum screen_redraw_border_type
screen_redraw_pane_border(struct window_pane *wp, u_int px, u_int py,
    int pane_status)
{
	struct options	*oo = wp->window->options;
	int		 split = 0;
	u_int	 	 ex = wp->xoff + wp->sx, ey = wp->yoff + wp->sy;

	/* Inside pane. */
	if (px >= wp->xoff && px < ex && py >= wp->yoff && py < ey)
		return (SCREEN_REDRAW_INSIDE);

	/* Get pane indicator. */
	switch (options_get_number(oo, "pane-border-indicators")) {
	case PANE_BORDER_COLOUR:
	case PANE_BORDER_BOTH:
		split = 1;
		break;
	}

	/* Left/right borders. */
	if (pane_status == PANE_STATUS_OFF) {
		if (screen_redraw_two_panes(wp->window, 0) && split) {
			if (wp->xoff == 0 && px == wp->sx && py <= wp->sy / 2)
				return (SCREEN_REDRAW_BORDER_RIGHT);
			if (wp->xoff != 0 &&
			    px == wp->xoff - 1 &&
			    py > wp->sy / 2)
				return (SCREEN_REDRAW_BORDER_LEFT);
		} else {
			if ((wp->yoff == 0 || py >= wp->yoff - 1) && py <= ey) {
				if (wp->xoff != 0 && px == wp->xoff - 1)
					return (SCREEN_REDRAW_BORDER_LEFT);
				if (px == ex)
					return (SCREEN_REDRAW_BORDER_RIGHT);
			}
		}
	} else {
		if ((wp->yoff == 0 || py >= wp->yoff - 1) && py <= ey) {
			if (wp->xoff != 0 && px == wp->xoff - 1)
				return (SCREEN_REDRAW_BORDER_LEFT);
			if (px == ex)
				return (SCREEN_REDRAW_BORDER_RIGHT);
		}
	}

	/* Top/bottom borders. */
	if (pane_status == PANE_STATUS_OFF) {
		if (screen_redraw_two_panes(wp->window, 1) && split) {
			if (wp->yoff == 0 && py == wp->sy && px <= wp->sx / 2)
				return (SCREEN_REDRAW_BORDER_BOTTOM);
			if (wp->yoff != 0 &&
			    py == wp->yoff - 1 &&
			    px > wp->sx / 2)
				return (SCREEN_REDRAW_BORDER_TOP);
		} else {
			if ((wp->xoff == 0 || px >= wp->xoff - 1) && px <= ex) {
				if (wp->yoff != 0 && py == wp->yoff - 1)
					return (SCREEN_REDRAW_BORDER_TOP);
				if (py == ey)
					return (SCREEN_REDRAW_BORDER_BOTTOM);
			}
		}
	} else if (pane_status == PANE_STATUS_TOP) {
		if ((wp->xoff == 0 || px >= wp->xoff - 1) && px <= ex) {
			if (wp->yoff != 0 && py == wp->yoff - 1)
				return (SCREEN_REDRAW_BORDER_TOP);
		}
	} else {
		if ((wp->xoff == 0 || px >= wp->xoff - 1) && px <= ex) {
			if (py == ey)
				return (SCREEN_REDRAW_BORDER_BOTTOM);
		}
	}

	/* Outside pane. */
	return (SCREEN_REDRAW_OUTSIDE);
}

/* Check if a cell is on a border. */
static int
screen_redraw_cell_border(struct client *c, u_int px, u_int py, int pane_status)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;

	/* Outside the window? */
	if (px > w->sx || py > w->sy)
		return (0);

	/* On the window border? */
	if (px == w->sx || py == w->sy)
		return (1);

	/* Check all the panes. */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		switch (screen_redraw_pane_border(wp, px, py, pane_status)) {
		case SCREEN_REDRAW_INSIDE:
			return (0);
		case SCREEN_REDRAW_OUTSIDE:
			break;
		default:
			return (1);
		}
	}

	return (0);
}

/* Work out type of border cell from surrounding cells. */
static int
screen_redraw_type_of_cell(struct client *c, u_int px, u_int py,
    int pane_status)
{
	struct window	*w = c->session->curw->window;
	u_int		 sx = w->sx, sy = w->sy;
	int		 borders = 0;

	/* Is this outside the window? */
	if (px > sx || py > sy)
		return (CELL_OUTSIDE);

	/*
	 * Construct a bitmask of whether the cells to the left (bit 4), right,
	 * top, and bottom (bit 1) of this cell are borders.
	 */
	if (px == 0 || screen_redraw_cell_border(c, px - 1, py, pane_status))
		borders |= 8;
	if (px <= sx && screen_redraw_cell_border(c, px + 1, py, pane_status))
		borders |= 4;
	if (pane_status == PANE_STATUS_TOP) {
		if (py != 0 &&
		    screen_redraw_cell_border(c, px, py - 1, pane_status))
			borders |= 2;
		if (screen_redraw_cell_border(c, px, py + 1, pane_status))
			borders |= 1;
	} else if (pane_status == PANE_STATUS_BOTTOM) {
		if (py == 0 ||
		    screen_redraw_cell_border(c, px, py - 1, pane_status))
			borders |= 2;
		if (py != sy - 1 &&
		    screen_redraw_cell_border(c, px, py + 1, pane_status))
			borders |= 1;
	} else {
		if (py == 0 ||
		    screen_redraw_cell_border(c, px, py - 1, pane_status))
			borders |= 2;
		if (screen_redraw_cell_border(c, px, py + 1, pane_status))
			borders |= 1;
	}

	/*
	 * Figure out what kind of border this cell is. Only one bit set
	 * doesn't make sense (can't have a border cell with no others
	 * connected).
	 */
	switch (borders) {
	case 15:	/* 1111, left right top bottom */
		return (CELL_JOIN);
	case 14:	/* 1110, left right top */
		return (CELL_BOTTOMJOIN);
	case 13:	/* 1101, left right bottom */
		return (CELL_TOPJOIN);
	case 12:	/* 1100, left right */
		return (CELL_LEFTRIGHT);
	case 11:	/* 1011, left top bottom */
		return (CELL_RIGHTJOIN);
	case 10:	/* 1010, left top */
		return (CELL_BOTTOMRIGHT);
	case 9:		/* 1001, left bottom */
		return (CELL_TOPRIGHT);
	case 7:		/* 0111, right top bottom */
		return (CELL_LEFTJOIN);
	case 6:		/* 0110, right top */
		return (CELL_BOTTOMLEFT);
	case 5:		/* 0101, right bottom */
		return (CELL_TOPLEFT);
	case 3:		/* 0011, top bottom */
		return (CELL_TOPBOTTOM);
	}
	return (CELL_OUTSIDE);
}

/* Check if cell inside a pane. */
static int
screen_redraw_check_cell(struct client *c, u_int px, u_int py, int pane_status,
    struct window_pane **wpp)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp, *active;
	int			 border;
	u_int			 right, line;

	*wpp = NULL;

	if (px > w->sx || py > w->sy)
		return (CELL_OUTSIDE);
	if (px == w->sx || py == w->sy) /* window border */
		return (screen_redraw_type_of_cell(c, px, py, pane_status));

	if (pane_status != PANE_STATUS_OFF) {
		active = wp = server_client_get_pane(c);
		do {
			if (!window_pane_visible(wp))
				goto next1;

			if (pane_status == PANE_STATUS_TOP)
				line = wp->yoff - 1;
			else
				line = wp->yoff + wp->sy;
			right = wp->xoff + 2 + wp->status_size - 1;

			if (py == line && px >= wp->xoff + 2 && px <= right)
				return (CELL_INSIDE);

		next1:
			wp = TAILQ_NEXT(wp, entry);
			if (wp == NULL)
				wp = TAILQ_FIRST(&w->panes);
		} while (wp != active);
	}

	active = wp = server_client_get_pane(c);
	do {
		if (!window_pane_visible(wp))
			goto next2;
		*wpp = wp;

		/*
		 * If definitely inside, return. If not on border, skip.
		 * Otherwise work out the cell.
		 */
		border = screen_redraw_pane_border(wp, px, py, pane_status);
		if (border == SCREEN_REDRAW_INSIDE)
			return (CELL_INSIDE);
		if (border == SCREEN_REDRAW_OUTSIDE)
			goto next2;
		return (screen_redraw_type_of_cell(c, px, py, pane_status));

	next2:
		wp = TAILQ_NEXT(wp, entry);
		if (wp == NULL)
			wp = TAILQ_FIRST(&w->panes);
	} while (wp != active);

	return (CELL_OUTSIDE);
}

/* Check if the border of a particular pane. */
static int
screen_redraw_check_is(u_int px, u_int py, int pane_status,
    struct window_pane *wp)
{
	enum screen_redraw_border_type	border;

	border = screen_redraw_pane_border(wp, px, py, pane_status);
	if (border != SCREEN_REDRAW_INSIDE && border != SCREEN_REDRAW_OUTSIDE)
		return (1);
	return (0);
}

/* Update pane status. */
static int
screen_redraw_make_pane_status(struct client *c, struct window_pane *wp,
    struct screen_redraw_ctx *rctx, enum pane_lines pane_lines)
{
	struct window		*w = wp->window;
	struct grid_cell	 gc;
	const char		*fmt;
	struct format_tree	*ft;
	char			*expanded;
	int			 pane_status = rctx->pane_status;
	u_int			 width, i, cell_type, px, py;
	struct screen_write_ctx	 ctx;
	struct screen		 old;

	ft = format_create(c, NULL, FORMAT_PANE|wp->id, FORMAT_STATUS);
	format_defaults(ft, c, c->session, c->session->curw, wp);

	if (wp == server_client_get_pane(c))
		style_apply(&gc, w->options, "pane-active-border-style", ft);
	else
		style_apply(&gc, w->options, "pane-border-style", ft);
	fmt = options_get_string(wp->options, "pane-border-format");

	expanded = format_expand_time(ft, fmt);
	if (wp->sx < 4)
		wp->status_size = width = 0;
	else
		wp->status_size = width = wp->sx - 4;

	memcpy(&old, &wp->status_screen, sizeof old);
	screen_init(&wp->status_screen, width, 1, 0);
	wp->status_screen.mode = 0;

	screen_write_start(&ctx, &wp->status_screen);

	for (i = 0; i < width; i++) {
		px = wp->xoff + 2 + i;
		if (rctx->pane_status == PANE_STATUS_TOP)
			py = wp->yoff - 1;
		else
			py = wp->yoff + wp->sy;
		cell_type = screen_redraw_type_of_cell(c, px, py, pane_status);
		screen_redraw_border_set(w, wp, pane_lines, cell_type, &gc);
		screen_write_cell(&ctx, &gc);
	}
	gc.attr &= ~GRID_ATTR_CHARSET;

	screen_write_cursormove(&ctx, 0, 0, 0);
	format_draw(&ctx, &gc, width, expanded, NULL, 0);
	screen_write_stop(&ctx);

	free(expanded);
	format_free(ft);

	if (grid_compare(wp->status_screen.grid, old.grid) == 0) {
		screen_free(&old);
		return (0);
	}
	screen_free(&old);
	return (1);
}

/* Draw pane status. */
static void
screen_redraw_draw_pane_status(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct window		*w = c->session->curw->window;
	struct tty		*tty = &c->tty;
	struct window_pane	*wp;
	struct screen		*s;
	u_int			 i, x, width, xoff, yoff, size;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		s = &wp->status_screen;

		size = wp->status_size;
		if (ctx->pane_status == PANE_STATUS_TOP)
			yoff = wp->yoff - 1;
		else
			yoff = wp->yoff + wp->sy;
		xoff = wp->xoff + 2;

		if (xoff + size <= ctx->ox ||
		    xoff >= ctx->ox + ctx->sx ||
		    yoff < ctx->oy ||
		    yoff >= ctx->oy + ctx->sy)
			continue;

		if (xoff >= ctx->ox && xoff + size <= ctx->ox + ctx->sx) {
			/* All visible. */
			i = 0;
			x = xoff - ctx->ox;
			width = size;
		} else if (xoff < ctx->ox && xoff + size > ctx->ox + ctx->sx) {
			/* Both left and right not visible. */
			i = ctx->ox;
			x = 0;
			width = ctx->sx;
		} else if (xoff < ctx->ox) {
			/* Left not visible. */
			i = ctx->ox - xoff;
			x = 0;
			width = size - i;
		} else {
			/* Right not visible. */
			i = 0;
			x = xoff - ctx->ox;
			width = size - x;
		}

		if (ctx->statustop)
			yoff += ctx->statuslines;
		yoff += ctx->menubar; /* MENU BAR */
		tty_draw_line(tty, s, i, 0, width, x, yoff - ctx->oy,
		    &grid_default_cell, NULL);
	}
	tty_cursor(tty, 0, 0);
}

/* Update status line and change flags if unchanged. */
static int
screen_redraw_update(struct client *c, int flags)
{
	struct window			*w = c->session->curw->window;
	struct window_pane		*wp;
	struct options			*wo = w->options;
	int				 redraw;
	enum pane_lines			 lines;
	struct screen_redraw_ctx	 ctx;

	if (c->message_string != NULL)
		redraw = status_message_redraw(c);
	else if (c->prompt_string != NULL && !c->prompt_dialog)
		redraw = status_prompt_redraw(c);
	else
		redraw = status_redraw(c);	/* prompt in a box: keep the bar */
	if (!redraw && (~flags & CLIENT_REDRAWSTATUSALWAYS))
		flags &= ~CLIENT_REDRAWSTATUS;

	if (c->overlay_draw != NULL)
		flags |= CLIENT_REDRAWOVERLAY;

	if (options_get_number(wo, "pane-border-status") != PANE_STATUS_OFF) {
		screen_redraw_set_context(c, &ctx);
		lines = options_get_number(wo, "pane-border-lines");
		redraw = 0;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (screen_redraw_make_pane_status(c, wp, &ctx, lines))
				redraw = 1;
		}
		if (redraw)
			flags |= CLIENT_REDRAWBORDERS;
	}
	return (flags);
}

/* Set up redraw context. */
static void
screen_redraw_set_context(struct client *c, struct screen_redraw_ctx *ctx)
{
	struct session	*s = c->session;
	struct options	*oo = s->options;
	struct window	*w = s->curw->window;
	struct options	*wo = w->options;
	u_int		 lines;

	memset(ctx, 0, sizeof *ctx);
	ctx->c = c;

	lines = status_line_size(c);
	if (c->message_string != NULL ||
	    (c->prompt_string != NULL && !c->prompt_dialog))
		lines = (lines == 0) ? 1 : lines;
	if (lines != 0 && options_get_number(oo, "status-position") == 0)
		ctx->statustop = 1;
	ctx->statuslines = lines;

	ctx->menubar = menu_bar_size(c); /* MENU BAR: reserved top line */
	ctx->desktop_top = desktop_top(c);   /* DESKTOP: inset */
	ctx->desktop_left = desktop_left(c);
	ctx->desktop_vert = desktop_vert(c);
	ctx->desktop_horiz = desktop_horiz(c);

	ctx->pane_status = options_get_number(wo, "pane-border-status");
	ctx->pane_lines = options_get_number(wo, "pane-border-lines");

	tty_window_offset(&c->tty, &ctx->ox, &ctx->oy, &ctx->sx, &ctx->sy);

	log_debug("%s: %s @%u ox=%u oy=%u sx=%u sy=%u %u/%d", __func__, c->name,
	    w->id, ctx->ox, ctx->oy, ctx->sx, ctx->sy, ctx->statuslines,
	    ctx->statustop);
}

/* Redraw entire screen. */
void
screen_redraw_screen(struct client *c)
{
	struct screen_redraw_ctx	ctx;
	int				flags;

	if (c->flags & CLIENT_SUSPENDED)
		return;

	flags = screen_redraw_update(c, c->flags);
	if ((flags & CLIENT_ALLREDRAWFLAGS) == 0)
		return;

	screen_redraw_set_context(c, &ctx);
	tty_sync_start(&c->tty);
	tty_update_mode(&c->tty, c->tty.mode, NULL);

	if (flags & (CLIENT_REDRAWWINDOW|CLIENT_REDRAWBORDERS)) {
		log_debug("%s: redrawing borders", c->name);
		if (desktop_enabled(c)) {
			/*
			 * DESKTOP: compose the whole desktop (hatch, all window
			 * frames and content) and send only the changed lines
			 * (damage). full = repaint everything (resize/move).
			 */
			screen_redraw_draw_desktop(&ctx,
			    (flags & CLIENT_REDRAWBORDERS) != 0);
		} else {
			if (ctx.pane_status != PANE_STATUS_OFF)
				screen_redraw_draw_pane_status(&ctx);
			screen_redraw_draw_borders(&ctx);
		}
	}
	if ((flags & CLIENT_REDRAWWINDOW) && !desktop_enabled(c)) {
		log_debug("%s: redrawing panes", c->name);
		screen_redraw_draw_panes(&ctx);
	}
	if (ctx.statuslines != 0 &&
	    (flags & (CLIENT_REDRAWSTATUS|CLIENT_REDRAWSTATUSALWAYS))) {
		log_debug("%s: redrawing status", c->name);
		screen_redraw_draw_status(&ctx);
	}
	/*
	 * MENU BAR: static content, so only redraw it on a borders redraw
	 * (startup/resize), never on window-content or status updates - that
	 * avoids it flickering while windows refresh.
	 */
	if (ctx.menubar != 0 && (flags & CLIENT_REDRAWBORDERS)) {
		log_debug("%s: redrawing menu bar", c->name);
		screen_redraw_draw_menubar(&ctx);
	}
	if (c->overlay_draw != NULL && (flags & CLIENT_REDRAWOVERLAY)) {
		log_debug("%s: redrawing overlay", c->name);
		c->overlay_draw(c, c->overlay_data, &ctx);
	}

	tty_reset(&c->tty);
}

/* Redraw a single pane. */
void
screen_redraw_pane(struct client *c, struct window_pane *wp)
{
	struct screen_redraw_ctx	 ctx;

	if (!window_pane_visible(wp))
		return;

	screen_redraw_set_context(c, &ctx);
	tty_sync_start(&c->tty);
	tty_update_mode(&c->tty, c->tty.mode, NULL);

	screen_redraw_draw_pane(&ctx, wp);

	tty_reset(&c->tty);
}

/* Get border cell style. */
static const struct grid_cell *
screen_redraw_draw_borders_style(struct screen_redraw_ctx *ctx, u_int x,
    u_int y, struct window_pane *wp)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct window		*w = s->curw->window;
	struct window_pane	*active = server_client_get_pane(c);
	struct options		*oo = w->options;
	struct format_tree	*ft;

	if (wp->border_gc_set)
		return (&wp->border_gc);
	wp->border_gc_set = 1;

	ft = format_create_defaults(NULL, c, s, s->curw, wp);
	if (screen_redraw_check_is(x, y, ctx->pane_status, active))
		style_apply(&wp->border_gc, oo, "pane-active-border-style", ft);
	else
		style_apply(&wp->border_gc, oo, "pane-border-style", ft);
	format_free(ft);

	return (&wp->border_gc);
}

/* Draw a border cell. */
static void
screen_redraw_draw_borders_cell(struct screen_redraw_ctx *ctx, u_int i, u_int j)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct window		*w = s->curw->window;
	struct options		*oo = w->options;
	struct tty		*tty = &c->tty;
	struct format_tree	*ft;
	struct window_pane	*wp, *active = server_client_get_pane(c);
	struct grid_cell	 gc;
	const struct grid_cell	*tmp;
	struct overlay_ranges	 r;
	u_int			 cell_type, x = ctx->ox + i, y = ctx->oy + j;
	int			 arrows = 0, border;
	int			 pane_status = ctx->pane_status, isolates;

	if (c->overlay_check != NULL) {
		c->overlay_check(c, c->overlay_data, x, y, 1, &r);
		if (r.nx[0] + r.nx[1] == 0)
			return;
	}

	cell_type = screen_redraw_check_cell(c, x, y, pane_status, &wp);
	if (cell_type == CELL_INSIDE)
		return;

	if (wp == NULL) {
		if (!ctx->no_pane_gc_set) {
			ft = format_create_defaults(NULL, c, s, s->curw, NULL);
			memcpy(&ctx->no_pane_gc, &grid_default_cell, sizeof gc);
			style_add(&ctx->no_pane_gc, oo, "pane-border-style",
			    ft);
			format_free(ft);
			ctx->no_pane_gc_set = 1;
		}
		memcpy(&gc, &ctx->no_pane_gc, sizeof gc);
	} else {
		tmp = screen_redraw_draw_borders_style(ctx, x, y, wp);
		if (tmp == NULL)
			return;
		memcpy(&gc, tmp, sizeof gc);

		if (server_is_marked(s, s->curw, marked_pane.wp) &&
		    screen_redraw_check_is(x, y, pane_status, marked_pane.wp))
			gc.attr ^= GRID_ATTR_REVERSE;
	}
	screen_redraw_border_set(w, wp, ctx->pane_lines, cell_type, &gc);

	if (cell_type == CELL_TOPBOTTOM &&
	    (c->flags & CLIENT_UTF8) &&
	    tty_term_has(tty->term, TTYC_BIDI))
		isolates = 1;
	else
		isolates = 0;

	if (ctx->statustop)
		tty_cursor(tty, i + ctx->desktop_left,
		    ctx->menubar + ctx->desktop_top + ctx->statuslines + j);
	else
		tty_cursor(tty, i + ctx->desktop_left,
		    ctx->menubar + ctx->desktop_top + j);
	if (isolates)
		tty_puts(tty, END_ISOLATE);

	switch (options_get_number(oo, "pane-border-indicators")) {
	case PANE_BORDER_ARROWS:
	case PANE_BORDER_BOTH:
		arrows = 1;
		break;
	}

	if (wp != NULL && arrows) {
		border = screen_redraw_pane_border(active, x, y, pane_status);
		if (((i == wp->xoff + 1 &&
		    (cell_type == CELL_LEFTRIGHT ||
		    (cell_type == CELL_TOPJOIN &&
		    border == SCREEN_REDRAW_BORDER_BOTTOM) ||
		    (cell_type == CELL_BOTTOMJOIN &&
		    border == SCREEN_REDRAW_BORDER_TOP))) ||
		    (j == wp->yoff + 1 &&
		    (cell_type == CELL_TOPBOTTOM ||
		    (cell_type == CELL_LEFTJOIN &&
		    border == SCREEN_REDRAW_BORDER_RIGHT) ||
		    (cell_type == CELL_RIGHTJOIN &&
		    border == SCREEN_REDRAW_BORDER_LEFT)))) &&
		    screen_redraw_check_is(x, y, pane_status, active)) {
			gc.attr |= GRID_ATTR_CHARSET;
			utf8_set(&gc.data, BORDER_MARKERS[border]);
		}
	}

	tty_cell(tty, &gc, &grid_default_cell, NULL, NULL);
	if (isolates)
		tty_puts(tty, START_ISOLATE);
}

/* Draw the borders. */
static void
screen_redraw_draw_borders(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct window		*w = s->curw->window;
	struct window_pane	*wp;
	u_int		 	 i, j;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	TAILQ_FOREACH(wp, &w->panes, entry)
		wp->border_gc_set = 0;

	for (j = 0; j < c->tty.sy - ctx->statuslines - ctx->menubar -
	    ctx->desktop_vert; j++) {
		for (i = 0; i < c->tty.sx - ctx->desktop_horiz; i++)
			screen_redraw_draw_borders_cell(ctx, i, j);
	}
}

/* Draw the panes. */
static void
screen_redraw_draw_panes(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (window_pane_visible(wp))
			screen_redraw_draw_pane(ctx, wp);
	}
}

/* Draw the status line. */
static void
screen_redraw_draw_status(struct screen_redraw_ctx *ctx)
{
	struct client	*c = ctx->c;
	struct window	*w = c->session->curw->window;
	struct tty	*tty = &c->tty;
	struct screen	*s = c->status.active;
	u_int		 i, y;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	if (ctx->statustop)
		y = ctx->menubar; /* MENU BAR: status sits below the menu bar */
	else
		y = c->tty.sy - ctx->statuslines;
	for (i = 0; i < ctx->statuslines; i++) {
		tty_draw_line(tty, s, 0, i, UINT_MAX, 0, y + i,
		    &grid_default_cell, NULL);
	}
}

/*
 * DESKTOP: draw the Turbo Vision desktop (solid blue) and a framed, titled
 * window box around the inset pane area.
 */
/* DESKTOP: draw one window's frame + shadow (+ content) into the area screen. */
static void
screen_redraw_desktop_one(struct client *c, struct screen_write_ctx *sctx,
    u_int area_h, u_int sx, struct window *w, u_int rx, u_int ry, u_int rw,
    u_int rh, int content)
{
	struct grid_cell	 framegc, shadowgc;
	struct window_pane	*wp;
	char			*title;
	u_int			 i, j, cw, ch;

	/*
	 * Drop shadow: darken whatever is already underneath (Turbo Vision
	 * style) rather than painting a solid black band - keep the cell's
	 * character but dim its colours.
	 */
	for (j = ry + 1; j < ry + rh && j < area_h; j++) {
		for (i = rx + rw; i < rx + rw + 2 && i < sx; i++) {
			grid_view_get_cell(sctx->s->grid, i, j, &shadowgc);
			shadowgc.fg = colour_join_rgb(0x40, 0x40, 0x40);
			shadowgc.bg = colour_join_rgb(0x00, 0x00, 0x00);
			shadowgc.attr &= ~GRID_ATTR_BRIGHT;
			screen_write_cursormove(sctx, i, j, 0);
			screen_write_cell(sctx, &shadowgc);
		}
	}
	if (ry + rh < area_h) {
		for (i = rx + 2; i < rx + rw + 2 && i < sx; i++) {
			grid_view_get_cell(sctx->s->grid, i, ry + rh, &shadowgc);
			shadowgc.fg = colour_join_rgb(0x40, 0x40, 0x40);
			shadowgc.bg = colour_join_rgb(0x00, 0x00, 0x00);
			shadowgc.attr &= ~GRID_ATTR_BRIGHT;
			screen_write_cursormove(sctx, i, ry + rh, 0);
			screen_write_cell(sctx, &shadowgc);
		}
	}

	/* Frame box with a centred title. */
	memcpy(&framegc, &grid_default_cell, sizeof framegc);
	framegc.fg = colour_join_rgb(0x00, 0x00, 0x00);
	framegc.bg = colour_join_rgb(0xc0, 0xc0, 0xc0);
	/*
	 * screen_write_box() starts the title at rx+2, which is exactly where
	 * the close box [■] goes (rx+2..rx+4), and the zoom box [↑] sits at
	 * rx+rw-5..rx+rw-3: pad the title past the close box and trim it
	 * before the zoom box, so neither eats into it.
	 */
	if (rw > 8) {
		char	nm[512];
		size_t	maxw = (rw > 14) ? (size_t)(rw - 12) : 1;

		if (strlen(w->name) > maxw) {
			while (maxw > 0 &&
			    ((u_char)w->name[maxw] & 0xc0) == 0x80)
				maxw--;		/* never split a UTF-8 char */
			snprintf(nm, sizeof nm, "%.*s", (int)maxw, w->name);
		} else
			strlcpy(nm, w->name, sizeof nm);
		xasprintf(&title, "    %s ", nm);
	} else
		xasprintf(&title, " %s ", w->name);
	screen_write_cursormove(sctx, rx, ry, 0);
	screen_write_box(sctx, rw, rh, BOX_LINES_DOUBLE, &framegc, title);
	free(title);

	/* Close box [■] at the top-left of the title bar (Turbo Vision). */
	if (rw > 8) {
		struct grid_cell	cbox;

		memcpy(&cbox, &framegc, sizeof cbox);
		screen_write_cursormove(sctx, rx + 2, ry, 0);
		screen_write_putc(sctx, &cbox, '[');
		cbox.fg = colour_join_rgb(0xaa, 0x00, 0x00); /* red square */
		cbox.data.data[0] = 0xe2; cbox.data.data[1] = 0x96;
		cbox.data.data[2] = 0xa0; /* U+25A0 ■ */
		cbox.data.have = cbox.data.size = 3; cbox.data.width = 1;
		screen_write_cell(sctx, &cbox);
		memcpy(&cbox, &framegc, sizeof cbox);
		screen_write_putc(sctx, &cbox, ']');

		/* Zoom box [↑] near the top-right (Turbo Vision). */
		if (rw > 12) {
			memcpy(&cbox, &framegc, sizeof cbox);
			screen_write_cursormove(sctx, rx + rw - 5, ry, 0);
			screen_write_putc(sctx, &cbox, '[');
			cbox.data.data[0] = 0xe2; cbox.data.data[1] = 0x86;
			cbox.data.data[2] = w->desktop_zoomed ? 0x93 : 0x91;
			cbox.data.have = cbox.data.size = 3; cbox.data.width = 1;
			screen_write_cell(sctx, &cbox); /* ↑ or ↓ */
			memcpy(&cbox, &framegc, sizeof cbox);
			screen_write_putc(sctx, &cbox, ']');
		}
	}

	/* (Per-pane vertical scrollbars are drawn last, after the junctions.) */

	/*
	 * Copy the window's content from its active pane grid, cell by cell
	 * through screen_write (mixing screen_write with the direct-grid
	 * fast_copy corrupts the compose buffer). Cells beyond the written
	 * content come back as blanks, showing the pane background.
	 */
	if (content && rw > 2 && rh > 2) {
		struct grid_cell	 cell;
		u_int			 iw = rw - 2, ih = rh - 2;
		u_int			 inset = claude_inset(c, w);

		/* Fill the interior with the window background (so pane borders
		 * and gaps show black, not the desktop). */
		memcpy(&cell, &grid_default_cell, sizeof cell);
		for (j = 0; j < ih; j++) {
			screen_write_cursormove(sctx, rx + 1, ry + 1 + j, 0);
			for (i = 0; i < iw; i++)
				screen_write_putc(sctx, &cell, ' ');
		}

		/* Draw every visible pane at its layout position (handles
		 * horizontal and vertical splits). */
		TAILQ_FOREACH(wp, &w->panes, entry) {
			struct grid	*gd = wp->screen->grid;
			u_int		 hsize = gd->hsize, px, py, cw2, ch2;

			if (!window_pane_visible(wp))
				continue;
			if (inset + wp->xoff >= iw || wp->yoff >= ih)
				continue;
			cw2 = screen_size_x(wp->screen);
			ch2 = screen_size_y(wp->screen);
			if (inset + wp->xoff + cw2 > iw)
				cw2 = iw - inset - wp->xoff;
			if (wp->yoff + ch2 > ih)
				ch2 = ih - wp->yoff;
			for (py = 0; py < ch2; py++) {
				screen_write_cursormove(sctx,
				    rx + 1 + inset + wp->xoff,
				    ry + 1 + wp->yoff + py, 0);
				for (px = 0; px < cw2; px++) {
					grid_get_cell(gd, px, hsize + py, &cell);
					if (cell.flags & GRID_FLAG_PADDING)
						continue;
					/* Apply the copy-mode selection highlight
					 * (the yellow style is on screen->sel,
					 * not in the raw grid cell). */
					if ((cell.flags & GRID_FLAG_SELECTED) &&
					    wp->screen->sel != NULL) {
						struct grid_cell sgc;

						screen_select_cell(wp->screen,
						    &sgc, &cell);
						sgc.flags &= ~GRID_FLAG_SELECTED;
						screen_write_cell(sctx, &sgc);
					} else
						screen_write_cell(sctx, &cell);
				}
			}
		}

		/* CLAUDE: conversation list column + its draggable band. */
		if (inset != 0)
			screen_redraw_claude_list(sctx, c, w, rx, ry, rw, rh);

		/* Draw the pane separators (draggable borders). */
		if (window_count_panes(w) > 1) {
			struct grid_cell	vgc, hgc;
			u_int			bx, by, p;

			memcpy(&vgc, &grid_default_cell, sizeof vgc);
			vgc.fg = colour_join_rgb(0xa8, 0xa8, 0xa8);
			vgc.bg = colour_join_rgb(0x00, 0x00, 0x00);
			memcpy(&hgc, &vgc, sizeof hgc);
			vgc.data.data[0] = 0xe2; vgc.data.data[1] = 0x94;
			vgc.data.data[2] = 0x82; /* U+2502 vertical */
			vgc.data.have = vgc.data.size = 3; vgc.data.width = 1;
			hgc.data.data[0] = 0xe2; hgc.data.data[1] = 0x94;
			hgc.data.data[2] = 0x80; /* U+2500 horizontal */
			hgc.data.have = hgc.data.size = 3; hgc.data.width = 1;

			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (!window_pane_visible(wp))
					continue;
				/* Panes are shifted right by the list column. */
				bx = inset + wp->xoff + wp->sx;	/* right border */
				if (bx < iw) {
					for (p = 0; p <= wp->sy &&
					    wp->yoff + p < ih; p++) {
						screen_write_cursormove(sctx,
						    rx + 1 + bx,
						    ry + 1 + wp->yoff + p, 0);
						screen_write_cell(sctx, &vgc);
					}
				}
				by = wp->yoff + wp->sy; /* border below */
				if (by < ih) {
					for (p = 0; p <= wp->sx &&
					    inset + wp->xoff + p < iw; p++) {
						screen_write_cursormove(sctx,
						    rx + 1 + inset + wp->xoff + p,
						    ry + 1 + by, 0);
						screen_write_cell(sctx, &hgc);
					}
				}
			}

			/*
			 * Vertical scrollbars, ONE PER PANE (every TVision view
			 * has its own): the frame's right column for panes
			 * touching the right edge, otherwise the separator
			 * column, over the pane's own rows. TScroller value =
			 * content line at the top of the viewport in [0, hsize]
			 * (live puts the indicator at the bottom). An interior
			 * pane's bar sits on its own last column, so the
			 * separator column to its right stays a plain draggable
			 * band. Drawn after the content (it overlays the pane's
			 * last column) and before the junction pass.
			 */
			TAILQ_FOREACH(wp, &w->panes, entry) {
				u_int	bx, by, bsize, oy, hsize, viewtop;

				if (!desktop_pane_scrollbar(c, wp, rx, ry, rw, rh,
				    &bx, &by, &bsize))
					continue;
				if (!window_copy_get_scroll(wp, &oy, &hsize,
				    NULL)) {
					oy = 0;
					hsize = screen_hsize(&wp->base);
				}
				viewtop = (hsize > oy) ? hsize - oy : 0;
				scrollbar_draw(sctx, bx, by, bsize, viewtop,
				    hsize, 0x000000, 0xc0c0c0);
			}

			/*
			 * Junctions: where a vertical and a horizontal separator
			 * cross or meet, replace the plain │/─ with the matching
			 * box-drawing glyph (┼ ┬ ┴ ├ ┤). Classify every interior
			 * cell first (snapshot), then rewrite from the snapshot so
			 * one rewrite never perturbs a neighbour's calculation.
			 */
			{
				struct grid	*agd = sctx->s->grid;
				u_char		*snap;
				u_int		 dx, dy;

				snap = xcalloc((size_t)iw * ih, 1);
				for (dy = 0; dy < ih; dy++) {
					for (dx = 0; dx < iw; dx++) {
						grid_get_cell(agd, rx + 1 + dx,
						    ry + 1 + dy, &vgc);
						if (vgc.data.size == 3 &&
						    vgc.data.data[0] == 0xe2 &&
						    vgc.data.data[1] == 0x94) {
							if (vgc.data.data[2] == 0x82)
								snap[dy * iw + dx] = 1;
							else if (vgc.data.data[2]
							    == 0x80)
								snap[dy * iw + dx] = 2;
						}
					}
				}
				memcpy(&vgc, &grid_default_cell, sizeof vgc);
				vgc.fg = colour_join_rgb(0xa8, 0xa8, 0xa8);
				vgc.bg = colour_join_rgb(0x00, 0x00, 0x00);
				vgc.data.data[0] = 0xe2; vgc.data.data[1] = 0x94;
				vgc.data.have = vgc.data.size = 3;
				vgc.data.width = 1;
				for (dy = 0; dy < ih; dy++) {
					for (dx = 0; dx < iw; dx++) {
						u_int	u, d, l, r, n;
						u_char	g;

						if (snap[dy * iw + dx] == 0)
							continue;
						u = (dy > 0 &&
						    snap[(dy-1)*iw+dx] == 1);
						d = (dy+1 < ih &&
						    snap[(dy+1)*iw+dx] == 1);
						l = (dx > 0 &&
						    snap[dy*iw+dx-1] == 2);
						r = (dx+1 < iw &&
						    snap[dy*iw+dx+1] == 2);
						n = u + d + l + r;
						if (n < 3)
							continue;
						if (u && d && l && r)
							g = 0xbc; /* ┼ */
						else if (u && d && r)
							g = 0x9c; /* ├ */
						else if (u && d && l)
							g = 0xa4; /* ┤ */
						else if (l && r && d)
							g = 0xac; /* ┬ */
						else
							g = 0xb4; /* ┴ */
						vgc.data.data[2] = g;
						screen_write_cursormove(sctx,
						    rx + 1 + dx, ry + 1 + dy, 0);
						screen_write_cell(sctx, &vgc);
					}
				}
				free(snap);
			}
		}
	}

}

/*
 * CLAUDE: draw the conversation list of a manager window - a Turbo Vision
 * list on the left (one row per pane, the active one highlighted), a
 * "[+ Nouvelle]" button on the last row, and the draggable band that
 * separates it from the conversation.
 */
static void
screen_redraw_claude_list(struct screen_write_ctx *sctx, struct client *c,
    struct window *w, u_int rx, u_int ry, u_int rw, u_int rh)
{
	struct grid_cell	 bg, gc, sel, dim, band, btn, mark;
	struct window_pane	*wp;
	u_int			 lw = claude_list_width(c, w);
	u_int			 ih = (rh > 2) ? rh - 2 : 0;
	u_int			 i, j, idx, n, tw, bx = 0, by = 0, bsize = 0;
	u_int			 vh, total, range;
	int			 sb;
	char			 buf[512], label[256];
	const char		*name;
	enum claude_row		 kind;

	if (lw == 0 || ih < 3)
		return;
	claude_sess_refresh(w);		/* saved conversations (throttled) */

	/*
	 * The list gets its own vertical scrollbar as soon as it overflows,
	 * on its last column - the text then has one column less, exactly as a
	 * pane gives up its last content column to its own bar.
	 */
	sb = claude_list_scrollbar(c, w, rx, ry, rw, rh, &bx, &by, &bsize);
	tw = sb ? lw - 1 : lw;

	memcpy(&bg, &grid_default_cell, sizeof bg);
	bg.fg = colour_join_rgb(0x00, 0x00, 0x00);
	bg.bg = colour_join_rgb(0xc0, 0xc0, 0xc0);
	memcpy(&gc, &bg, sizeof gc);
	memcpy(&sel, &bg, sizeof sel);
	sel.fg = colour_join_rgb(0xff, 0xff, 0xff);
	sel.bg = colour_join_rgb(0x00, 0x00, 0xa8);
	memcpy(&dim, &bg, sizeof dim);		/* saved sessions: quieter */
	dim.fg = colour_join_rgb(0x40, 0x40, 0x40);
	memcpy(&btn, &bg, sizeof btn);
	btn.fg = colour_join_rgb(0x00, 0x00, 0x00);
	btn.bg = colour_join_rgb(0x00, 0xa8, 0x00);
	memcpy(&mark, &bg, sizeof mark);	/* multi-selection */
	mark.fg = colour_join_rgb(0x00, 0x00, 0x00);
	mark.bg = colour_join_rgb(0x00, 0xa8, 0xa8);

	/* Column background. */
	for (j = 0; j < ih; j++) {
		screen_write_cursormove(sctx, rx + 1, ry + 1 + j, 0);
		for (i = 0; i < lw; i++)
			screen_write_putc(sctx, &bg, ' ');
	}

	/*
	 * One pass over the rows, asking claude_list_row() what each one is -
	 * the same answer the mouse gets, so a click can never land elsewhere
	 * than what was drawn.
	 */
	for (j = 0; j < ih; j++) {
		struct grid_cell	*cell = &gc;

		kind = claude_list_row(c, w, j, &idx);
		if (kind == CLAUDE_ROW_NONE)
			continue;
		label[0] = '\0';

		switch (kind) {
		case CLAUDE_ROW_HEADER: {
			u_int	tot = claude_mem_total(w) / 1024;	/* MB */
			char	mem[16] = "";

			cell = &bg;
			cell->attr |= GRID_ATTR_BRIGHT;
			/* MEMORY: total on the right, when there is room. */
			if (tot != 0 && lw >= 19) {
				if (tot >= 1024) {
					snprintf(mem, sizeof mem, "%u,%uG",
					    tot / 1024, (tot % 1024) / 103);
				} else
					snprintf(mem, sizeof mem, "%uM", tot);
			}
			snprintf(label, sizeof label, " Conversations%*s",
			    (int)(lw - 14), mem);
			if (w->claude_lowmem)	/* red: machine short of memory */
				cell->fg = colour_join_rgb(0xa8, 0x00, 0x00);
			break;
		}
		case CLAUDE_ROW_CONV:
			n = 0;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (n++ == idx)
					break;
			}
			if (wp == NULL)
				continue;
			/*
			 * Real conversation name: the title the program
			 * publishes (Claude Code sets it through OSC 0/2, and
			 * shells usually do too), else the directory name.
			 */
			name = wp->base.title;
			if (name == NULL || *name == '\0') {
				name = (wp->cwd != NULL) ?
				    strrchr(wp->cwd, '/') : NULL;
				if (name != NULL && name[1] != '\0')
					name++;
				else if (wp->cwd != NULL && *wp->cwd != '\0')
					name = wp->cwd;
				else
					name = "shell";
			}
			/*
			 * Envelope when the agent bus has new mail; a tick
			 * when the row is part of the multi-selection.
			 */
			snprintf(label, sizeof label, "%s%u %s%s",
			    wp->claude_marked ? "\342\234\223" :
			    (wp == w->active) ? ">" : " ", idx + 1,
			    wp->claude_unread ? "\342\234\211 " : "", name);
			/*
			 * MEMORY: "445M" right-aligned. The figure is the point
			 * of the column when memory runs short, so it is the
			 * NAME that gives way: cut to leave the figure its room.
			 */
			if (wp->claude_mem >= 1024 && tw >= 12) {
				char	mem[12];
				u_int	keep, l = 0;
				size_t	b = 0;

				snprintf(mem, sizeof mem, "%uM",
				    wp->claude_mem / 1024);
				keep = tw - strlen(mem) - 1;
				/* Cut the label at `keep` columns, whole chars. */
				while (label[b] != '\0') {
					size_t	n = 1;
					char	ch[8];

					while (label[b + n] != '\0' &&
					    ((u_char)label[b + n] & 0xc0) == 0x80)
						n++;
					if (n >= sizeof ch)
						break;
					memcpy(ch, label + b, n);
					ch[n] = '\0';
					if (l + utf8_cstrwidth(ch) > keep)
						break;
					l += utf8_cstrwidth(ch);
					b += n;
				}
				label[b] = '\0';
				snprintf(label + b, sizeof label - b, "%*s",
				    (int)(tw - l), mem);
			}
			if (wp == w->active)
				cell = &sel;
			else if (wp->claude_marked)
				cell = &mark;
			break;
		case CLAUDE_ROW_SESSHDR:
			cell = &bg;
			cell->attr |= GRID_ATTR_BRIGHT;
			snprintf(label, sizeof label, " Reprendre");
			break;
		case CLAUDE_ROW_SESS:
			name = claude_sess_label(idx);
			if (name == NULL)
				continue;
			snprintf(label, sizeof label, "%s %s",
			    claude_sess_marked(idx) ? "\342\234\223" : " ", name);
			if ((int)idx == w->claude_sel_sess)
				cell = &sel;
			else
				cell = claude_sess_marked(idx) ? &mark : &dim;
			break;
		case CLAUDE_ROW_NEW:
			cell = &btn;
			snprintf(label, sizeof label, " [+ Nouvelle]");
			break;
		default:
			continue;
		}

		/* The header and the button keep the full width. */
		n = (kind == CLAUDE_ROW_HEADER || kind == CLAUDE_ROW_NEW) ?
		    lw : tw;
		/*
		 * Cut and pad in COLUMNS: the tick and the envelope are several
		 * bytes wide for one column, a byte count left the row short.
		 */
		{
			size_t	b = 0, o = 0, k;
			u_int	cols = 0, cw;
			char	ch[8];

			while (label[b] != '\0') {
				k = 1;
				while (label[b + k] != '\0' &&
				    ((u_char)label[b + k] & 0xc0) == 0x80)
					k++;
				if (k >= sizeof ch || o + k >= sizeof buf - 1)
					break;
				memcpy(ch, label + b, k);
				ch[k] = '\0';
				cw = utf8_cstrwidth(ch);
				if (cols + cw > n)
					break;
				memcpy(buf + o, ch, k);
				o += k;
				b += k;
				cols += cw;
			}
			while (cols < n && o < sizeof buf - 1) {
				buf[o++] = ' ';
				cols++;
			}
			buf[o] = '\0';
		}
		screen_write_cursormove(sctx, rx + 1, ry + 1 + j, 0);
		screen_write_puts(sctx, cell, "%s", buf);
		cell->attr &= ~GRID_ATTR_BRIGHT;
	}

	/* The list's scrollbar, over the rows it scrolls. */
	if (sb && claude_list_geom(c, w, &vh, &total, &range)) {
		scrollbar_draw(sctx, bx, by, bsize, w->claude_scroll, range,
		    0x000000, 0xc0c0c0);
	}

	/* The band between the list and the conversation (draggable). */
	memcpy(&band, &grid_default_cell, sizeof band);
	band.fg = colour_join_rgb(0xa8, 0xa8, 0xa8);
	band.bg = colour_join_rgb(0x00, 0x00, 0x00);
	band.data.data[0] = 0xe2; band.data.data[1] = 0x94;
	band.data.data[2] = 0x82;	/* │ U+2502 */
	band.data.have = band.data.size = 3;
	band.data.width = 1;
	for (j = 0; j < ih; j++) {
		screen_write_cursormove(sctx, rx + 1 + lw, ry + 1 + j, 0);
		screen_write_cell(sctx, &band);
	}
}

static void
screen_redraw_draw_desktop(struct screen_redraw_ctx *ctx, int full)
{
	struct client		*c = ctx->c;
	struct tty		*tty = &c->tty;
	struct session		*s = c->session;
	struct window		*cw;
	struct winlink		*wl;
	struct screen		 area;
	struct screen_write_ctx	 sctx;
	struct grid_cell	 deskgc;
	u_int			 rx, ry, rw, rh, wx, wy, ww, wh;
	u_int			 area_top, area_h, i, j, blit_top, blit_bot;
	int			 ph;

	/* Guard curw before dereferencing it (session teardown transient). */
	if (s == NULL || s->curw == NULL)
		return;
	cw = s->curw->window;
	/*
	 * DESKTOP: the placeholder window (all real windows closed) has no
	 * rectangle - the desktop is drawn bare, with nothing on it.
	 */
	ph = desktop_placeholder(cw);
	rx = ry = rw = rh = 0;
	if (!ph && !desktop_get_rect(c, &rx, &ry, &rw, &rh))
		return;

	area_top = ctx->menubar + (ctx->statustop ? ctx->statuslines : 0);
	if (tty->sy <= area_top + (ctx->statustop ? 0 : ctx->statuslines))
		return;
	area_h = tty->sy - area_top - (ctx->statustop ? 0 : ctx->statuslines);
	if (area_h < rh || tty->sx < rw)
		return;

	/*
	 * Desktop cell: exactly Turbo Vision's cpAppColor[1] = 0x71, i.e. VGA
	 * attribute background 7 (light grey) + foreground 1 (blue), with the
	 * 0xB0 light-shade pattern -> blue dots on a light-grey ground.
	 */
	memcpy(&deskgc, &grid_default_cell, sizeof deskgc);
	deskgc.fg = colour_join_rgb(0x00, 0x00, 0xaa); /* VGA blue (1) */
	deskgc.bg = colour_join_rgb(0xaa, 0xaa, 0xaa); /* VGA light grey (7) */
	deskgc.data.data[0] = 0xe2; /* U+2591 LIGHT SHADE ('░') */
	deskgc.data.data[1] = 0x96;
	deskgc.data.data[2] = 0x91;
	deskgc.data.have = deskgc.data.size = 3;
	deskgc.data.width = 1;

	screen_init(&area, tty->sx, area_h, 0);
	screen_write_start(&sctx, &area);

	/* Fill the whole area with the hatched desktop pattern. */
	for (j = 0; j < area_h; j++) {
		screen_write_cursormove(&sctx, 0, j, 0);
		for (i = 0; i < tty->sx; i++)
			screen_write_cell(&sctx, &deskgc);
	}

	/* Background windows (all but the current), with their content. */
	blit_top = area_h;
	blit_bot = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window == cw)
			continue;
		if (desktop_get_rect_w(c, wl->window, &wx, &wy, &ww, &wh)) {
			screen_redraw_desktop_one(c, &sctx, area_h, tty->sx,
			    wl->window, wx, wy, ww, wh, 1);
			if (wy < blit_top)
				blit_top = wy;
			if (wy + wh + 1 > blit_bot)
				blit_bot = wy + wh + 1;
		}
	}
	/* Current window on top (front), content composited from its grid. */
	if (ph) {
		blit_top = 0;		/* nothing on it: repaint everything */
		blit_bot = area_h;
	} else {
		screen_redraw_desktop_one(c, &sctx, area_h, tty->sx, cw, rx, ry,
		    rw, rh, 1);
		if (ry < blit_top)
			blit_top = ry;
		if (ry + rh + 1 > blit_bot)
			blit_bot = ry + rh + 1;
	}

	screen_write_stop(&sctx);

	/*
	 * Turbo Vision style damage: diff the freshly composed area against the
	 * persistent back-buffer and send ONLY the lines that changed. This
	 * removes flicker (unchanged lines are never repainted) and lets every
	 * window refresh independently. A borders redraw (resize/move/overlay
	 * close) forces a full repaint.
	 */
	(void)blit_top;
	(void)blit_bot;
	if (!c->desktop_buffer_valid ||
	    screen_size_x(&c->desktop_buffer) != tty->sx ||
	    screen_size_y(&c->desktop_buffer) != area_h) {
		screen_free(&c->desktop_buffer);
		screen_init(&c->desktop_buffer, tty->sx, area_h, 0);
		c->desktop_buffer_valid = 1;
		full = 1; /* buffer just (re)created: repaint everything */
	}
	for (j = 0; j < area_h; j++) {
		struct grid_cell	ca, cb;
		struct overlay_ranges	r;
		u_int			k;
		int			changed = full;

		for (i = 0; !changed && i < tty->sx; i++) {
			grid_get_cell(area.grid, i, j, &ca);
			grid_get_cell(c->desktop_buffer.grid, i, j, &cb);
			if (ca.data.width != cb.data.width ||
			    ca.attr != cb.attr || ca.fg != cb.fg ||
			    ca.bg != cb.bg || ca.data.size != cb.data.size ||
			    memcmp(ca.data.data, cb.data.data, ca.data.size) != 0)
				changed = 1;
		}
		if (!changed)
			continue;

		/*
		 * Draw the changed line, but skip cells covered by an overlay
		 * (dropdown menu/popup) so the desktop never paints over it.
		 * Covered cells are left out of the back-buffer so they are
		 * repainted once the overlay closes.
		 */
		if (c->overlay_check != NULL)
			c->overlay_check(c, c->overlay_data, 0, area_top + j,
			    tty->sx, &r);
		else {
			r.px[0] = 0;
			r.nx[0] = tty->sx;
			for (k = 1; k < OVERLAY_MAX_RANGES; k++)
				r.nx[k] = 0;
		}
		for (k = 0; k < OVERLAY_MAX_RANGES; k++) {
			if (r.nx[k] == 0)
				continue;
			tty_draw_line(tty, &area, r.px[k], j, r.nx[k], r.px[k],
			    area_top + j, &grid_default_cell, NULL);
			for (i = r.px[k]; i < r.px[k] + r.nx[k]; i++) {
				grid_get_cell(area.grid, i, j, &ca);
				grid_view_set_cell(c->desktop_buffer.grid, i, j,
				    &ca);
			}
		}
	}
	screen_free(&area);
}

/* MENU BAR: draw the menu bar on the top line of the screen. */
static void
screen_redraw_draw_menubar(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct tty		*tty = &c->tty;
	struct screen		 menu;
	struct screen_write_ctx	 sctx;
	struct grid_cell	 gc;
	struct format_tree	*ft;
	char			*fmt;
	char			*expanded;
	u_int			 offset;

	if (ctx->menubar == 0)
		return;
	fmt = menu_bar_build(); /* MENU BAR: content compiled in (C) */

	memcpy(&gc, &grid_default_cell, sizeof gc);
	ft = format_create(c, NULL, FORMAT_NONE, 0);
	format_defaults(ft, c, s, NULL, NULL);
	expanded = format_expand(ft, fmt);
	free(fmt);

	screen_init(&menu, tty->sx, 1, 0);
	screen_write_start(&sctx, &menu);
	screen_write_cursormove(&sctx, 0, 0, 0);
	for (offset = 0; offset < tty->sx; offset++)
		screen_write_putc(&sctx, &gc, ' ');
	screen_write_cursormove(&sctx, 0, 0, 0);
	status_free_ranges(&c->menubar_ranges); /* MENU BAR: refresh zones */
	format_draw(&sctx, &gc, tty->sx, expanded, &c->menubar_ranges, 0);
	screen_write_stop(&sctx);

	tty_draw_line(tty, &menu, 0, 0, tty->sx, 0, 0, &grid_default_cell, NULL);

	screen_free(&menu);
	free(expanded);
	format_free(ft);
}

/* Draw one pane. */
static void
screen_redraw_draw_pane(struct screen_redraw_ctx *ctx, struct window_pane *wp)
{
	struct client		*c = ctx->c;
	struct window		*w = c->session->curw->window;
	struct tty		*tty = &c->tty;
	struct screen		*s = wp->screen;
	struct colour_palette	*palette = &wp->palette;
	struct grid_cell	 defaults;
	u_int			 i, j, top, x, y, width;

	log_debug("%s: %s @%u %%%u", __func__, c->name, w->id, wp->id);

	if (wp->xoff + wp->sx <= ctx->ox || wp->xoff >= ctx->ox + ctx->sx)
		return;
	if (ctx->statustop)
		top = ctx->statuslines;
	else
		top = 0;
	top += ctx->menubar + ctx->desktop_top; /* MENU BAR + DESKTOP inset */
	for (j = 0; j < wp->sy; j++) {
		if (wp->yoff + j < ctx->oy || wp->yoff + j >= ctx->oy + ctx->sy)
			continue;
		y = top + wp->yoff + j - ctx->oy;

		if (wp->xoff >= ctx->ox &&
		    wp->xoff + wp->sx <= ctx->ox + ctx->sx) {
			/* All visible. */
			i = 0;
			x = wp->xoff - ctx->ox;
			width = wp->sx;
		} else if (wp->xoff < ctx->ox &&
		    wp->xoff + wp->sx > ctx->ox + ctx->sx) {
			/* Both left and right not visible. */
			i = ctx->ox;
			x = 0;
			width = ctx->sx;
		} else if (wp->xoff < ctx->ox) {
			/* Left not visible. */
			i = ctx->ox - wp->xoff;
			x = 0;
			width = wp->sx - i;
		} else {
			/* Right not visible. */
			i = 0;
			x = wp->xoff - ctx->ox;
			width = ctx->sx - x;
		}
		log_debug("%s: %s %%%u line %u,%u at %u,%u, width %u",
		    __func__, c->name, wp->id, i, j, x, y, width);

		tty_default_colours(&defaults, wp);
		tty_draw_line(tty, s, i, j, width, x + ctx->desktop_left, y,
		    &defaults, palette); /* DESKTOP: inset left */
	}

#ifdef ENABLE_SIXEL
	tty_draw_images(c, wp, s);
#endif
}
