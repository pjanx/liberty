/*
 * liberty-xui.c: the ultimate C unlibrary: hybrid terminal/X11 UI
 *
 * Copyright (c) 2016 - 2024, Přemysl Eric Janouch <p@janouch.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// This file includes some common stuff to build terminal/X11 applications with.
// It assumes you've already included liberty.c, and may include liberty-xdg.c.

#include <ncurses.h>

// It is surprisingly hard to find a good library to handle Unicode shenanigans,
// and there's enough of those for it to be impractical to reimplement them.
//
//                        GLib        ICU   libunistring  utf8proc libgrapheme
// Decently sized           .          .          x          x          x
// Grapheme breaks          .          x          .          x          x
// Character width          x          .          x          x          .
// Locale handling          .          .          x          .          .
// Liberal license          .          x          .          x          x
//
// Also note that the ICU API is icky and uses UTF-16 for its primary encoding.
//
// Currently we're chugging along with libunistring but utf8proc seems viable.
// Non-Unicode locales can mostly be handled with simple iconv like in tdv.
// Similarly grapheme breaks can be guessed at using character width (a basic
// test here is Zalgo text).
//
// None of this is ever going to work too reliably anyway because terminals
// and Unicode don't go awfully well together.  In particular, character cell
// devices have some problems with double-wide characters.

#include <unistr.h>
#include <uniwidth.h>
#include <uniconv.h>
#include <unicase.h>

#include <termios.h>
#ifdef HAVE_RESIZETERM
#include <sys/ioctl.h>
#endif  // HAVE_RESIZETERM

// ncurses is notoriously retarded for input handling, and in past versions
// used to process mouse events unreliably.  Moreover, rxvt-unicode only
// supports the 1006 mode that ncurses also supports mode starting with 9.25.
#include "termo.h"

// Carefully chosen to limit the possibility of ever hitting termo keymods.
enum { XUI_KEYMOD_DOUBLE_CLICK = 1 << 15 };

// Elementary port of TUI facilities to X11.
#ifdef LIBERTY_XUI_WANT_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>

#define LIBERTY_XDG_WANT_X11
#define LIBERTY_XDG_WANT_ICONS
#include "liberty-xdg.c"
#endif  // LIBERTY_XUI_WANT_X11

// The application needs to implement these.
static void app_quit (void);
static void app_layout (void);
static bool app_process_termo_event (termo_key_t *event);
static bool app_process_mouse (termo_mouse_event_t type,
	int x, int y, int button, int modifiers);
static bool app_on_insufficient_color (void);
static void app_on_clipboard_copy (const char *text);

// This could be overridable, however thus far row_buffer and line_editor both
// depend on XUI being initialized.
static bool xui_is_character_in_locale (ucs4_t ch);

// --- Utilities ---------------------------------------------------------------

// Unlike poller_timers_get_current_time(), this has a hard dependency
// on _POSIX_TIMERS, and can be used with both realtime nad monotonic clocks.
static int64_t
clock_msec (clockid_t clock)
{
	struct timespec tp;
	hard_assert (clock_gettime (clock, &tp) != -1);
	return (int64_t) tp.tv_sec * 1000 + (int64_t) tp.tv_nsec / 1000000;
}

// --- Configurable display attributes -----------------------------------------

struct attrs
{
	short fg;                           ///< Foreground colour index
	short bg;                           ///< Background colour index
	chtype attrs;                       ///< Other attributes
};

/// Decode attributes in the value using a subset of the git config format,
/// ignoring all errors since it doesn't affect functionality
static struct attrs
attrs_decode (const char *value)
{
	struct strv v = strv_make ();
	cstr_split (value, " ", true, &v);

	int colors = 0;
	struct attrs attrs = { -1, -1, 0 };
	for (char **it = v.vector; *it; it++)
	{
		char *end = NULL;
		long n = strtol (*it, &end, 10);
		if (*it != end && !*end && n >= SHRT_MIN && n <= SHRT_MAX)
		{
			if (colors == 0) attrs.fg = n;
			if (colors == 1) attrs.bg = n;
			colors++;
		}
		else if (!strcmp (*it, "bold"))    attrs.attrs |= A_BOLD;
		else if (!strcmp (*it, "dim"))     attrs.attrs |= A_DIM;
		else if (!strcmp (*it, "ul"))      attrs.attrs |= A_UNDERLINE;
		else if (!strcmp (*it, "blink"))   attrs.attrs |= A_BLINK;
		else if (!strcmp (*it, "reverse")) attrs.attrs |= A_REVERSE;
#ifdef A_ITALIC
		else if (!strcmp (*it, "italic"))  attrs.attrs |= A_ITALIC;
#endif  // A_ITALIC
	}
	strv_free (&v);
	return attrs;
}

// --- Line editor -------------------------------------------------------------

enum line_editor_action
{
	LINE_EDITOR_B_CHAR,                 ///< Go back a character
	LINE_EDITOR_F_CHAR,                 ///< Go forward a character
	LINE_EDITOR_B_WORD,                 ///< Go back a word
	LINE_EDITOR_F_WORD,                 ///< Go forward a word
	LINE_EDITOR_HOME,                   ///< Go to start of line
	LINE_EDITOR_END,                    ///< Go to end of line

	LINE_EDITOR_UPCASE_WORD,            ///< Convert word to uppercase
	LINE_EDITOR_DOWNCASE_WORD,          ///< Convert word to lowercase
	LINE_EDITOR_CAPITALIZE_WORD,        ///< Capitalize word

	LINE_EDITOR_B_DELETE,               ///< Delete last character
	LINE_EDITOR_F_DELETE,               ///< Delete next character
	LINE_EDITOR_B_KILL_WORD,            ///< Delete last word
	LINE_EDITOR_B_KILL_LINE,            ///< Delete everything up to BOL
	LINE_EDITOR_F_KILL_LINE,            ///< Delete everything up to EOL
};

struct line_editor
{
	int point;                          ///< Caret index into line data
	ucs4_t *line;                       ///< Line data, 0-terminated
	int *w;                             ///< Codepoint widths, 0-terminated
	size_t len;                         ///< Editor length
	size_t alloc;                       ///< Editor allocated
	char prompt;                        ///< Prompt character

	void (*on_changed) (void);          ///< Callback on text change
	void (*on_end) (bool);              ///< Callback on abort
};

static void
line_editor_free (struct line_editor *self)
{
	free (self->line);
	free (self->w);
}

/// Notify whomever invoked the editor that it's been either confirmed or
/// cancelled and clean up editor state
static void
line_editor_abort (struct line_editor *self, bool status)
{
	self->on_end (status);
	self->on_changed = NULL;

	free (self->line);
	self->line = NULL;
	free (self->w);
	self->w = NULL;
	self->alloc = 0;
	self->len = 0;
	self->point = 0;
	self->prompt = 0;
}

/// Start the line editor; remember to fill in "change" and "end" callbacks
static void
line_editor_start (struct line_editor *self, char prompt)
{
	self->alloc = 16;
	self->line = xcalloc (self->alloc, sizeof *self->line);
	self->w = xcalloc (self->alloc, sizeof *self->w);
	self->len = 0;
	self->point = 0;
	self->prompt = prompt;
}

static void
line_editor_changed (struct line_editor *self)
{
	self->line[self->len] = 0;
	self->w[self->len] = 0;

	if (self->on_changed)
		self->on_changed ();
}

static void
line_editor_move (struct line_editor *self, int to, int from, int len)
{
	memmove (self->line + to, self->line + from,
		sizeof *self->line * len);
	memmove (self->w + to, self->w + from,
		sizeof *self->w * len);
}

static void
line_editor_insert (struct line_editor *self, ucs4_t codepoint)
{
	while (self->alloc - self->len < 2 /* inserted + sentinel */)
	{
		self->alloc <<= 1;
		self->line = xreallocarray
			(self->line, sizeof *self->line, self->alloc);
		self->w = xreallocarray
			(self->w, sizeof *self->w, self->alloc);
	}

	line_editor_move (self, self->point + 1, self->point,
		self->len - self->point);
	self->line[self->point] = codepoint;
	self->w[self->point] = xui_is_character_in_locale (codepoint)
		? uc_width (codepoint, locale_charset ())
		: 1 /* the replacement question mark */;

	self->point++;
	self->len++;
	line_editor_changed (self);
}

static bool
line_editor_action (struct line_editor *self, enum line_editor_action action)
{
	switch (action)
	{
	default:
		return soft_assert (!"unknown line editor action");

	case LINE_EDITOR_B_CHAR:
		if (self->point < 1)
			return false;
		do self->point--;
		while (self->point > 0
			&& !self->w[self->point]);
		return true;
	case LINE_EDITOR_F_CHAR:
		if (self->point + 1 > (int) self->len)
			return false;
		do self->point++;
		while (self->point < (int) self->len
			&& !self->w[self->point]);
		return true;
	case LINE_EDITOR_B_WORD:
	{
		if (self->point < 1)
			return false;
		int i = self->point;
		while (i && self->line[--i] == ' ');
		while (i-- && self->line[i] != ' ');
		self->point = ++i;
		return true;
	}
	case LINE_EDITOR_F_WORD:
	{
		if (self->point + 1 > (int) self->len)
			return false;
		int i = self->point;
		while (i < (int) self->len && self->line[i] == ' ') i++;
		while (i < (int) self->len && self->line[i] != ' ') i++;
		self->point = i;
		return true;
	}
	case LINE_EDITOR_HOME:
		self->point = 0;
		return true;
	case LINE_EDITOR_END:
		self->point = self->len;
		return true;

	case LINE_EDITOR_UPCASE_WORD:
	{
		int i = self->point;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
			self->line[i] = uc_toupper (self->line[i]);
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_DOWNCASE_WORD:
	{
		int i = self->point;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
			self->line[i] = uc_tolower (self->line[i]);
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_CAPITALIZE_WORD:
	{
		int i = self->point;
		ucs4_t (*converter) (ucs4_t) = uc_totitle;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
		{
			self->line[i] = converter (self->line[i]);
			converter = uc_tolower;
		}
		self->point = i;
		line_editor_changed (self);
		return true;
	}

	case LINE_EDITOR_B_DELETE:
	{
		if (self->point < 1)
			return false;
		int len = 1;
		while (self->point - len > 0
			&& !self->w[self->point - len])
			len++;
		line_editor_move (self, self->point - len, self->point,
			self->len - self->point);
		self->len -= len;
		self->point -= len;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_F_DELETE:
	{
		if (self->point + 1 > (int) self->len)
			return false;
		int len = 1;
		while (self->point + len < (int) self->len
			&& !self->w[self->point + len])
			len++;
		self->len -= len;
		line_editor_move (self, self->point, self->point + len,
			self->len - self->point);
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_B_KILL_WORD:
	{
		if (self->point < 1)
			return false;

		int i = self->point;
		while (i && self->line[--i] == ' ');
		while (i-- && self->line[i] != ' ');
		i++;

		line_editor_move (self, i, self->point, (self->len - self->point));
		self->len -= self->point - i;
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_B_KILL_LINE:
		self->len -= self->point;
		line_editor_move (self, 0, self->point, self->len);
		self->point = 0;
		line_editor_changed (self);
		return true;
	case LINE_EDITOR_F_KILL_LINE:
		self->len = self->point;
		line_editor_changed (self);
		return true;
	}
}

// --- Terminal output ---------------------------------------------------------

// Necessary abstraction to simplify aligned, formatted character output

struct row_char
{
	ucs4_t c;                           ///< Unicode codepoint
	chtype attrs;                       ///< Special attributes
	int width;                          ///< How many cells this takes
};

struct row_buffer
{
	ARRAY (struct row_char, chars)      ///< Characters
	int total_width;                    ///< Total width of all characters
};

static struct row_buffer
row_buffer_make (void)
{
	struct row_buffer self = {};
	ARRAY_INIT_SIZED (self.chars, 256);
	return self;
}

static void
row_buffer_free (struct row_buffer *self)
{
	free (self->chars);
}

static void
row_buffer_append_c (struct row_buffer *self, ucs4_t c, chtype attrs)
{
	struct row_char current = { .attrs = attrs, .c = c };
	struct row_char invalid = { .attrs = attrs, .c = '?', .width = 1 };

	current.width = uc_width (current.c, locale_charset ());
	if (current.width < 0 || !xui_is_character_in_locale (current.c))
		current = invalid;

	ARRAY_RESERVE (self->chars, 1);
	self->chars[self->chars_len++] = current;
	self->total_width += current.width;
}

/// Replace invalid chars and push all codepoints to the array w/ attributes.
static void
row_buffer_append (struct row_buffer *self, const char *str, chtype attrs)
{
	// The encoding is only really used internally for some corner cases
	const char *encoding = locale_charset ();

	// Note that this function is a hotspot, try to keep it decently fast
	struct row_char current = { .attrs = attrs };
	struct row_char invalid = { .attrs = attrs, .c = '?', .width = 1 };
	const uint8_t *next = (const uint8_t *) str;
	while ((next = u8_next (&current.c, next)))
	{
		current.width = uc_width (current.c, encoding);
		if (current.width < 0 || !xui_is_character_in_locale (current.c))
			current = invalid;

		ARRAY_RESERVE (self->chars, 1);
		self->chars[self->chars_len++] = current;
		self->total_width += current.width;
	}
}

/// Pop as many codepoints as needed to free up "space" character cells.
/// Given the suffix nature of combining marks, this should work pretty fine.
static int
row_buffer_pop_cells (struct row_buffer *self, int space)
{
	int made = 0;
	while (self->chars_len && made < space)
		made += self->chars[--self->chars_len].width;
	self->total_width -= made;
	return made;
}

static void
row_buffer_space (struct row_buffer *self, int width, chtype attrs)
{
	if (width < 0)
		return;

	ARRAY_RESERVE (self->chars, (size_t) width);

	struct row_char space = { .attrs = attrs, .c = ' ', .width = 1 };
	self->total_width += width;
	while (width-- > 0)
		self->chars[self->chars_len++] = space;
}

static void
row_buffer_ellipsis (struct row_buffer *self, int target)
{
	if (self->total_width <= target
	 || !row_buffer_pop_cells (self, self->total_width - target))
		return;

	// We use attributes from the last character we've removed,
	// assuming that we don't shrink the array (and there's no real need)
	ucs4_t ellipsis = 0x2026; // …
	if (xui_is_character_in_locale (ellipsis))
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 1);
		if (self->total_width + 1 <= target)
			row_buffer_append (self, "…",   self->chars[self->chars_len].attrs);
	}
	else if (target >= 3)
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 3);
		if (self->total_width + 3 <= target)
			row_buffer_append (self, "...", self->chars[self->chars_len].attrs);
	}
}

static void
row_buffer_align (struct row_buffer *self, int target, chtype attrs)
{
	row_buffer_ellipsis (self, target);
	row_buffer_space (self, target - self->total_width, attrs);
}

static void
row_buffer_print (uint32_t *ucs4, chtype attrs)
{
	// This assumes that we can reset the attribute set without consequences
	char *str = u32_strconv_to_locale (ucs4);
	if (str)
	{
		attrset (attrs);
		addstr (str);
		attrset (0);
		free (str);
	}
}

static void
row_buffer_flush (struct row_buffer *self)
{
	if (!self->chars_len)
		return;

	// We only NUL-terminate the chunks because of the libunistring API
	uint32_t chunk[self->chars_len + 1], *insertion_point = chunk;
	for (size_t i = 0; i < self->chars_len; i++)
	{
		struct row_char *iter = self->chars + i;
		if (i && iter[0].attrs != iter[-1].attrs)
		{
			row_buffer_print (chunk, iter[-1].attrs);
			insertion_point = chunk;
		}
		*insertion_point++ = iter->c;
		*insertion_point = 0;
	}
	row_buffer_print (chunk, self->chars[self->chars_len - 1].attrs);
}

// --- XUI ---------------------------------------------------------------------

struct widget;

/// Draw a widget on the window
typedef void (*widget_render_fn) (struct widget *self);

/// The widget has been placed
typedef void (*widget_allocated_fn) (struct widget *self);

/// Extended attributes
enum { XUI_ATTR_MONOSPACE = 1 << 0 };

/// A minimal abstraction appropriate for both TUI and GUI widgets.
/// Units for the widget's region are frontend-specific.
/// Having this as a linked list simplifies layouting and memory management.
struct widget
{
	LIST_HEADER (struct widget)

	int x;                              ///< X coordinate
	int y;                              ///< Y coordinate
	int width;                          ///< Width, initialized by UI methods
	int height;                         ///< Height, initialized by UI methods

	widget_render_fn on_render;         ///< Render callback
	widget_allocated_fn on_allocated;   ///< Allocation callback
	struct widget *children;            ///< Child widgets of containers
	chtype attrs;                       ///< Rendition, in Curses terms
	unsigned extended_attrs;            ///< XUI-specific attributes

	int id;                             ///< Post-layouting identification
	int userdata;                       ///< Action ID/Tab index/...
	char text[];                        ///< Any text label
};

static void
widget_destroy (struct widget *self)
{
	LIST_FOR_EACH (struct widget, w, self->children)
		widget_destroy (w);
	free (self);
}

static void
widget_move (struct widget *w, int dx, int dy)
{
	w->x += dx;
	w->y += dy;
	LIST_FOR_EACH (struct widget, child, w->children)
		widget_move (child, dx, dy);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct ui
{
	struct widget *(*padding)
		(chtype attrs, float width, float height);
	struct widget *(*label)
		(chtype attrs, unsigned extended, const char *label);

	void (*render) (void);
	void (*flip) (void);
	void (*winch) (void);
	void (*destroy) (void);
};

#ifdef LIBERTY_XUI_WANT_X11

/// Wraps Xft fonts into a linked list with fallbacks.
struct x11_font_link
{
	struct x11_font_link *next;
	XftFont *font;
};

enum
{
	X11_FONT_BOLD      = 1 << 0,
	X11_FONT_ITALIC    = 1 << 1,
	X11_FONT_MONOSPACE = 1 << 2,
};

struct x11_font
{
	struct x11_font *next;              ///< Next in a linked list

	struct x11_font_link *list;         ///< Fonts of varying Unicode coverage
	unsigned style;                     ///< X11_FONT_* flags
	FcPattern *pattern;                 ///< Original unsubstituted pattern
	FcCharSet *unavailable;             ///< Couldn't find a font for these
};

#endif  // LIBERTY_XUI_WANT_X11

struct xui
{
	struct poller_idle refresh_event;   ///< Refresh the window's contents
	struct poller_idle flip_event;      ///< Draw rendered widgets on screen

	// User interface:

	struct ui *ui;                      ///< User interface interface
	struct widget *widgets;             ///< Layouted widgets
	int width;                          ///< Window width
	int height;                         ///< Window height
	int hunit;                          ///< Horizontal unit
	int vunit;                          ///< Vertical unit
	bool focused;                       ///< Whether the window has focus

	// Terminal:

	termo_t *tk;                        ///< termo handle (TUI/X11)
	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_timer tk_timer;       ///< termo timeout timer
	bool locale_is_utf8;                ///< The locale is Unicode

	// X11:

#ifdef LIBERTY_XUI_WANT_X11
	XIM x11_im;                         ///< Input method
	XIC x11_ic;                         ///< Input method context
	Display *dpy;                       ///< X display handle
	struct poller_fd x11_event;         ///< X11 events on wire
	struct poller_idle xpending_event;  ///< X11 events possibly in I/O queues
	int xkb_base_event_code;            ///< Xkb base event code
	Window x11_window;                  ///< Application window
	Pixmap x11_pixmap;                  ///< Off-screen bitmap
	Region x11_clip;                    ///< Invalidated region
	Picture x11_pixmap_picture;         ///< XRender wrap for x11_pixmap
	XftDraw *xft_draw;                  ///< Xft rendering context
	struct x11_font *xft_fonts;         ///< Font collection
	char *x11_selection;                ///< CLIPBOARD selection
	struct xdg_xsettings x11_xsettings; ///< XSETTINGS

	int32_t x11_double_click_time;      ///< Maximum delay for double clicks
	int32_t x11_double_click_distance;  ///< Maximum distance for double clicks
	const char *x11_fontname;           ///< Fontconfig font name
	const char *x11_fontname_monospace; ///< Fontconfig monospace font name
	XRenderColor *x_fg;                 ///< Foreground per attribute
	XRenderColor *x_bg;                 ///< Background per attribute
#endif  // LIBERTY_XUI_WANT_X11
}
g_xui;

static void
xui_invalidate (void)
{
	poller_idle_set (&g_xui.refresh_event);
}

static bool
xui_process_termo_event (termo_key_t *event)
{
	if (event->type == TERMO_TYPE_FOCUS)
		g_xui.focused = !!event->code.focused;
	return app_process_termo_event (event);
}

// --- TUI ---------------------------------------------------------------------

static void
tui_flush_buffer (struct widget *self, struct row_buffer *buf)
{
	move (self->y, self->x);

	if (self->y >= 0 && self->y < g_xui.height)
	{
		int space = MIN (self->width, g_xui.width - self->x);
		row_buffer_align (buf, space, self->attrs);
		row_buffer_flush (buf);
	}
	row_buffer_free (buf);
}

static void
tui_render_padding (struct widget *self)
{
	// TODO: This should work even for heights != 1.
	struct row_buffer buf = row_buffer_make ();
	tui_flush_buffer (self, &buf);
}

static struct widget *
tui_make_padding (chtype attrs, float width, float height)
{
	struct widget *w = xcalloc (1, sizeof *w + 2);
	w->text[0] = ' ';
	w->on_render = tui_render_padding;
	w->attrs = attrs;
	w->width = width * 2;
	w->height = height;
	return w;
}

static void
tui_render_label (struct widget *self)
{
	struct row_buffer buf = row_buffer_make ();
	row_buffer_append (&buf, self->text, self->attrs);
	tui_flush_buffer (self, &buf);
}

static struct widget *
tui_make_label (chtype attrs, unsigned extended, const char *label)
{
	(void) extended;

	size_t len = strlen (label);
	struct widget *w = xcalloc (1, sizeof *w + len + 1);
	w->on_render = tui_render_label;
	w->attrs = attrs;
	w->extended_attrs = extended;
	memcpy (w->text, label, len);

	struct row_buffer buf = row_buffer_make ();
	row_buffer_append (&buf, w->text, w->attrs);
	w->width = buf.total_width;
	w->height = 1;
	row_buffer_free (&buf);
	return w;
}

static void
tui_render_widgets (struct widget *head)
{
	LIST_FOR_EACH (struct widget, w, head)
	{
		if (w->width < 0 || w->height < 0)
			continue;
		if (w->on_render)
			w->on_render (w);
		tui_render_widgets (w->children);
	}
}

static void
tui_render (void)
{
	erase ();
	tui_render_widgets (g_xui.widgets);
}

static void
tui_flip (void)
{
	// Curses handles double-buffering for us automatically.
	refresh ();
}

static void
tui_winch (void)
{
	// The standard endwin/refresh sequence makes the terminal flicker
#if defined HAVE_RESIZETERM && defined TIOCGWINSZ
	struct winsize size;
	if (!ioctl (STDOUT_FILENO, TIOCGWINSZ, (char *) &size))
	{
		char *row = getenv ("LINES");
		char *col = getenv ("COLUMNS");
		unsigned long tmp;
		resizeterm (
			(row && xstrtoul (&tmp, row, 10)) ? tmp : size.ws_row,
			(col && xstrtoul (&tmp, col, 10)) ? tmp : size.ws_col);
	}
#else  // HAVE_RESIZETERM && TIOCGWINSZ
	endwin ();
	refresh ();
#endif  // HAVE_RESIZETERM && TIOCGWINSZ

	g_xui.width = COLS;
	g_xui.height = LINES;
	xui_invalidate ();
}

static void
tui_destroy (void)
{
	endwin ();
}

static struct ui tui_ui =
{
	.padding     = tui_make_padding,
	.label       = tui_make_label,

	.render      = tui_render,
	.flip        = tui_flip,
	.winch       = tui_winch,
	.destroy     = tui_destroy,
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
tui_on_tty_event (termo_key_t *event, int64_t event_ts)
{
	// Simple double click detection via release--press delay, only a bit
	// complicated by the fact that we don't know what's being released
	static termo_key_t last_event;
	static int64_t last_event_ts;
	static int last_button;

	int y, x, button, y_last, x_last, modifiers = 0;
	termo_mouse_event_t type, type_last;
	if (termo_interpret_mouse (g_xui.tk, event, &type, &button, &y, &x))
	{
		if (termo_interpret_mouse
			(g_xui.tk, &last_event, &type_last, NULL, &y_last, &x_last)
		 && event_ts - last_event_ts < 500
		 && type_last == TERMO_MOUSE_RELEASE && type == TERMO_MOUSE_PRESS
		 && y_last == y && x_last == x && last_button == button)
		{
			modifiers |= XUI_KEYMOD_DOUBLE_CLICK;
			// Prevent interpreting triple clicks as two double clicks.
			last_button = 0;
		}
		else if (type == TERMO_MOUSE_PRESS)
			last_button = button;

		if (!app_process_mouse (type, x, y, button, modifiers))
			beep ();
	}
	else if (!xui_process_termo_event (event))
		beep ();

	last_event = *event;
	last_event_ts = event_ts;
}

static void
tui_on_tty_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;
	if (fd->revents & ~(POLLIN | POLLHUP | POLLERR))
		print_debug ("fd %d: unexpected revents: %d", fd->fd, fd->revents);

	poller_timer_reset (&g_xui.tk_timer);
	termo_advisereadable (g_xui.tk);

	termo_key_t event = {};
	int64_t event_ts = clock_msec (CLOCK_BEST);
	termo_result_t res;
	while ((res = termo_getkey (g_xui.tk, &event)) == TERMO_RES_KEY)
		tui_on_tty_event (&event, event_ts);

	if (res == TERMO_RES_AGAIN)
		poller_timer_set (&g_xui.tk_timer, termo_get_waittime (g_xui.tk));
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
		app_quit ();
}

static void
tui_on_key_timer (void *user_data)
{
	(void) user_data;

	termo_key_t event;
	if (termo_getkey_force (g_xui.tk, &event) == TERMO_RES_KEY)
		if (!xui_process_termo_event (&event))
			beep ();
}

static void
tui_init (struct poller *poller, struct attrs *attrs, size_t attrs_len)
{
	(void) poller;

	poller_fd_set (&g_xui.tty_event, POLLIN);
	if (!termo_start (g_xui.tk) || !initscr () || nonl () == ERR)
		exit_fatal ("failed to set up the terminal");

	termo_set_mouse_tracking_mode (g_xui.tk, TERMO_MOUSE_TRACKING_DRAG);

	curs_set (0);

	g_xui.ui = &tui_ui;
	g_xui.width = COLS;
	g_xui.height = LINES;
	g_xui.vunit = 1;
	g_xui.hunit = 1;

	// The application should fall back to something at least nearly colourless
	if (start_color () == ERR
	 || use_default_colors () == ERR
	 || COLOR_PAIRS <= (int) attrs_len)
	{
		app_on_insufficient_color ();
		return;
	}

	for (size_t a = 0; a < attrs_len; a++)
	{
		if (attrs[a].fg >= -1 && attrs[a].bg >= -1
		 && init_pair (a + 1, attrs[a].fg, attrs[a].bg) == OK)
			attrs[a].attrs |= COLOR_PAIR (a + 1);
		else if (app_on_insufficient_color ())
			return;
	}
}

// --- X11 ---------------------------------------------------------------------

#ifdef LIBERTY_XUI_WANT_X11

static XRenderColor x11_default_fg = { .alpha = 0xffff };
static XRenderColor x11_default_bg = { 0xffff, 0xffff, 0xffff, 0xffff };
static XErrorHandler x11_default_error_handler;

static struct x11_font_link *
x11_font_link_new (XftFont *font)
{
	struct x11_font_link *self = xcalloc (1, sizeof *self);
	self->font = font;
	return self;
}

static void
x11_font_link_destroy (struct x11_font_link *self)
{
	XftFontClose (g_xui.dpy, self->font);
	free (self);
}

static struct x11_font_link *
x11_font_link_open (FcPattern *pattern)
{
	XftFont *font = XftFontOpenPattern (g_xui.dpy, pattern);
	if (!font)
	{
		FcPatternDestroy (pattern);
		return NULL;
	}
	return x11_font_link_new (font);
}

static struct x11_font *
x11_font_open (unsigned style)
{
	FcPattern *pattern = (style & X11_FONT_MONOSPACE)
		? FcNameParse ((const FcChar8 *) g_xui.x11_fontname_monospace)
		: FcNameParse ((const FcChar8 *) g_xui.x11_fontname);
	if (style & X11_FONT_BOLD)
		FcPatternAdd (pattern, FC_STYLE, (FcValue) {
			.type = FcTypeString, .u.s = (FcChar8 *) "Bold" }, FcFalse);
	if (style & X11_FONT_ITALIC)
		FcPatternAdd (pattern, FC_STYLE, (FcValue) {
			.type = FcTypeString, .u.s = (FcChar8 *) "Italic" }, FcFalse);

	FcPattern *substituted = FcPatternDuplicate (pattern);
	FcConfigSubstitute (NULL, substituted, FcMatchPattern);

	FcResult result = 0;
	FcPattern *match = XftFontMatch (g_xui.dpy,
		DefaultScreen (g_xui.dpy), substituted, &result);
	FcPatternDestroy (substituted);
	struct x11_font_link *link = NULL;
	if (!match || !(link = x11_font_link_open (match)))
	{
		FcPatternDestroy (pattern);
		return NULL;
	}

	struct x11_font *self = xcalloc (1, sizeof *self);
	self->list = link;
	self->style = style;
	self->pattern = pattern;
	self->unavailable = FcCharSetCreate ();
	return self;
}

static void
x11_font_destroy (struct x11_font *self)
{
	FcPatternDestroy (self->pattern);
	FcCharSetDestroy (self->unavailable);
	LIST_FOR_EACH (struct x11_font_link, iter, self->list)
		x11_font_link_destroy (iter);
	free (self);
}

/// Find or instantiate a font that can render the character given by cp.
static XftFont *
x11_font_cover_codepoint (struct x11_font *self, ucs4_t cp)
{
	if (FcCharSetHasChar (self->unavailable, cp))
		return self->list->font;

	struct x11_font_link **used = &self->list;
	for (; *used; used = &(*used)->next)
		if (XftCharExists (g_xui.dpy, (*used)->font, cp))
			return (*used)->font;

	FcCharSet *set = FcCharSetCreate ();
	FcCharSetAddChar (set, cp);
	FcPattern *needle = FcPatternDuplicate (self->pattern);
	FcPatternAddCharSet (needle, FC_CHARSET, set);
	FcConfigSubstitute (NULL, needle, FcMatchPattern);

	FcResult result = 0;
	FcPattern *match
		= XftFontMatch (g_xui.dpy, DefaultScreen (g_xui.dpy), needle, &result);
	FcCharSetDestroy (set);
	FcPatternDestroy (needle);
	if (!match)
		goto fail;

	struct x11_font_link *new = x11_font_link_open (match);
	if (!new)
		goto fail;

	// The reverse may happen simply due to race conditions.
	if (XftCharExists (g_xui.dpy, new->font, cp))
		return (*used = new)->font;

	x11_font_link_destroy (new);
fail:
	FcCharSetAddChar (self->unavailable, cp);
	return self->list->font;
}

// TODO: Perhaps produce an array of FT_UInt glyph indexes, mainly so that
//   x11_font_{hadvance,draw,render}() can use the same data, through the use
//   of a new function that collects the spans in a data structure.
static size_t
x11_font_span (struct x11_font *self, const uint8_t *text, XftFont **font)
{
	hard_assert (self->list != NULL);

	// Xft similarly just stops on invalid UTF-8.
	ucs4_t cp = 0;
	const uint8_t *p = text;
	if (!(p = u8_next (&cp, p)))
		return 0;

	*font = x11_font_cover_codepoint (self, cp);
	for (const uint8_t *end = NULL; (end = u8_next (&cp, p)); p = end)
	{
		if (x11_font_cover_codepoint (self, cp) != *font)
			break;
	}
	return p - text;
}

static int
x11_font_draw (struct x11_font *self, XftColor *color, int x, int y,
	const char *text)
{
	int advance = 0;
	size_t len = 0;
	XftFont *font = NULL;
	while ((len = x11_font_span (self, (const uint8_t *) text, &font)))
	{
		if (color)
		{
			XftDrawStringUtf8 (g_xui.xft_draw, color, font,
				x + advance, y + self->list->font->ascent,
				(const FcChar8 *) text, len);
		}

		XGlyphInfo extents = {};
		XftTextExtentsUtf8 (g_xui.dpy,
			font, (const FcChar8 *) text, len, &extents);
		text += len;
		advance += extents.xOff;
	}
	return advance;
}

static int
x11_font_hadvance (struct x11_font *self, const char *text)
{
	return x11_font_draw (self, NULL, 0, 0, text);
}

static int
x11_font_render (struct x11_font *self, int op, Picture src, int srcx, int srcy,
	int x, int y, const char *text)
{
	int advance = 0;
	size_t len = 0;
	XftFont *font = NULL;
	while ((len = x11_font_span (self, (const uint8_t *) text, &font)))
	{
		if (src)
		{
			XftTextRenderUtf8 (g_xui.dpy,
				op, src, font, g_xui.x11_pixmap_picture,
				srcx, srcy, x + advance, y + self->list->font->ascent,
				(const FcChar8 *) text, len);
		}

		XGlyphInfo extents = {};
		XftTextExtentsUtf8 (g_xui.dpy,
			font, (const FcChar8 *) text, len, &extents);
		text += len;
		advance += extents.xOff;
	}
	return advance;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct x11_font *
x11_widget_font (struct widget *self)
{
	unsigned style = 0;
	if (self->attrs & A_BOLD)
		style |= X11_FONT_BOLD;
#ifdef A_ITALIC
	if (self->attrs & A_ITALIC)
		style |= X11_FONT_ITALIC;
#endif  // A_ITALIC
	if (self->extended_attrs & XUI_ATTR_MONOSPACE)
		style |= X11_FONT_MONOSPACE;

	struct x11_font **font = &g_xui.xft_fonts;
	for (; *font; font = &(*font)->next)
		if ((*font)->style == style)
			return *font;
	if ((*font = x11_font_open (style)))
		return *font;

	// But FontConfig has a tendency to always return something.
	return g_xui.xft_fonts;
}

static XRenderColor *
x11_fg_attrs (chtype attrs)
{
	int pair = PAIR_NUMBER (attrs);
	if (!pair--)
		return &x11_default_fg;
	return (attrs & A_REVERSE) ? &g_xui.x_bg[pair] : &g_xui.x_fg[pair];
}

static XRenderColor *
x11_fg (struct widget *self)
{
	return x11_fg_attrs (self->attrs);
}

static XRenderColor *
x11_bg_attrs (chtype attrs)
{
	int pair = PAIR_NUMBER (attrs);
	if (!pair--)
		return &x11_default_bg;
	return (attrs & A_REVERSE) ? &g_xui.x_fg[pair] : &g_xui.x_bg[pair];
}

static XRenderColor *
x11_bg (struct widget *self)
{
	return x11_bg_attrs (self->attrs);
}

static void
x11_render_padding (struct widget *self)
{
	if (PAIR_NUMBER (self->attrs))
	{
		XRenderFillRectangle (g_xui.dpy, PictOpSrc, g_xui.x11_pixmap_picture,
			x11_bg (self), self->x, self->y, self->width, self->height);
	}
	if (self->attrs & A_UNDERLINE)
	{
		XRenderFillRectangle (g_xui.dpy, PictOpSrc, g_xui.x11_pixmap_picture,
			x11_fg (self), self->x, self->y + self->height - 1, self->width, 1);
	}
}

static struct widget *
x11_make_padding (chtype attrs, float width, float height)
{
	struct widget *w = xcalloc (1, sizeof *w + 2);
	w->text[0] = ' ';
	w->on_render = x11_render_padding;
	w->attrs = attrs;
	w->width = g_xui.vunit * width;
	w->height = g_xui.vunit * height;
	return w;
}

static void
x11_render_label (struct widget *self)
{
	x11_render_padding (self);

	int space = MIN (self->width, g_xui.width - self->x);
	if (space <= 0)
		return;

	// TODO: Try to avoid re-measuring on each render.
	struct x11_font *font = x11_widget_font (self);
	int advance = x11_font_hadvance (font, self->text);
	if (advance <= space)
	{
		XftColor color = { .color = *x11_fg (self) };
		x11_font_draw (font, &color, self->x, self->y, self->text);
		return;
	}

	// XRender doesn't extend gradients beyond their end stops.
	XRenderColor solid = *x11_fg (self), colors[3] = { solid, solid, solid };
	colors[2].alpha = 0;

	double portion = MIN (1, 2.0 * font->list->font->height / space);
	XFixed stops[3] = { 0, XDoubleToFixed (1 - portion), XDoubleToFixed (1) };
	XLinearGradient gradient = { {}, { XDoubleToFixed (space), 0 } };

	// Note that this masking is a very expensive operation.
	Picture source =
		XRenderCreateLinearGradient (g_xui.dpy, &gradient, stops, colors, 3);
	x11_font_render (font, PictOpOver, source, -self->x, 0, self->x, self->y,
		self->text);
	XRenderFreePicture (g_xui.dpy, source);
}

static struct widget *
x11_make_label (chtype attrs, unsigned extended, const char *label)
{
	// Xft renders combining marks by themselves, NFC improves it a bit.
	// We'd have to use HarfBuzz to do this correctly.
	size_t label_len = strlen (label) + 1, normalized_len = 0;
	uint8_t *normalized = u8_normalize (UNINORM_NFC,
		(const uint8_t *) label, label_len, NULL, &normalized_len);
	if (!normalized)
	{
		normalized = memcpy (xmalloc (label_len), label, label_len);
		normalized_len = label_len;
	}

	struct widget *w = xcalloc (1, sizeof *w + normalized_len);
	w->on_render = x11_render_label;
	w->attrs = attrs;
	w->extended_attrs = extended;
	memcpy (w->text, normalized, normalized_len);
	free (normalized);

	struct x11_font *font = x11_widget_font (w);
	w->width = x11_font_hadvance (font, w->text);
	w->height = font->list->font->height;
	return w;
}

static void
x11_render_widget (struct widget *w, const XRectangle *clip)
{
	if (w->width < 0 || w->height < 0)
		return;

	// Children may set their own clips, so reset before each sibling.
	// We need to go through Xft, or XftTextRenderUtf8() might skip glyphs.
	if (clip)
		XftDrawSetClipRectangles (g_xui.xft_draw, 0, 0, clip, 1);
	else
		XftDrawSetClip (g_xui.xft_draw, None);

	if (w->on_render)
		w->on_render (w);

	// We set clips on containers, not on individual widgets.
	XRectangle subclip = { w->x, w->y, w->width, w->height };
	if (clip)
	{
		int x1 = MAX (clip->x, w->x);
		int y1 = MAX (clip->y, w->y);
		int x2 = MIN (clip->x + clip->width,  w->x + w->width);
		int y2 = MIN (clip->y + clip->height, w->y + w->height);
		if (x1 >= x2 || y1 >= y2)
			return;

		subclip.x = x1;
		subclip.y = y1;
		subclip.width  = x2 - x1;
		subclip.height = y2 - y1;
	}

	LIST_FOR_EACH (struct widget, child, w->children)
		x11_render_widget (child, &subclip);
}

static void
x11_render (void)
{
	XRenderFillRectangle (g_xui.dpy, PictOpSrc, g_xui.x11_pixmap_picture,
		&x11_default_bg, 0, 0, g_xui.width, g_xui.height);

	LIST_FOR_EACH (struct widget, w, g_xui.widgets)
		x11_render_widget (w, NULL);

	XRectangle r = { 0, 0, g_xui.width, g_xui.height };
	XUnionRectWithRegion (&r, g_xui.x11_clip, g_xui.x11_clip);
	poller_idle_set (&g_xui.xpending_event);
}

static void
x11_flip (void)
{
	// This exercise in futility doesn't seem to affect CPU usage much.
	XRectangle r = {};
	XClipBox (g_xui.x11_clip, &r);
	XCopyArea (g_xui.dpy, g_xui.x11_pixmap, g_xui.x11_window,
		DefaultGC (g_xui.dpy, DefaultScreen (g_xui.dpy)),
		r.x, r.y, r.width, r.height, r.x, r.y);

	XSubtractRegion (g_xui.x11_clip, g_xui.x11_clip, g_xui.x11_clip);
	poller_idle_set (&g_xui.xpending_event);
}

static void
x11_destroy (void)
{
	XDestroyIC (g_xui.x11_ic);
	XCloseIM (g_xui.x11_im);
	XDestroyRegion (g_xui.x11_clip);
	XDestroyWindow (g_xui.dpy, g_xui.x11_window);
	XRenderFreePicture (g_xui.dpy, g_xui.x11_pixmap_picture);
	XFreePixmap (g_xui.dpy, g_xui.x11_pixmap);
	XftDrawDestroy (g_xui.xft_draw);
	LIST_FOR_EACH (struct x11_font, font, g_xui.xft_fonts)
		x11_font_destroy (font);
	cstr_set (&g_xui.x11_selection, NULL);
	xdg_xsettings_free (&g_xui.x11_xsettings);

	free (g_xui.x_fg);
	free (g_xui.x_bg);

	poller_fd_reset (&g_xui.x11_event);
	XCloseDisplay (g_xui.dpy);

	// Xft hooks called in XCloseDisplay() don't clean up everything.
	FcFini ();
}

static struct ui x11_ui =
{
	.padding     = x11_make_padding,
	.label       = x11_make_label,

	.render      = x11_render,
	.flip        = x11_flip,
	.destroy     = x11_destroy,
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static termo_sym_t
x11_convert_keysym (KeySym keysym)
{
	// Leaving out TERMO_TYPE_FUNCTION, TERMO_SYM_DEL (N/A),
	// and TERMO_SYM_SPACE (governed by TERMO_FLAG_SPACESYMBOL, not in use).
	switch (keysym)
	{
	case XK_BackSpace:     return TERMO_SYM_BACKSPACE;
	case XK_Tab:           return TERMO_SYM_TAB;
	case XK_ISO_Left_Tab:  return TERMO_SYM_TAB;
	case XK_Return:        return TERMO_SYM_ENTER;
	case XK_Escape:        return TERMO_SYM_ESCAPE;

	case XK_Up:            return TERMO_SYM_UP;
	case XK_Down:          return TERMO_SYM_DOWN;
	case XK_Left:          return TERMO_SYM_LEFT;
	case XK_Right:         return TERMO_SYM_RIGHT;
	case XK_Begin:         return TERMO_SYM_BEGIN;
	case XK_Find:          return TERMO_SYM_FIND;
	case XK_Insert:        return TERMO_SYM_INSERT;
	case XK_Delete:        return TERMO_SYM_DELETE;
	case XK_Select:        return TERMO_SYM_SELECT;
	case XK_Page_Up:       return TERMO_SYM_PAGEUP;
	case XK_Page_Down:     return TERMO_SYM_PAGEDOWN;
	case XK_Home:          return TERMO_SYM_HOME;
	case XK_End:           return TERMO_SYM_END;

	case XK_Cancel:        return TERMO_SYM_CANCEL;
	case XK_Clear:         return TERMO_SYM_CLEAR;
	// TERMO_SYM_CLOSE
	// TERMO_SYM_COMMAND
	// TERMO_SYM_COPY
	// TERMO_SYM_EXIT
	case XK_Help:          return TERMO_SYM_HELP;
	// TERMO_SYM_MARK
	// TERMO_SYM_MESSAGE
	// TERMO_SYM_MOVE
	// TERMO_SYM_OPEN
	// TERMO_SYM_OPTIONS
	case XK_Print:         return TERMO_SYM_PRINT;
	case XK_Redo:          return TERMO_SYM_REDO;
	// TERMO_SYM_REFERENCE
	// TERMO_SYM_REFRESH
	// TERMO_SYM_REPLACE
	// TERMO_SYM_RESTART
	// TERMO_SYM_RESUME
	// TERMO_SYM_SAVE
	// TERMO_SYM_SUSPEND
	case XK_Undo:          return TERMO_SYM_UNDO;

	case XK_KP_0:          return TERMO_SYM_KP0;
	case XK_KP_1:          return TERMO_SYM_KP1;
	case XK_KP_2:          return TERMO_SYM_KP2;
	case XK_KP_3:          return TERMO_SYM_KP3;
	case XK_KP_4:          return TERMO_SYM_KP4;
	case XK_KP_5:          return TERMO_SYM_KP5;
	case XK_KP_6:          return TERMO_SYM_KP6;
	case XK_KP_7:          return TERMO_SYM_KP7;
	case XK_KP_8:          return TERMO_SYM_KP8;
	case XK_KP_9:          return TERMO_SYM_KP9;
	case XK_KP_Enter:      return TERMO_SYM_KPENTER;
	case XK_KP_Add:        return TERMO_SYM_KPPLUS;
	case XK_KP_Subtract:   return TERMO_SYM_KPMINUS;
	case XK_KP_Multiply:   return TERMO_SYM_KPMULT;
	case XK_KP_Divide:     return TERMO_SYM_KPDIV;
	case XK_KP_Separator:  return TERMO_SYM_KPCOMMA;
	case XK_KP_Decimal:    return TERMO_SYM_KPPERIOD;
	case XK_KP_Equal:      return TERMO_SYM_KPEQUALS;
	}
	return TERMO_SYM_UNKNOWN;
}

static bool
on_x11_keypress (XEvent *e)
{
	// A kibibyte long buffer will have to suffice for anyone.
	XKeyEvent *ev = &e->xkey;
	char buf[1 << 10] = {}, *p = buf;
	KeySym keysym = None;
	Status status = 0;
	int len = Xutf8LookupString
		(g_xui.x11_ic, ev, buf, sizeof buf, &keysym, &status);
	if (status == XBufferOverflow)
		print_warning ("input method overflow");

	termo_key_t key = {};
	if (ev->state & ShiftMask)
		key.modifiers |= TERMO_KEYMOD_SHIFT;
	if (ev->state & ControlMask)
		key.modifiers |= TERMO_KEYMOD_CTRL;
	if (ev->state & Mod1Mask)
		key.modifiers |= TERMO_KEYMOD_ALT;

	if (keysym >= XK_F1 && keysym <= XK_F35)
	{
		key.type = TERMO_TYPE_FUNCTION;
		key.code.number = 1 + keysym - XK_F1;
		return xui_process_termo_event (&key);
	}
	if ((key.code.sym = x11_convert_keysym (keysym)) != TERMO_SYM_UNKNOWN)
	{
		key.type = TERMO_TYPE_KEYSYM;
		return xui_process_termo_event (&key);
	}

	bool result = true;
	if (len)
	{
		key.type = TERMO_TYPE_KEY;
		key.modifiers &= ~TERMO_KEYMOD_SHIFT;

		int32_t cp = 0;
		struct utf8_iter iter = { .s = buf, .len = len };
		size_t cp_len = 0;
		while ((cp = utf8_iter_next (&iter, &cp_len)) >= 0)
		{
			termo_key_t k = key;
			memcpy (k.multibyte, p, MIN (cp_len, sizeof k.multibyte - 1));
			p += cp_len;

			// This is all unfortunate, but probably in the right place.
			if (!cp)
			{
				k.code.codepoint = ' ';
				if (ev->state & ShiftMask)
					k.modifiers |= TERMO_KEYMOD_SHIFT;
			}
			else if (cp >= 32)
				k.code.codepoint = cp;
			else if (ev->state & ShiftMask)
				k.code.codepoint = cp + 64;
			else
				k.code.codepoint = cp + 96;
			if (!xui_process_termo_event (&k))
				result = false;
		}
	}
	return result;
}

static void
x11_init_pixmap (void)
{
	int screen = DefaultScreen (g_xui.dpy);
	g_xui.x11_pixmap = XCreatePixmap (g_xui.dpy, g_xui.x11_window,
		MAX (g_xui.width, 1), MAX (g_xui.height, 1),
		DefaultDepth (g_xui.dpy, screen));

	Visual *visual = DefaultVisual (g_xui.dpy, screen);
	XRenderPictFormat *format = XRenderFindVisualFormat (g_xui.dpy, visual);
	g_xui.x11_pixmap_picture
		= XRenderCreatePicture (g_xui.dpy, g_xui.x11_pixmap, format, 0, NULL);
}

static char *
x11_find_text (struct widget *list, int x, int y)
{
	struct widget *target = NULL;
	LIST_FOR_EACH (struct widget, w, list)
		if (x >= w->x && x < w->x + w->width
		 && y >= w->y && y < w->y + w->height)
			target = w;
	if (!target)
		return NULL;

	char *result = x11_find_text (target->children, x, y);
	if (result)
		return result;
	return xstrdup (target->text);
}

// TODO: OSC 52 exists for terminals, so make it possible to enable that there.
static bool
x11_process_press (int x, int y, int button, int modifiers)
{
	if (button != Button3)
		goto out;

	char *text = x11_find_text (g_xui.widgets, x, y);
	if (!text || !*(cstr_strip_in_place (text, " \t")))
	{
		free (text);
		goto out;
	}

	cstr_set (&g_xui.x11_selection, text);
	XSetSelectionOwner (g_xui.dpy, XInternAtom (g_xui.dpy, "CLIPBOARD", False),
		g_xui.x11_window, CurrentTime);
	app_on_clipboard_copy (g_xui.x11_selection);
	return true;

out:
	return app_process_mouse (TERMO_MOUSE_PRESS, x, y, button, modifiers);
}

static int
x11_state_to_modifiers (unsigned int state)
{
	int modifiers = 0;
	if (state & ShiftMask)    modifiers |= TERMO_KEYMOD_SHIFT;
	if (state & ControlMask)  modifiers |= TERMO_KEYMOD_CTRL;
	if (state & Mod1Mask)     modifiers |= TERMO_KEYMOD_ALT;
	return modifiers;
}

static bool
on_x11_input_event (XEvent *ev)
{
	static XEvent last_press_event;
	if (ev->type == KeyPress)
	{
		last_press_event = (XEvent) {};
		return on_x11_keypress (ev);
	}
	if (ev->type == MotionNotify)
	{
		return app_process_mouse (TERMO_MOUSE_DRAG,
			ev->xmotion.x, ev->xmotion.y, 1 /* Button1MotionMask */,
			x11_state_to_modifiers (ev->xmotion.state));
	}

	// This is nearly the same as tui_on_tty_event().
	int x = ev->xbutton.x, y = ev->xbutton.y;
	unsigned int button = ev->xbutton.button;
	int modifiers = x11_state_to_modifiers (ev->xbutton.state);
	if (ev->type == ButtonPress
	 && ev->xbutton.time - last_press_event.xbutton.time
		< (Time) g_xui.x11_double_click_time
	 && abs (last_press_event.xbutton.x - x) < g_xui.x11_double_click_distance
	 && abs (last_press_event.xbutton.y - y) < g_xui.x11_double_click_distance
	 && last_press_event.xbutton.button == button)
	{
		modifiers |= XUI_KEYMOD_DOUBLE_CLICK;
		// Prevent interpreting triple clicks as two double clicks.
		last_press_event = (XEvent) {};
	}
	else if (ev->type == ButtonPress)
		last_press_event = *ev;

	if (ev->type == ButtonPress)
		return x11_process_press (x, y, button, modifiers);
	if (ev->type == ButtonRelease)
		return app_process_mouse
			(TERMO_MOUSE_RELEASE, x, y, button, modifiers);
	return false;
}

static void
on_x11_selection_request (XSelectionRequestEvent *ev)
{
	Atom xa_targets = XInternAtom (g_xui.dpy, "TARGETS", False);
	Atom xa_compound_text = XInternAtom (g_xui.dpy, "COMPOUND_TEXT", False);
	Atom xa_utf8 = XInternAtom (g_xui.dpy, "UTF8_STRING", False);
	Atom targets[] = { xa_targets, XA_STRING, xa_compound_text, xa_utf8 };

	XEvent response = {};
	bool ok = false;
	Atom property = ev->property ? ev->property : ev->target;
	if (!g_xui.x11_selection)
		goto out;

	XICCEncodingStyle style = 0;
	if ((ok = ev->target == xa_targets))
	{
		XChangeProperty (g_xui.dpy, ev->requestor, property,
			XA_ATOM, 32, PropModeReplace,
			(const unsigned char *) targets, N_ELEMENTS (targets));
		goto out;
	}
	else if (ev->target == XA_STRING)
		style = XStringStyle;
	else if (ev->target == xa_compound_text)
		style = XCompoundTextStyle;
	else if (ev->target == xa_utf8)
		style = XUTF8StringStyle;
	else
		goto out;

	// XXX: We let it crash us with BadLength, but we may, e.g., use INCR.
	XTextProperty text = {};
	if ((ok = !Xutf8TextListToTextProperty
		 (g_xui.dpy, &g_xui.x11_selection, 1, style, &text)))
	{
		XChangeProperty (g_xui.dpy, ev->requestor, property,
			text.encoding, text.format, PropModeReplace,
			text.value, text.nitems);
	}
	XFree (text.value);

out:
	response.xselection.type = SelectionNotify;
	// XXX: We should check it against the event causing XSetSelectionOwner().
	response.xselection.time = ev->time;
	response.xselection.requestor = ev->requestor;
	response.xselection.selection = ev->selection;
	response.xselection.target = ev->target;
	response.xselection.property = ok ? property : None;
	XSendEvent (g_xui.dpy, ev->requestor, False, 0, &response);
}

static void
on_x11_event (XEvent *ev)
{
	termo_key_t key = {};
	switch (ev->type)
	{
	case Expose:
	{
		XRectangle r = { ev->xexpose.x, ev->xexpose.y,
			ev->xexpose.width, ev->xexpose.height };
		XUnionRectWithRegion (&r, g_xui.x11_clip, g_xui.x11_clip);
		poller_idle_set (&g_xui.flip_event);
		break;
	}
	case ConfigureNotify:
		if (g_xui.width == ev->xconfigure.width
		 && g_xui.height == ev->xconfigure.height)
			break;

		g_xui.width = ev->xconfigure.width;
		g_xui.height = ev->xconfigure.height;

		XRenderFreePicture (g_xui.dpy, g_xui.x11_pixmap_picture);
		XFreePixmap (g_xui.dpy, g_xui.x11_pixmap);
		x11_init_pixmap ();
		XftDrawChange (g_xui.xft_draw, g_xui.x11_pixmap);
		xui_invalidate ();
		break;
	case SelectionRequest:
		on_x11_selection_request (&ev->xselectionrequest);
		break;
	case SelectionClear:
		cstr_set (&g_xui.x11_selection, NULL);
		break;
	// UnmapNotify can be received when restarting the window manager.
	// Should this turn out to be unreliable (window not destroyed by WM
	// upon closing), opt for the WM_DELETE_WINDOW protocol as well.
	case DestroyNotify:
		app_quit ();
		break;
	case FocusIn:
		key.type = TERMO_TYPE_FOCUS;
		key.code.focused = true;
		xui_process_termo_event (&key);
		break;
	case FocusOut:
		key.type = TERMO_TYPE_FOCUS;
		key.code.focused = false;
		xui_process_termo_event (&key);
		break;
	case KeyPress:
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		if (!on_x11_input_event (ev))
			XkbBell (g_xui.dpy, ev->xany.window, 0, None);
	}
}

static void
on_x11_pending (void *user_data)
{
	(void) user_data;

	XkbEvent ev;
	while (XPending (g_xui.dpy))
	{
		if (XNextEvent (g_xui.dpy, &ev.core))
			exit_fatal ("XNextEvent returned non-zero");
		if (XFilterEvent (&ev.core, None))
			continue;

		on_x11_event (&ev.core);
	}

	poller_idle_reset (&g_xui.xpending_event);
}

static void
on_x11_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;
	on_x11_pending (user_data);
}

static int
on_x11_error (Display *dpy, XErrorEvent *event)
{
	// Without opting for WM_DELETE_WINDOW, this window can become destroyed
	// and hence invalid at any time.  We don't use the Window much,
	// so we should be fine ignoring these errors.
	if ((event->error_code == BadWindow
		&& event->resourceid == g_xui.x11_window)
	 || (event->error_code == BadDrawable
		&& event->resourceid == g_xui.x11_window))
		return app_quit (), 0;

	// XXX: The simplest possible way of discarding selection management errors.
	//   XCB would be a small win here, but it is a curse at the same time.
	if (event->error_code == BadWindow && event->resourceid != g_xui.x11_window)
		return 0;

	return x11_default_error_handler (dpy, event);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static XRenderColor
x11_convert_color (int color)
{
	hard_assert (color >= 0 && color <= 255);

	static const uint16_t base[16] =
	{
		0x000, 0x800, 0x080, 0x880, 0x008, 0x808, 0x088, 0xccc,
		0x888, 0xf00, 0x0f0, 0xff0, 0x00f, 0xf0f, 0x0ff, 0xfff,
	};

	XRenderColor c = { .alpha = 0xffff };
	if (color < 16)
	{
		c.red   = 0x1111 *        (base[color] >> 8);
		c.green = 0x1111 * (0xf & (base[color] >> 4));
		c.blue  = 0x1111 * (0xf & (base[color]));
	}
	else if (color >= 232)
		c.red = c.green = c.blue = 0x0101 * (8 + (color - 232) * 10);
	else
	{
		color -= 16;

		int r =  color / 36;
		int g = (color / 6) % 6;
		int b = (color % 6);
		c.red   = 0x0101 * !!r * (55 + 40 * r);
		c.green = 0x0101 * !!g * (55 + 40 * g);
		c.blue  = 0x0101 * !!b * (55 + 40 * b);
	}
	return c;
}

static void
x11_init_attributes (struct attrs *attrs, size_t attrs_len)
{
	g_xui.x_fg = xcalloc (attrs_len, sizeof g_xui.x_fg[0]);
	g_xui.x_bg = xcalloc (attrs_len, sizeof g_xui.x_bg[0]);
	for (size_t a = 0; a < attrs_len; a++)
	{
		g_xui.x_fg[a] = x11_default_fg;
		g_xui.x_bg[a] = x11_default_bg;
		if (attrs[a].fg >= 256 || attrs[a].fg < -1
		 || attrs[a].bg >= 256 || attrs[a].bg < -1)
			continue;

		if (attrs[a].fg != -1)
			g_xui.x_fg[a] = x11_convert_color (attrs[a].fg);
		if (attrs[a].bg != -1)
			g_xui.x_bg[a] = x11_convert_color (attrs[a].bg);

		attrs[a].attrs |= COLOR_PAIR (a + 1);
	}
}

static void
x11_init (struct poller *poller, struct attrs *app_attrs, size_t app_attrs_len)
{
	// https://tedyin.com/posts/a-brief-intro-to-linux-input-method-framework/
	if (!XSupportsLocale ())
		print_warning ("locale not supported by Xlib");
	XSetLocaleModifiers ("");

	if (!(g_xui.dpy = XkbOpenDisplay
		(NULL, &g_xui.xkb_base_event_code, NULL, NULL, NULL, NULL)))
		exit_fatal ("cannot open display");
	if (!XftDefaultHasRender (g_xui.dpy))
		exit_fatal ("XRender is not supported");
	if (!(g_xui.x11_im = XOpenIM (g_xui.dpy, NULL, NULL, NULL)))
		exit_fatal ("failed to open an input method");

	x11_default_error_handler = XSetErrorHandler (on_x11_error);

	set_cloexec (ConnectionNumber (g_xui.dpy));
	g_xui.x11_event = poller_fd_make (poller, ConnectionNumber (g_xui.dpy));
	g_xui.x11_event.dispatcher = on_x11_ready;
	poller_fd_set (&g_xui.x11_event, POLLIN);

	// Whenever something causes Xlib to read its socket, it can make
	// the I/O event above fail to trigger for whatever might have ended up
	// in its queue.  So always use this instead of XSync:
	g_xui.xpending_event = poller_idle_make (poller);
	g_xui.xpending_event.dispatcher = on_x11_pending;
	poller_idle_set (&g_xui.xpending_event);

	x11_init_attributes (app_attrs, app_attrs_len);

	// https://www.freedesktop.org/wiki/Specifications/XSettingsRegistry/
	// TODO: Try to use Xft/{Antialias,DPI,HintStyle,Hinting,RGBA}
	//   from XSETTINGS.  Sadly, Gtk/FontName is in the Pango format,
	//   which is rather difficult to parse.
	g_xui.x11_xsettings = xdg_xsettings_make ();
	xdg_xsettings_update (&g_xui.x11_xsettings, g_xui.dpy);

	if (!FcInit ())
		print_warning ("Fontconfig initialization failed");
	if (!(g_xui.xft_fonts = x11_font_open (0)))
		exit_fatal ("cannot open a font");

	int screen = DefaultScreen (g_xui.dpy);
	Colormap cmap = DefaultColormap (g_xui.dpy, screen);
	XColor default_bg =
	{
		.red    = x11_default_bg.red,
		.green  = x11_default_bg.green,
		.blue   = x11_default_bg.blue,
	};
	if (!XAllocColor (g_xui.dpy, cmap, &default_bg))
		exit_fatal ("X11 setup failed");

	XSetWindowAttributes attrs =
	{
		.event_mask = StructureNotifyMask | ExposureMask | FocusChangeMask
			| KeyPressMask | ButtonPressMask | ButtonReleaseMask
			| Button1MotionMask,
		.bit_gravity = NorthWestGravity,
		.background_pixel = default_bg.pixel,
	};

	// Approximate the average width of a character to half of the em unit.
	g_xui.vunit = g_xui.xft_fonts->list->font->height;
	g_xui.hunit = g_xui.vunit / 2;
	// Base the window's size on the regular font size.
	// Roughly trying to match the 80x24 default dimensions of terminals.
	g_xui.height = 24 * g_xui.vunit;
	g_xui.width = g_xui.height * 4 / 3;

	long im_event_mask = 0;
	if (!XGetIMValues (g_xui.x11_im, XNFilterEvents, &im_event_mask, NULL))
		attrs.event_mask |= im_event_mask;

	Visual *visual = DefaultVisual (g_xui.dpy, screen);
	g_xui.x11_window = XCreateWindow (g_xui.dpy,
		RootWindow (g_xui.dpy, screen), 100, 100,
		g_xui.width, g_xui.height, 0, CopyFromParent, InputOutput, visual,
		CWEventMask | CWBackPixel | CWBitGravity, &attrs);
	g_xui.x11_clip = XCreateRegion ();

	XTextProperty prop = {};
	char *name = PROGRAM_NAME;
	if (!Xutf8TextListToTextProperty (g_xui.dpy,
			&name, 1, XUTF8StringStyle, &prop))
		XSetWMName (g_xui.dpy, g_xui.x11_window, &prop);
	XFree (prop.value);

	// It should not be outlandish to expect to find a program icon,
	// although it should be possible to use a "DBus well-known name".
	const char *icon_theme_name = NULL;
	const struct xdg_xsettings_setting *setting =
		str_map_find (&g_xui.x11_xsettings.settings, "Net/IconThemeName");
	if (setting != NULL && setting->type == XDG_XSETTINGS_STRING)
		icon_theme_name = setting->string.str;
	icon_theme_set_window_icon (g_xui.dpy,
		g_xui.x11_window, icon_theme_name, name);

	if ((setting = str_map_find (&g_xui.x11_xsettings.settings,
			"Net/DoubleClickTime"))
	 && setting->type == XDG_XSETTINGS_INTEGER && setting->integer >= 0)
		g_xui.x11_double_click_time = setting->integer;
	if ((setting = str_map_find (&g_xui.x11_xsettings.settings,
			"Net/DoubleClickDistance"))
	 && setting->type == XDG_XSETTINGS_INTEGER && setting->integer >= 0)
		g_xui.x11_double_click_distance = setting->integer;

	// TODO: It is possible to do, e.g., on-the-spot.
	XIMStyle im_style = XIMPreeditNothing | XIMStatusNothing;
	XIMStyles *im_styles = NULL;
	bool im_style_found = false;
	if (!XGetIMValues (g_xui.x11_im, XNQueryInputStyle, &im_styles, NULL)
	 && im_styles)
	{
		for (unsigned i = 0; i < im_styles->count_styles; i++)
			im_style_found |= im_styles->supported_styles[i] == im_style;
		XFree (im_styles);
	}
	if (!im_style_found)
		print_warning ("failed to find the desired input method style");
	if (!(g_xui.x11_ic = XCreateIC (g_xui.x11_im,
			XNInputStyle, im_style,
			XNClientWindow, g_xui.x11_window,
			NULL)))
		exit_fatal ("failed to open an input context");

	XSetICFocus (g_xui.x11_ic);

	x11_init_pixmap ();
	g_xui.xft_draw = XftDrawCreate (g_xui.dpy, g_xui.x11_pixmap, visual, cmap);
	g_xui.ui = &x11_ui;

	XMapWindow (g_xui.dpy, g_xui.x11_window);
}

#endif  // LIBERTY_XUI_WANT_X11

// --- Containers --------------------------------------------------------------

static void
xui_on_hbox_allocated (struct widget *self)
{
	int parts = 0, width = self->width;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		if (w->width < 0)
			parts -= w->width;
		else
			width -= w->width;
	}

	int remaining = MAX (width, 0),
		part_width = parts ? remaining / parts : 0;
	struct widget *last = NULL;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		w->height = self->height;
		if (w->width < 0)
		{
			remaining -= (w->width *= -part_width);
			last = w;
		}
	}
	if (last)
		last->width += remaining;

	int x = self->x, y = self->y;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		widget_move (w, x - w->x, y - w->y);
		x += w->width;

		if (w->on_allocated)
			w->on_allocated (w);
	}
}

static struct widget *
xui_hbox (struct widget *head)
{
	struct widget *self = xcalloc (1, sizeof *self);
	self->children = head;
	self->on_allocated = xui_on_hbox_allocated;

	LIST_FOR_EACH (struct widget, w, head)
	{
		self->height = MAX (self->height, w->height);
		self->width += MAX (0, w->width);
	}
	return self;
}

static void
xui_on_vbox_allocated (struct widget *self)
{
	int parts = 0, height = self->height;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		if (w->height < 0)
			parts -= w->height;
		else
			height -= w->height;
	}

	int remaining = MAX (height, 0),
		part_height = parts ? remaining / parts : 0;
	struct widget *last = NULL;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		w->width = self->width;
		if (w->height < 0)
		{
			remaining -= (w->height *= -part_height);
			last = w;
		}
	}
	if (last)
		last->height += remaining;

	int x = self->x, y = self->y;
	LIST_FOR_EACH (struct widget, w, self->children)
	{
		widget_move (w, x - w->x, y - w->y);
		y += w->height;

		if (w->on_allocated)
			w->on_allocated (w);
	}
}

static struct widget *
xui_vbox (struct widget *head)
{
	struct widget *self = xcalloc (1, sizeof *self);
	self->children = head;
	self->on_allocated = xui_on_vbox_allocated;

	LIST_FOR_EACH (struct widget, w, head)
	{
		self->width = MAX (self->width, w->width);
		self->height += MAX (0, w->height);
	}
	return self;
}

// --- XUI ---------------------------------------------------------------------

static bool
xui_is_character_in_locale (ucs4_t ch)
{
	// Avoid the overhead joined with calling iconv() for all characters.
	if (g_xui.locale_is_utf8)
		return true;

	// The library really creates a new conversion object every single time
	// and doesn't provide any smarter APIs.  Luckily, most users use UTF-8.
	size_t len;
	char *tmp = u32_conv_to_encoding (locale_charset (), iconveh_error,
		&ch, 1, NULL, NULL, &len);
	if (!tmp)
		return false;
	free (tmp);
	return true;
}

static void
xui_on_flip (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g_xui.flip_event);

	// Waste of time, and may cause X11 to render uninitialised pixmaps.
	if (/*g.polling &&*/ !g_xui.refresh_event.active)
		g_xui.ui->flip ();
}

static void
xui_on_refresh (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g_xui.refresh_event);

	LIST_FOR_EACH (struct widget, w, g_xui.widgets)
		widget_destroy (w);

	g_xui.widgets = NULL;
	app_layout ();

	// Keep whatever the application gave them, for flexibility.
	LIST_FOR_EACH (struct widget, w, g_xui.widgets)
		if (w->on_allocated)
			w->on_allocated (w);

	g_xui.ui->render ();
	poller_idle_set (&g_xui.flip_event);
}

static void
xui_preinit (void)
{
	TERMO_CHECK_VERSION;
	if (!(g_xui.tk = termo_new (STDIN_FILENO, NULL, TERMO_FLAG_NOSTART)))
		exit_fatal ("failed to initialize termo");

	// This is also approximately what libunistring does internally,
	// since the locale name is canonicalized by locale_charset().
	// Note that non-Unicode locales are handled pretty inefficiently.
	g_xui.locale_is_utf8 = !strcasecmp_ascii (locale_charset (), "UTF-8");

	// Presumably, although not necessarily; unsure if queryable at all.
	g_xui.focused = true;

#ifdef LIBERTY_XUI_WANT_X11
	// Note that XSETTINGS overrides some values in the init.
	g_xui.x11_double_click_time = 500;
	g_xui.x11_double_click_distance = 5;
	g_xui.x11_fontname = "sans\\-serif-11";
	g_xui.x11_fontname_monospace = "monospace-11";
#endif  // LIBERTY_XUI_WANT_X11
}

static void
xui_start (struct poller *poller,
	bool force_x11, struct attrs *attrs, size_t attrs_len)
{
	(void) force_x11;

	g_xui.refresh_event = poller_idle_make (poller);
	g_xui.refresh_event.dispatcher = xui_on_refresh;
	g_xui.flip_event = poller_idle_make (poller);
	g_xui.flip_event.dispatcher = xui_on_flip;

	// Always initialized, but only activated with the TUI.
	g_xui.tty_event = poller_fd_make (poller, STDIN_FILENO);
	g_xui.tty_event.dispatcher = tui_on_tty_readable;
	g_xui.tk_timer = poller_timer_make (poller);
	g_xui.tk_timer.dispatcher = tui_on_key_timer;

#ifdef LIBERTY_XUI_WANT_X11
	if (force_x11 || (!isatty (STDIN_FILENO) && getenv ("DISPLAY")))
		x11_init (poller, attrs, attrs_len);
	else
#endif  // LIBERTY_XUI_WANT_X11
		tui_init (poller, attrs, attrs_len);
}

static void
xui_stop (void)
{
	poller_idle_reset (&g_xui.refresh_event);
	poller_idle_reset (&g_xui.flip_event);
	poller_fd_reset (&g_xui.tty_event);
	poller_timer_reset (&g_xui.tk_timer);

	g_xui.ui->destroy ();
	LIST_FOR_EACH (struct widget, w, g_xui.widgets)
		widget_destroy (w);

	termo_destroy (g_xui.tk);
}
