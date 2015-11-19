/*
 * liberty.c: the ultimate C unlibrary
 *
 * Copyright (c) 2014 - 2015, Přemysl Janouch <p.janouch@gmail.com>
 * All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
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

#define _POSIX_C_SOURCE 199309L
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <regex.h>
#include <libgen.h>
#include <syslog.h>
#include <fnmatch.h>
#include <iconv.h>
#include <pwd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>

#ifdef __unix__
// This file may define the "BSD" macro...
#include <sys/param.h>
// ...as well as these conflicting ones
#undef MIN
#undef MAX
#endif // __unix__

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif // ! NI_MAXHOST

#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif // ! NI_MAXSERV

#ifdef LIBERTY_WANT_SSL
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif // LIBERTY_WANT_SSL

#include <getopt.h>
#include "siphash.c"

extern char **environ;

#ifdef _POSIX_MONOTONIC_CLOCK
#define CLOCK_BEST CLOCK_MONOTONIC
#else // ! _POSIX_MONOTIC_CLOCK
#define CLOCK_BEST CLOCK_REALTIME
#endif // ! _POSIX_MONOTONIC_CLOCK

#if defined __GNUC__
#define ATTRIBUTE_PRINTF(x, y) __attribute__ ((format (printf, x, y)))
#else // ! __GNUC__
#define ATTRIBUTE_PRINTF(x, y)
#endif // ! __GNUC__

#if defined __GNUC__ && __GNUC__ >= 4
#define ATTRIBUTE_SENTINEL __attribute__ ((sentinel))
#else // ! __GNUC__ || __GNUC__ < 4
#define ATTRIBUTE_SENTINEL
#endif // ! __GNUC__ || __GNUC__ < 4

#define N_ELEMENTS(a) (sizeof (a) / sizeof ((a)[0]))

#define BLOCK_START  do {
#define BLOCK_END    } while (0)

#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

#define STRINGIFY(x) #x
#define XSTRINGIFY(x) STRINGIFY (x)

// --- Logging -----------------------------------------------------------------

static void
log_message_stdio (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;
	FILE *stream = stderr;

	fputs (quote, stream);
	vfprintf (stream, fmt, ap);
	fputs ("\n", stream);
}

static void (*g_log_message_real) (void *, const char *, const char *, va_list)
	= log_message_stdio;

static void
log_message (void *user_data, const char *quote, const char *fmt, ...)
	ATTRIBUTE_PRINTF (3, 4);

static void
log_message (void *user_data, const char *quote, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	g_log_message_real (user_data, quote, fmt, ap);
	va_end (ap);
}

// `fatal' is reserved for unexpected failures that would harm further operation

#ifndef print_fatal_data
#define print_fatal_data    NULL
#endif

#ifndef print_error_data
#define print_error_data    NULL
#endif

#ifndef print_warning_data
#define print_warning_data  NULL
#endif

#ifndef print_status_data
#define print_status_data   NULL
#endif

#define print_fatal(...) \
	log_message (print_fatal_data,   "fatal: ",   __VA_ARGS__)
#define print_error(...) \
	log_message (print_error_data,   "error: ",   __VA_ARGS__)
#define print_warning(...) \
	log_message (print_warning_data, "warning: ", __VA_ARGS__)
#define print_status(...) \
	log_message (print_status_data,  "-- ",       __VA_ARGS__)

#define exit_fatal(...)                                                        \
	BLOCK_START                                                                \
		print_fatal (__VA_ARGS__);                                             \
		exit (EXIT_FAILURE);                                                   \
	BLOCK_END

// --- Debugging and assertions ------------------------------------------------

// We should check everything that may possibly fail with at least a soft
// assertion, so that any causes for problems don't slip us by silently.
//
// `g_soft_asserts_are_deadly' may be useful while running inside a debugger.

static bool g_debug_mode;               ///< Debug messages are printed
static bool g_soft_asserts_are_deadly;  ///< soft_assert() aborts as well

#ifndef print_debug_data
#define print_debug_data   NULL
#endif

#define print_debug(...)                                                       \
	BLOCK_START                                                                \
		if (g_debug_mode)                                                      \
			log_message (print_debug_data, "debug: ", __VA_ARGS__);            \
	BLOCK_END

// A few other debugging shorthands for when failures are allowed
#define LOG_FUNC_FAILURE(name, desc)                                           \
	print_debug ("%s: %s: %s", __func__, (name), (desc))
#define LOG_LIBC_FAILURE(name)                                                 \
	print_debug ("%s: %s: %s", __func__, (name), strerror (errno))

static void
assertion_failure_handler (bool is_fatal, const char *file, int line,
	const char *function, const char *condition)
{
	const char *slash = strrchr (file, '/');
	if (slash)
		file = slash + 1;
	if (is_fatal)
	{
		print_fatal ("assertion failed [%s:%d in function %s]: %s",
			file, line, function, condition);
		abort ();
	}
	else
		print_debug ("assertion failed [%s:%d in function %s]: %s",
			file, line, function, condition);
}

#define soft_assert(condition)                                                 \
	((condition) ? true :                                                      \
		(assertion_failure_handler (g_soft_asserts_are_deadly,                 \
		__FILE__, __LINE__, __func__, #condition), false))

#define hard_assert(condition)                                                 \
	((condition) ? (void) 0 :                                                  \
		assertion_failure_handler (true,                                       \
		__FILE__, __LINE__, __func__, #condition))

// --- Safe memory management --------------------------------------------------

// When a memory allocation fails and we need the memory, we're usually pretty
// much fucked.  Use the non-prefixed versions when there's a legitimate
// worry that an unrealistic amount of memory may be requested for allocation.

// XXX: it's not a good idea to use print_message() as it may want to allocate
//   further memory for printf() and the output streams.  That may fail.

static void *
xmalloc (size_t n)
{
	void *p = malloc (n);
	if (!p)
		exit_fatal ("malloc: %s", strerror (errno));
	return p;
}

static void *
xcalloc (size_t n, size_t m)
{
	void *p = calloc (n, m);
	if (!p && n && m)
		exit_fatal ("calloc: %s", strerror (errno));
	return p;
}

static void *
xrealloc (void *o, size_t n)
{
	void *p = realloc (o, n);
	if (!p && n)
		exit_fatal ("realloc: %s", strerror (errno));
	return p;
}

static void *
xreallocarray (void *o, size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m)
	{
		errno = ENOMEM;
		exit_fatal ("reallocarray: %s", strerror (errno));
	}
	return xrealloc (o, n * m);
}

static char *
xstrdup (const char *s)
{
	return strcpy (xmalloc (strlen (s) + 1), s);
}

static char *
xstrndup (const char *s, size_t n)
{
	size_t size = strlen (s);
	if (n > size)
		n = size;

	char *copy = xmalloc (n + 1);
	memcpy (copy, s, n);
	copy[n] = '\0';
	return copy;
}

// --- Double-linked list helpers ----------------------------------------------

#define LIST_HEADER(type)                                                      \
	type *next;                                                                \
	type *prev;

#define LIST_PREPEND(head, link)                                               \
	BLOCK_START                                                                \
		(link)->prev = NULL;                                                   \
		(link)->next = (head);                                                 \
		if ((link)->next)                                                      \
			(link)->next->prev = (link);                                       \
		(head) = (link);                                                       \
	BLOCK_END

#define LIST_UNLINK(head, link)                                                \
	BLOCK_START                                                                \
		if ((link)->prev)                                                      \
			(link)->prev->next = (link)->next;                                 \
		else                                                                   \
			(head) = (link)->next;                                             \
		if ((link)->next)                                                      \
			(link)->next->prev = (link)->prev;                                 \
	BLOCK_END

#define LIST_APPEND_WITH_TAIL(head, tail, link)                                \
	BLOCK_START                                                                \
		(link)->prev = (tail);                                                 \
		(link)->next = NULL;                                                   \
		if ((link)->prev)                                                      \
			(link)->prev->next = (link);                                       \
		else                                                                   \
			(head) = (link);                                                   \
		(tail) = (link);                                                       \
	BLOCK_END

#define LIST_INSERT_WITH_TAIL(head, tail, link, following)                     \
	BLOCK_START                                                                \
		if (following)                                                         \
			LIST_APPEND_WITH_TAIL ((head), (following)->prev, (link));         \
		else                                                                   \
			LIST_APPEND_WITH_TAIL ((head), (tail), (link));                    \
		(link)->next = (following);                                            \
	BLOCK_END

#define LIST_UNLINK_WITH_TAIL(head, tail, link)                                \
	BLOCK_START                                                                \
		if ((tail) == (link))                                                  \
			(tail) = (link)->prev;                                             \
		LIST_UNLINK ((head), (link));                                          \
	BLOCK_END

#define LIST_FOR_EACH(type, iter, list)                                        \
	for (type *iter = (list), *next;                                           \
		(iter && (next = iter->next)) || iter;                                 \
		iter = next)

// --- Dynamically allocated string array --------------------------------------

struct str_vector
{
	char **vector;
	size_t len;
	size_t alloc;
};

static void
str_vector_init (struct str_vector *self)
{
	self->alloc = 4;
	self->len = 0;
	self->vector = xcalloc (sizeof *self->vector, self->alloc);
}

static void
str_vector_free (struct str_vector *self)
{
	unsigned i;
	for (i = 0; i < self->len; i++)
		free (self->vector[i]);

	free (self->vector);
	self->vector = NULL;
}

static void
str_vector_reset (struct str_vector *self)
{
	str_vector_free (self);
	str_vector_init (self);
}

static void
str_vector_add_owned (struct str_vector *self, char *s)
{
	self->vector[self->len] = s;
	if (++self->len >= self->alloc)
		self->vector = xreallocarray (self->vector,
			sizeof *self->vector, (self->alloc <<= 1));
	self->vector[self->len] = NULL;
}

static void
str_vector_add (struct str_vector *self, const char *s)
{
	str_vector_add_owned (self, xstrdup (s));
}

static void
str_vector_add_args (struct str_vector *self, const char *s, ...)
	ATTRIBUTE_SENTINEL;

static void
str_vector_add_args (struct str_vector *self, const char *s, ...)
{
	va_list ap;

	va_start (ap, s);
	while (s)
	{
		str_vector_add (self, s);
		s = va_arg (ap, const char *);
	}
	va_end (ap);
}

static void
str_vector_add_vector (struct str_vector *self, char **vector)
{
	while (*vector)
		str_vector_add (self, *vector++);
}

static char *
str_vector_steal (struct str_vector *self, size_t i)
{
	hard_assert (i < self->len);
	char *tmp = self->vector[i];
	memmove (self->vector + i, self->vector + i + 1,
		(self->len-- - i) * sizeof *self->vector);
	return tmp;
}

static void
str_vector_remove (struct str_vector *self, size_t i)
{
	free (str_vector_steal (self, i));
}

// --- Dynamically allocated strings -------------------------------------------

// Basically a string builder to abstract away manual memory management.

struct str
{
	char *str;                          ///< String data, null terminated
	size_t alloc;                       ///< How many bytes are allocated
	size_t len;                         ///< How long the string actually is
};

/// We don't care about allocations that are way too large for the content, as
/// long as the allocation is below the given threshold.  (Trivial heuristics.)
#define STR_SHRINK_THRESHOLD (1 << 20)

static void
str_init (struct str *self)
{
	self->alloc = 16;
	self->len = 0;
	self->str = strcpy (xmalloc (self->alloc), "");
}

static void
str_free (struct str *self)
{
	free (self->str);
	self->str = NULL;
	self->alloc = 0;
	self->len = 0;
}

static void
str_reset (struct str *self)
{
	str_free (self);
	str_init (self);
}

static char *
str_steal (struct str *self)
{
	char *str = self->str;
	self->str = NULL;
	str_free (self);
	return str;
}

static void
str_ensure_space (struct str *self, size_t n)
{
	// We allocate at least one more byte for the terminating null character
	size_t new_alloc = self->alloc;
	while (new_alloc <= self->len + n)
		new_alloc <<= 1;
	if (new_alloc != self->alloc)
		self->str = xrealloc (self->str, (self->alloc = new_alloc));
}

static void
str_append_data (struct str *self, const void *data, size_t n)
{
	str_ensure_space (self, n);
	memcpy (self->str + self->len, data, n);
	self->len += n;
	self->str[self->len] = '\0';
}

static void
str_append_c (struct str *self, char c)
{
	str_append_data (self, &c, 1);
}

static void
str_append (struct str *self, const char *s)
{
	str_append_data (self, s, strlen (s));
}

static void
str_append_str (struct str *self, const struct str *another)
{
	str_append_data (self, another->str, another->len);
}

static int
str_append_vprintf (struct str *self, const char *fmt, va_list va)
{
	va_list ap;
	int size;

	va_copy (ap, va);
	size = vsnprintf (NULL, 0, fmt, ap);
	va_end (ap);

	if (size < 0)
		return -1;

	va_copy (ap, va);
	str_ensure_space (self, size);
	size = vsnprintf (self->str + self->len, self->alloc - self->len, fmt, ap);
	va_end (ap);

	if (size > 0)
		self->len += size;

	return size;
}

static int
str_append_printf (struct str *self, const char *fmt, ...)
	ATTRIBUTE_PRINTF (2, 3);

static int
str_append_printf (struct str *self, const char *fmt, ...)
{
	va_list ap;

	va_start (ap, fmt);
	int size = str_append_vprintf (self, fmt, ap);
	va_end (ap);
	return size;
}

static void
str_remove_slice (struct str *self, size_t start, size_t length)
{
	size_t end = start + length;
	hard_assert (end <= self->len);
	memmove (self->str + start, self->str + end, self->len - end);
	self->str[self->len -= length] = '\0';

	// Shrink the string if the allocation becomes way too large
	if (self->alloc >= STR_SHRINK_THRESHOLD && self->len < (self->alloc >> 2))
		self->str = xrealloc (self->str, self->alloc >>= 2);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
str_pack_u8 (struct str *self, uint8_t x)
{
	str_append_data (self, &x, 1);
}

static void
str_pack_u16 (struct str *self, uint16_t x)
{
	uint8_t tmp[2] = { x >> 8, x };
	str_append_data (self, tmp, sizeof tmp);
}

static void
str_pack_u32 (struct str *self, uint32_t x)
{
	uint32_t u = x;
	uint8_t tmp[4] = { u >> 24, u >> 16, u >> 8, u };
	str_append_data (self, tmp, sizeof tmp);
}

static void
str_pack_u64 (struct str *self, uint64_t x)
{
	uint8_t tmp[8] =
		{ x >> 56, x >> 48, x >> 40, x >> 32, x >> 24, x >> 16, x >> 8, x };
	str_append_data (self, tmp, sizeof tmp);
}

#define str_pack_i8(self, x)   str_pack_u8  ((self), (uint8_t)  (x))
#define str_pack_i16(self, x)  str_pack_u16 ((self), (uint16_t) (x))
#define str_pack_i32(self, x)  str_pack_u32 ((self), (uint32_t) (x))
#define str_pack_i64(self, x)  str_pack_u64 ((self), (uint64_t) (x))

// --- Errors ------------------------------------------------------------------

// Error reporting utilities.  Inspired by GError, only much simpler.

struct error
{
	char *message;                      ///< Textual description of the event
};

static void
error_set (struct error **e, const char *message, ...) ATTRIBUTE_PRINTF (2, 3);

static void
error_set (struct error **e, const char *message, ...)
{
	if (!e)
		return;

	va_list ap;
	va_start (ap, message);
	int size = vsnprintf (NULL, 0, message, ap);
	va_end (ap);

	hard_assert (size >= 0);

	struct error *tmp = xmalloc (sizeof *tmp);
	tmp->message = xmalloc (size + 1);

	va_start (ap, message);
	size = vsnprintf (tmp->message, size + 1, message, ap);
	va_end (ap);

	hard_assert (size >= 0);

	soft_assert (*e == NULL);
	*e = tmp;
}

static void
error_free (struct error *e)
{
	free (e->message);
	free (e);
}

static void
error_propagate (struct error **destination, struct error *source)
{
	if (!destination)
	{
		error_free (source);
		return;
	}

	soft_assert (*destination == NULL);
	*destination = source;
}

// --- File descriptor utilities -----------------------------------------------

static void
set_cloexec (int fd)
{
	soft_assert (fcntl (fd, F_SETFD, fcntl (fd, F_GETFD) | FD_CLOEXEC) != -1);
}

static bool
set_blocking (int fd, bool blocking)
{
	int flags = fcntl (fd, F_GETFL);
	if (flags == -1)
		exit_fatal ("%s: %s", "fcntl", strerror (errno));

	bool prev = !(flags & O_NONBLOCK);
	if (blocking)
		flags &= ~O_NONBLOCK;
	else
		flags |=  O_NONBLOCK;

	hard_assert (fcntl (fd, F_SETFL, flags) != -1);
	return prev;
}

static void
xclose (int fd)
{
	while (close (fd) == -1)
		if (!soft_assert (errno == EINTR))
			break;
}

// --- Randomness --------------------------------------------------------------

static bool
random_bytes (void *output, size_t len, struct error **e)
{
	bool result = false;
	int fd = open ("/dev/urandom", O_RDONLY);
	ssize_t got = 0;

	if (fd < 0)
	{
		error_set (e, "%s: %s", "open", strerror (errno));
		return false;
	}
	else if ((got = read (fd, output, len)) < 0)
		error_set (e, "%s: %s", "read", strerror (errno));
	else if (got != (ssize_t) len)
		error_set (e, "can't get enough bytes from the device");
	else
		result = true;

	xclose (fd);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static unsigned char g_siphash_key[16] = "SipHash 2-4 key!";

static inline void
siphash_wrapper_randomize (void)
{
	// I guess there's no real need to be this paranoic, so we ignore failures
	soft_assert (random_bytes (g_siphash_key, sizeof g_siphash_key, NULL));
}

static inline uint64_t
siphash_wrapper (const void *m, size_t len)
{
	return siphash (g_siphash_key, m, len);
}

// --- String hash map ---------------------------------------------------------

// The most basic <string, managed pointer> map (or associative array).

struct str_map_link
{
	LIST_HEADER (struct str_map_link)

	void *data;                         ///< Payload
	size_t key_length;                  ///< Length of the key without '\0'
	char key[];                         ///< The key for this link
};

struct str_map
{
	struct str_map_link **map;          ///< The hash table data itself
	size_t alloc;                       ///< Number of allocated entries
	size_t len;                         ///< Number of entries in the table
	void (*free) (void *);              ///< Callback to destruct the payload

	/// Callback that transforms all key values for storage and comparison;
	/// has to behave exactly like strxfrm().
	size_t (*key_xfrm) (char *dest, const char *src, size_t n);

	bool shrink_lock;                   ///< Lock against autoshrinking
};

#define STR_MAP_MIN_ALLOC 16

typedef void (*str_map_free_fn) (void *);

static void
str_map_init (struct str_map *self)
{
	self->alloc = STR_MAP_MIN_ALLOC;
	self->len = 0;
	self->free = NULL;
	self->key_xfrm = NULL;
	self->map = xcalloc (self->alloc, sizeof *self->map);
	self->shrink_lock = false;
}

static void
str_map_clear (struct str_map *self)
{
	struct str_map_link **iter, **end = self->map + self->alloc;
	struct str_map_link *link, *tmp;

	for (iter = self->map; iter < end; iter++)
		for (link = *iter; link; link = tmp)
		{
			tmp = link->next;
			if (self->free)
				self->free (link->data);
			free (link);
		}

	self->len = 0;
	memset (self->map, 0, self->alloc * sizeof *self->map);
}

static void
str_map_free (struct str_map *self)
{
	str_map_clear (self);
	free (self->map);
	self->map = NULL;
}

static uint64_t
str_map_pos (struct str_map *self, const char *s)
{
	size_t mask = self->alloc - 1;
	return siphash_wrapper (s, strlen (s)) & mask;
}

static uint64_t
str_map_link_hash (struct str_map_link *self)
{
	return siphash_wrapper (self->key, self->key_length);
}

static void
str_map_resize (struct str_map *self, size_t new_size)
{
	struct str_map_link **old_map = self->map;
	size_t i, old_size = self->alloc;

	// Only powers of two, so that we don't need to compute the modulo
	hard_assert ((new_size & (new_size - 1)) == 0);
	size_t mask = new_size - 1;

	self->alloc = new_size;
	self->map = xcalloc (self->alloc, sizeof *self->map);
	for (i = 0; i < old_size; i++)
	{
		struct str_map_link *iter = old_map[i], *next_iter;
		while (iter)
		{
			next_iter = iter->next;
			uint64_t pos = str_map_link_hash (iter) & mask;
			LIST_PREPEND (self->map[pos], iter);
			iter = next_iter;
		}
	}

	free (old_map);
}

static void
str_map_shrink (struct str_map *self)
{
	if (self->shrink_lock)
		return;

	// The array should be at least 1/4 full
	size_t new_alloc = self->alloc;
	while (self->len < (new_alloc >> 2)
		&& new_alloc >= (STR_MAP_MIN_ALLOC << 1))
		new_alloc >>= 1;
	if (new_alloc != self->alloc)
		str_map_resize (self, new_alloc);
}

static void
str_map_set_real (struct str_map *self, const char *key, void *value)
{
	uint64_t pos = str_map_pos (self, key);
	struct str_map_link *iter = self->map[pos];
	for (; iter; iter = iter->next)
	{
		if (strcmp (key, iter->key))
			continue;

		// Storing the same data doesn't destroy it
		if (self->free && value != iter->data)
			self->free (iter->data);

		if (value)
		{
			iter->data = value;
			return;
		}

		LIST_UNLINK (self->map[pos], iter);
		free (iter);
		self->len--;

		str_map_shrink (self);
		return;
	}

	if (!value)
		return;

	if (self->len >= self->alloc)
	{
		str_map_resize (self, self->alloc << 1);
		pos = str_map_pos (self, key);
	}

	// Link in a new element for the given <key, value> pair
	size_t key_length = strlen (key);
	struct str_map_link *link = xmalloc (sizeof *link + key_length + 1);
	link->data = value;
	link->key_length = key_length;
	memcpy (link->key, key, key_length + 1);

	LIST_PREPEND (self->map[pos], link);
	self->len++;
}

static void
str_map_set (struct str_map *self, const char *key, void *value)
{
	if (!self->key_xfrm)
	{
		str_map_set_real (self, key, value);
		return;
	}
	char tmp[self->key_xfrm (NULL, key, 0) + 1];
	self->key_xfrm (tmp, key, sizeof tmp);
	str_map_set_real (self, tmp, value);
}

static void *
str_map_find_real (struct str_map *self, const char *key)
{
	struct str_map_link *iter = self->map[str_map_pos (self, key)];
	for (; iter; iter = iter->next)
		if (!strcmp (key, (const char *) iter + sizeof *iter))
			return iter->data;
	return NULL;
}

static void *
str_map_find (struct str_map *self, const char *key)
{
	if (!self->key_xfrm)
		return str_map_find_real (self, key);

	char tmp[self->key_xfrm (NULL, key, 0) + 1];
	self->key_xfrm (tmp, key, sizeof tmp);
	return str_map_find_real (self, tmp);
}

static void *
str_map_steal (struct str_map *self, const char *key)
{
	void *value = str_map_find (self, key);
	if (value)
	{
		str_map_free_fn free_saved = self->free;
		self->free = NULL;
		str_map_set (self, key, NULL);
		self->free = free_saved;
	}
	return value;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// This iterator is intended for accessing and eventually adding links.
// Use `link' directly to access the data.

struct str_map_iter
{
	struct str_map *map;                ///< The map we're iterating
	size_t next_index;                  ///< Next table index to search
	struct str_map_link *link;          ///< Current link
};

static void
str_map_iter_init (struct str_map_iter *self, struct str_map *map)
{
	self->map = map;
	self->next_index = 0;
	self->link = NULL;
}

static void *
str_map_iter_next (struct str_map_iter *self)
{
	struct str_map *map = self->map;
	if (self->link)
		self->link = self->link->next;
	while (!self->link)
	{
		if (self->next_index >= map->alloc)
			return NULL;
		self->link = map->map[self->next_index++];
	}
	return self->link->data;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// This iterator is intended for accessing and eventually removing links.
// Use `link' directly to access the data.

struct str_map_unset_iter
{
	struct str_map_iter iter;           ///< Regular iterator
	struct str_map_link *link;          ///< Current link
	struct str_map_link *next;          ///< Next link
};

static void
str_map_unset_iter_init (struct str_map_unset_iter *self, struct str_map *map)
{
	str_map_iter_init (&self->iter, map);
	self->iter.map->shrink_lock = true;
	(void) str_map_iter_next (&self->iter);
	self->next = self->iter.link;
}

static void *
str_map_unset_iter_next (struct str_map_unset_iter *self)
{
	if (!(self->link = self->next))
		return NULL;
	(void) str_map_iter_next (&self->iter);
	self->next = self->iter.link;
	return self->link->data;
}

static void
str_map_unset_iter_free (struct str_map_unset_iter *self)
{
	self->iter.map->shrink_lock = false;
	str_map_shrink (self->iter.map);
}

// --- Event loop --------------------------------------------------------------

#ifdef LIBERTY_WANT_POLLER

// Basically the poor man's GMainLoop/libev/libuv.  It might make some sense
// to instead use those tested and proven libraries but we don't need much
// and it's interesting to implement.

// Actually it mustn't be totally shitty as scanning exercises it quite a bit.
// We sacrifice some memory to allow for O(1) and O(log n) operations.

typedef void (*poller_fd_fn) (const struct pollfd *, void *);
typedef void (*poller_timer_fn) (void *);
typedef void (*poller_idle_fn) (void *);

#define POLLER_MIN_ALLOC 16

struct poller_timer
{
	struct poller_timers *timers;       ///< The timers part of our poller
	ssize_t index;                      ///< Where we are in the array, or -1

	int64_t when;                       ///< When is the timer to expire

	poller_timer_fn dispatcher;         ///< Event dispatcher
	void *user_data;                    ///< User data
};

struct poller_fd
{
	struct poller *poller;              ///< Our poller
	ssize_t index;                      ///< Where we are in the array, or -1

	int fd;                             ///< Our file descriptor
	short events;                       ///< The poll() events we registered for
	bool closed;                        ///< Whether fd has been closed already

	poller_fd_fn dispatcher;            ///< Event dispatcher
	void *user_data;                    ///< User data
};

struct poller_idle
{
	LIST_HEADER (struct poller_idle)
	struct poller *poller;              ///< Our poller

	bool active;                        ///< Whether we're on the list

	poller_idle_fn dispatcher;          ///< Event dispatcher
	void *user_data;                    ///< User data
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct poller_timers
{
	struct poller_timer **heap;         ///< Min-heap of timers
	size_t len;                         ///< Number of scheduled timers
	size_t alloc;                       ///< Number of timers allocated
};

static void
poller_timers_init (struct poller_timers *self)
{
	self->alloc = POLLER_MIN_ALLOC;
	self->len = 0;
	self->heap = xmalloc (self->alloc * sizeof *self->heap);
}

static void
poller_timers_free (struct poller_timers *self)
{
	free (self->heap);
}

static int64_t
poller_timers_get_current_time (void)
{
#ifdef _POSIX_TIMERS
	struct timespec tp;
	hard_assert (clock_gettime (CLOCK_BEST, &tp) != -1);
	return (int64_t) tp.tv_sec * 1000 + (int64_t) tp.tv_nsec / 1000000;
#else
	struct timeval tp;
	gettimeofday (&tp, NULL);
	return (int64_t) tp.tv_sec * 1000 + (int64_t) tp.tv_usec / 1000;
#endif
}

static void
poller_timers_heapify_down (struct poller_timers *self, size_t index)
{
	typedef struct poller_timer *timer_t;
	timer_t *end = self->heap + self->len;

	while (true)
	{
		timer_t *parent = self->heap + index;
		timer_t *left   = self->heap + 2 * index + 1;
		timer_t *right  = self->heap + 2 * index + 2;

		timer_t *lowest = parent;
		if (left  < end && (*left) ->when < (*lowest)->when)
			lowest = left;
		if (right < end && (*right)->when < (*lowest)->when)
			lowest = right;
		if (parent == lowest)
			break;

		timer_t tmp = *parent;
		*parent = *lowest;
		*lowest = tmp;

		(*parent)->index = parent - self->heap;
		(*lowest)->index = lowest - self->heap;
		index = lowest - self->heap;
	}
}

static void
poller_timers_remove_at_index (struct poller_timers *self, size_t index)
{
	hard_assert (index < self->len);
	self->heap[index]->index = -1;
	if (index == --self->len)
		return;

	self->heap[index] = self->heap[self->len];
	self->heap[index]->index = index;
	poller_timers_heapify_down (self, index);
}

static void
poller_timers_dispatch (struct poller_timers *self)
{
	int64_t now = poller_timers_get_current_time ();
	while (self->len && self->heap[0]->when <= now)
	{
		struct poller_timer *timer = self->heap[0];
		poller_timers_remove_at_index (self, 0);
		timer->dispatcher (timer->user_data);
	}
}

static void
poller_timers_heapify_up (struct poller_timers *self, size_t index)
{
	while (index != 0)
	{
		size_t parent = (index - 1) / 2;
		if (self->heap[parent]->when <= self->heap[index]->when)
			break;

		struct poller_timer *tmp = self->heap[parent];
		self->heap[parent] = self->heap[index];
		self->heap[index] = tmp;

		self->heap[parent]->index = parent;
		self->heap[index] ->index = index;
		index = parent;
	}
}

static void
poller_timers_set (struct poller_timers *self, struct poller_timer *timer)
{
	hard_assert (timer->timers == self);
	if (timer->index != -1)
	{
		poller_timers_heapify_down (self, timer->index);
		poller_timers_heapify_up (self, timer->index);
		return;
	}

	if (self->len == self->alloc)
		self->heap = xreallocarray (self->heap,
			self->alloc <<= 1, sizeof *self->heap);
	self->heap[self->len] = timer;
	timer->index = self->len;
	poller_timers_heapify_up (self, self->len++);
}

static int
poller_timers_get_poll_timeout (struct poller_timers *self)
{
	if (!self->len)
		return -1;

	int64_t timeout = self->heap[0]->when - poller_timers_get_current_time ();
	if (timeout <= 0)
		return 0;
	if (timeout > INT_MAX)
		return INT_MAX;
	return timeout;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_idle_dispatch (struct poller_idle *list)
{
	struct poller_idle *iter, *next;
	for (iter = list; iter; iter = next)
	{
		next = iter->next;
		iter->dispatcher (iter->user_data);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifdef __linux__
#include <sys/epoll.h>

struct poller
{
	int epoll_fd;                       ///< The epoll FD
	struct poller_fd **fds;             ///< Information associated with each FD
	int *dummy;                         ///< For poller_remove_from_dispatch()
	struct epoll_event *revents;        ///< Output array for epoll_wait()
	size_t len;                         ///< Number of polled descriptors
	size_t alloc;                       ///< Number of entries allocated

	struct poller_timers timers;        ///< Timeouts
	struct poller_idle *idle;           ///< Idle events

	int revents_len;                    ///< Number of entries in `revents'
};

static void
poller_init (struct poller *self)
{
	self->epoll_fd = epoll_create (POLLER_MIN_ALLOC);
	hard_assert (self->epoll_fd != -1);
	set_cloexec (self->epoll_fd);

	self->len = 0;
	self->alloc = POLLER_MIN_ALLOC;
	self->fds = xcalloc (self->alloc, sizeof *self->fds);
	self->dummy = xcalloc (self->alloc, sizeof *self->dummy);
	self->revents = xcalloc (self->alloc, sizeof *self->revents);
	self->revents_len = 0;

	poller_timers_init (&self->timers);
	self->idle = NULL;
}

static void
poller_free (struct poller *self)
{
	for (size_t i = 0; i < self->len; i++)
	{
		struct poller_fd *fd = self->fds[i];
		hard_assert (epoll_ctl (self->epoll_fd,
			EPOLL_CTL_DEL, fd->fd, (void *) "") != -1);
	}

	poller_timers_free (&self->timers);

	xclose (self->epoll_fd);
	free (self->fds);
	free (self->dummy);
	free (self->revents);
}

static void
poller_ensure_space (struct poller *self)
{
	if (self->len < self->alloc)
		return;

	self->alloc <<= 1;
	hard_assert (self->alloc != 0);

	self->revents = xreallocarray
		(self->revents, sizeof *self->revents, self->alloc);
	self->fds = xreallocarray
		(self->fds, sizeof *self->fds, self->alloc);
	self->dummy = xreallocarray
		(self->dummy, sizeof *self->dummy, self->alloc);
}

static short
poller_epoll_to_poll_events (uint32_t events)
{
	short result = 0;
	if (events & EPOLLIN)   result |= POLLIN;
	if (events & EPOLLOUT)  result |= POLLOUT;
	if (events & EPOLLERR)  result |= POLLERR;
	if (events & EPOLLHUP)  result |= POLLHUP;
	if (events & EPOLLPRI)  result |= POLLPRI;
	return result;
}

static uint32_t
poller_poll_to_epoll_events (short events)
{
	uint32_t result = 0;
	if (events & POLLIN)   result |= EPOLLIN;
	if (events & POLLOUT)  result |= EPOLLOUT;
	if (events & POLLERR)  result |= EPOLLERR;
	if (events & POLLHUP)  result |= EPOLLHUP;
	if (events & POLLPRI)  result |= EPOLLPRI;
	return result;
}

static void
poller_set (struct poller *self, struct poller_fd *fd)
{
	hard_assert (fd->poller == self);
	bool modifying = true;
	if (fd->index == -1)
	{
		poller_ensure_space (self);
		self->fds[fd->index = self->len++] = fd;
		modifying = false;
	}

	struct epoll_event event;
	event.events = poller_poll_to_epoll_events (fd->events);
	event.data.ptr = fd;
	hard_assert (epoll_ctl (self->epoll_fd,
		modifying ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd->fd, &event) != -1);
}

static int
poller_compare_fds (const void *ax, const void *bx)
{
	const struct epoll_event *ay = ax, *by = bx;
	struct poller_fd *a = ay->data.ptr, *b = by->data.ptr;
	return a->fd - b->fd;
}

static void
poller_remove_from_dispatch (struct poller *self, const struct poller_fd *fd)
{
	if (!self->revents_len)
		return;

	struct epoll_event key = { .data.ptr = (void *) fd }, *fd_event;
	if ((fd_event = bsearch (&key, self->revents,
		self->revents_len, sizeof *self->revents, poller_compare_fds)))
	{
		fd_event->events = -1;

		// Don't let any further bsearch()'s touch possibly freed memory
		int *dummy = self->dummy + (fd_event - self->revents);
		*dummy = fd->fd;
		fd_event->data.ptr =
			(uint8_t *) dummy - offsetof (struct poller_fd, fd);
	}
}

static void
poller_remove_at_index (struct poller *self, size_t index)
{
	hard_assert (index < self->len);
	struct poller_fd *fd = self->fds[index];
	fd->index = -1;

	poller_remove_from_dispatch (self, fd);
	if (!fd->closed)
		hard_assert (epoll_ctl (self->epoll_fd,
			EPOLL_CTL_DEL, fd->fd, (void *) "") != -1);

	if (index != --self->len)
	{
		self->fds[index] = self->fds[self->len];
		self->fds[index]->index = index;
	}
}

static void
poller_run (struct poller *self)
{
	// Not reentrant
	hard_assert (!self->revents_len);

	int n_fds;
	do
		n_fds = epoll_wait (self->epoll_fd, self->revents, self->alloc,
			self->idle ? 0 : poller_timers_get_poll_timeout (&self->timers));
	while (n_fds == -1 && errno == EINTR);

	if (n_fds == -1)
		exit_fatal ("%s: %s", "epoll", strerror (errno));

	// Sort them by file descriptor number for binary search
	qsort (self->revents, n_fds, sizeof *self->revents, poller_compare_fds);
	self->revents_len = n_fds;

	poller_timers_dispatch (&self->timers);
	poller_idle_dispatch (self->idle);

	for (int i = 0; i < n_fds; i++)
	{
		struct epoll_event *revents = self->revents + i;
		if (revents->events == (uint32_t) -1)
			continue;

		struct poller_fd *fd = revents->data.ptr;
		hard_assert (fd->index != -1);

		struct pollfd pfd;
		pfd.fd = fd->fd;
		pfd.revents = poller_epoll_to_poll_events (revents->events);
		pfd.events = fd->events;

		fd->dispatcher (&pfd, fd->user_data);
	}

	self->revents_len = 0;
}

#elif defined (BSD)

// Mac OS X's kqueue is fatally broken, or so I've been told; leaving it out.
// Otherwise this is sort of similar to the epoll version.

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

struct poller
{
	int kqueue_fd;                      ///< The kqueue FD
	struct poller_fd **fds;             ///< Information associated with each FD
	struct kevent *revents;             ///< Output array for kevent()
	size_t len;                         ///< Number of polled descriptors
	size_t alloc;                       ///< Number of entries allocated

	struct poller_timers timers;        ///< Timeouts
	struct poller_idle *idle;           ///< Idle events

	int revents_len;                    ///< Number of entries in `revents'
};

static void
poller_init (struct poller *self)
{
	self->kqueue_fd = kqueue ();
	hard_assert (self->kqueue_fd != -1);
	set_cloexec (self->kqueue_fd);

	self->len = 0;
	self->alloc = POLLER_MIN_ALLOC;
	self->fds = xcalloc (self->alloc, sizeof *self->fds);
	self->revents = xcalloc (self->alloc, sizeof *self->revents);
	self->revents_len = 0;

	poller_timers_init (&self->timers);
	self->idle = NULL;
}

static void
poller_free (struct poller *self)
{
	poller_timers_free (&self->timers);

	xclose (self->kqueue_fd);
	free (self->fds);
	free (self->revents);
}

static void
poller_ensure_space (struct poller *self)
{
	if (self->len < self->alloc)
		return;

	self->alloc <<= 1;
	hard_assert (self->alloc != 0);

	self->revents = xreallocarray
		(self->revents, sizeof *self->revents, self->alloc);
	self->fds = xreallocarray
		(self->fds, sizeof *self->fds, self->alloc);
}

static void
poller_set (struct poller *self, struct poller_fd *fd)
{
	hard_assert (fd->poller == self);
	bool modifying = true;
	if (fd->index == -1)
	{
		poller_ensure_space (self);
		self->fds[fd->index = self->len++] = fd;
		modifying = false;
	}

	// We have to watch for readability and writeability separately;
	// to simplify matters, we can just disable what we don't desire to receive
	struct kevent changes[2];
	EV_SET (&changes[0], fd->fd, EVFILT_READ,  EV_ADD, 0, 0, fd);
	EV_SET (&changes[1], fd->fd, EVFILT_WRITE, EV_ADD, 0, 0, fd);
	changes[0].flags |= (fd->events & POLLIN)  ? EV_ENABLE : EV_DISABLE;
	changes[1].flags |= (fd->events & POLLOUT) ? EV_ENABLE : EV_DISABLE;

	if (kevent (self->kqueue_fd,
		changes, N_ELEMENTS (changes), NULL, 0, NULL) == -1)
		exit_fatal ("%s: %s", "kevent", strerror (errno));
}

static int
poller_compare_fds (const void *ax, const void *bx)
{
	const struct kevent *ay = ax, *by = bx;
	return (int) ay->ident - (int) by->ident;
}

static void
poller_dummify (struct kevent *fd_event)
{
	fd_event->flags = USHRT_MAX;
}

static void
poller_remove_from_dispatch (struct poller *self, const struct poller_fd *fd)
{
	if (!self->revents_len)
		return;

	struct kevent key = { .ident = fd->fd }, *fd_event;
	if (!(fd_event = bsearch (&key, self->revents,
		self->revents_len, sizeof *self->revents, poller_compare_fds)))
		return;

	// The FD may appear twice -- both for reading and writing
	int index = fd_event - self->revents;

	if (index > 0
	 && !poller_compare_fds (&key, fd_event - 1))
		poller_dummify (fd_event - 1);

	poller_dummify (fd_event);

	if (index < self->revents_len - 1
	 && !poller_compare_fds (&key, fd_event + 1))
		poller_dummify (fd_event + 1);
}

static void
poller_remove_at_index (struct poller *self, size_t index)
{
	hard_assert (index < self->len);
	struct poller_fd *fd = self->fds[index];
	fd->index = -1;

	poller_remove_from_dispatch (self, fd);

	if (index != --self->len)
	{
		self->fds[index] = self->fds[self->len];
		self->fds[index]->index = index;
	}

	if (fd->closed)
		return;

	struct kevent changes[2];
	EV_SET (&changes[0], fd->fd, EVFILT_READ,  EV_DELETE, 0, 0, fd);
	EV_SET (&changes[1], fd->fd, EVFILT_WRITE, EV_DELETE, 0, 0, fd);

	if (kevent (self->kqueue_fd,
		changes, N_ELEMENTS (changes), NULL, 0, NULL) == -1)
		exit_fatal ("%s: %s", "kevent", strerror (errno));
}

static struct timespec
poller_timeout_to_timespec (int ms)
{
	return (struct timespec)
	{
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000 * 1000
	};
}

static short
poller_kqueue_to_poll_events (struct kevent *event)
{
	short result = 0;
	if (event->filter == EVFILT_READ)
	{
		result |= POLLIN;
		if ((event->flags & EV_EOF) && event->fflags)
			result |= POLLERR;
	}
	if (event->filter == EVFILT_WRITE)  result |= POLLOUT;
	if (event->flags & EV_EOF)          result |= POLLHUP;
	return result;
}

static void
poller_run (struct poller *self)
{
	// Not reentrant
	hard_assert (!self->revents_len);

	int n_fds;
	do
	{
		struct timespec ts = poller_timeout_to_timespec
			(self->idle ? 0 : poller_timers_get_poll_timeout (&self->timers));
		n_fds = kevent (self->kqueue_fd,
			NULL, 0, self->revents, self->len, &ts);
	}
	while (n_fds == -1 && errno == EINTR);

	if (n_fds == -1)
		exit_fatal ("%s: %s", "kevent", strerror (errno));

	// Sort them by file descriptor number for binary search
	qsort (self->revents, n_fds, sizeof *self->revents, poller_compare_fds);
	self->revents_len = n_fds;

	poller_timers_dispatch (&self->timers);
	poller_idle_dispatch (self->idle);

	for (int i = 0; i < n_fds; i++)
	{
		struct kevent *event = self->revents + i;
		if (event->flags == USHRT_MAX)
			continue;

		struct poller_fd *fd = event->udata;
		hard_assert (fd->index != -1);

		struct pollfd pfd;
		pfd.fd = fd->fd;
		pfd.revents = poller_kqueue_to_poll_events (event);
		pfd.events = fd->events;

		// Read and write events are separate in the kqueue API -- merge them
		int sibling = 1;
		while (i + sibling < n_fds
			&& !poller_compare_fds (event, event + sibling))
			pfd.revents |= poller_kqueue_to_poll_events (event + sibling++);
		if ((pfd.revents & POLLHUP) && (pfd.revents & POLLOUT))
			pfd.revents &= ~POLLOUT;
		i += --sibling;

		fd->dispatcher (&pfd, fd->user_data);
	}

	self->revents_len = 0;
}

#else  // ! BSD

struct poller
{
	struct pollfd *fds;                 ///< Polled descriptors
	struct poller_fd **fds_data;        ///< Additional information for each FD
	size_t len;                         ///< Number of polled descriptors
	size_t alloc;                       ///< Number of entries allocated

	struct poller_timers timers;        ///< Timers
	struct poller_idle *idle;           ///< Idle events
	int dispatch_next;                  ///< The next dispatched FD or -1
};

static void
poller_init (struct poller *self)
{
	self->alloc = POLLER_MIN_ALLOC;
	self->len = 0;
	self->fds = xcalloc (self->alloc, sizeof *self->fds);
	self->fds_data = xcalloc (self->alloc, sizeof *self->fds_data);
	poller_timers_init (&self->timers);
	self->dispatch_next = -1;
}

static void
poller_free (struct poller *self)
{
	free (self->fds);
	free (self->fds_data);
	poller_timers_free (&self->timers);
}

static void
poller_ensure_space (struct poller *self)
{
	if (self->len < self->alloc)
		return;

	self->alloc <<= 1;
	self->fds = xreallocarray (self->fds, sizeof *self->fds, self->alloc);
	self->fds_data = xreallocarray
		(self->fds_data, sizeof *self->fds_data, self->alloc);
}

static void
poller_set (struct poller *self, struct poller_fd *fd)
{
	hard_assert (fd->poller == self);
	if (fd->index == -1)
	{
		poller_ensure_space (self);
		self->fds_data[fd->index = self->len++] = fd;
	}

	struct pollfd *new_entry = self->fds + fd->index;
	memset (new_entry, 0, sizeof *new_entry);
	new_entry->fd = fd->fd;
	new_entry->events = fd->events;
}

static void
poller_remove_at_index (struct poller *self, size_t index)
{
	hard_assert (index < self->len);
	struct poller_fd *fd = self->fds_data[index];
	fd->index = -1;

	if (index == --self->len)
		return;

	// Make sure that we don't disrupt the dispatch loop; kind of crude
	if ((int) index < self->dispatch_next)
	{
		memmove (self->fds + index, self->fds + index + 1,
			(self->len - index) * sizeof *self->fds);
		memmove (self->fds_data + index, self->fds_data + index + 1,
			(self->len - index) * sizeof *self->fds_data);
		for (size_t i = index; i < self->len; i++)
			self->fds_data[i]->index = i;

		self->dispatch_next--;
	}
	else
	{
		self->fds[index]      = self->fds     [self->len];
		self->fds_data[index] = self->fds_data[self->len];
		self->fds_data[index]->index = index;
	}
}

static void
poller_run (struct poller *self)
{
	// Not reentrant
	hard_assert (self->dispatch_next == -1);

	int result;
	do
		result = poll (self->fds, self->len,
			self->idle ? 0 : poller_timers_get_poll_timeout (&self->timers));
	while (result == -1 && errno == EINTR);

	if (result == -1)
		exit_fatal ("%s: %s", "poll", strerror (errno));

	poller_timers_dispatch (&self->timers);
	poller_idle_dispatch (self->idle);

	for (int i = 0; i < (int) self->len; )
	{
		struct pollfd pfd = self->fds[i];
		struct poller_fd *fd = self->fds_data[i];
		self->dispatch_next = ++i;
		if (pfd.revents)
			fd->dispatcher (&pfd, fd->user_data);
		i = self->dispatch_next;
	}

	self->dispatch_next = -1;
}

#endif  // ! BSD

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_timer_init (struct poller_timer *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);
	self->timers = &poller->timers;
	self->index = -1;
}

static void
poller_timer_set (struct poller_timer *self, int timeout_ms)
{
	self->when = poller_timers_get_current_time () + timeout_ms;
	poller_timers_set (self->timers, self);
}

static bool
poller_timer_is_active (struct poller_timer *self)
{
	return self->index != -1;
}

static void
poller_timer_reset (struct poller_timer *self)
{
	if (self->index != -1)
		poller_timers_remove_at_index (self->timers, self->index);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_idle_init (struct poller_idle *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);
	self->poller = poller;
}

static void
poller_idle_set (struct poller_idle *self)
{
	if (self->active)
		return;

	LIST_PREPEND (self->poller->idle, self);
	self->active = true;
}

static void
poller_idle_reset (struct poller_idle *self)
{
	if (!self->active)
		return;

	LIST_UNLINK (self->poller->idle, self);
	self->prev = NULL;
	self->next = NULL;
	self->active = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_fd_init (struct poller_fd *self, struct poller *poller, int fd)
{
	memset (self, 0, sizeof *self);
	self->poller = poller;
	self->index = -1;
	self->fd = fd;
}

static void
poller_fd_set (struct poller_fd *self, short events)
{
	self->events = events;
	poller_set (self->poller, self);
}

static void
poller_fd_reset (struct poller_fd *self)
{
	if (self->index != -1)
		poller_remove_at_index (self->poller, self->index);
}

#endif // LIBERTY_WANT_POLLER

// --- libuv-style write adaptor -----------------------------------------------

// Makes it possible to use iovec to write multiple data chunks at once.

typedef struct write_req write_req_t;
struct write_req
{
	LIST_HEADER (write_req_t)
	struct iovec data;                  ///< Data to be written
};

typedef struct write_queue write_queue_t;
struct write_queue
{
	write_req_t *head;                  ///< The head of the queue
	write_req_t *tail;                  ///< The tail of the queue
	size_t head_offset;                 ///< Offset into the head
	size_t len;
};

static void
write_queue_init (struct write_queue *self)
{
	self->head = self->tail = NULL;
	self->head_offset = 0;
	self->len = 0;
}

static void
write_queue_free (struct write_queue *self)
{
	for (write_req_t *iter = self->head, *next; iter; iter = next)
	{
		next = iter->next;
		free (iter->data.iov_base);
		free (iter);
	}
}

static void
write_queue_add (struct write_queue *self, write_req_t *req)
{
	LIST_APPEND_WITH_TAIL (self->head, self->tail, req);
	self->len++;
}

static void
write_queue_processed (struct write_queue *self, size_t len)
{
	while (self->head
		&& self->head_offset + len >= self->head->data.iov_len)
	{
		write_req_t *head = self->head;
		len -= (head->data.iov_len - self->head_offset);
		self->head_offset = 0;

		LIST_UNLINK_WITH_TAIL (self->head, self->tail, head);
		self->len--;
		free (head->data.iov_base);
		free (head);
	}
	self->head_offset += len;
}

static bool
write_queue_is_empty (struct write_queue *self)
{
	return self->head == NULL;
}

// --- Message reader ----------------------------------------------------------

struct msg_reader
{
	struct str buf;                     ///< Input buffer
	uint64_t offset;                    ///< Current offset in the buffer
};

static void
msg_reader_init (struct msg_reader *self)
{
	str_init (&self->buf);
	self->offset = 0;
}

static void
msg_reader_free (struct msg_reader *self)
{
	str_free (&self->buf);
}

static void
msg_reader_compact (struct msg_reader *self)
{
	str_remove_slice (&self->buf, 0, self->offset);
	self->offset = 0;
}

static void
msg_reader_feed (struct msg_reader *self, const void *data, size_t len)
{
	// TODO: have some mechanism to prevent flooding
	msg_reader_compact (self);
	str_append_data (&self->buf, data, len);
}

static void *
msg_reader_get (struct msg_reader *self, size_t *len)
{
	// Try to read in the length of the message
	if (self->offset + sizeof (uint64_t) > self->buf.len)
		return NULL;

	uint8_t *x = (uint8_t *) self->buf.str + self->offset;
	uint64_t msg_len
		= (uint64_t) x[0] << 56 | (uint64_t) x[1] << 48
		| (uint64_t) x[2] << 40 | (uint64_t) x[3] << 32
		| (uint64_t) x[4] << 24 | (uint64_t) x[5] << 16
		| (uint64_t) x[6] << 8  | (uint64_t) x[7];

	if (msg_len < sizeof msg_len)
	{
		// The message is shorter than its header
		// TODO: have some mechanism to report errors
		return NULL;
	}

	if (self->offset + msg_len < self->offset)
	{
		// Trying to read an insane amount of data but whatever
		msg_reader_compact (self);
		return NULL;
	}

	// Check if we've got the full message in the buffer and return it
	if (self->offset + msg_len > self->buf.len)
		return NULL;

	// We have to subtract the header from the reported length
	void *data = self->buf.str + self->offset + sizeof msg_len;
	self->offset += msg_len;
	*len = msg_len - sizeof msg_len;
	return data;
}

// --- Message unpacker --------------------------------------------------------

struct msg_unpacker
{
	const char *data;
	size_t offset;
	size_t len;
};

static void
msg_unpacker_init (struct msg_unpacker *self, const void *data, size_t len)
{
	self->data = data;
	self->len = len;
	self->offset = 0;
}

static size_t
msg_unpacker_get_available (struct msg_unpacker *self)
{
	return self->len - self->offset;
}

#define UNPACKER_INT_BEGIN                                                     \
	if (self->len - self->offset < sizeof *value)                              \
		return false;                                                          \
	uint8_t *x = (uint8_t *) self->data + self->offset;                        \
	self->offset += sizeof *value;

static bool
msg_unpacker_u8 (struct msg_unpacker *self, uint8_t *value)
{
	UNPACKER_INT_BEGIN
	*value = x[0];
	return true;
}

static bool
msg_unpacker_u16 (struct msg_unpacker *self, uint16_t *value)
{
	UNPACKER_INT_BEGIN
	*value
		= (uint16_t) x[0] << 8  | (uint16_t) x[1];
	return true;
}

static bool
msg_unpacker_u32 (struct msg_unpacker *self, uint32_t *value)
{
	UNPACKER_INT_BEGIN
	*value
		= (uint32_t) x[0] << 24 | (uint32_t) x[1] << 16
		| (uint32_t) x[2] << 8  | (uint32_t) x[3];
	return true;
}

static bool
msg_unpacker_u64 (struct msg_unpacker *self, uint64_t *value)
{
	UNPACKER_INT_BEGIN
	*value
		= (uint64_t) x[0] << 56 | (uint64_t) x[1] << 48
		| (uint64_t) x[2] << 40 | (uint64_t) x[3] << 32
		| (uint64_t) x[4] << 24 | (uint64_t) x[5] << 16
		| (uint64_t) x[6] << 8  | (uint64_t) x[7];
	return true;
}

#define msg_unpacker_i8(self, value)                                           \
	msg_unpacker_u8  ((self), (uint8_t *) (value))
#define msg_unpacker_i16(self, value)                                          \
	msg_unpacker_u16 ((self), (uint16_t *) (value))
#define msg_unpacker_i32(self, value)                                          \
	msg_unpacker_u32 ((self), (uint32_t *) (value))
#define msg_unpacker_i64(self, value)                                          \
	msg_unpacker_u64 ((self), (uint64_t *) (value))

#undef UNPACKER_INT_BEGIN

// --- Message packer and writer -----------------------------------------------

// Use str_pack_*() or other methods to append to the internal buffer, then
// flush it to get a nice frame.  Handy for iovec.

struct msg_writer
{
	struct str buf;                     ///< Holds the message data
};

static void
msg_writer_init (struct msg_writer *self)
{
	str_init (&self->buf);
	// Placeholder for message length
	str_append_data (&self->buf, "\x00\x00\x00\x00" "\x00\x00\x00\x00", 8);
}

static void *
msg_writer_flush (struct msg_writer *self, size_t *len)
{
	// Update the message length
	uint64_t x = self->buf.len;
	uint8_t tmp[8] =
		{ x >> 56, x >> 48, x >> 40, x >> 32, x >> 24, x >> 16, x >> 8, x };
	memcpy (self->buf.str, tmp, sizeof tmp);

	*len = x;
	return str_steal (&self->buf);
}

// --- ASCII -------------------------------------------------------------------

#define TRIVIAL_STRXFRM(name, fn)                                              \
	static size_t                                                              \
	name (char *dest, const char *src, size_t n)                               \
	{                                                                          \
		size_t len = strlen (src);                                             \
		while (n-- && (*dest++ = (fn) (*src++)))                               \
			;                                                                  \
		return len;                                                            \
	}

static int
tolower_ascii (int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int
toupper_ascii (int c)
{
	return c >= 'A' && c <= 'Z' ? c : c - ('a' - 'A');
}

TRIVIAL_STRXFRM (tolower_ascii_strxfrm, tolower_ascii)
TRIVIAL_STRXFRM (toupper_ascii_strxfrm, toupper_ascii)

static int
strcasecmp_ascii (const char *a, const char *b)
{
	int x;
	while (*a || *b)
		if ((x = tolower_ascii (*(const unsigned char *) a++)
			- tolower_ascii (*(const unsigned char *) b++)))
			return x;
	return 0;
}

static int
strncasecmp_ascii (const char *a, const char *b, size_t n)
{
	int x;
	while (n-- && (*a || *b))
		if ((x = tolower_ascii (*(const unsigned char *) a++)
			- tolower_ascii (*(const unsigned char *) b++)))
			return x;
	return 0;
}

static bool
isalpha_ascii (int c)
{
	c &= ~32;
	return c >= 'A' && c <= 'Z';
}

static bool
isdigit_ascii (int c)
{
	return c >= '0' && c <= '9';
}

static bool
isalnum_ascii (int c)
{
	return isalpha_ascii (c) || isdigit_ascii (c);
}

static bool
isspace_ascii (int c)
{
	return c == ' '  || c == '\f' || c == '\n'
		|| c == '\r' || c == '\t' || c == '\v';
}

// --- UTF-8 -------------------------------------------------------------------

/// Return a pointer to the next UTF-8 character, or NULL on error
static const char *
utf8_next (const char *s, size_t len, int32_t *codepoint)
{
	// End of string, we go no further
	if (!len)
		return NULL;

	// In the middle of a character -> error
	const uint8_t *p = (const unsigned char *) s;
	if ((*p & 0xC0) == 0x80)
		return NULL;

	// Find out how long the sequence is
	unsigned mask = 0xC0;
	unsigned tail_len = 0;
	while ((*p & mask) == mask)
	{
		// Invalid start of sequence
		if (mask == 0xFE)
			return NULL;

		mask |= mask >> 1;
		tail_len++;
	}

	// Check the rest of the sequence
	if (tail_len > --len)
		return NULL;

	uint32_t cp = *p++ & ~mask;
	while (tail_len--)
	{
		if ((*p & 0xC0) != 0x80)
			return NULL;
		cp = cp << 6 | (*p++ & 0x3F);
	}
	if (codepoint)
		*codepoint = cp;
	return (const char *) p;
}

/// Very rough UTF-8 validation, just makes sure codepoints can be iterated
static bool
utf8_validate (const char *s, size_t len)
{
	const char *next;
	while (len)
	{
		int32_t codepoint;
		// TODO: better validations
		if (!(next = utf8_next (s, len, &codepoint))
		 || codepoint > 0x10FFFF)
			return false;

		len -= next - s;
		s = next;
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct utf8_iter
{
	const char *s;                      ///< String iterator
	size_t len;                         ///< How many bytes remain
};

static void
utf8_iter_init (struct utf8_iter *self, const char *s)
{
	self->len = strlen ((self->s = s));
}

static int32_t
utf8_iter_next (struct utf8_iter *self, size_t *len)
{
	if (!self->len)
		return -1;

	const char *old = self->s;
	int32_t codepoint;
	if (!soft_assert ((self->s = utf8_next (old, self->len, &codepoint))))
	{
		// Invalid UTF-8
		self->len = 0;
		return -1;
	}

	size_t advance = self->s - old;
	self->len -= advance;
	if (len) *len = advance;
	return codepoint;
}

// --- Base 64 -----------------------------------------------------------------

static uint8_t g_base64_table[256] =
{
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 62, 64, 64, 64, 63,
	52, 53, 54, 55, 56, 57, 58, 59,  60, 61, 64, 64, 64,  0, 64, 64,
	64,  0,  1,  2,  3,  4,  5,  6,   7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22,  23, 24, 25, 64, 64, 64, 64, 64,
	64, 26, 27, 28, 29, 30, 31, 32,  33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48,  49, 50, 51, 64, 64, 64, 64, 64,

	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64,  64, 64, 64, 64, 64, 64, 64, 64,
};

static inline bool
base64_decode_group (const char **s, bool ignore_ws, struct str *output)
{
	uint8_t input[4];
	size_t loaded = 0;
	for (; loaded < 4; (*s)++)
	{
		if (!**s)
			return loaded == 0;
		if (!ignore_ws || !isspace_ascii (**s))
			input[loaded++] = **s;
	}

	size_t len = 3;
	if (input[0] == '=' || input[1] == '=')
		return false;
	if (input[2] == '=' && input[3] != '=')
		return false;
	if (input[2] == '=')
		len--;
	if (input[3] == '=')
		len--;

	uint8_t a = g_base64_table[input[0]];
	uint8_t b = g_base64_table[input[1]];
	uint8_t c = g_base64_table[input[2]];
	uint8_t d = g_base64_table[input[3]];

	if (((a | b) | (c | d)) & 0x40)
		return false;

	uint32_t block = a << 18 | b << 12 | c << 6 | d;
	switch (len)
	{
	case 1:
		str_append_c (output, block >> 16);
		break;
	case 2:
		str_append_c (output, block >> 16);
		str_append_c (output, block >> 8);
		break;
	case 3:
		str_append_c (output, block >> 16);
		str_append_c (output, block >> 8);
		str_append_c (output, block);
	}
	return true;
}

static bool
base64_decode (const char *s, bool ignore_ws, struct str *output)
{
	while (*s)
		if (!base64_decode_group (&s, ignore_ws, output))
			return false;
	return true;
}

static void
base64_encode (const void *data, size_t len, struct str *output)
{
	const char *alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	const uint8_t *p = data;
	size_t n_groups = len / 3;
	size_t tail = len - n_groups * 3;
	uint32_t group;

	for (; n_groups--; p += 3)
	{
		group = p[0] << 16 | p[1] << 8 | p[2];
		str_append_c (output, alphabet[(group >> 18) & 63]);
		str_append_c (output, alphabet[(group >> 12) & 63]);
		str_append_c (output, alphabet[(group >>  6) & 63]);
		str_append_c (output, alphabet[ group        & 63]);
	}

	switch (tail)
	{
	case 2:
		group = p[0] << 16 | p[1] << 8;
		str_append_c (output, alphabet[(group >> 18) & 63]);
		str_append_c (output, alphabet[(group >> 12) & 63]);
		str_append_c (output, alphabet[(group >>  6) & 63]);
		str_append_c (output, '=');
		break;
	case 1:
		group = p[0] << 16;
		str_append_c (output, alphabet[(group >> 18) & 63]);
		str_append_c (output, alphabet[(group >> 12) & 63]);
		str_append_c (output, '=');
		str_append_c (output, '=');
	default:
		break;
	}
}

// --- Utilities ---------------------------------------------------------------

static void
cstr_split_ignore_empty (const char *s, char delimiter, struct str_vector *out)
{
	const char *begin = s, *end;

	while ((end = strchr (begin, delimiter)))
	{
		if (begin != end)
			str_vector_add_owned (out, xstrndup (begin, end - begin));
		begin = ++end;
	}

	if (*begin)
		str_vector_add (out, begin);
}

static char *
cstr_strip_in_place (char *s, const char *stripped_chars)
{
	char *end = s + strlen (s);
	while (end > s && strchr (stripped_chars, end[-1]))
		*--end = '\0';

	char *start = s + strspn (s, stripped_chars);
	if (start > s)
		memmove (s, start, end - start + 1);
	return s;
}

static void
cstr_transform (char *s, int (*tolower) (int c))
{
	for (; *s; s++)
		*s = tolower (*s);
}

static char *
cstr_cut_until (const char *s, const char *alphabet)
{
	return xstrndup (s, strcspn (s, alphabet));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
join_str_vector (const struct str_vector *v, char delimiter)
{
	if (!v->len)
		return xstrdup ("");

	struct str result;
	str_init (&result);
	str_append (&result, v->vector[0]);
	for (size_t i = 1; i < v->len; i++)
		str_append_printf (&result, "%c%s", delimiter, v->vector[i]);
	return str_steal (&result);
}

static char *xstrdup_printf (const char *, ...) ATTRIBUTE_PRINTF (1, 2);

static char *
xstrdup_printf (const char *format, ...)
{
	va_list ap;
	struct str tmp;
	str_init (&tmp);
	va_start (ap, format);
	str_append_vprintf (&tmp, format, ap);
	va_end (ap);
	return str_steal (&tmp);
}

static char *
iconv_xstrdup (iconv_t conv, char *in, size_t in_len, size_t *out_len)
{
	char *buf, *buf_ptr;
	size_t out_left, buf_alloc;

	buf = buf_ptr = xmalloc (out_left = buf_alloc = 64);

	char *in_ptr = in;
	if (in_len == (size_t) -1)
		in_len = strlen (in) + 1;

	while (iconv (conv, (char **) &in_ptr, &in_len,
		(char **) &buf_ptr, &out_left) == (size_t) -1)
	{
		if (errno != E2BIG)
		{
			free (buf);
			return NULL;
		}
		out_left += buf_alloc;
		char *new_buf = xrealloc (buf, buf_alloc <<= 1);
		buf_ptr += new_buf - buf;
		buf = new_buf;
	}
	if (out_len)
		*out_len = buf_alloc - out_left;
	return buf;
}

static bool
set_boolean_if_valid (bool *out, const char *s)
{
	if      (!strcasecmp (s, "yes"))    *out = true;
	else if (!strcasecmp (s, "no"))     *out = false;
	else if (!strcasecmp (s, "on"))     *out = true;
	else if (!strcasecmp (s, "off"))    *out = false;
	else if (!strcasecmp (s, "true"))   *out = true;
	else if (!strcasecmp (s, "false"))  *out = false;
	else return false;

	return true;
}

static bool
xstrtoul (unsigned long *out, const char *s, int base)
{
	char *end;
	errno = 0;
	*out = strtoul (s, &end, base);
	return errno == 0 && !*end && end != s;
}

static bool
read_line (FILE *fp, struct str *s)
{
	int c;
	bool at_end = true;

	str_reset (s);
	while ((c = fgetc (fp)) != EOF)
	{
		at_end = false;
		if (c == '\r')
			continue;
		if (c == '\n')
			break;
		str_append_c (s, c);
	}

	return !at_end;
}

static char *
format_host_port_pair (const char *host, const char *port)
{
	// For when binding to the NULL address; would an asterisk be better?
	if (!host)
		host = "";

	// IPv6 addresses mess with the "colon notation"; let's go with RFC 2732
	if (strchr (host, ':'))
		return xstrdup_printf ("[%s]:%s", host, port);
	return xstrdup_printf ("%s:%s", host, port);
}

// --- File system -------------------------------------------------------------

static bool
ensure_directory_existence (const char *path, struct error **e)
{
	struct stat st;

	if (stat (path, &st))
	{
		if (mkdir (path, S_IRWXU | S_IRWXG | S_IRWXO))
		{
			error_set (e, "cannot create directory `%s': %s",
				path, strerror (errno));
			return false;
		}
	}
	else if (!S_ISDIR (st.st_mode))
	{
		error_set (e, "cannot create directory `%s': %s",
			path, "file exists but is not a directory");
		return false;
	}
	return true;
}

static bool
mkdir_with_parents (char *path, struct error **e)
{
	char *p = path;

	// XXX: This is prone to the TOCTTOU problem.  The solution would be to
	//   rewrite the function using the {mkdir,fstat}at() functions from
	//   POSIX.1-2008, ideally returning a file descriptor to the open
	//   directory, with the current code as a fallback.  Or to use chdir().
	while ((p = strchr (p + 1, '/')))
	{
		*p = '\0';
		bool success = ensure_directory_existence (path, e);
		*p = '/';

		if (!success)
			return false;
	}

	return ensure_directory_existence (path, e);
}

static bool
str_append_env_path (struct str *output, const char *var, bool only_absolute)
{
	const char *value = getenv (var);

	if (!value || (only_absolute && *value != '/'))
		return false;

	str_append (output, value);
	return true;
}

static void
get_xdg_home_dir (struct str *output, const char *var, const char *def)
{
	str_reset (output);
	if (!str_append_env_path (output, var, true))
	{
		str_append_env_path (output, "HOME", false);
		str_append_c (output, '/');
		str_append (output, def);
	}
}

static char *
resolve_relative_filename_generic
	(struct str_vector *paths, const char *tail, const char *filename)
{
	struct str file;
	str_init (&file);

	char *result = NULL;
	for (unsigned i = 0; i < paths->len; i++)
	{
		// As per XDG spec, relative paths are ignored
		if (*paths->vector[i] != '/')
			continue;

		str_reset (&file);
		str_append_printf (&file, "%s/%s%s", paths->vector[i], tail, filename);

		struct stat st;
		if (!stat (file.str, &st))
		{
			result = str_steal (&file);
			break;
		}
	}

	str_free (&file);
	return result;
}

static void
get_xdg_config_dirs (struct str_vector *out)
{
	struct str config_home;
	str_init (&config_home);
	get_xdg_home_dir (&config_home, "XDG_CONFIG_HOME", ".config");
	str_vector_add (out, config_home.str);
	str_free (&config_home);

	const char *xdg_config_dirs;
	if (!(xdg_config_dirs = getenv ("XDG_CONFIG_DIRS")))
		xdg_config_dirs = "/etc/xdg";
	cstr_split_ignore_empty (xdg_config_dirs, ':', out);
}

static char *
resolve_relative_config_filename (const char *filename)
{
	struct str_vector paths;
	str_vector_init (&paths);
	get_xdg_config_dirs (&paths);
	char *result = resolve_relative_filename_generic
		(&paths, PROGRAM_NAME "/", filename);
	str_vector_free (&paths);
	return result;
}

static void
get_xdg_data_dirs (struct str_vector *out)
{
	struct str data_home;
	str_init (&data_home);
	get_xdg_home_dir (&data_home, "XDG_DATA_HOME", ".local/share");
	str_vector_add (out, data_home.str);
	str_free (&data_home);

	const char *xdg_data_dirs;
	if (!(xdg_data_dirs = getenv ("XDG_DATA_DIRS")))
		xdg_data_dirs = "/usr/local/share/:/usr/share/";
	cstr_split_ignore_empty (xdg_data_dirs, ':', out);
}

static char *
resolve_relative_data_filename (const char *filename)
{
	struct str_vector paths;
	str_vector_init (&paths);
	get_xdg_data_dirs (&paths);
	char *result = resolve_relative_filename_generic
		(&paths, PROGRAM_NAME "/", filename);
	str_vector_free (&paths);
	return result;
}

static char *
resolve_relative_runtime_filename (const char *filename)
{
	struct str path;
	str_init (&path);

	const char *runtime_dir = getenv ("XDG_RUNTIME_DIR");
	if (runtime_dir && *runtime_dir == '/')
		str_append (&path, runtime_dir);
	else
		get_xdg_home_dir (&path, "XDG_DATA_HOME", ".local/share");
	str_append_printf (&path, "/%s/%s", PROGRAM_NAME, filename);

	// Try to create the file's ancestors;
	// typically the user will want to immediately create a file in there
	const char *last_slash = strrchr (path.str, '/');
	if (last_slash && last_slash != path.str)
	{
		char *copy = xstrndup (path.str, last_slash - path.str);
		(void) mkdir_with_parents (copy, NULL);
		free (copy);
	}
	return str_steal (&path);
}

static char *
try_expand_tilde (const char *filename)
{
	size_t until_slash = strcspn (filename, "/");
	if (!until_slash)
	{
		struct str expanded;
		str_init (&expanded);
		str_append_env_path (&expanded, "HOME", false);
		str_append (&expanded, filename);
		return str_steal (&expanded);
	}

	int buf_len = sysconf (_SC_GETPW_R_SIZE_MAX);
	if (buf_len < 0)
		buf_len = 1024;
	struct passwd pwd, *success = NULL;

	char *user = xstrndup (filename, until_slash);
	char *buf = xmalloc (buf_len);
	while (getpwnam_r (user, &pwd, buf, buf_len, &success) == ERANGE)
		buf = xrealloc (buf, buf_len <<= 1);
	free (user);

	char *result = NULL;
	if (success)
		result = xstrdup_printf ("%s%s", pwd.pw_dir, filename + until_slash);
	free (buf);
	return result;
}

static char *
resolve_filename (const char *filename, char *(*relative_cb) (const char *))
{
	// Absolute path is absolute
	if (*filename == '/')
		return xstrdup (filename);

	// We don't want to use wordexp() for this as it may execute /bin/sh
	if (*filename == '~')
	{
		// Paths to home directories ought to be absolute
		char *expanded = try_expand_tilde (filename + 1);
		if (expanded)
			return expanded;
		print_debug ("failed to expand the home directory in `%s'", filename);
	}
	return relative_cb (filename);
}

// --- OpenSSL -----------------------------------------------------------------

#ifdef LIBERTY_WANT_SSL

#define XSSL_ERROR_TRY_AGAIN INT_MAX

/// A small wrapper around SSL_get_error() to simplify further handling
static int
xssl_get_error (SSL *ssl, int result, const char **error_info)
{
	int error = SSL_get_error (ssl, result);
	switch (error)
	{
	case SSL_ERROR_NONE:
	case SSL_ERROR_ZERO_RETURN:
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		return error;
	case SSL_ERROR_SYSCALL:
		if ((error = ERR_get_error ()))
			*error_info = ERR_reason_error_string (error);
		else if (result == 0)
			// An EOF that's not according to the protocol is still an EOF
			return SSL_ERROR_ZERO_RETURN;
		else
		{
			if (errno == EINTR)
				return XSSL_ERROR_TRY_AGAIN;
			*error_info = strerror (errno);
		}
		return SSL_ERROR_SSL;
	default:
		if ((error = ERR_get_error ()))
			*error_info = ERR_reason_error_string (error);
		else
			*error_info = "unknown error";
		return SSL_ERROR_SSL;
	}
}

#endif  // LIBERTY_WANT_SSL

// --- Regular expressions -----------------------------------------------------

static regex_t *
regex_compile (const char *regex, int flags, struct error **e)
{
	regex_t *re = xmalloc (sizeof *re);
	int err = regcomp (re, regex, flags);
	if (!err)
		return re;

	char buf[regerror (err, re, NULL, 0)];
	regerror (err, re, buf, sizeof buf);

	free (re);
	error_set (e, "%s: %s", "failed to compile regular expression", buf);
	return NULL;
}

static void
regex_free (void *regex)
{
	regfree (regex);
	free (regex);
}

// The cost of hashing a string is likely to be significantly smaller than that
// of compiling the whole regular expression anew, so here is a simple cache.
// Adding basic support for subgroups is easy: check `re_nsub' and output into
// a `struct str_vector' (if all we want is the substrings).

static void
regex_cache_init (struct str_map *cache)
{
	str_map_init (cache);
	cache->free = regex_free;
}

static bool
regex_cache_match (struct str_map *cache, const char *regex, int flags,
	const char *s, struct error **e)
{
	regex_t *re = str_map_find (cache, regex);
	if (!re)
	{
		re = regex_compile (regex, flags, e);
		if (!re)
			return false;
		str_map_set (cache, regex, re);
	}
	return regexec (re, s, 0, NULL, 0) != REG_NOMATCH;
}

// --- Simple configuration ----------------------------------------------------

// The keys are stripped of surrounding whitespace, the values are not.

struct simple_config_item
{
	const char *key;
	const char *default_value;
	const char *description;
};

static void
simple_config_load_defaults
	(struct str_map *config, const struct simple_config_item *table)
{
	for (; table->key != NULL; table++)
		if (table->default_value)
			str_map_set (config, table->key, xstrdup (table->default_value));
		else
			str_map_set (config, table->key, NULL);
}

static bool
simple_config_update_from_file (struct str_map *config, struct error **e)
{
	char *filename = resolve_filename
		(PROGRAM_NAME ".conf", resolve_relative_config_filename);
	if (!filename)
		return true;

	FILE *fp = fopen (filename, "r");
	if (!fp)
	{
		error_set (e, "could not open `%s' for reading: %s",
			filename, strerror (errno));
		free (filename);
		return false;
	}

	struct str line;
	str_init (&line);

	bool errors = false;
	for (unsigned line_no = 1; read_line (fp, &line); line_no++)
	{
		char *start = line.str;
		if (*start == '#')
			continue;

		while (isspace (*start))
			start++;

		char *end = strchr (start, '=');
		if (end)
		{
			char *value = end + 1;
			do
				*end = '\0';
			while (isspace (*--end));

			str_map_set (config, start, xstrdup (value));
		}
		else if (*start)
		{
			error_set (e, "line %u in config: %s", line_no, "malformed input");
			errors = true;
			break;
		}
	}

	str_free (&line);
	fclose (fp);
	free (filename);
	return !errors;
}

static char *
simple_config_write_default (const char *filename, const char *prolog,
	const struct simple_config_item *table, struct error **e)
{
	struct str path, base;

	str_init (&path);
	str_init (&base);

	if (filename)
	{
		char *tmp = xstrdup (filename);
		str_append (&path, dirname (tmp));
		strcpy (tmp, filename);
		str_append (&base, basename (tmp));
		free (tmp);
	}
	else
	{
		get_xdg_home_dir (&path, "XDG_CONFIG_HOME", ".config");
		str_append (&path, "/" PROGRAM_NAME);
		str_append (&base, PROGRAM_NAME ".conf");
	}

	if (!mkdir_with_parents (path.str, e))
		goto error;

	str_append_c (&path, '/');
	str_append_str (&path, &base);

	FILE *fp = fopen (path.str, "w");
	if (!fp)
	{
		error_set (e, "could not open `%s' for writing: %s",
			path.str, strerror (errno));
		goto error;
	}

	if (prolog)
		fputs (prolog, fp);

	errno = 0;
	for (; table->key != NULL; table++)
	{
		fprintf (fp, "# %s\n", table->description);
		if (table->default_value)
			fprintf (fp, "%s=%s\n", table->key, table->default_value);
		else
			fprintf (fp, "#%s=\n", table->key);
	}
	fclose (fp);
	if (errno)
	{
		error_set (e, "writing to `%s' failed: %s", path.str, strerror (errno));
		goto error;
	}

	str_free (&base);
	return str_steal (&path);

error:
	str_free (&base);
	str_free (&path);
	return NULL;
}

/// Convenience wrapper suitable for most simple applications
static void
call_simple_config_write_default
	(const char *hint, const struct simple_config_item *table)
{
	static const char *prolog =
	"# " PROGRAM_NAME " " PROGRAM_VERSION " configuration file\n"
	"#\n"
	"# Relative paths are searched for in ${XDG_CONFIG_HOME:-~/.config}\n"
	"# /" PROGRAM_NAME " as well as in $XDG_CONFIG_DIRS/" PROGRAM_NAME "\n"
	"\n";

	struct error *e = NULL;
	char *filename = simple_config_write_default (hint, prolog, table, &e);
	if (!filename)
	{
		print_error ("%s", e->message);
		error_free (e);
		exit (EXIT_FAILURE);
	}
	print_status ("configuration written to `%s'", filename);
	free (filename);
}

// --- Option handler ----------------------------------------------------------

// Simple wrapper for the getopt_long API to make it easier to use and maintain.

#define OPT_USAGE_ALIGNMENT_COLUMN 30   ///< Alignment for option descriptions

enum
{
	OPT_OPTIONAL_ARG  = (1 << 0),       ///< The argument is optional
	OPT_LONG_ONLY     = (1 << 1)        ///< Ignore the short name in opt_string
};

// All options need to have both a short name, and a long name.  The short name
// is what is returned from opt_handler_get().  It is possible to define a value
// completely out of the character range combined with the OPT_LONG_ONLY flag.
//
// When `arg_hint' is defined, the option is assumed to have an argument.

struct opt
{
	int short_name;                     ///< The single-letter name
	const char *long_name;              ///< The long name
	const char *arg_hint;               ///< Option argument hint
	int flags;                          ///< Option flags
	const char *description;            ///< Option description
};

struct opt_handler
{
	int argc;                           ///< The number of program arguments
	char **argv;                        ///< Program arguments

	const char *arg_hint;               ///< Program arguments hint
	const char *description;            ///< Description of the program

	const struct opt *opts;             ///< The list of options
	size_t opts_len;                    ///< The length of the option array

	struct option *options;             ///< The list of options for getopt
	char *opt_string;                   ///< The `optstring' for getopt
};

static void
opt_handler_free (struct opt_handler *self)
{
	free (self->options);
	free (self->opt_string);
}

static void
opt_handler_init (struct opt_handler *self, int argc, char **argv,
	const struct opt *opts, const char *arg_hint, const char *description)
{
	memset (self, 0, sizeof *self);
	self->argc = argc;
	self->argv = argv;
	self->arg_hint = arg_hint;
	self->description = description;

	size_t len = 0;
	for (const struct opt *iter = opts; iter->long_name; iter++)
		len++;

	self->opts = opts;
	self->opts_len = len;
	self->options = xcalloc (len + 1, sizeof *self->options);

	struct str opt_string;
	str_init (&opt_string);

	for (size_t i = 0; i < len; i++)
	{
		const struct opt *opt = opts + i;
		struct option *mapped = self->options + i;

		mapped->name = opt->long_name;
		if (!opt->arg_hint)
			mapped->has_arg = no_argument;
		else if (opt->flags & OPT_OPTIONAL_ARG)
			mapped->has_arg = optional_argument;
		else
			mapped->has_arg = required_argument;
		mapped->val = opt->short_name;

		if (opt->flags & OPT_LONG_ONLY)
			continue;

		str_append_c (&opt_string, opt->short_name);
		if (opt->arg_hint)
		{
			str_append_c (&opt_string, ':');
			if (opt->flags & OPT_OPTIONAL_ARG)
				str_append_c (&opt_string, ':');
		}
	}

	self->opt_string = str_steal (&opt_string);
}

static void
opt_handler_usage (struct opt_handler *self, FILE *stream)
{
	struct str usage;
	str_init (&usage);

	str_append_printf (&usage, "Usage: %s [OPTION]... %s\n",
		self->argv[0], self->arg_hint ? self->arg_hint : "");
	str_append_printf (&usage, "%s\n\n", self->description);

	for (size_t i = 0; i < self->opts_len; i++)
	{
		struct str row;
		str_init (&row);

		const struct opt *opt = self->opts + i;
		if (!(opt->flags & OPT_LONG_ONLY))
			str_append_printf (&row, "  -%c, ", opt->short_name);
		else
			str_append (&row, "      ");
		str_append_printf (&row, "--%s", opt->long_name);
		if (opt->arg_hint)
			str_append_printf (&row, (opt->flags & OPT_OPTIONAL_ARG)
				? " [%s]" : " %s", opt->arg_hint);

		// TODO: keep the indent if there are multiple lines
		if (row.len + 2 <= OPT_USAGE_ALIGNMENT_COLUMN)
		{
			str_append (&row, "  ");
			str_append_printf (&usage, "%-*s%s\n",
				OPT_USAGE_ALIGNMENT_COLUMN, row.str, opt->description);
		}
		else
			str_append_printf (&usage, "%s\n%-*s%s\n", row.str,
				OPT_USAGE_ALIGNMENT_COLUMN, "", opt->description);

		str_free (&row);
	}

	fputs (usage.str, stream);
	str_free (&usage);
}

static int
opt_handler_get (struct opt_handler *self)
{
	return getopt_long (self->argc, self->argv,
		self->opt_string, self->options, NULL);
}

// --- Unit tests --------------------------------------------------------------

// This is modeled after GTest, only remarkably simpler.

typedef void (*test_fn) (const void *data, void *fixture);

struct test_unit
{
	LIST_HEADER (struct test_unit)

	char *name;                         ///< Name of the test
	size_t fixture_size;                ///< Fixture size
	const void *user_data;              ///< User data

	test_fn setup;                      ///< Fixture setup callback
	test_fn test;                       ///< The test
	test_fn teardown;                   ///< Fixture teardown callback
};

struct test
{
	struct test_unit *tests;            ///< List of tests
	struct test_unit *tests_tail;       ///< End of the list of tests

	struct str_map whitelist;           ///< Whitelisted tests
	struct str_map blacklist;           ///< Blacklisted tests

	unsigned list_only : 1;             ///< Just list all tests
};

static void
test_init (struct test *self, int argc, char **argv)
{
	memset (self, 0, sizeof *self);
	str_map_init (&self->whitelist);
	str_map_init (&self->blacklist);

	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'p', "pass", "NAME", 0, "only run tests glob-matching the name" },
		{ 's', "skip", "NAME", 0, "skip all tests glob-matching the name" },
		{ 'l', "list", NULL, 0, "list all available tests" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh;
	opt_handler_init (&oh, argc, argv, opts, NULL, "Unit test runner");

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'd':
		g_debug_mode = true;
		break;
	case 'h':
		opt_handler_usage (&oh, stdout);
		exit (EXIT_SUCCESS);
	case 'p':
		str_map_set (&self->whitelist, optarg, (void *) 1);
		break;
	case 's':
		str_map_set (&self->blacklist, optarg, (void *) 1);
		break;
	case 'l':
		self->list_only = true;
		break;
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	if (argc)
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	opt_handler_free (&oh);
}

static void
test_add_internal (struct test *self, const char *name, size_t fixture_size,
	const void *user_data, test_fn setup, test_fn test, test_fn teardown)
{
	hard_assert (test != NULL);
	hard_assert (name != NULL);

	struct test_unit *unit = xcalloc (1, sizeof *unit);
	unit->name = xstrdup (name);
	unit->fixture_size = fixture_size;
	unit->user_data = user_data;

	unit->setup = setup;
	unit->test = test;
	unit->teardown = teardown;

	LIST_APPEND_WITH_TAIL (self->tests, self->tests_tail, unit);
}

#define test_add(self, name, fixture_type, user_data, setup, test, teardown)   \
	test_add_internal ((self), (name), sizeof (fixture_type), (user_data),     \
		(test_fn) (setup), (test_fn) (test), (test_fn) (teardown))

#define test_add_simple(self, name, user_data, test)                           \
	test_add_internal ((self), (name), 0, (user_data),                         \
		NULL, (test_fn) (test), NULL)

static bool
str_map_glob_match (struct str_map *self, const char *entry)
{
	struct str_map_iter iter;
	str_map_iter_init (&iter, self);
	while (str_map_iter_next (&iter))
		if (!fnmatch (iter.link->key, entry, 0))
			return true;
	return false;
}

static bool
test_is_allowed (struct test *self, const char *name)
{
	bool allowed = true;
	if (self->whitelist.len)
		allowed  =  str_map_glob_match (&self->whitelist, name);
	if (self->blacklist.len)
		allowed &= !str_map_glob_match (&self->blacklist, name);
	return allowed;
}

static int
test_run (struct test *self)
{
	g_soft_asserts_are_deadly = true;
	LIST_FOR_EACH (struct test_unit, iter, self->tests)
	{
		if (!test_is_allowed (self, iter->name))
			continue;

		if (self->list_only)
		{
			printf ("%s\n", iter->name);
			continue;
		}

		void *fixture = xcalloc (1, iter->fixture_size);
		if (iter->setup)
			iter->setup (iter->user_data, fixture);

		fprintf (stderr, "%s: ", iter->name);
		iter->test (iter->user_data, fixture);
		fprintf (stderr, "OK\n");

		if (iter->teardown)
			iter->teardown (iter->user_data, fixture);
		free (fixture);
	}

	LIST_FOR_EACH (struct test_unit, iter, self->tests)
	{
		free (iter->name);
		free (iter);
	}

	str_map_free (&self->whitelist);
	str_map_free (&self->blacklist);
	return 0;
}

// --- Connector ---------------------------------------------------------------

#ifdef LIBERTY_WANT_POLLER

// This is a helper that tries to establish a connection with any address on
// a given list.  Sadly it also introduces a bit of a callback hell.

struct connector_target
{
	LIST_HEADER (struct connector_target)

	char *hostname;                     ///< Target hostname or address
	char *service;                      ///< Target service name or port

	struct addrinfo *results;           ///< Resolved target
	struct addrinfo *iter;              ///< Current endpoint
};

static struct connector_target *
connector_target_new (void)
{
	struct connector_target *self = xmalloc (sizeof *self);
	return self;
}

static void
connector_target_destroy (struct connector_target *self)
{
	free (self->hostname);
	free (self->service);
	freeaddrinfo (self->results);
	free (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct connector
{
	int socket;                         ///< Socket FD for the connection
	struct poller_fd connected_event;   ///< We've connected or failed
	struct connector_target *targets;   ///< Targets
	struct connector_target *targets_t; ///< Tail of targets

	void *user_data;                    ///< User data for callbacks

	// You may destroy the connector object in these two main callbacks:

	/// Connection has been successfully established
	void (*on_connected) (void *user_data, int socket);
	/// Failed to establish a connection to either target
	void (*on_failure) (void *user_data);

	// Optional:

	/// Connecting to a new address
	void (*on_connecting) (void *user_data, const char *address);
	/// Connecting to the last address has failed
	void (*on_error) (void *user_data, const char *error);
};

static void
connector_notify_connecting (struct connector *self,
	struct connector_target *target, struct addrinfo *gai_iter)
{
	if (!self->on_connecting)
		return;

	const char *real_host = target->hostname;

	// We don't really need this, so we can let it quietly fail
	char buf[NI_MAXHOST];
	int err = getnameinfo (gai_iter->ai_addr, gai_iter->ai_addrlen,
		buf, sizeof buf, NULL, 0, NI_NUMERICHOST);
	if (err)
		LOG_FUNC_FAILURE ("getnameinfo", gai_strerror (err));
	else
		real_host = buf;

	char *address = format_host_port_pair (real_host, target->service);
	self->on_connecting (self->user_data, address);
	free (address);
}

static void
connector_notify_error (struct connector *self, const char *error)
{
	if (self->on_error)
		self->on_error (self->user_data, error);
}

static void
connector_prepare_next (struct connector *self)
{
	struct connector_target *target = self->targets;
	if (!(target->iter = target->iter->ai_next))
	{
		LIST_UNLINK_WITH_TAIL (self->targets, self->targets_t, target);
		connector_target_destroy (target);
	}
}

static void
connector_step (struct connector *self)
{
	struct connector_target *target = self->targets;
	if (!target)
	{
		// Total failure, none of the targets has succeeded
		self->on_failure (self->user_data);
		return;
	}

	struct addrinfo *gai_iter = target->iter;
	hard_assert (gai_iter != NULL);

	connector_notify_connecting (self, target, gai_iter);

	int fd = socket (gai_iter->ai_family,
		gai_iter->ai_socktype, gai_iter->ai_protocol);
	if (fd == -1)
	{
		connector_notify_error (self, strerror (errno));

		connector_prepare_next (self);
		connector_step (self);
		return;
	}

	set_cloexec (fd);
	set_blocking (fd, false);

	int yes = 1;
	soft_assert (setsockopt (fd, SOL_SOCKET, SO_KEEPALIVE,
		&yes, sizeof yes) != -1);

	if (!connect (fd, gai_iter->ai_addr, gai_iter->ai_addrlen))
	{
		set_blocking (fd, true);
		self->on_connected (self->user_data, fd);
		return;
	}
	if (errno != EINPROGRESS)
	{
		connector_notify_error (self, strerror (errno));
		xclose (fd);

		connector_prepare_next (self);
		connector_step (self);
		return;
	}

	self->connected_event.fd = self->socket = fd;
	poller_fd_set (&self->connected_event, POLLOUT);

	connector_prepare_next (self);
}

static void
connector_on_ready (const struct pollfd *pfd, struct connector *self)
{
	// See http://cr.yp.to/docs/connect.html if this doesn't work.
	// The second connect() method doesn't work with DragonflyBSD.

	int error = 0;
	socklen_t error_len = sizeof error;
	hard_assert (!getsockopt (pfd->fd,
		SOL_SOCKET, SO_ERROR, &error, &error_len));

	if (error)
	{
		connector_notify_error (self, strerror (error));

		poller_fd_reset (&self->connected_event);
		xclose (self->socket);
		self->socket = -1;

		connector_step (self);
	}
	else
	{
		poller_fd_reset (&self->connected_event);
		self->socket = -1;

		set_blocking (pfd->fd, true);
		self->on_connected (self->user_data, pfd->fd);
	}
}

static void
connector_init (struct connector *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);
	self->socket = -1;
	poller_fd_init (&self->connected_event, poller, self->socket);
	self->connected_event.user_data = self;
	self->connected_event.dispatcher = (poller_fd_fn) connector_on_ready;
}

static void
connector_free (struct connector *self)
{
	poller_fd_reset (&self->connected_event);
	if (self->socket != -1)
		xclose (self->socket);

	LIST_FOR_EACH (struct connector_target, iter, self->targets)
		connector_target_destroy (iter);
}

static bool
connector_add_target (struct connector *self,
	const char *hostname, const char *service, struct error **e)
{
	struct addrinfo hints, *results;
	memset (&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;

	// TODO: even this should be done asynchronously, most likely in
	//   a thread pool, similarly to how libuv does it
	int err = getaddrinfo (hostname, service, &hints, &results);
	if (err)
	{
		error_set (e, "%s: %s", "getaddrinfo", gai_strerror (err));
		return false;
	}

	struct connector_target *target = connector_target_new ();
	target->hostname = xstrdup (hostname);
	target->service = xstrdup (service);
	target->results = results;
	target->iter = target->results;

	LIST_APPEND_WITH_TAIL (self->targets, self->targets_t, target);
	return true;
}

#endif // LIBERTY_WANT_POLLER

// --- Protocol modules --------------------------------------------------------

#include "liberty-proto.c"
