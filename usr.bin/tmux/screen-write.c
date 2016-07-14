/* $OpenBSD: screen-write.c,v 1.90 2016/06/06 07:28:52 nicm Exp $ */

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

static void	screen_write_initctx(struct screen_write_ctx *,
		    struct tty_ctx *);
static void	screen_write_save_last(struct screen_write_ctx *,
		    struct tty_ctx *);

static int	screen_write_overwrite(struct screen_write_ctx *,
		    struct grid_cell *, u_int);
static int	screen_write_combine(struct screen_write_ctx *,
		    const struct utf8_data *);

static const struct grid_cell screen_write_pad_cell = {
	GRID_FLAG_PADDING, 0, { .fg = 8 }, { .bg = 8 }, { { 0 }, 0, 0, 0 }
};

/* Initialise writing with a window. */
void
screen_write_start(struct screen_write_ctx *ctx, struct window_pane *wp,
    struct screen *s)
{
	ctx->wp = wp;
	if (wp != NULL && s == NULL)
		ctx->s = wp->screen;
	else
		ctx->s = s;
}

/* Finish writing. */
void
screen_write_stop(__unused struct screen_write_ctx *ctx)
{
}

/* Reset screen state. */
void
screen_write_reset(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;

	screen_reset_tabs(s);
	screen_write_scrollregion(ctx, 0, screen_size_y(s) - 1);

	s->mode &= ~(MODE_INSERT|MODE_KCURSOR|MODE_KKEYPAD|MODE_FOCUSON);
	s->mode &= ~(ALL_MOUSE_MODES|MODE_MOUSE_UTF8|MODE_MOUSE_SGR);

	screen_write_clearscreen(ctx);
	screen_write_cursormove(ctx, 0, 0);
}

/* Write character. */
void
screen_write_putc(struct screen_write_ctx *ctx, const struct grid_cell *gcp,
    u_char ch)
{
	struct grid_cell	gc;

	memcpy(&gc, gcp, sizeof gc);

	utf8_set(&gc.data, ch);
	screen_write_cell(ctx, &gc);
}

/* Calculate string length, with embedded formatting. */
size_t
screen_write_cstrlen(const char *fmt, ...)
{
	va_list	ap;
	char   *msg, *msg2, *ptr, *ptr2;
	size_t	size;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	msg2 = xmalloc(strlen(msg) + 1);

	ptr = msg;
	ptr2 = msg2;
	while (*ptr != '\0') {
		if (ptr[0] == '#' && ptr[1] == '[') {
			while (*ptr != ']' && *ptr != '\0')
				ptr++;
			if (*ptr == ']')
				ptr++;
			continue;
		}
		*ptr2++ = *ptr++;
	}
	*ptr2 = '\0';

	size = screen_write_strlen("%s", msg2);

	free(msg);
	free(msg2);

	return (size);
}

/* Calculate string length. */
size_t
screen_write_strlen(const char *fmt, ...)
{
	va_list			ap;
	char   	       	       *msg;
	struct utf8_data	ud;
	u_char 	      	       *ptr;
	size_t			left, size = 0;
	enum utf8_state		more;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	ptr = msg;
	while (*ptr != '\0') {
		if (*ptr > 0x7f && utf8_open(&ud, *ptr) == UTF8_MORE) {
			ptr++;

			left = strlen(ptr);
			if (left < (size_t)ud.size - 1)
				break;
			while ((more = utf8_append(&ud, *ptr)) == UTF8_MORE)
				ptr++;
			ptr++;

			if (more == UTF8_DONE)
				size += ud.width;
		} else {
			if (*ptr > 0x1f && *ptr < 0x7f)
				size++;
			ptr++;
		}
	}

	free(msg);
	return (size);
}

/* Write simple string (no UTF-8 or maximum length). */
void
screen_write_puts(struct screen_write_ctx *ctx, const struct grid_cell *gcp,
    const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	screen_write_vnputs(ctx, -1, gcp, fmt, ap);
	va_end(ap);
}

/* Write string with length limit (-1 for unlimited). */
void
screen_write_nputs(struct screen_write_ctx *ctx, ssize_t maxlen,
    const struct grid_cell *gcp, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	screen_write_vnputs(ctx, maxlen, gcp, fmt, ap);
	va_end(ap);
}

void
screen_write_vnputs(struct screen_write_ctx *ctx, ssize_t maxlen,
    const struct grid_cell *gcp, const char *fmt, va_list ap)
{
	struct grid_cell	gc;
	struct utf8_data       *ud = &gc.data;
	char   		       *msg;
	u_char 		       *ptr;
	size_t		 	left, size = 0;
	enum utf8_state		more;

	memcpy(&gc, gcp, sizeof gc);
	xvasprintf(&msg, fmt, ap);

	ptr = msg;
	while (*ptr != '\0') {
		if (*ptr > 0x7f && utf8_open(ud, *ptr) == UTF8_MORE) {
			ptr++;

			left = strlen(ptr);
			if (left < (size_t)ud->size - 1)
				break;
			while ((more = utf8_append(ud, *ptr)) == UTF8_MORE)
				ptr++;
			ptr++;

			if (more != UTF8_DONE)
				continue;
			if (maxlen > 0 && size + ud->width > (size_t)maxlen) {
				while (size < (size_t)maxlen) {
					screen_write_putc(ctx, &gc, ' ');
					size++;
				}
				break;
			}
			size += ud->width;
			screen_write_cell(ctx, &gc);
		} else {
			if (maxlen > 0 && size + 1 > (size_t)maxlen)
				break;

			if (*ptr == '\001')
				gc.attr ^= GRID_ATTR_CHARSET;
			else if (*ptr > 0x1f && *ptr < 0x7f) {
				size++;
				screen_write_putc(ctx, &gc, *ptr);
			}
			ptr++;
		}
	}

	free(msg);
}

/* Write string, similar to nputs, but with embedded formatting (#[]). */
void
screen_write_cnputs(struct screen_write_ctx *ctx, ssize_t maxlen,
    const struct grid_cell *gcp, const char *fmt, ...)
{
	struct grid_cell	 gc;
	struct utf8_data	*ud = &gc.data;
	va_list			 ap;
	char			*msg;
	u_char 			*ptr, *last;
	size_t			 left, size = 0;
	enum utf8_state		 more;

	memcpy(&gc, gcp, sizeof gc);

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	ptr = msg;
	while (*ptr != '\0') {
		if (ptr[0] == '#' && ptr[1] == '[') {
			ptr += 2;
			last = ptr + strcspn(ptr, "]");
			if (*last == '\0') {
				/* No ]. Not much point in doing anything. */
				break;
			}
			*last = '\0';

			style_parse(gcp, &gc, ptr);
			ptr = last + 1;
			continue;
		}

		if (*ptr > 0x7f && utf8_open(ud, *ptr) == UTF8_MORE) {
			ptr++;

			left = strlen(ptr);
			if (left < (size_t)ud->size - 1)
				break;
			while ((more = utf8_append(ud, *ptr)) == UTF8_MORE)
				ptr++;
			ptr++;

			if (more != UTF8_DONE)
				continue;
			if (maxlen > 0 && size + ud->width > (size_t)maxlen) {
				while (size < (size_t)maxlen) {
					screen_write_putc(ctx, &gc, ' ');
					size++;
				}
				break;
			}
			size += ud->width;
			screen_write_cell(ctx, &gc);
		} else {
			if (maxlen > 0 && size + 1 > (size_t)maxlen)
				break;

			if (*ptr > 0x1f && *ptr < 0x7f) {
				size++;
				screen_write_putc(ctx, &gc, *ptr);
			}
			ptr++;
		}
	}

	free(msg);
}

/* Copy from another screen. */
void
screen_write_copy(struct screen_write_ctx *ctx,
    struct screen *src, u_int px, u_int py, u_int nx, u_int ny)
{
	struct screen		*s = ctx->s;
	struct grid		*gd = src->grid;
	struct grid_line	*gl;
	struct grid_cell	 gc;
	u_int		 	 xx, yy, cx, cy, ax, bx;

	cx = s->cx;
	cy = s->cy;
	for (yy = py; yy < py + ny; yy++) {
		gl = &gd->linedata[yy];
		if (yy < gd->hsize + gd->sy) {
			/*
			 * Find start and end position and copy between
			 * them. Limit to the real end of the line then use a
			 * clear EOL only if copying to the end, otherwise
			 * could overwrite whatever is there already.
			 */
			if (px > gl->cellsize)
				ax = gl->cellsize;
			else
				ax = px;
			if (px + nx == gd->sx && px + nx > gl->cellsize)
				bx = gl->cellsize;
			else
				bx = px + nx;

			for (xx = ax; xx < bx; xx++) {
				grid_get_cell(gd, xx, yy, &gc);
				screen_write_cell(ctx, &gc);
			}
			if (px + nx == gd->sx && px + nx > gl->cellsize)
				screen_write_clearendofline(ctx);
		} else
			screen_write_clearline(ctx);
		cy++;
		screen_write_cursormove(ctx, cx, cy);
	}
}

/* Set up context for TTY command. */
static void
screen_write_initctx(struct screen_write_ctx *ctx, struct tty_ctx *ttyctx)
{
	struct screen	*s = ctx->s;

	ttyctx->wp = ctx->wp;

	ttyctx->ocx = s->cx;
	ttyctx->ocy = s->cy;

	ttyctx->orlower = s->rlower;
	ttyctx->orupper = s->rupper;
}

/* Save last cell on screen. */
static void
screen_write_save_last(struct screen_write_ctx *ctx, struct tty_ctx *ttyctx)
{
	struct screen		*s = ctx->s;
	struct grid		*gd = s->grid;
	struct grid_cell	 gc;
	u_int			 xx;

	memcpy(&gc, &grid_default_cell, sizeof gc);
	for (xx = 1; xx <= screen_size_x(s); xx++) {
		grid_view_get_cell(gd, screen_size_x(s) - xx, s->cy, &gc);
		if (~gc.flags & GRID_FLAG_PADDING)
			break;
	}
	ttyctx->last_width = xx;
	memcpy(&ttyctx->last_cell, &gc, sizeof ttyctx->last_cell);
}

/* Set a mode. */
void
screen_write_mode_set(struct screen_write_ctx *ctx, int mode)
{
	struct screen	*s = ctx->s;

	s->mode |= mode;
}

/* Clear a mode. */
void
screen_write_mode_clear(struct screen_write_ctx *ctx, int mode)
{
	struct screen	*s = ctx->s;

	s->mode &= ~mode;
}

/* Cursor up by ny. */
void
screen_write_cursorup(struct screen_write_ctx *ctx, u_int ny)
{
	struct screen	*s = ctx->s;

	if (ny == 0)
		ny = 1;

	if (s->cy < s->rupper) {
		/* Above region. */
		if (ny > s->cy)
			ny = s->cy;
	} else {
		/* Below region. */
		if (ny > s->cy - s->rupper)
			ny = s->cy - s->rupper;
	}
	if (s->cx == screen_size_x(s))
	    s->cx--;
	if (ny == 0)
		return;

	s->cy -= ny;
}

/* Cursor down by ny. */
void
screen_write_cursordown(struct screen_write_ctx *ctx, u_int ny)
{
	struct screen	*s = ctx->s;

	if (ny == 0)
		ny = 1;

	if (s->cy > s->rlower) {
		/* Below region. */
		if (ny > screen_size_y(s) - 1 - s->cy)
			ny = screen_size_y(s) - 1 - s->cy;
	} else {
		/* Above region. */
		if (ny > s->rlower - s->cy)
			ny = s->rlower - s->cy;
	}
	if (s->cx == screen_size_x(s))
	    s->cx--;
	if (ny == 0)
		return;

	s->cy += ny;
}

/* Cursor right by nx.  */
void
screen_write_cursorright(struct screen_write_ctx *ctx, u_int nx)
{
	struct screen	*s = ctx->s;

	if (nx == 0)
		nx = 1;

	if (nx > screen_size_x(s) - 1 - s->cx)
		nx = screen_size_x(s) - 1 - s->cx;
	if (nx == 0)
		return;

	s->cx += nx;
}

/* Cursor left by nx. */
void
screen_write_cursorleft(struct screen_write_ctx *ctx, u_int nx)
{
	struct screen	*s = ctx->s;

	if (nx == 0)
		nx = 1;

	if (nx > s->cx)
		nx = s->cx;
	if (nx == 0)
		return;

	s->cx -= nx;
}

/* Backspace; cursor left unless at start of wrapped line when can move up. */
void
screen_write_backspace(struct screen_write_ctx *ctx)
{
	struct screen		*s = ctx->s;
	struct grid_line	*gl;

	if (s->cx == 0) {
		if (s->cy == 0)
			return;
		gl = &s->grid->linedata[s->grid->hsize + s->cy - 1];
		if (gl->flags & GRID_LINE_WRAPPED) {
			s->cy--;
			s->cx = screen_size_x(s) - 1;
		}
	} else
		s->cx--;
}

/* VT100 alignment test. */
void
screen_write_alignmenttest(struct screen_write_ctx *ctx)
{
	struct screen		*s = ctx->s;
	struct tty_ctx	 	 ttyctx;
	struct grid_cell       	 gc;
	u_int			 xx, yy;

	screen_write_initctx(ctx, &ttyctx);

	memcpy(&gc, &grid_default_cell, sizeof gc);
	utf8_set(&gc.data, 'E');

	for (yy = 0; yy < screen_size_y(s); yy++) {
		for (xx = 0; xx < screen_size_x(s); xx++)
			grid_view_set_cell(s->grid, xx, yy, &gc);
	}

	s->cx = 0;
	s->cy = 0;

	s->rupper = 0;

	s->rlower = screen_size_y(s) - 1;

	tty_write(tty_cmd_alignmenttest, &ttyctx);
}

/* Insert nx characters. */
void
screen_write_insertcharacter(struct screen_write_ctx *ctx, u_int nx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	if (nx == 0)
		nx = 1;

	if (nx > screen_size_x(s) - s->cx)
		nx = screen_size_x(s) - s->cx;
	if (nx == 0)
		return;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cx <= screen_size_x(s) - 1)
		grid_view_insert_cells(s->grid, s->cx, s->cy, nx);

	ttyctx.num = nx;
	tty_write(tty_cmd_insertcharacter, &ttyctx);
}

/* Delete nx characters. */
void
screen_write_deletecharacter(struct screen_write_ctx *ctx, u_int nx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	if (nx == 0)
		nx = 1;

	if (nx > screen_size_x(s) - s->cx)
		nx = screen_size_x(s) - s->cx;
	if (nx == 0)
		return;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cx <= screen_size_x(s) - 1)
		grid_view_delete_cells(s->grid, s->cx, s->cy, nx);

	ttyctx.num = nx;
	tty_write(tty_cmd_deletecharacter, &ttyctx);
}

/* Clear nx characters. */
void
screen_write_clearcharacter(struct screen_write_ctx *ctx, u_int nx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	if (nx == 0)
		nx = 1;

	if (nx > screen_size_x(s) - s->cx)
		nx = screen_size_x(s) - s->cx;
	if (nx == 0)
		return;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cx <= screen_size_x(s) - 1)
		grid_view_clear(s->grid, s->cx, s->cy, nx, 1);

	ttyctx.num = nx;
	tty_write(tty_cmd_clearcharacter, &ttyctx);
}

/* Insert ny lines. */
void
screen_write_insertline(struct screen_write_ctx *ctx, u_int ny)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	if (ny == 0)
		ny = 1;

	if (s->cy < s->rupper || s->cy > s->rlower) {
		if (ny > screen_size_y(s) - s->cy)
			ny = screen_size_y(s) - s->cy;
		if (ny == 0)
			return;

		screen_write_initctx(ctx, &ttyctx);

		grid_view_insert_lines(s->grid, s->cy, ny);

		ttyctx.num = ny;
		tty_write(tty_cmd_insertline, &ttyctx);
		return;
	}

	if (ny > s->rlower + 1 - s->cy)
		ny = s->rlower + 1 - s->cy;
	if (ny == 0)
		return;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cy < s->rupper || s->cy > s->rlower)
		grid_view_insert_lines(s->grid, s->cy, ny);
	else
		grid_view_insert_lines_region(s->grid, s->rlower, s->cy, ny);

	ttyctx.num = ny;
	tty_write(tty_cmd_insertline, &ttyctx);
}

/* Delete ny lines. */
void
screen_write_deleteline(struct screen_write_ctx *ctx, u_int ny)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	if (ny == 0)
		ny = 1;

	if (s->cy < s->rupper || s->cy > s->rlower) {
		if (ny > screen_size_y(s) - s->cy)
			ny = screen_size_y(s) - s->cy;
		if (ny == 0)
			return;

		screen_write_initctx(ctx, &ttyctx);

		grid_view_delete_lines(s->grid, s->cy, ny);

		ttyctx.num = ny;
		tty_write(tty_cmd_deleteline, &ttyctx);
		return;
	}

	if (ny > s->rlower + 1 - s->cy)
		ny = s->rlower + 1 - s->cy;
	if (ny == 0)
		return;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cy < s->rupper || s->cy > s->rlower)
		grid_view_delete_lines(s->grid, s->cy, ny);
	else
		grid_view_delete_lines_region(s->grid, s->rlower, s->cy, ny);

	ttyctx.num = ny;
	tty_write(tty_cmd_deleteline, &ttyctx);
}

/* Clear line at cursor. */
void
screen_write_clearline(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	screen_write_initctx(ctx, &ttyctx);

	grid_view_clear(s->grid, 0, s->cy, screen_size_x(s), 1);

	tty_write(tty_cmd_clearline, &ttyctx);
}

/* Clear to end of line from cursor. */
void
screen_write_clearendofline(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;
	u_int		 sx;

	screen_write_initctx(ctx, &ttyctx);

	sx = screen_size_x(s);

	if (s->cx <= sx - 1)
		grid_view_clear(s->grid, s->cx, s->cy, sx - s->cx, 1);

	tty_write(tty_cmd_clearendofline, &ttyctx);
}

/* Clear to start of line from cursor. */
void
screen_write_clearstartofline(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;
	u_int		 sx;

	screen_write_initctx(ctx, &ttyctx);

	sx = screen_size_x(s);

	if (s->cx > sx - 1)
		grid_view_clear(s->grid, 0, s->cy, sx, 1);
	else
		grid_view_clear(s->grid, 0, s->cy, s->cx + 1, 1);

	tty_write(tty_cmd_clearstartofline, &ttyctx);
}

/* Move cursor to px,py.  */
void
screen_write_cursormove(struct screen_write_ctx *ctx, u_int px, u_int py)
{
	struct screen	*s = ctx->s;

	if (px > screen_size_x(s) - 1)
		px = screen_size_x(s) - 1;
	if (py > screen_size_y(s) - 1)
		py = screen_size_y(s) - 1;

	s->cx = px;
	s->cy = py;
}

/* Reverse index (up with scroll).  */
void
screen_write_reverseindex(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;

	screen_write_initctx(ctx, &ttyctx);

	if (s->cy == s->rupper)
		grid_view_scroll_region_down(s->grid, s->rupper, s->rlower);
	else if (s->cy > 0)
		s->cy--;

	tty_write(tty_cmd_reverseindex, &ttyctx);
}

/* Set scroll region. */
void
screen_write_scrollregion(struct screen_write_ctx *ctx, u_int rupper,
    u_int rlower)
{
	struct screen	*s = ctx->s;

	if (rupper > screen_size_y(s) - 1)
		rupper = screen_size_y(s) - 1;
	if (rlower > screen_size_y(s) - 1)
		rlower = screen_size_y(s) - 1;
	if (rupper >= rlower)	/* cannot be one line */
		return;

	/* Cursor moves to top-left. */
	s->cx = 0;
	s->cy = 0;

	s->rupper = rupper;
	s->rlower = rlower;
}

/* Line feed. */
void
screen_write_linefeed(struct screen_write_ctx *ctx, int wrapped)
{
	struct screen		*s = ctx->s;
	struct grid_line	*gl;
	struct tty_ctx	 	 ttyctx;

	screen_write_initctx(ctx, &ttyctx);

	gl = &s->grid->linedata[s->grid->hsize + s->cy];
	if (wrapped)
		gl->flags |= GRID_LINE_WRAPPED;
	else
		gl->flags &= ~GRID_LINE_WRAPPED;

	if (s->cy == s->rlower)
		grid_view_scroll_region_up(s->grid, s->rupper, s->rlower);
	else if (s->cy < screen_size_y(s) - 1)
		s->cy++;

	ttyctx.num = wrapped;
	tty_write(tty_cmd_linefeed, &ttyctx);
}

/* Carriage return (cursor to start of line). */
void
screen_write_carriagereturn(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;

	s->cx = 0;
}

/* Clear to end of screen from cursor. */
void
screen_write_clearendofscreen(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;
	u_int		 sx, sy;

	screen_write_initctx(ctx, &ttyctx);

	sx = screen_size_x(s);
	sy = screen_size_y(s);

	/* Scroll into history if it is enabled and clearing entire screen. */
	if (s->cy == 0 && s->grid->flags & GRID_HISTORY)
		grid_view_clear_history(s->grid);
	else {
		if (s->cx <= sx - 1)
			grid_view_clear(s->grid, s->cx, s->cy, sx - s->cx, 1);
		grid_view_clear(s->grid, 0, s->cy + 1, sx, sy - (s->cy + 1));
	}

	tty_write(tty_cmd_clearendofscreen, &ttyctx);
}

/* Clear to start of screen. */
void
screen_write_clearstartofscreen(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;
	u_int		 sx;

	screen_write_initctx(ctx, &ttyctx);

	sx = screen_size_x(s);

	if (s->cy > 0)
		grid_view_clear(s->grid, 0, 0, sx, s->cy);
	if (s->cx > sx - 1)
		grid_view_clear(s->grid, 0, s->cy, sx, 1);
	else
		grid_view_clear(s->grid, 0, s->cy, s->cx + 1, 1);

	tty_write(tty_cmd_clearstartofscreen, &ttyctx);
}

/* Clear entire screen. */
void
screen_write_clearscreen(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct tty_ctx	 ttyctx;
	u_int		 sx = screen_size_x(s);
	u_int		 sy = screen_size_y(s);

	screen_write_initctx(ctx, &ttyctx);

	/* Scroll into history if it is enabled. */
	if (s->grid->flags & GRID_HISTORY)
		grid_view_clear_history(s->grid);
	else
		grid_view_clear(s->grid, 0, 0, sx, sy);

	tty_write(tty_cmd_clearscreen, &ttyctx);
}

/* Clear entire history. */
void
screen_write_clearhistory(struct screen_write_ctx *ctx)
{
	struct screen	*s = ctx->s;
	struct grid	*gd = s->grid;

	grid_move_lines(gd, 0, gd->hsize, gd->sy);
	gd->hsize = 0;
}

/* Write cell data. */
void
screen_write_cell(struct screen_write_ctx *ctx, const struct grid_cell *gc)
{
	struct screen		*s = ctx->s;
	struct grid		*gd = s->grid;
	struct tty_ctx		 ttyctx;
	u_int		 	 width, xx, last;
	struct grid_cell 	 tmp_gc, now_gc;
	int			 insert, skip, selected;

	/* Ignore padding. */
	if (gc->flags & GRID_FLAG_PADDING)
		return;
	width = gc->data.width;

	/*
	 * If this is a wide character and there is no room on the screen, for
	 * the entire character, don't print it.
	 */
	if (!(s->mode & MODE_WRAP)
	    && (width > 1 && (width > screen_size_x(s) ||
		(s->cx != screen_size_x(s)
		 && s->cx > screen_size_x(s) - width))))
		return;

	/*
	 * If the width is zero, combine onto the previous character, if
	 * there is space.
	 */
	if (width == 0) {
		if (screen_write_combine(ctx, &gc->data) == 0) {
			screen_write_initctx(ctx, &ttyctx);
			tty_write(tty_cmd_utf8character, &ttyctx);
		}
		return;
	}

	/* Initialise the redraw context. */
	screen_write_initctx(ctx, &ttyctx);

	/* If in insert mode, make space for the cells. */
	if ((s->mode & MODE_INSERT) && s->cx <= screen_size_x(s) - width) {
		xx = screen_size_x(s) - s->cx - width;
		grid_move_cells(s->grid, s->cx + width, s->cx, s->cy, xx);
		insert = 1;
	} else
		insert = 0;
	skip = !insert;

	/* Check this will fit on the current line and wrap if not. */
	if ((s->mode & MODE_WRAP) && s->cx > screen_size_x(s) - width) {
		screen_write_linefeed(ctx, 1);
		s->cx = 0;	/* carriage return */
		skip = 0;
	}

	/* Sanity check cursor position. */
	if (s->cx > screen_size_x(s) - width || s->cy > screen_size_y(s) - 1)
		return;

	/* Handle overwriting of UTF-8 characters. */
	grid_view_get_cell(gd, s->cx, s->cy, &now_gc);
	if (screen_write_overwrite(ctx, &now_gc, width))
		skip = 0;

	/*
	 * If the new character is UTF-8 wide, fill in padding cells. Have
	 * already ensured there is enough room.
	 */
	for (xx = s->cx + 1; xx < s->cx + width; xx++) {
		grid_view_set_cell(gd, xx, s->cy, &screen_write_pad_cell);
		skip = 0;
	}

	/* If no change, do not draw. */
	if (skip)
		skip = (memcmp(&now_gc, gc, sizeof now_gc) == 0);

	/* Update the selection the flag and set the cell. */
	selected = screen_check_selection(s, s->cx, s->cy);
	if (selected && ~gc->flags & GRID_FLAG_SELECTED) {
		skip = 0;
		memcpy(&tmp_gc, gc, sizeof tmp_gc);
		tmp_gc.flags |= GRID_FLAG_SELECTED;
		grid_view_set_cell(gd, s->cx, s->cy, &tmp_gc);
	} else if (!selected && gc->flags & GRID_FLAG_SELECTED) {
		skip = 0;
		memcpy(&tmp_gc, gc, sizeof tmp_gc);
		tmp_gc.flags &= ~GRID_FLAG_SELECTED;
		grid_view_set_cell(gd, s->cx, s->cy, &tmp_gc);
	} else if (!skip)
		grid_view_set_cell(gd, s->cx, s->cy, gc);

	/*
	 * Move the cursor. If not wrapping, stick at the last character and
	 * replace it.
	 */
	last = !(s->mode & MODE_WRAP);
	if (s->cx <= screen_size_x(s) - last - width)
		s->cx += width;
	else
		s->cx = screen_size_x(s) - last;

	/* Create space for character in insert mode. */
	if (insert) {
		ttyctx.num = width;
		tty_write(tty_cmd_insertcharacter, &ttyctx);
	}

	/* Save last cell if it will be needed. */
	if (!skip && ctx->wp != NULL && ttyctx.ocx > ctx->wp->sx - width)
		screen_write_save_last(ctx, &ttyctx);

	/* Write to the screen. */
	if (selected) {
		memcpy(&tmp_gc, &s->sel.cell, sizeof tmp_gc);
		utf8_copy(&tmp_gc.data, &gc->data);
		tmp_gc.attr = tmp_gc.attr & ~GRID_ATTR_CHARSET;
		tmp_gc.attr |= gc->attr & GRID_ATTR_CHARSET;
		tmp_gc.flags = gc->flags;
		tmp_gc.flags &= ~(GRID_FLAG_FGRGB|GRID_FLAG_BGRGB);
		tmp_gc.flags &= ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
		tmp_gc.flags |= s->sel.cell.flags &
		    (GRID_FLAG_FG256|GRID_FLAG_BG256);
		ttyctx.cell = &tmp_gc;
		tty_write(tty_cmd_cell, &ttyctx);
	} else if (!skip) {
		ttyctx.cell = gc;
		tty_write(tty_cmd_cell, &ttyctx);
	}
}

/* Combine a UTF-8 zero-width character onto the previous. */
static int
screen_write_combine(struct screen_write_ctx *ctx, const struct utf8_data *ud)
{
	struct screen		*s = ctx->s;
	struct grid		*gd = s->grid;
	struct grid_cell	 gc;

	/* Can't combine if at 0. */
	if (s->cx == 0)
		return (-1);

	/* Empty data is out. */
	if (ud->size == 0)
		fatalx("UTF-8 data empty");

	/* Retrieve the previous cell. */
	grid_view_get_cell(gd, s->cx - 1, s->cy, &gc);

	/* Check there is enough space. */
	if (gc.data.size + ud->size > sizeof gc.data.data)
		return (-1);

	/* Append the data. */
	memcpy(gc.data.data + gc.data.size, ud->data, ud->size);
	gc.data.size += ud->size;

	/* Set the new cell. */
	grid_view_set_cell(gd, s->cx - 1, s->cy, &gc);

	return (0);
}

/*
 * UTF-8 wide characters are a bit of an annoyance. They take up more than one
 * cell on the screen, so following cells must not be drawn by marking them as
 * padding.
 *
 * So far, so good. The problem is, when overwriting a padding cell, or a UTF-8
 * character, it is necessary to also overwrite any other cells which covered
 * by the same character.
 */
static int
screen_write_overwrite(struct screen_write_ctx *ctx, struct grid_cell *gc,
    u_int width)
{
	struct screen		*s = ctx->s;
	struct grid		*gd = s->grid;
	struct grid_cell	 tmp_gc;
	u_int			 xx;
	int			 done = 0;

	if (gc->flags & GRID_FLAG_PADDING) {
		/*
		 * A padding cell, so clear any following and leading padding
		 * cells back to the character. Don't overwrite the current
		 * cell as that happens later anyway.
		 */
		xx = s->cx + 1;
		while (--xx > 0) {
			grid_view_get_cell(gd, xx, s->cy, &tmp_gc);
			if (~tmp_gc.flags & GRID_FLAG_PADDING)
				break;
			grid_view_set_cell(gd, xx, s->cy, &grid_default_cell);
		}

		/* Overwrite the character at the start of this padding. */
		grid_view_set_cell(gd, xx, s->cy, &grid_default_cell);
		done = 1;
	}

	/*
	 * Overwrite any padding cells that belong to a UTF-8 character
	 * we'll be overwriting with the current character.
	 */
	if (gc->data.width != 1 || gc->flags & GRID_FLAG_PADDING) {
		xx = s->cx + width - 1;
		while (++xx < screen_size_x(s)) {
			grid_view_get_cell(gd, xx, s->cy, &tmp_gc);
			if (~tmp_gc.flags & GRID_FLAG_PADDING)
				break;
			grid_view_set_cell(gd, xx, s->cy, &grid_default_cell);
			done = 1;
		}
	}

	return (done);
}

void
screen_write_setselection(struct screen_write_ctx *ctx, u_char *str, u_int len)
{
	struct tty_ctx	ttyctx;

	screen_write_initctx(ctx, &ttyctx);
	ttyctx.ptr = str;
	ttyctx.num = len;

	tty_write(tty_cmd_setselection, &ttyctx);
}

void
screen_write_rawstring(struct screen_write_ctx *ctx, u_char *str, u_int len)
{
	struct tty_ctx	ttyctx;

	screen_write_initctx(ctx, &ttyctx);
	ttyctx.ptr = str;
	ttyctx.num = len;

	tty_write(tty_cmd_rawstring, &ttyctx);
}
