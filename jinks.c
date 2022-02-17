#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <sys/ioctl.h>

#define szstr(str) \
	str, (sizeof(str) - 1)
#define err(msg) \
	fprintf(stderr, msg)
#define ALLOC "Error allocating memory.\n"
#define ROW_TOO_SMALL "Error setting row size to 0.\n"

typedef struct {
	unsigned ref;
	unsigned size;
	unsigned char sgr[];
} jx_fmt;

typedef struct {
	jx_fmt *fmt;
	char utf[4];
} jx_char;

typedef struct {
	int w;
	int lo_x, hi_x;
	jx_char col[];
} jx_row;

typedef struct {
	unsigned ref;
	int w, h;
	int lo_y, hi_y;
	jx_row **row;
} jx_buf;

typedef struct jx_ptcl {
	int x;
	int y;
	int new_x;
	int new_y;

	jx_char c;

	struct jx_ptcl *next;
} jx_ptcl;

#define STREAM_SIZE    128
#define FMT_CACHE_SIZE 128

struct jx_win {
	bool fullscreen;

	struct termios old_termios;
	struct termios raw_termios;

	struct {
		unsigned i;
		char ch[STREAM_SIZE];
	} stream;

	jx_fmt *fmt_cache[FMT_CACHE_SIZE];
	jx_fmt *fmt;

	int w, h;
	int max_w, max_h;
	jx_buf *buf;
	jx_ptcl *ptcl;

	unsigned cursor_x, cursor_y;
	bool show_cursor;

	void (*reflow)(void *);
	void *reflow_data;
} jx_win_struct;
struct jx_win *jx_win;

void flush_stream ()
{
	if (jx_win_struct.stream.i) {
		write(1, jx_win_struct.stream.ch, jx_win_struct.stream.i);
		jx_win_struct.stream.i = 0;
	}
}

void stream_char (char c)
{
	jx_win_struct.stream.ch[jx_win_struct.stream.i++] = c;
	if (jx_win_struct.stream.i == STREAM_SIZE)
		flush_stream();
}

void stream (char *str, unsigned size)
{
	while (jx_win_struct.stream.i + size >= STREAM_SIZE) {
		memcpy(jx_win_struct.stream.ch + jx_win_struct.stream.i,
		       str,
		       STREAM_SIZE - jx_win_struct.stream.i);
		str += STREAM_SIZE - jx_win_struct.stream.i;
		size -= STREAM_SIZE - jx_win_struct.stream.i;
		jx_win_struct.stream.i = STREAM_SIZE;
		flush_stream();
	}
	if (size) {
		memcpy(jx_win_struct.stream.ch + jx_win_struct.stream.i,
		       str,
		       size);
		jx_win_struct.stream.i += size;
	}
}

jx_fmt *ref_fmt (jx_fmt *fmt)
{
	if (fmt) fmt->ref += 1;
	return fmt;
}

jx_fmt *deref_fmt (jx_fmt *fmt)
{
	if (fmt) {
		fmt->ref -= 1;
		if (!fmt->ref)
			free(fmt);
	}
	return NULL;
}

jx_fmt *replace_fmt (jx_fmt **dest, jx_fmt *src)
{
	if (*dest) deref_fmt(*dest);
	*dest = ref_fmt(src);
	return src;
}

#define DONE 0

unsigned size_fmt (unsigned sgr, va_list args)
{
	unsigned size = 0;
	while (sgr) {
		size++;
		sgr = va_arg(args, unsigned);
	}
	return size;
}

unsigned hash_fmt (unsigned sgr, va_list args)
{
	unsigned hash = 1;
	while (sgr) {
		hash *= sgr;
		sgr = va_arg(args, unsigned);
	}
	return hash % (FMT_CACHE_SIZE - 1);
}

bool cmp_fmt (jx_fmt *fmt, unsigned sgr, va_list args)
{
	if (!fmt) return sgr ? false : true;

	for (unsigned i = 0; i < fmt->size; i++) {
		if (fmt->sgr[i] != sgr)
			return false;
		sgr = va_arg(args, unsigned);
	}
	return (!sgr);
}

jx_fmt *new_fmt (unsigned size, unsigned sgr, va_list args)
{
	jx_fmt *fmt = malloc(sizeof(jx_fmt) + size);
	if (!fmt) {
		err(ALLOC);
		return NULL;
	}

	fmt->size = size;
	for (unsigned i = 0; i < size; i++) {
		fmt->sgr[i] = sgr;
		sgr = va_arg(args, unsigned);
	}

	return fmt;
}

jx_fmt *set_fmt (unsigned sgr, ...)
{
	if (!sgr) return replace_fmt(&jx_win_struct.fmt, NULL);

	va_list args;

	va_start(args, sgr);
	unsigned size = size_fmt(sgr, args);
	va_end(args);

	va_start(args, sgr);
	unsigned hash = hash_fmt(sgr, args);
	va_end(args);

	jx_fmt *fmt = jx_win_struct.fmt_cache[hash];
	va_start(args, sgr);
	bool same = cmp_fmt(fmt, sgr, args);
	va_end(args);

	if (!same) {
		va_start(args, sgr);
		fmt = new_fmt(size, sgr, args);
		jx_win_struct.fmt_cache[hash] = fmt;
		va_end(args);
	}

	return replace_fmt(&jx_win_struct.fmt, fmt);
}

#define DEFAULT_FMT "\033[0m"

void stream_fmt (jx_fmt *fmt)
{
	char ch[4];

	if (jx_win_struct.fmt != fmt) {
		if (fmt) {
			stream(szstr(DEFAULT_FMT) - 1);
			ch[0] = ';';
			for (unsigned i = 0; i < fmt->size; i++) {
				if (fmt->sgr[i] > 99) {
					ch[1] = '0' + (fmt->sgr[i] / 100);
					ch[2] = '0' + ((fmt->sgr[i] / 10) % 10);
					ch[3] = '0' + (fmt->sgr[i] % 10);
					stream(ch, 4);
				} else if (fmt->sgr[i] > 9) {
					ch[1] = '0' + (fmt->sgr[i] / 10);
					ch[2] = '0' + (fmt->sgr[i] % 10);
					stream(ch, 3);
				} else {
					ch[1] = '0' + fmt->sgr[i];
					stream(ch, 2);
				}
			}
			stream_char('m');
		} else stream(szstr(DEFAULT_FMT));
		replace_fmt(&jx_win_struct.fmt, fmt);
	}
}

#define utf_size(utf) ( \
	(*(utf) & 128 && *(utf) & 64) ? ( \
		(*(utf) & 32) ? ( \
			(*(utf) & 16) ? ( \
				(  *(utf + 1) & 128 && \
				 !(*(utf + 1) & 64) && \
				   *(utf + 2) & 128 && \
				 !(*(utf + 2) & 64) && \
				   *(utf + 3) & 128 && \
				 !(*(utf + 3) & 64)) ? 4 : 1 \
			) : ( \
				(  *(utf + 1) & 128 && \
				 !(*(utf + 1) & 64) && \
				   *(utf + 2) & 128 && \
				 !(*(utf + 2) & 64)) ? 3 : 1 \
			) \
		) : ( \
			(  *(utf + 1) & 128 && \
			 !(*(utf + 1) & 64)) ? 2 : 1 \
		) \
	) : 1 \
)

unsigned utf_len (char *utf)
{
	unsigned len = 0;
	unsigned size;
	while (*utf) {
		len += 1;
		utf += utf_size(utf);
	}
	return len;
}

#define SKIP_SPACE "\033[C"

void stream_jx_char (jx_char c)
{
	stream_fmt(c.fmt);
	if (c.utf[0]) {
		stream(c.utf, utf_size(c.utf));
	} else stream(szstr(SKIP_SPACE));
}

jx_row *new_row (int w)
{
	jx_row *row = calloc(1, sizeof(jx_row) + w * sizeof(jx_char));
	if (!row) err(ALLOC);

	row->w = w;
	row->lo_x = w;
	row->hi_x = 0;

	return row;
}

jx_row *free_row (jx_row *row)
{
	if (row) {
		for (int x = 0; x < row->w; x++)
			deref_fmt(row->col[x].fmt);
		free(row);
	}
	return NULL;
}

jx_row *resize_row (jx_row *row, int w)
{
	if (!row) 
		return new_row(w);
	if (w < 1) {
		err(ROW_TOO_SMALL);
		return row;
	}
	if (row->w == w)
		return row;

	for (int i = w; i < row->w; i++) {
		if (row->col[i].fmt)
			deref_fmt(row->col[i].fmt);
	}

	jx_row *new = realloc(row, sizeof(jx_row) + w * sizeof(jx_char));
	if (!new) {
		err(ALLOC);
		return row;
	}
	row = new;

	for (int x = row->w; x < w; x++) {
		row->col[x].fmt = NULL;
		row->col[x].utf[0] = 0;
	}

	row->w = w;

	return row;
}

void stream_row (jx_row *row, int start, int w)
{
	if (start < 0) {
		w += start;
		start = 0;
	}
	if (start + w > row->w)
		w = row->w - start;
	if (start >= row->w || w < 1)
		return;

	for (int x = start; x < start + w; x++)
		stream_jx_char(row->col[x]);
}

void row_putchar (jx_row *row, unsigned x, jx_fmt *fmt, char *utf)
{
	if (x < row->lo_x)
		row->lo_x = x;
	if (x > row->hi_x)
		row->hi_x = x;

	replace_fmt(&row->col[x].fmt, fmt);
	memcpy(row->col[x].utf, utf, utf_size(utf));
}

void fill_row (jx_row *row, unsigned x, unsigned w, jx_fmt *fmt, char *utf)
{
	if (x < row->lo_x)
		row->lo_x = x;
	if (x + w - 1 > row->hi_x)
		row->hi_x = x + w - 1;

	while (w) {
		row_putchar(row, x, fmt, utf);
		x++;
		w--;
	}
}

void row_put (jx_row *row, unsigned x, unsigned w, char *utf)
{
	if (x >= row->w)
		return;
	if (x + w > row->w)
		w = row->w - x;

	while (w) {
		row_putchar(row, x, jx_win_struct.fmt, utf);
		utf += utf_size(utf);
		x++;
		w--;
	}
}

void row_blit (jx_row *dest, unsigned to_x, unsigned max_w, jx_row *src, unsigned from_x, unsigned w)
{
	if (from_x > src->w)
		return;
	if (to_x > dest->w)
		return;
	if (from_x + w > src->w)
		w = src->w - from_x;
	if (to_x + max_w > dest->w)
		max_w = dest->w - to_x;
	if (w > max_w)
		w = max_w;

	while (w) {
		row_putchar(dest,
		            to_x,
		            src->col[from_x].fmt,
		            src->col[from_x].utf);
		to_x++;
		from_x++;
		w--;
	}
}

jx_buf *ref_buf (jx_buf *buf)
{
	if (buf) buf->ref += 1;
	return buf;
}

jx_buf *deref_buf (jx_buf *buf)
{
	if (buf) {
		buf->ref -= 1;
		if (!buf->ref) {
			for (int y = 0; y < buf->h; y++)
				free_row(buf->row[y]);
			free(buf->row);
			free(buf);
		}
	}
	return NULL;
}

jx_buf *new_buf (int w, int h)
{
	jx_buf *buf = calloc(1, sizeof(jx_buf));
	if (buf) buf->row = calloc(h, sizeof(jx_row *));
	if (!buf || !buf->row) {
		err(ALLOC);
		if (buf) free(buf);
		return NULL;
	}

	for (int y = 0; y < h; y++) {
		if (!(buf->row[y] = new_row(w))) {
			for (int i = 0; i < y; i++)
				free_row(buf->row[i]);
			free(buf->row);
			free(buf);
			return NULL;
		}
	}

	buf->w = w;
	buf->h = h;
	buf->lo_y = h;
	buf->hi_y = 0;
	return buf;
}

typedef struct {
	int x;
	int y;
	int w;
	int h;
} jx_rect;

jx_rect intersect_rect (jx_rect a, jx_rect b)
{
	jx_rect inter;

	if (a.x < b.x) {
		if (a.x + a.w > b.x) {
			if (a.x + a.w < b.x + b.w) {
				inter.x = b.x;
				inter.w = a.x + a.w - b.x;
			} else {
				inter.x = b.x;
				inter.w = b.w;
			}
		} else {
			inter.x = 0;
			inter.w = 0;
		}
	} else {
		if (b.x + b.w > a.x) {
			if (b.x + b.w < a.x + a.w) {
				inter.x = a.x;
				inter.w = b.x + b.w - a.x;
			} else {
				inter.x = a.x;
				inter.w = a.w;
			}
		} else {
			inter.x = 0;
			inter.w = 0;
		}
	}

	if (a.y < b.y) {
		if (a.y + a.h > b.y) {
			if (a.y + a.h < b.y + b.h) {
				inter.y = b.y;
				inter.h = a.y + a.h - b.y;
			} else {
				inter.y = b.y;
				inter.h = b.h;
			}
		} else {
			inter.y = 0;
			inter.h = 0;
		}
	} else {
		if (b.y + b.h > a.y) {
			if (b.y + b.h < a.y + a.h) {
				inter.y = a.y;
				inter.h = b.y + b.h - a.y;
			} else {
				inter.y = a.y;
				inter.h = a.h;
			}
		} else {
			inter.y = 0;
			inter.h = 0;
		}
	}

	return inter;
}

jx_rect get_rect (jx_buf *buf, jx_rect *rect_p)
{
	jx_rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = buf->w;
	rect.h = buf->h;

	if (rect_p) {
		return intersect_rect(rect, *rect_p);
	} else return rect;
}

void buf_fill_rect (jx_buf *buf, jx_rect *rect_p, jx_fmt *fill_fmt, char *fill)
{
	jx_rect rect = get_rect(buf, rect_p);
	if (rect.y < buf->lo_y)
		buf->lo_y = rect.y;
	if (rect.y + rect.h - 1 > buf->hi_y)
		buf->hi_y = rect.y + rect.h - 1;

	while (rect.h) {

		fill_row(buf->row[rect.y], rect.x, rect.w, fill_fmt, fill);
		rect.y++;
		rect.h--;
	}
}

void buf_putchar (jx_buf *buf, int x, int y, jx_fmt *fmt, char *utf)
{
	if (y >= 0 && y < buf->h && x >= 0 && x < buf->w) {
		if (y < buf->lo_y)
			buf->lo_y = y;
		if (y > buf->hi_y)
			buf->hi_y = y;

		row_putchar(buf->row[y], x, fmt, utf);
	}
}

#define fill_rect(rect_p, fill_fmt, fill) \
	buf_fill_rect(jx_win->buf, rect_p, fill_fmt, fill)

jx_ptcl *add_ptcl (int x, int y, jx_fmt *fmt, char *utf)
{
	jx_ptcl *ptcl = malloc(sizeof(jx_ptcl));
	if (!ptcl) {
		err(ALLOC);
		return NULL;
	}

	ptcl->x = -1;
	ptcl->y = -1;
	ptcl->new_x = x;
	ptcl->new_y = y;
	ptcl->c.fmt = ref_fmt(fmt);
	memcpy(ptcl->c.utf, utf, utf_size(utf));
	ptcl->next = jx_win->ptcl;
	jx_win->ptcl = ptcl;

	return ptcl;
}

#define MAX_DIGITS 16
#define MOVE_PREFIX "\033["
#define MOVE_STREAM_SIZE (MAX_DIGITS + sizeof(MOVE_PREFIX))

void stream_move (unsigned delta, char dir)
{
	if (!delta) return;

	char ch[MOVE_STREAM_SIZE];
	unsigned i = MOVE_STREAM_SIZE - 2;
	ch[i + 1] = dir;
	do {
		ch[i] = '0' + (delta % 10);
		delta /= 10;
		i--;
	} while (delta);
	i -= sizeof(MOVE_PREFIX) - 2;
	memcpy(ch + i, MOVE_PREFIX, sizeof(MOVE_PREFIX) - 1);
	stream(ch + i, MOVE_STREAM_SIZE - i);
}

void stream_move_to (unsigned x, unsigned y)
{
	if (x < jx_win_struct.cursor_x) {
		stream_move(jx_win_struct.cursor_x - x, 'D');
	} else if (x > jx_win_struct.cursor_x) {
		stream_move(x - jx_win_struct.cursor_x, 'C');
	}
	if (y < jx_win_struct.cursor_y) {
		stream_move(jx_win_struct.cursor_y - y, 'A');
	} else if (y > jx_win_struct.cursor_y) {
		stream_move(y - jx_win_struct.cursor_y, 'B');
	}
	jx_win_struct.cursor_x = x;
	jx_win_struct.cursor_y = y;
}

void redraw ();
void resize (int i)
{
	struct winsize ws;
	ioctl(1, TIOCGWINSZ, &ws);

	jx_win_struct.w = ws.ws_col;
	jx_win_struct.h = ws.ws_row;

	if (jx_win) {
		if (jx_win->reflow)
			jx_win->reflow(jx_win->reflow_data);
		redraw();
	}
}

void set_reflow_fn (void (*fn)(void *), void *data)
{
	if (jx_win) {
		jx_win->reflow = fn;
		jx_win->reflow_data = data;
		if (fn) fn(data);
	}
}

#define JX_INIT "\033[?25l\033[?1049h\033[2J\033[1;1H"
#define JX_QUIT "\033[?25h\033[2J\033[?1049l"

#define JX_HIDE_CURSOR "\033[?25l"
#define JX_SHOW_CURSOR "\033[?25h"
#define JX_ALT_BUFFER  "\033[?1049h"
#define JX_OLD_BUFFER  "\033[?1049l"

void redraw ()
{
	tcsetattr(0, TCSANOW, &jx_win->old_termios);
	stream(szstr(JX_HIDE_CURSOR));

	int lo_w = jx_win->w < jx_win->buf->w ? jx_win->w : jx_win->buf->w;
	int lo_h = jx_win->h < jx_win->buf->h ? jx_win->h : jx_win->buf->h;

	set_fmt(DONE);
	stream_move_to(0, 0);
	for (int y = 0; y < lo_h - 1; y++) {
		stream_row(jx_win->buf->row[y], 0, lo_w);
		stream_char('\n');
	}
	stream_row(jx_win->buf->row[jx_win->buf->h - 1], 0, lo_w);

	jx_win->cursor_x = jx_win->buf->w;
	jx_win->cursor_y = jx_win->buf->h - 1;

	flush_stream();

	tcsetattr(0, TCSANOW, &jx_win->raw_termios);
}

#define JX_WINSZ      1
#define JX_FULLSCREEN 2

bool jinks_start (unsigned code, ...)
{
	resize(0);

	int w = jx_win_struct.w;
	int h = jx_win_struct.h;

	va_list args;
	va_start(args, code);
	while (code) {
		switch (code) {
			case JX_WINSZ:
				w = va_arg(args, int);
				h = va_arg(args, int);
				break;
			case JX_FULLSCREEN:
				jx_win_struct.fullscreen = true;
				stream(szstr(JX_ALT_BUFFER));
				break;
		}
		code = va_arg(args, unsigned);
	}
	va_end(args);

	if (!(jx_win_struct.buf = new_buf(w, h)))
		return false;
	buf_fill_rect(jx_win_struct.buf, NULL, NULL, " ");

	signal(SIGWINCH, resize);

	// Create non-blocking key handling termios
	tcgetattr(0, &jx_win_struct.raw_termios);
	memcpy(&jx_win_struct.old_termios, &jx_win_struct.raw_termios, sizeof(struct termios));
	cfmakeraw(&jx_win_struct.raw_termios);

	stream(szstr(JX_HIDE_CURSOR));

	jx_win = &jx_win_struct;

	redraw();

	return true;
}

void jinks_end ()
{
	tcsetattr(0, TCSANOW, &jx_win->old_termios);
	stream(szstr(DEFAULT_FMT));
	stream(szstr(JX_SHOW_CURSOR));
	if (jx_win->fullscreen) {
		jx_win->fullscreen = false;
		stream(szstr(JX_OLD_BUFFER));
	}
	flush_stream();

	jx_win->buf = deref_buf(jx_win->buf);
	jx_win = NULL;

}

void flip ()
{
	for (jx_ptcl *ptcl = jx_win->ptcl; ptcl; ptcl = ptcl->next) {
		if ((ptcl->x != ptcl->new_x || ptcl->y != ptcl->new_y) &&
		    ptcl->x >= 0 && ptcl->x < jx_win->buf->w &&
		    ptcl->y >= 0 && ptcl->y < jx_win->buf->h) {
			jx_win->buf->lo_y = 0;
			if (ptcl->y > jx_win->buf->hi_y)
				jx_win->buf->hi_y = ptcl->y;
			jx_win->buf->row[ptcl->y]->lo_x = 0;
			jx_win->buf->row[ptcl->y]->hi_x = jx_win->buf->w - 1;
		}
	}

	int y;
	for (y = jx_win->buf->lo_y; y <= jx_win->buf->hi_y; y++) {
		stream_move_to(jx_win->buf->row[y]->lo_x, y);
		if (jx_win->buf->row[y]->lo_x <= jx_win->buf->row[y]->hi_x) {
			stream_row(jx_win->buf->row[y],
			           jx_win->buf->row[y]->lo_x,
			           jx_win->buf->row[y]->hi_x - jx_win->buf->row[y]->lo_x + 1);
			jx_win->cursor_x = jx_win->buf->row[y]->hi_x + 2;
		}
		jx_win->buf->row[y]->lo_x = jx_win->buf->row[y]->w;
		jx_win->buf->row[y]->hi_x = 0;
	}

	for (jx_ptcl *ptcl = jx_win->ptcl; ptcl; ptcl = ptcl->next) {
		ptcl->x = ptcl->new_x;
		ptcl->y = ptcl->new_y;

		if (ptcl->x >= 0 && ptcl->x < jx_win->buf->w &&
		    ptcl->y >= 0 && ptcl->y < jx_win->buf->h) {
			stream_move_to(ptcl->x, ptcl->y);
			stream_jx_char(ptcl->c);
		}
	}

	flush_stream();

	jx_win->buf->lo_y = jx_win->buf->h;
	jx_win->buf->hi_y = 0;
}

#define STDIN 0

int jx_kbhit ()
{
	struct timeval timeout;
	fd_set rdset;

	FD_ZERO(&rdset);
	FD_SET(STDIN, &rdset);
	timeout.tv_sec  = 0;
	timeout.tv_usec = 0;

	return select(STDIN + 1, &rdset, NULL, NULL, &timeout);
}

unsigned jx_getkey ()
{
	unsigned k = 0;
	read(STDIN, &k, sizeof(unsigned));
	fflush(STDIN);
	return k;
}

#define JX_KEY_ESC   27
#define JX_KEY_DOWN  4348699
#define JX_KEY_UP    4283163
#define JX_KEY_LEFT  4479771
#define JX_KEY_RIGHT 4414235

int main (int argc, char **args)
{
	char ch[4];
	jx_rect rect;
	jx_ptcl *man;

	if (jinks_start(JX_WINSZ, 20, 20, DONE)) {
		for (int i = 0; i < 10; i++) {
			rect.x = rand() % jx_win->buf->w;
			rect.y = rand() % jx_win->buf->h;
			rect.w = rand() % (jx_win->buf->w - rect.x);
			rect.h = rand() % (jx_win->buf->h - rect.y);
			ch[0] = 'a' + (rand() % 26);

			fill_rect(&rect, NULL, ch);
		}
		man = add_ptcl(0, 0, NULL, "@");
		for (bool run = true; run; ) {
			flip();

			while (!jx_kbhit());
			switch (jx_getkey()) {
				case JX_KEY_ESC:
					run = false;
					break;
				case JX_KEY_LEFT:
					man->new_x -= 1;
					break;
				case JX_KEY_RIGHT:
					man->new_x += 1;
					break;
				case JX_KEY_UP:
					man->new_y -= 1;
					break;
				case JX_KEY_DOWN:
					man->new_y += 1;
					break;
			}
		}
		jinks_end();
	}
	printf("%i %i\n", man->x, man->y);

	return 0;
}
