/*
 * liberty-tui.c: the ultimate C unlibrary: TUI
 *
 * Copyright (c) 2016 - 2017, Přemysl Eric Janouch <p@janouch.name>
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

// This file includes some common stuff to build TUI applications with

#include <ncurses.h>

// It is surprisingly hard to find a good library to handle Unicode shenanigans,
// and there's enough of those for it to be impractical to reimplement them.
//
//                         GLib          ICU     libunistring    utf8proc
// Decently sized            .            .            x            x
// Grapheme breaks           .            x            .            x
// Character width           x            .            x            x
// Locale handling           .            .            x            .
// Liberal license           .            x            .            x
//
// Also note that the ICU API is icky and uses UTF-16 for its primary encoding.
//
// Currently we're chugging along with libunistring but utf8proc seems viable.
// Non-Unicode locales can mostly be handled with simple iconv like in sdtui.
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

// --- Terminal output ---------------------------------------------------------

// Necessary abstraction to simplify aligned, formatted character output

// This callback you need to implement in the application
static bool app_is_character_in_locale (ucs4_t ch);

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
		if (current.width < 0 || !app_is_character_in_locale (current.c))
			current = invalid;

		ARRAY_RESERVE (self->chars, 1);
		self->chars[self->chars_len++] = current;
		self->total_width += current.width;
	}
}

static void
row_buffer_append_args (struct row_buffer *self, const char *s, ...)
	ATTRIBUTE_SENTINEL;

static void
row_buffer_append_args (struct row_buffer *self, const char *s, ...)
{
	va_list ap;
	va_start (ap, s);

	while (s)
	{
		row_buffer_append (self, s, va_arg (ap, chtype));
		s = va_arg (ap, const char *);
	}
	va_end (ap);
}

static void
row_buffer_append_buffer (struct row_buffer *self, const struct row_buffer *rb)
{
	ARRAY_RESERVE (self->chars, rb->chars_len);
	memcpy (self->chars + self->chars_len, rb->chars,
		rb->chars_len * sizeof *rb->chars);

	self->chars_len   += rb->chars_len;
	self->total_width += rb->total_width;
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
	if (app_is_character_in_locale (ellipsis))
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
