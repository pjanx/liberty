/*
 * liberty.c: the ultimate C unlibrary
 *
 * Copyright (c) 2014 - 2024, Přemysl Eric Janouch <p@janouch.name>
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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <setjmp.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
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
#include <pthread.h>

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

#ifdef CLOCK_MONOTONIC_RAW
// This should be more accurate for shorter intervals
#define CLOCK_BEST CLOCK_MONOTONIC_RAW
#elif defined _POSIX_MONOTONIC_CLOCK
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

#define CONTAINER_OF(pointer, type, member) \
	((type *) ((char *) pointer - offsetof (type, member)))

char *liberty = "They who can give up essential liberty to obtain a little "
	"temporary safety deserve neither liberty nor safety.";

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
	size_t len = strlen (s) + 1;
	return memcpy (xmalloc (len), s, len);
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

// --- Simple array support ----------------------------------------------------

// The most basic helper macros to make working with arrays not suck

#define ARRAY(type, name) type *name; size_t name ## _len, name ## _alloc;
#define ARRAY_INIT_SIZED(a, n)                                                 \
	BLOCK_START                                                                \
		(a) = xcalloc ((a ## _alloc) = (n), sizeof *(a));                      \
		(a ## _len) = 0;                                                       \
	BLOCK_END
#define ARRAY_INIT(a) ARRAY_INIT_SIZED (a, 16)
#define ARRAY_RESERVE(a, n)                                                    \
	BLOCK_START                                                                \
		while ((a ## _alloc) - (a ## _len) < n)                                \
			(a) = xreallocarray ((a), sizeof *(a), (a ## _alloc) <<= 1);       \
	BLOCK_END

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

struct strv
{
	char **vector;
	size_t len;
	size_t alloc;
};

static struct strv
strv_make (void)
{
	struct strv self;
	self.alloc = 4;
	self.len = 0;
	self.vector = xcalloc (self.alloc, sizeof *self.vector);
	return self;
}

static void
strv_free (struct strv *self)
{
	unsigned i;
	for (i = 0; i < self->len; i++)
		free (self->vector[i]);

	free (self->vector);
	self->vector = NULL;
}

static void
strv_reset (struct strv *self)
{
	strv_free (self);
	*self = strv_make ();
}

static void
strv_append_owned (struct strv *self, char *s)
{
	self->vector[self->len] = s;
	if (++self->len >= self->alloc)
		self->vector = xreallocarray (self->vector,
			sizeof *self->vector, (self->alloc <<= 1));
	self->vector[self->len] = NULL;
}

static void
strv_append (struct strv *self, const char *s)
{
	strv_append_owned (self, xstrdup (s));
}

static void
strv_append_args (struct strv *self, const char *s, ...)
	ATTRIBUTE_SENTINEL;

static void
strv_append_args (struct strv *self, const char *s, ...)
{
	va_list ap;

	va_start (ap, s);
	while (s)
	{
		strv_append (self, s);
		s = va_arg (ap, const char *);
	}
	va_end (ap);
}

static void
strv_append_vector (struct strv *self, char **vector)
{
	while (*vector)
		strv_append (self, *vector++);
}

static char *
strv_steal (struct strv *self, size_t i)
{
	hard_assert (i < self->len);
	char *tmp = self->vector[i];
	memmove (self->vector + i, self->vector + i + 1,
		(self->len-- - i) * sizeof *self->vector);
	return tmp;
}

static void
strv_remove (struct strv *self, size_t i)
{
	free (strv_steal (self, i));
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

static struct str
str_make (void)
{
	struct str self;
	self.alloc = 16;
	self.len = 0;
	self.str = strcpy (xmalloc (self.alloc), "");
	return self;
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
	*self = str_make ();
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
str_reserve (struct str *self, size_t n)
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
	str_reserve (self, n);
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
	str_reserve (self, size);
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

// --- Reading binary numbers --------------------------------------------------

// Doing this byte by byte prevents unaligned memory access issues.

static uint64_t
peek_u64be (const uint8_t *p)
{
	return (uint64_t) p[0] << 56 | (uint64_t) p[1] << 48
		| (uint64_t) p[2] << 40 | (uint64_t) p[3] << 32
		| (uint64_t) p[4] << 24 | (uint64_t) p[5] << 16 | p[6] << 8 | p[7];
}

static uint32_t
peek_u32be (const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t
peek_u16be (const uint8_t *p)
{
	return (uint16_t) p[0] << 8 | p[1];
}

static uint64_t
peek_u64le (const uint8_t *p)
{
	return (uint64_t) p[7] << 56 | (uint64_t) p[6] << 48
		| (uint64_t) p[5] << 40 | (uint64_t) p[4] << 32
		| (uint64_t) p[3] << 24 | (uint64_t) p[2] << 16 | p[1] << 8 | p[0];
}

static uint32_t
peek_u32le (const uint8_t *p)
{
	return (uint32_t) p[3] << 24 | (uint32_t) p[2] << 16 | p[1] << 8 | p[0];
}

static uint16_t
peek_u16le (const uint8_t *p)
{
	return (uint16_t) p[1] << 8 | p[0];
}

struct peeker
{
	uint64_t (*u64) (const uint8_t *);
	uint32_t (*u32) (const uint8_t *);
	uint16_t (*u16) (const uint8_t *);
};

static const struct peeker peeker_be = {peek_u64be, peek_u32be, peek_u16be};
static const struct peeker peeker_le = {peek_u64le, peek_u32le, peek_u16le};

// --- Errors ------------------------------------------------------------------

// Error reporting utilities.  Inspired by GError, only much simpler.

struct error
{
	char *message;                      ///< Textual description of the event
};

static bool
error_set (struct error **e, const char *message, ...) ATTRIBUTE_PRINTF (2, 3);

static bool
error_set (struct error **e, const char *message, ...)
{
	if (!e)
		return false;

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
	return false;
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

static bool
xwrite (int fd, const char *data, size_t len, struct error **e)
{
	size_t written = 0;
	while (written < len)
	{
		ssize_t res = write (fd, data + written, len - written);
		if (res >= 0)
			written += res;
		else if (errno != EINTR)
			return error_set (e, "%s", strerror (errno));
	}
	return true;
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
		return error_set (e, "%s: %s", "open", strerror (errno));
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

static struct str_map
str_map_make (str_map_free_fn free)
{
	struct str_map self;
	self.alloc = STR_MAP_MIN_ALLOC;
	self.len = 0;
	self.free = free;
	self.key_xfrm = NULL;
	self.map = xcalloc (self.alloc, sizeof *self.map);
	self.shrink_lock = false;
	return self;
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
str_map_pos (const struct str_map *self, const char *s)
{
	size_t mask = self->alloc - 1;
	return siphash_wrapper (s, strlen (s)) & mask;
}

static uint64_t
str_map_link_hash (const struct str_map_link *self)
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
str_map_find_real (const struct str_map *self, const char *key)
{
	struct str_map_link *iter = self->map[str_map_pos (self, key)];
	for (; iter; iter = iter->next)
		if (!strcmp (key, (const char *) iter + sizeof *iter))
			return iter->data;
	return NULL;
}

static void *
str_map_find (const struct str_map *self, const char *key)
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
	const struct str_map *map;          ///< The map we're iterating
	size_t next_index;                  ///< Next table index to search
	struct str_map_link *link;          ///< Current link
};

static struct str_map_iter
str_map_iter_make (const struct str_map *map)
{
	return (struct str_map_iter) { .map = map, .next_index = 0, .link = NULL };
}

static void *
str_map_iter_next (struct str_map_iter *self)
{
	const struct str_map *map = self->map;
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

static struct str_map_unset_iter
str_map_unset_iter_make (struct str_map *map)
{
	struct str_map_unset_iter self;
	self.iter = str_map_iter_make (map);
	map->shrink_lock = true;
	(void) str_map_iter_next (&self.iter);
	self.next = self.iter.link;
	return self;
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
	// So that we don't have to store another non-const pointer
	struct str_map *map = (struct str_map *) self->iter.map;

	map->shrink_lock = false;
	str_map_shrink (map);
}

// --- Asynchronous jobs -------------------------------------------------------

// For operations that can block execution but can be run independently on the
// rest of the program, such as getaddrinfo(), read(), write(), fsync().
//
// The async structure is meant to be extended for the various usages with
// new fields and provide an appropriate callback for its destruction.
//
// This is designed so that it can be used in other event loops than poller.

#ifdef LIBERTY_WANT_ASYNC

struct async;
typedef void (*async_fn) (struct async *);

struct async
{
	LIST_HEADER (struct async)
	struct async_manager *manager;      ///< Our manager object

	// "cancelled" may not be accessed or modified by the worker thread

	pthread_t worker;                   ///< Worker thread ID
	bool started;                       ///< Worker thread ID is valid
	bool cancelled;                     ///< Task has been cancelled

	async_fn execute;                   ///< Worker main function
	async_fn dispatcher;                ///< Main thread result dispatcher
	async_fn destroy;                   ///< Destroys the whole object
};

struct async_manager
{
	pthread_mutex_t lock;               ///< Lock for the queues
	struct async *running;              ///< Queue of running jobs
	struct async *finished;             ///< Queue of completed/cancelled jobs

	// It's upon the user to call async_manager_dispatch() to retry the delayed.
	// It's somewhat questionable if this feature is of any use. Possibly if we
	// provide a means of actively limiting the amount of running async jobs.

	struct async *delayed;              ///< Resource exhaustion queue

	// We need the pipe in order to abort polling (instead of using EINTR)

	pthread_cond_t finished_cond;       ///< Signals that a task has finished
	int finished_pipe[2];               ///< Signals that a task has finished
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct async
async_make (struct async_manager *manager)
{
	return (struct async) { .manager = manager };
}

/// Only allowed from the main thread once the job has been started but before
/// the results have been dispatched
static void
async_cancel (struct async *self)
{
	if (self->started)
		soft_assert (!pthread_cancel (self->worker));
	self->cancelled = true;
}

static void
async_cleanup (void *user_data)
{
	struct async *self = user_data;

	hard_assert (!pthread_mutex_lock (&self->manager->lock));
	LIST_UNLINK (self->manager->running, self);
	LIST_PREPEND (self->manager->finished, self);
	hard_assert (!pthread_mutex_unlock (&self->manager->lock));

	hard_assert (!pthread_cond_broadcast (&self->manager->finished_cond));
	hard_assert (write (self->manager->finished_pipe[1], "", 1) > 0);
}

static void *
async_routine (void *user_data)
{
	// Beware that we mustn't trigger any cancellation point before we set up
	// the cleanup handler, otherwise we'd need to disable it first
	struct async *self = user_data;
	pthread_cleanup_push (async_cleanup, self);

	self->execute (self);

	pthread_cleanup_pop (true);
	return NULL;
}

static bool
async_run (struct async *self)
{
	hard_assert (!pthread_mutex_lock (&self->manager->lock));
	LIST_PREPEND (self->manager->running, self);
	hard_assert (!pthread_mutex_unlock (&self->manager->lock));

	// Block all signals so that the new thread doesn't receive any (inherited)
	sigset_t all_blocked, old_blocked;
	hard_assert (!sigfillset (&all_blocked));
	hard_assert (!pthread_sigmask (SIG_SETMASK, &all_blocked, &old_blocked));

	int error = pthread_create (&self->worker, NULL, async_routine, self);

	// Now that we've created the thread, resume signal processing as usual
	hard_assert (!pthread_sigmask (SIG_SETMASK, &old_blocked, NULL));

	if (error)
	{
		hard_assert (error == EAGAIN);

		hard_assert (!pthread_mutex_lock (&self->manager->lock));
		LIST_UNLINK (self->manager->running, self);
		hard_assert (!pthread_mutex_unlock (&self->manager->lock));

		// FIXME: we probably want to have some kind of a limit on the queue
		LIST_PREPEND (self->manager->delayed, self);
		return false;
	}
	return (self->started = true);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct async *
async_manager_dispatch_fetch (struct async_manager *self)
{
	// We don't want to hold the mutex for too long, mainly to prevent
	// a deadlock when trying to add an async job while dispatching another
	hard_assert (!pthread_mutex_lock (&self->lock));
	struct async *result;
	if ((result = self->finished))
		LIST_UNLINK (self->finished, result);
	hard_assert (!pthread_mutex_unlock (&self->lock));
	return result;
}

static void
async_manager_dispatch (struct async_manager *self)
{
	char dummy;
	while (read (self->finished_pipe[0], &dummy, 1) > 0)
		;  // Just emptying the signalling pipe

	struct async *iter;
	while ((iter = async_manager_dispatch_fetch (self)))
	{
		// The thread has finished execution already
		soft_assert (!pthread_join (iter->worker, NULL));

		if (iter->dispatcher && !iter->cancelled)
			iter->dispatcher (iter);
		if (iter->destroy)
			iter->destroy (iter);
	}

	LIST_FOR_EACH (struct async, iter, self->delayed)
	{
		LIST_UNLINK (self->delayed, iter);
		if (iter->cancelled)
		{
			if (iter->destroy)
				iter->destroy (iter);
		}
		else if (!async_run (iter))
			break;
	}
}

static void
async_manager_cancel_all (struct async_manager *self)
{
	hard_assert (!pthread_mutex_lock (&self->lock));

	// Cancel all running jobs
	LIST_FOR_EACH (struct async, iter, self->running)
		soft_assert (!pthread_cancel (iter->worker));

	// Wait until no jobs are running anymore (we need to release the lock
	// here so that worker threads can move their jobs to the finished queue)
	while (self->running)
		hard_assert (!pthread_cond_wait (&self->finished_cond, &self->lock));

	// Mark everything cancelled so that it's not actually dispatched
	LIST_FOR_EACH (struct async, iter, self->finished)
		iter->cancelled = true;
	LIST_FOR_EACH (struct async, iter, self->delayed)
		iter->cancelled = true;

	hard_assert (!pthread_mutex_unlock (&self->lock));
	async_manager_dispatch (self);
}

static struct async_manager
async_manager_make (void)
{
	struct async_manager self = {};
	hard_assert (!pthread_mutex_init (&self.lock, NULL));
	hard_assert (!pthread_cond_init (&self.finished_cond, NULL));

	hard_assert (!pipe (self.finished_pipe));
	hard_assert (set_blocking (self.finished_pipe[0], false));
	set_cloexec (self.finished_pipe[0]);
	set_cloexec (self.finished_pipe[1]);
	return self;
}

static void
async_manager_free (struct async_manager *self)
{
	async_manager_cancel_all (self);
	hard_assert (!pthread_cond_destroy (&self->finished_cond));
	hard_assert (!pthread_mutex_destroy (&self->lock));

	xclose (self->finished_pipe[0]);
	xclose (self->finished_pipe[1]);
}

#endif // LIBERTY_WANT_ASYNC

// --- Event loop --------------------------------------------------------------

#ifdef LIBERTY_WANT_POLLER

// Basically the poor man's GMainLoop/libev/libuv.  It might make some sense
// to instead use those tested and proven libraries but we don't need much
// and it's interesting to implement.

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

	// Make triple sure that no forked child is keeping the FD,
	// otherwise we may access freed memory on Linux (poor epoll design)
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

// The heap could definitely be made faster but we'll prefer simplicity
struct poller_timers
{
	struct poller_timer **heap;         ///< Min-heap of timers
	size_t len;                         ///< Number of scheduled timers
	size_t alloc;                       ///< Number of timers allocated
};

static struct poller_timers
poller_timers_make (void)
{
	struct poller_timers self;
	self.alloc = POLLER_MIN_ALLOC;
	self.len = 0;
	self.heap = xmalloc (self.alloc * sizeof *self.heap);
	return self;
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
poller_timers_get_poll_timeout (const struct poller_timers *self)
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
poller_idle_dispatch (const struct poller_idle *list)
{
	const struct poller_idle *iter, *next;
	for (iter = list; iter; iter = next)
	{
		next = iter->next;
		iter->dispatcher (iter->user_data);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct poller_common
{
	struct poller_timers timers;        ///< Timeouts
	struct poller_idle *idle;           ///< Idle events
#ifdef LIBERTY_WANT_ASYNC
	struct async_manager async;         ///< Asynchronous jobs
	struct poller_fd async_event;       ///< Asynchronous jobs have finished
#endif // LIBERTY_WANT_ASYNC
};

static void poller_common_init (struct poller_common *, struct poller *);
static void poller_common_free (struct poller_common *);
static int poller_common_get_timeout (const struct poller_common *);
static void poller_common_dispatch (struct poller_common *);

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
	struct poller_common common;        ///< Stuff common to all backends
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

	poller_common_init (&self->common, self);
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

	poller_common_free (&self->common);

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

#define poller_post_fork(self)

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
			poller_common_get_timeout (&self->common));
	while (n_fds == -1 && errno == EINTR);

	if (n_fds == -1)
		exit_fatal ("%s: %s", "epoll", strerror (errno));

	// Sort them by file descriptor number for binary search
	qsort (self->revents, n_fds, sizeof *self->revents, poller_compare_fds);
	self->revents_len = n_fds;

	poller_common_dispatch (&self->common);

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

// Sort of similar to the epoll version.  Let's hope Darwin isn't broken,
// that'd mean reimplementing this in terms of select() just because of Crapple.
#elif defined (BSD) || defined (__APPLE__)

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
	struct poller_common common;        ///< Stuff common to all backends
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
	poller_common_init (&self->common, self);
}

static void
poller_free (struct poller *self)
{
	xclose (self->kqueue_fd);
	free (self->fds);
	free (self->revents);
	poller_common_free (&self->common);
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
	if (fd->index == -1)
	{
		poller_ensure_space (self);
		self->fds[fd->index = self->len++] = fd;
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

static void
poller_post_fork (struct poller *self)
{
	// The kqueue FD isn't preserved across forks, need to recreate it
	self->kqueue_fd = kqueue ();
	hard_assert (self->kqueue_fd != -1);
	set_cloexec (self->kqueue_fd);

	for (size_t i = 0; i < self->len; i++)
		poller_set (self, self->fds[i]);
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
		int timeout = poller_common_get_timeout (&self->common);
		struct timespec ts = poller_timeout_to_timespec (timeout);
		n_fds = kevent (self->kqueue_fd,
			NULL, 0, self->revents, self->len, timeout < 0 ? NULL : &ts);
	}
	while (n_fds == -1 && errno == EINTR);

	if (n_fds == -1)
		exit_fatal ("%s: %s", "kevent", strerror (errno));

	// Sort them by file descriptor number for binary search
	qsort (self->revents, n_fds, sizeof *self->revents, poller_compare_fds);
	self->revents_len = n_fds;

	poller_common_dispatch (&self->common);

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
	struct poller_common common;        ///< Stuff common to all backends
	int dispatch_next;                  ///< The next dispatched FD or -1
};

static void
poller_init (struct poller *self)
{
	self->alloc = POLLER_MIN_ALLOC;
	self->len = 0;
	self->fds = xcalloc (self->alloc, sizeof *self->fds);
	self->fds_data = xcalloc (self->alloc, sizeof *self->fds_data);
	poller_common_init (&self->common, self);
	self->dispatch_next = -1;
}

static void
poller_free (struct poller *self)
{
	free (self->fds);
	free (self->fds_data);
	poller_common_free (&self->common);
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

#define poller_post_fork(self)

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
			poller_common_get_timeout (&self->common));
	while (result == -1 && errno == EINTR);

	if (result == -1)
		exit_fatal ("%s: %s", "poll", strerror (errno));

	poller_common_dispatch (&self->common);

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

static struct poller_timer
poller_timer_make (struct poller *poller)
{
	return (struct poller_timer)
		{ .timers = &poller->common.timers, .index = -1, };
}

static void
poller_timer_set (struct poller_timer *self, int timeout_ms)
{
	self->when = poller_timers_get_current_time () + timeout_ms;
	poller_timers_set (self->timers, self);
}

static bool
poller_timer_is_active (const struct poller_timer *self)
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

static struct poller_idle
poller_idle_make (struct poller *poller)
{
	return (struct poller_idle) { .poller = poller };
}

static void
poller_idle_set (struct poller_idle *self)
{
	if (self->active)
		return;

	LIST_PREPEND (self->poller->common.idle, self);
	self->active = true;
}

static void
poller_idle_reset (struct poller_idle *self)
{
	if (!self->active)
		return;

	LIST_UNLINK (self->poller->common.idle, self);
	self->prev = NULL;
	self->next = NULL;
	self->active = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct poller_fd
poller_fd_make (struct poller *poller, int fd)
{
	return (struct poller_fd) { .poller = poller, .index = -1, .fd = fd };
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

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_common_dummy_dispatcher (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;
	(void) user_data;

	// The async manager will empty the pipe when we invoke dispatch
}

static void
poller_common_init (struct poller_common *self, struct poller *poller)
{
	self->timers = poller_timers_make ();
	self->idle = NULL;
#ifdef LIBERTY_WANT_ASYNC
	self->async = async_manager_make ();

	self->async_event = poller_fd_make (poller, self->async.finished_pipe[0]);
	poller_fd_set (&self->async_event, POLLIN);
	self->async_event.dispatcher = poller_common_dummy_dispatcher;
	self->async_event.user_data = self;
#else // ! LIBERTY_WANT_ASYNC
	(void) poller;
#endif // ! LIBERTY_WANT_ASYNC
}

static void
poller_common_free (struct poller_common *self)
{
	poller_timers_free (&self->timers);
#ifdef LIBERTY_WANT_ASYNC
	async_manager_free (&self->async);
#endif // LIBERTY_WANT_ASYNC
}

static int
poller_common_get_timeout (const struct poller_common *self)
{
	if (self->idle)
		return 0;

	int timeout = poller_timers_get_poll_timeout (&self->timers);
#ifdef LIBERTY_WANT_ASYNC
	// This is completely arbitrary, in general we have no idea when to retry,
	// however one second doesn't sound like a particularly bad number
	if (self->async.delayed)
		timeout = MIN (timeout, 1000);
#endif // LIBERTY_WANT_ASYNC
	return timeout;
}

static void
poller_common_dispatch (struct poller_common *self)
{
	poller_timers_dispatch (&self->timers);
	poller_idle_dispatch (self->idle);
#ifdef LIBERTY_WANT_ASYNC
	async_manager_dispatch (&self->async);
#endif // LIBERTY_WANT_ASYNC
}

#endif // LIBERTY_WANT_POLLER

// --- Asynchronous jobs -------------------------------------------------------

#ifdef LIBERTY_WANT_ASYNC

/// The callback takes ownership of the returned list
typedef void (*async_getaddrinfo_fn) (int, struct addrinfo *, void *);

struct async_getaddrinfo
{
	struct async async;                 ///< Parent object

	int gai_result;                     ///< Direct result from getaddrinfo()
	char *host;                         ///< gai() argument: host
	char *service;                      ///< gai() argument: service
	struct addrinfo hints;              ///< gai() argument: hints
	struct addrinfo *result;            ///< Resulting addresses from gai()

	async_getaddrinfo_fn dispatcher;    ///< Event dispatcher
	void *user_data;                    ///< User data
};

static void
async_getaddrinfo_execute (struct async *async)
{
	struct async_getaddrinfo *self = (struct async_getaddrinfo *) async;
	self->gai_result =
		getaddrinfo (self->host, self->service, &self->hints, &self->result);
}

static void
async_getaddrinfo_dispatch (struct async *async)
{
	struct async_getaddrinfo *self = (struct async_getaddrinfo *) async;
	self->dispatcher (self->gai_result, self->result, self->user_data);
	self->result = NULL;
}

static void
async_getaddrinfo_destroy (struct async *async)
{
	struct async_getaddrinfo *self = (struct async_getaddrinfo *) async;
	free (self->host);
	free (self->service);

	if (self->result)
		freeaddrinfo (self->result);

	free (self);
}

static struct async_getaddrinfo *
async_getaddrinfo (struct async_manager *manager,
	const char *host, const char *service, const struct addrinfo *hints)
{
	struct async_getaddrinfo *self = xcalloc (1, sizeof *self);
	self->async = async_make (manager);

	if (host)     self->host = xstrdup (host);
	if (service)  self->service = xstrdup (service);
	if (hints)    memcpy (&self->hints, hints, sizeof *hints);

	self->async.execute    = async_getaddrinfo_execute;
	self->async.dispatcher = async_getaddrinfo_dispatch;
	self->async.destroy    = async_getaddrinfo_destroy;

	async_run (&self->async);
	return self;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef void (*async_getnameinfo_fn) (int, char *, char *, void *);

struct async_getnameinfo
{
	struct async async;                 ///< Parent object

	int gni_result;                     ///< Direct result from getnameinfo()
	char host[NI_MAXHOST];              ///< gni() result: host name
	char service[NI_MAXSERV];           ///< gni() result: service name

	struct sockaddr *address;           ///< gni() argument: address
	socklen_t address_len;              ///< Size of the address
	int flags;                          ///< gni() argument: flags

	async_getnameinfo_fn dispatcher;    ///< Event dispatcher
	void *user_data;                    ///< User data
};

static void
async_getnameinfo_execute (struct async *async)
{
	struct async_getnameinfo *self = (struct async_getnameinfo *) async;
	self->gni_result = getnameinfo (self->address, self->address_len,
		self->host, sizeof self->host,
		self->service, sizeof self->service, self->flags);
}

static void
async_getnameinfo_dispatch (struct async *async)
{
	struct async_getnameinfo *self = (struct async_getnameinfo *) async;
	self->dispatcher (self->gni_result, self->host, self->service,
		self->user_data);
}

static void
async_getnameinfo_destroy (struct async *async)
{
	struct async_getnameinfo *self = (struct async_getnameinfo *) async;
	free (self->address);
	free (self);
}

static struct async_getnameinfo *
async_getnameinfo (struct async_manager *manager,
	const struct sockaddr *sa, socklen_t sa_len, int flags)
{
	struct async_getnameinfo *self = xcalloc (1, sizeof *self);
	self->async = async_make (manager);

	self->address = memcpy (xmalloc (sa_len), sa, sa_len);
	self->address_len = sa_len;
	self->flags = flags;

	self->async.execute    = async_getnameinfo_execute;
	self->async.dispatcher = async_getnameinfo_dispatch;
	self->async.destroy    = async_getnameinfo_destroy;

	async_run (&self->async);
	return self;
}

#endif // LIBERTY_WANT_ASYNC

// --- libuv-style write adaptor -----------------------------------------------

// Makes it possible to use iovec to write multiple data chunks at once.

struct write_req
{
	LIST_HEADER (struct write_req)
	struct iovec data;                  ///< Data to be written
};

struct write_queue
{
	struct write_req *head;             ///< The head of the queue
	struct write_req *tail;             ///< The tail of the queue
	size_t head_offset;                 ///< Offset into the head
	size_t len;
};

static struct write_queue
write_queue_make (void)
{
	return (struct write_queue) {};
}

static void
write_queue_free (struct write_queue *self)
{
	LIST_FOR_EACH (struct write_req, iter, self->head)
	{
		free (iter->data.iov_base);
		free (iter);
	}
}

static void
write_queue_add (struct write_queue *self, struct write_req *req)
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
		struct write_req *head = self->head;
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
write_queue_is_empty (const struct write_queue *self)
{
	return self->head == NULL;
}

// --- Message reader ----------------------------------------------------------

struct msg_reader
{
	struct str buf;                     ///< Input buffer
	uint64_t offset;                    ///< Current offset in the buffer
};

static struct msg_reader
msg_reader_make (void)
{
	return (struct msg_reader) { .buf = str_make (), .offset = 0 };
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
	uint64_t msg_len = peek_u64be (x);
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

static struct msg_unpacker
msg_unpacker_make (const void *data, size_t len)
{
	return (struct msg_unpacker) { .data = data, .len = len, .offset = 0 };
}

static size_t
msg_unpacker_get_available (const struct msg_unpacker *self)
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
	*value = peek_u16be (x);
	return true;
}

static bool
msg_unpacker_u32 (struct msg_unpacker *self, uint32_t *value)
{
	UNPACKER_INT_BEGIN
	*value = peek_u32be (x);
	return true;
}

static bool
msg_unpacker_u64 (struct msg_unpacker *self, uint64_t *value)
{
	UNPACKER_INT_BEGIN
	*value = peek_u64be (x);
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

static struct msg_writer
msg_writer_make (void)
{
	struct msg_writer self = { .buf = str_make () };
	// Placeholder for message length
	str_append_data (&self.buf, "\x00\x00\x00\x00" "\x00\x00\x00\x00", 8);
	return self;
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
	return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
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
iscntrl_ascii (int c)
{
	return (c >= 0 && c < 32) || c == 0x7f;
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

/// Return the value of the UTF-8 character at `*s` and advance the pointer
/// to the next one.  Returns -2 if there is only a partial but possibly valid
/// character sequence, or -1 on other errors.  Either way, `*s` is untouched.
static int32_t
utf8_decode (const char **s, size_t len)
{
	// End of string, we go no further
	if (!len)
		return -1;

	// Find out how long the sequence is (0 for ASCII)
	unsigned mask = 0x80;
	unsigned sequence_len = 0;

	const uint8_t *p = (const uint8_t *) *s, *end = p + len;
	while ((*p & mask) == mask)
	{
		// Invalid start of sequence
		if (mask == 0xFE)
			return -1;

		mask |= mask >> 1;
		sequence_len++;
	}

	// In the middle of a character
	// or an overlong sequence (subset, possibly MUTF-8, not supported)
	if (sequence_len == 1 || *p == 0xC0 || *p == 0xC1)
		return -1;

	// Check the rest of the sequence
	uint32_t cp = *p++ & ~mask;
	while (sequence_len && --sequence_len)
	{
		if (p == end)
			return -2;
		if ((*p & 0xC0) != 0x80)
			return -1;
		cp = cp << 6 | (*p++ & 0x3F);
	}
	*s = (const char *) p;
	return cp;
}

static inline bool
utf8_validate_cp (int32_t cp)
{
	// RFC 3629, CESU-8 not allowed
	return cp >= 0 && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

/// Very rough UTF-8 validation, just makes sure codepoints can be iterated
static bool
utf8_validate (const char *s, size_t len)
{
	const char *end = s + len;
	int32_t codepoint;
	while ((codepoint = utf8_decode (&s, end - s)) >= 0
		&& utf8_validate_cp (codepoint))
		;
	return s == end;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct utf8_iter
{
	const char *s;                      ///< String iterator
	size_t len;                         ///< How many bytes remain
};

static struct utf8_iter
utf8_iter_make (const char *s)
{
	return (struct utf8_iter) { .s = s, .len = strlen (s) };
}

static int32_t
utf8_iter_next (struct utf8_iter *self, size_t *len)
{
	if (!self->len)
		return -1;

	const char *old = self->s;
	int32_t codepoint = utf8_decode (&self->s, self->len);
	if (!soft_assert (codepoint >= 0))
	{
		// Invalid UTF-8
		self->len = 0;
		return codepoint;
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
cstr_set (char **s, char *new)
{
	free (*s);
	*s = new;
}

static void
cstr_split (const char *s, const char *delimiters, bool ignore_empty,
	struct strv *out)
{
	const char *begin = s, *end;
	while ((end = strpbrk (begin, delimiters)))
	{
		if (!ignore_empty || begin != end)
			strv_append_owned (out, xstrndup (begin, end - begin));
		begin = ++end;
	}
	if (!ignore_empty || *begin)
		strv_append (out, begin);
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
cstr_transform (char *s, int (*xform) (int c))
{
	for (; *s; s++)
		*s = xform (*s);
}

static char *
cstr_cut_until (const char *s, const char *alphabet)
{
	return xstrndup (s, strcspn (s, alphabet));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
strv_join (const struct strv *v, const char *delimiter)
{
	if (!v->len)
		return xstrdup ("");

	struct str result = str_make ();
	str_append (&result, v->vector[0]);
	for (size_t i = 1; i < v->len; i++)
		str_append_printf (&result, "%s%s", delimiter, v->vector[i]);
	return str_steal (&result);
}

static char *xstrdup_printf (const char *, ...) ATTRIBUTE_PRINTF (1, 2);

static char *
xstrdup_printf (const char *format, ...)
{
	va_list ap;
	struct str tmp = str_make ();
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
		// XXX: out_len will be one character longer than the string!
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
read_line (FILE *fp, struct str *line)
{
	str_reset (line);

	int c;
	while ((c = fgetc (fp)) != '\n')
	{
		if (c == EOF)
			return line->len != 0;
		if (c != '\r')
			str_append_c (line, c);
	}
	return true;
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

static int
lock_pid_file (const char *path, struct error **e)
{
	// When using XDG_RUNTIME_DIR, the file needs to either have its
	// access time bumped every 6 hours, or have the sticky bit set
	int fd = open (path, O_RDWR | O_CREAT,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH /* 644 */ | S_ISVTX /* sticky */);
	if (fd < 0)
	{
		error_set (e, "can't open `%s': %s", path, strerror (errno));
		return -1;
	}

	set_cloexec (fd);

	struct flock lock =
	{
		.l_type = F_WRLCK,
		.l_start = 0,
		.l_whence = SEEK_SET,
		.l_len = 0,
	};
	if (fcntl (fd, F_SETLK, &lock))
	{
		error_set (e, "can't lock `%s': %s", path, strerror (errno));
		xclose (fd);
		return -1;
	}

	struct str pid = str_make ();
	str_append_printf (&pid, "%ld", (long) getpid ());

	if (ftruncate (fd, 0)
	 || write (fd, pid.str, pid.len) != (ssize_t) pid.len)
	{
		error_set (e, "can't write to `%s': %s", path, strerror (errno));
		xclose (fd);
		return -1;
	}
	str_free (&pid);

	// Intentionally not closing the file descriptor; it must stay alive
	// for the entire life of the application
	return fd;
}

static bool
ensure_directory_existence (const char *path, struct error **e)
{
	struct stat st;

	if (stat (path, &st))
	{
		if (mkdir (path, S_IRWXU | S_IRWXG | S_IRWXO))
		{
			return error_set (e, "cannot create directory `%s': %s",
				path, strerror (errno));
		}
	}
	else if (!S_ISDIR (st.st_mode))
	{
		return error_set (e, "cannot create directory `%s': %s",
			path, "file exists but is not a directory");
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
	(struct strv *paths, const char *tail, const char *filename)
{
	for (unsigned i = 0; i < paths->len; i++)
	{
		// As per XDG spec, relative paths are ignored
		if (*paths->vector[i] != '/')
			continue;

		char *file = xstrdup_printf
			("%s/%s%s", paths->vector[i], tail, filename);

		struct stat st;
		if (!stat (file, &st))
			return file;
		free (file);
	}
	return NULL;
}

static void
get_xdg_config_dirs (struct strv *out)
{
	struct str config_home = str_make ();
	get_xdg_home_dir (&config_home, "XDG_CONFIG_HOME", ".config");
	strv_append (out, config_home.str);
	str_free (&config_home);

	const char *xdg_config_dirs;
	if (!(xdg_config_dirs = getenv ("XDG_CONFIG_DIRS")) || !*xdg_config_dirs)
		xdg_config_dirs = "/etc/xdg";
	cstr_split (xdg_config_dirs, ":", true, out);
}

static char *
resolve_relative_config_filename (const char *filename)
{
	struct strv paths = strv_make ();
	get_xdg_config_dirs (&paths);
	char *result = resolve_relative_filename_generic
		(&paths, PROGRAM_NAME "/", filename);
	strv_free (&paths);
	return result;
}

static void
get_xdg_data_dirs (struct strv *out)
{
	struct str data_home = str_make ();
	get_xdg_home_dir (&data_home, "XDG_DATA_HOME", ".local/share");
	strv_append (out, data_home.str);
	str_free (&data_home);

	const char *xdg_data_dirs;
	if (!(xdg_data_dirs = getenv ("XDG_DATA_DIRS")) || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share/:/usr/share/";
	cstr_split (xdg_data_dirs, ":", true, out);
}

static char *
resolve_relative_data_filename (const char *filename)
{
	struct strv paths = strv_make ();
	get_xdg_data_dirs (&paths);
	char *result = resolve_relative_filename_generic
		(&paths, PROGRAM_NAME "/", filename);
	strv_free (&paths);
	return result;
}

static char *
resolve_relative_runtime_filename_finish (struct str path)
{
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
resolve_relative_runtime_filename (const char *filename)
{
	struct str path = str_make ();
	const char *runtime_dir = getenv ("XDG_RUNTIME_DIR");
	if (runtime_dir && *runtime_dir == '/')
		str_append (&path, runtime_dir);
	else
		get_xdg_home_dir (&path, "XDG_DATA_HOME", ".local/share");

	str_append_printf (&path, "/%s/%s", PROGRAM_NAME, filename);
	return resolve_relative_runtime_filename_finish (path);
}

/// This differs from resolve_relative_runtime_filename() in that we expect
/// the filename to be something like a pattern for mkstemp(), so the resulting
/// path can reside in a system-wide directory with no risk of a conflict.
/// However, we have to take care about permissions.  Do we even need this?
static char *
resolve_relative_runtime_template (const char *template)
{
	struct str path = str_make ();
	const char *runtime_dir = getenv ("XDG_RUNTIME_DIR");
	const char *tmpdir = getenv ("TMPDIR");
	if (runtime_dir && *runtime_dir == '/')
		str_append_printf (&path, "%s/%s", runtime_dir, PROGRAM_NAME);
	else if (tmpdir && *tmpdir == '/')
		str_append_printf (&path, "%s/%s.%d", tmpdir, PROGRAM_NAME, geteuid ());
	else
		str_append_printf (&path, "/tmp/%s.%d", PROGRAM_NAME, geteuid ());

	str_append_printf (&path, "/%s", template);
	return resolve_relative_runtime_filename_finish (path);
}

static char *
try_expand_tilde (const char *filename)
{
	size_t until_slash = strcspn (filename, "/");
	if (!until_slash)
	{
		struct str expanded = str_make ();
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
// a `struct strv' (if all we want is the substrings).

static struct str_map
regex_cache_make (void)
{
	return str_map_make (regex_free);
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

// --- Simple file I/O ---------------------------------------------------------

static bool
read_file (const char *filename, struct str *output, struct error **e)
{
	FILE *fp = fopen (filename, "rb");
	if (!fp)
	{
		return error_set (e, "could not open `%s' for reading: %s",
			filename, strerror (errno));
	}

	char buf[BUFSIZ];
	size_t len;

	while ((len = fread (buf, 1, sizeof buf, fp)) == sizeof buf)
		str_append_data (output, buf, len);
	str_append_data (output, buf, len);

	bool success = !ferror (fp);
	fclose (fp);

	if (success)
		return true;

	return error_set (e, "error while reading `%s': %s",
		filename, strerror (errno));
}

/// Overwrites filename contents with data; creates directories as needed
static bool
write_file (const char *filename, const void *data, size_t data_len,
	struct error **e)
{
	char *dir = xstrdup (filename);
	bool parents_created = mkdir_with_parents (dirname (dir), e);
	free (dir);
	if (!parents_created)
		return false;

	FILE *fp = fopen (filename, "w");
	if (!fp)
	{
		return error_set (e, "could not open `%s' for writing: %s",
			filename, strerror (errno));
	}

	fwrite (data, data_len, 1, fp);
	bool success = !ferror (fp) && !fflush (fp)
		&& (!fsync (fileno (fp)) || errno == EINVAL);
	fclose (fp);

	if (!success)
	{
		return error_set (e, "writing to `%s' failed: %s",
			filename, strerror (errno));
	}
	return true;
}

/// Wrapper for write_file() that makes sure that the new data has been written
/// to disk in its entirety before overriding the old file
static bool
write_file_safe (const char *filename, const void *data, size_t data_len,
	struct error **e)
{
	// XXX: ideally we would also open the directory, use *at() versions
	//   of functions and call fsync() on the directory as appropriate
	// FIXME: this should behave similarly to mkstemp(), just with 0666;
	//   as it is, this function is not particularly safe
	char *temp = xstrdup_printf ("%s.new", filename);
	bool success = write_file (temp, data, data_len, e);
	if (success && !(success = !rename (temp, filename)))
		error_set (e, "could not rename `%s' to `%s': %s",
			temp, filename, strerror (errno));
	free (temp);
	return success;
}

// --- Simple configuration ----------------------------------------------------

// This is the bare minimum to make an application configurable.
// Keys are stripped of surrounding whitespace, values are not.

struct simple_config_item { const char *key, *default_value, *description; };

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
	struct str s = str_make ();
	bool ok = !filename || read_file (filename, &s, e);
	size_t line_no = 0;
	for (char *x = strtok (s.str, "\r\n"); ok && x; x = strtok (NULL, "\r\n"))
	{
		line_no++;
		if (strchr ("#", *(x += strspn (x, " \t"))))
			continue;

		char *equals = strchr (x, '=');
		if (!equals || equals == x)
			ok = error_set (e, "%s: malformed line %zu", filename, line_no);
		else
		{
			char *end = equals++;
			do *end = '\0'; while (strchr (" \t", *--end));
			str_map_set (config, x, xstrdup (equals));
		}
	}
	str_free (&s);
	free (filename);
	return ok;
}

static char *
write_configuration_file (const char *path_hint, const struct str *data,
	struct error **e)
{
	struct str path = str_make ();
	if (path_hint)
		str_append (&path, path_hint);
	else
	{
		get_xdg_home_dir (&path, "XDG_CONFIG_HOME", ".config");
		str_append (&path, "/" PROGRAM_NAME "/" PROGRAM_NAME ".conf");
	}

	if (!write_file_safe (path.str, data->str, data->len, e))
	{
		str_free (&path);
		return NULL;
	}
	return str_steal (&path);
}

static char *
simple_config_write_default (const char *path_hint, const char *prolog,
	const struct simple_config_item *table, struct error **e)
{
	struct str data = str_make ();
	if (prolog)
		str_append (&data, prolog);

	for (; table->key != NULL; table++)
	{
		str_append_printf (&data, "# %s\n", table->description);
		if (table->default_value)
			str_append_printf (&data, "%s=%s\n",
				table->key, table->default_value);
		else
			str_append_printf (&data, "#%s=\n", table->key);
	}

	char *path = write_configuration_file (path_hint, &data, e);
	str_free (&data);
	return path;
}

/// Convenience wrapper suitable for most simple applications
static void
call_simple_config_write_default
	(const char *path_hint, const struct simple_config_item *table)
{
	static const char *prolog =
	"# " PROGRAM_NAME " " PROGRAM_VERSION " configuration file\n"
	"#\n"
	"# Relative paths are searched for in ${XDG_CONFIG_HOME:-~/.config}\n"
	"# /" PROGRAM_NAME " as well as in $XDG_CONFIG_DIRS/" PROGRAM_NAME "\n"
	"\n";

	struct error *e = NULL;
	char *filename = simple_config_write_default (path_hint, prolog, table, &e);
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

static struct opt_handler
opt_handler_make (int argc, char **argv,
	const struct opt *opts, const char *arg_hint, const char *description)
{
	struct opt_handler self =
	{
		.argc = argc,
		.argv = argv,
		.arg_hint = arg_hint,
		.description = description,
	};

	size_t len = 0;
	for (const struct opt *iter = opts; iter->long_name; iter++)
		len++;

	self.opts = opts;
	self.opts_len = len;
	self.options = xcalloc (len + 1, sizeof *self.options);

	struct str opt_string = str_make ();
	for (size_t i = 0; i < len; i++)
	{
		const struct opt *opt = opts + i;
		struct option *mapped = self.options + i;

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
	self.opt_string = str_steal (&opt_string);
	return self;
}

static void
opt_handler_usage (const struct opt_handler *self, FILE *stream)
{
	struct str usage = str_make ();
	str_append_printf (&usage, "Usage: %s [OPTION]... %s\n",
		self->argv[0], self->arg_hint ? self->arg_hint : "");
	str_append_printf (&usage, "%s\n\n", self->description);

	for (size_t i = 0; i < self->opts_len; i++)
	{
		struct str row = str_make ();
		const struct opt *opt = self->opts + i;
		if (!(opt->flags & OPT_LONG_ONLY))
			str_append_printf (&row, "  -%c, ", opt->short_name);
		else
			str_append (&row, "      ");
		str_append_printf (&row, "--%s", opt->long_name);
		if (opt->arg_hint)
			str_append_printf (&row, (opt->flags & OPT_OPTIONAL_ARG)
				? "[=%s]" : " %s", opt->arg_hint);

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
opt_handler_get (const struct opt_handler *self)
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
	unsigned can_fork  : 1;             ///< Forking doesn't break anything
};

static void
test_init (struct test *self, int argc, char **argv)
{
	memset (self, 0, sizeof *self);
	self->whitelist = str_map_make (NULL);
	self->blacklist = str_map_make (NULL);

	// Usually this shouldn't pose a problem but let's make it optional
	self->can_fork = true;

	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'p', "pass", "NAME", 0, "only run tests glob-matching the name" },
		{ 's', "skip", "NAME", 0, "skip all tests glob-matching the name" },
		{ 'S', "single-process", NULL, 0, "don't fork for each test" },
		{ 'l', "list", NULL, 0, "list all available tests" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, NULL, "Unit test runner");

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

	case 'S':  self->can_fork = false;  break;
	case 'l':  self->list_only = true;  break;

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
	struct str_map_iter iter = str_map_iter_make (self);
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

static void
test_unit_run (struct test_unit *self)
{
	void *fixture = xcalloc (1, self->fixture_size);
	if (self->setup)
		self->setup (self->user_data, fixture);

	self->test (self->user_data, fixture);

	if (self->teardown)
		self->teardown (self->user_data, fixture);
	free (fixture);
}

static bool
test_unit_run_forked (struct test_unit *self)
{
	pid_t child = fork ();
	if (child == -1)
	{
		print_error ("%s: %s", "fork", strerror (errno));
		return false;
	}
	else if (!child)
	{
		test_unit_run (self);
		_exit (EXIT_SUCCESS);
	}

	int status = 0;
	if (waitpid (child, &status, WUNTRACED) == -1)
		print_error ("%s: %s", "waitpid", strerror (errno));
	else if (WIFSTOPPED (status))
	{
		print_error ("test child has been stopped");
		(void) kill (child, SIGKILL);
	}
	else if (WIFSIGNALED (status))
		print_error ("test child was killed by signal %d", WTERMSIG (status));
	else if (WEXITSTATUS (status) != 0)
		print_error ("test child exited with status %d", WEXITSTATUS (status));
	else
		return true;
	return false;
}

static bool
test_run_unit (struct test *self, struct test_unit *unit)
{
	fprintf (stderr, "%s: ", unit->name);

	if (!self->can_fork)
		test_unit_run (unit);
	else if (!test_unit_run_forked (unit))
		return false;

	fprintf (stderr, "OK\n");
	return true;
}

static int
test_run (struct test *self)
{
	g_soft_asserts_are_deadly = true;

	bool failure = false;
	LIST_FOR_EACH (struct test_unit, iter, self->tests)
	{
		if (!test_is_allowed (self, iter->name))
			continue;
		if (self->list_only)
			printf ("%s\n", iter->name);
		else if (!test_run_unit (self, iter))
			failure = true;
	}

	LIST_FOR_EACH (struct test_unit, iter, self->tests)
	{
		free (iter->name);
		free (iter);
	}

	str_map_free (&self->whitelist);
	str_map_free (&self->blacklist);
	return failure;
}

// --- Connector ---------------------------------------------------------------

#if defined LIBERTY_WANT_POLLER && defined LIBERTY_WANT_ASYNC

// This is a helper that tries to establish a connection with any address on
// a given list.  Sadly it also introduces a bit of a callback hell.

struct connector_target
{
	LIST_HEADER (struct connector_target)
	struct connector *connector;        ///< Parent connector

	char *hostname;                     ///< Target hostname or address
	char *service;                      ///< Target service name or port

	struct async *getaddrinfo_event;    ///< Address resolution
	struct error *getaddrinfo_error;    ///< Address resolution error

	struct addrinfo *results;           ///< Resolved target
	struct addrinfo *iter;              ///< Current endpoint
};

static struct connector_target *
connector_target_new (void)
{
	struct connector_target *self = xcalloc (1, sizeof *self);
	return self;
}

static void
connector_target_destroy (struct connector_target *self)
{
	if (self->getaddrinfo_event)
		async_cancel (self->getaddrinfo_event);
	if (self->getaddrinfo_error)
		error_free (self->getaddrinfo_error);
	if (self->results)
		freeaddrinfo (self->results);

	free (self->hostname);
	free (self->service);
	free (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct connector
{
	struct poller *poller;              ///< Poller
	int socket;                         ///< Socket FD for the connection
	struct poller_fd connected_event;   ///< We've connected or failed
	struct connector_target *targets;   ///< Targets
	struct connector_target *targets_t; ///< Tail of targets

	void *user_data;                    ///< User data for callbacks

	// You may destroy the connector object in these two main callbacks:

	/// Connection has been successfully established;
	/// the hostname is mainly intended for TLS Server Name Indication
	void (*on_connected) (void *user_data, int socket, const char *hostname);
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
	char buf[NI_MAXHOST];

	if (gai_iter)
	{
		// We don't really need this, so we can let it quietly fail
		int err = getnameinfo (gai_iter->ai_addr, gai_iter->ai_addrlen,
			buf, sizeof buf, NULL, 0, NI_NUMERICHOST);
		if (err)
			LOG_FUNC_FAILURE ("getnameinfo", gai_strerror (err));
		else
			real_host = buf;
	}

	char *address = format_host_port_pair (real_host, target->service);
	self->on_connecting (self->user_data, address);
	free (address);
}

static void
connector_notify_connected (struct connector *self, int fd)
{
	set_blocking (fd, true);
	self->on_connected (self->user_data, fd, self->targets->hostname);
}

static void
connector_prepare_next (struct connector *self)
{
	struct connector_target *target = self->targets;
	if (!target->iter || !(target->iter = target->iter->ai_next))
	{
		LIST_UNLINK_WITH_TAIL (self->targets, self->targets_t, target);
		connector_target_destroy (target);
	}
}

static void connector_handle_error (struct connector *self, const char *error);

/// See if there's any target remaining at all -- it can however either still
/// be waiting for address resolution to finish, or have already failed
static bool
connector_check_target (struct connector *self, struct connector_target *target)
{
	if (!target)
		self->on_failure (self->user_data);
	else if (target->getaddrinfo_error)
	{
		connector_notify_connecting (self, target, NULL);
		connector_handle_error (self, target->getaddrinfo_error->message);
	}
	else if (target->results)
		return true;
	return false;
}

static void
connector_step (struct connector *self)
{
	struct connector_target *target = self->targets;
	if (!connector_check_target (self, target))
		return;

	struct addrinfo *gai_iter = target->iter;
	hard_assert (gai_iter != NULL);

	connector_notify_connecting (self, target, gai_iter);

	int fd = socket (gai_iter->ai_family,
		gai_iter->ai_socktype, gai_iter->ai_protocol);
	if (fd == -1)
	{
		connector_handle_error (self, strerror (errno));
		return;
	}

	set_cloexec (fd);
	set_blocking (fd, false);

	int yes = 1;
	soft_assert (setsockopt (fd, SOL_SOCKET, SO_KEEPALIVE,
		&yes, sizeof yes) != -1);

	if (!connect (fd, gai_iter->ai_addr, gai_iter->ai_addrlen))
		connector_notify_connected (self, fd);
	else if (errno == EINPROGRESS)
	{
		self->connected_event.fd = self->socket = fd;
		poller_fd_set (&self->connected_event, POLLOUT);
	}
	else
	{
		connector_handle_error (self, strerror (errno));
		xclose (fd);
	}
}

static void
connector_handle_error (struct connector *self, const char *error)
{
	if (self->on_error)
		self->on_error (self->user_data, error);

	connector_prepare_next (self);
	connector_step (self);
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
		poller_fd_reset (&self->connected_event);
		xclose (self->socket);
		self->socket = -1;

		connector_handle_error (self, strerror (error));
	}
	else
	{
		poller_fd_reset (&self->connected_event);
		self->socket = -1;
		connector_notify_connected (self, pfd->fd);
	}
}

static void
connector_init (struct connector *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);
	self->poller = poller;
	self->socket = -1;
	self->connected_event = poller_fd_make (poller, self->socket);
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

static void
connector_on_getaddrinfo (int err, struct addrinfo *results, void *user_data)
{
	struct connector_target *self = user_data;

	if (err)
	{
		error_set (&self->getaddrinfo_error,
			"%s: %s", "getaddrinfo", gai_strerror (err));
	}

	self->results = self->iter = results;
	self->getaddrinfo_event = NULL;

	// We've been waiting for this address to be resolved
	if (self == self->connector->targets)
		connector_step (self->connector);
}

/// Connection will be attempted asynchronously once you add any target
static void
connector_add_target (struct connector *self,
	const char *hostname, const char *service)
{
	struct connector_target *target = connector_target_new ();
	target->connector = self;
	target->hostname = xstrdup (hostname);
	target->service = xstrdup (service);

	struct addrinfo hints;
	memset (&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;

	struct async_getaddrinfo *gai = async_getaddrinfo
		(&self->poller->common.async, hostname, service, &hints);

	gai->dispatcher = connector_on_getaddrinfo;
	gai->user_data = target;
	target->getaddrinfo_event = &gai->async;

	LIST_APPEND_WITH_TAIL (self->targets, self->targets_t, target);
}

#endif // defined LIBERTY_WANT_POLLER && defined LIBERTY_WANT_ASYNC

// --- Simple network I/O ------------------------------------------------------

enum socket_io_result
{
	SOCKET_IO_OK = 0,                   ///< Completed successfully
	SOCKET_IO_EOF,                      ///< Connection shut down by peer
	SOCKET_IO_ERROR                     ///< Connection error
};

static enum socket_io_result
socket_io_try_read (int socket_fd, struct str *rb)
{
	// Flood protection, cannot afford to read too much at once
	size_t read_limit = rb->len + (1 << 20);
	if (read_limit < rb->len)
		read_limit = SIZE_MAX;

	ssize_t n_read;
	while (rb->len < read_limit)
	{
		str_reserve (rb, 1024);
		n_read = read (socket_fd, rb->str + rb->len,
			rb->alloc - rb->len - 1 /* null byte */);

		if (n_read > 0)
		{
			rb->str[rb->len += n_read] = '\0';
			continue;
		}
		if (n_read == 0)
			return SOCKET_IO_EOF;

		if (errno == EAGAIN)
			return SOCKET_IO_OK;
		if (errno == EINTR)
			continue;

		int errno_save = errno;
		LOG_LIBC_FAILURE ("read");
		errno = errno_save;
		return SOCKET_IO_ERROR;
	}
	return SOCKET_IO_OK;
}

static enum socket_io_result
socket_io_try_write (int socket_fd, struct str *wb)
{
	ssize_t n_written;
	while (wb->len)
	{
		n_written = write (socket_fd, wb->str, wb->len);
		if (n_written >= 0)
		{
			str_remove_slice (wb, 0, n_written);
			continue;
		}

		if (errno == EAGAIN)
			return SOCKET_IO_OK;
		if (errno == EINTR)
			continue;

		int errno_save = errno;
		LOG_LIBC_FAILURE ("write");
		errno = errno_save;
		return SOCKET_IO_ERROR;
	}
	return SOCKET_IO_OK;
}

// --- Advanced configuration --------------------------------------------------

// This is a more powerful configuration format, adding key-value maps and
// simplifying item validation and dynamic handling of changes.  All strings
// must be encoded in UTF-8.
//
// The syntax is roughly described by the following parsing expression grammar:
//
//   config  = entries eof  # as if there were implicit curly braces around
//   entries = (newline* pair)* newline*
//   pair    = key newline* lws '=' newline* value (&endobj / newline / eof)
//   key     = string / !null !boolean lws [A-Za-z_][0-9A-Za-z_]*
//   value   = object / string / integer / null / boolean
//
//   object  = lws '{' entries endobj
//   endobj  = lws '}'
//
//   quoted  = lws '"' (!["\\] char / '\\' escape)* '"'
//           / lws '`' (![`] char)* '`'
//   string  = (quoted)+
//   char    = [\0-\177]  # or any Unicode codepoint in the UTF-8 encoding
//   escape  = [\\"abfnrtv] / [xX][0-9A-Fa-f][0-9A-Fa-f]? / [0-7][0-7]?[0-7]?
//
//   integer = lws [-+]? [0-9]+  # whatever strtoll() accepts on your system
//   null    = lws 'null'
//   boolean = lws 'yes'  / lws 'YES'  / lws 'no'    / lws 'NO'
//           / lws 'on'   / lws 'ON'   / lws 'off'   / lws 'OFF'
//           / lws 'true' / lws 'TRUE' / lws 'false' / lws 'FALSE'
//
//   newline = lws comment? '\n'
//   eof     = lws comment? !.
//   lws     = [ \t\r]*  # linear whitespace (plus CR as it is insignificant)
//   comment = '#' (!'\n' .)*

enum config_item_type
{
	CONFIG_ITEM_NULL,                   ///< No value
	CONFIG_ITEM_OBJECT,                 ///< Key-value map
	CONFIG_ITEM_BOOLEAN,                ///< Truth value
	CONFIG_ITEM_INTEGER,                ///< Integer
	CONFIG_ITEM_STRING,                 ///< Arbitrary string of characters
	CONFIG_ITEM_STRING_ARRAY            ///< Comma-separated list of strings
};

struct config_item
{
	enum config_item_type type;         ///< Type of the item
	union
	{
		struct str_map object;          ///< Key-value data
		bool boolean;                   ///< Boolean data
		int64_t integer;                ///< Integer data
		struct str string;              ///< String data
	}
	value;                              ///< The value of this item

	const struct config_schema *schema; ///< Schema describing this value
	void *user_data;                    ///< User value attached by schema owner
};

struct config_schema
{
	const char *name;                   ///< Name of the item
	const char *comment;                ///< User-readable description

	enum config_item_type type;         ///< Required type
	const char *default_;               ///< Default as a configuration snippet

	/// Check if the new value can be accepted.
	/// In addition to this, "type" and having a default is considered.
	bool (*validate) (const struct config_item *, struct error **e);

	/// The value has changed
	void (*on_change) (struct config_item *);
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static const char *
config_item_type_name (enum config_item_type type)
{
	switch (type)
	{
	case CONFIG_ITEM_NULL:          return "null";
	case CONFIG_ITEM_BOOLEAN:       return "boolean";
	case CONFIG_ITEM_INTEGER:       return "integer";
	case CONFIG_ITEM_STRING:        return "string";
	case CONFIG_ITEM_STRING_ARRAY:  return "string array";

	default:
		hard_assert (!"invalid config item type value");
		return NULL;
	}
}

static bool
config_item_type_is_string (enum config_item_type type)
{
	return type == CONFIG_ITEM_STRING
		|| type == CONFIG_ITEM_STRING_ARRAY;
}

static void
config_item_free (struct config_item *self)
{
	switch (self->type)
	{
	case CONFIG_ITEM_STRING:
	case CONFIG_ITEM_STRING_ARRAY:
		str_free (&self->value.string);
		break;
	case CONFIG_ITEM_OBJECT:
		str_map_free (&self->value.object);
	default:
		break;
	}
}

static void
config_item_destroy (struct config_item *self)
{
	config_item_free (self);
	free (self);
}

/// Doesn't do any validations or handle schemas, just moves source data
/// to the target item and destroys the source item
static void
config_item_move (struct config_item *self, struct config_item *source)
{
	// Not quite sure how to handle that
	hard_assert (!source->schema);

	config_item_free (self);
	self->type = source->type;
	memcpy (&self->value, &source->value, sizeof source->value);
	free (source);
}

static struct config_item *
config_item_new (enum config_item_type type)
{
	struct config_item *self = xcalloc (1, sizeof *self);
	self->type = type;
	return self;
}

static struct config_item *
config_item_null (void)
{
	return config_item_new (CONFIG_ITEM_NULL);
}

static struct config_item *
config_item_boolean (bool b)
{
	struct config_item *self = config_item_new (CONFIG_ITEM_BOOLEAN);
	self->value.boolean = b;
	return self;
}

static struct config_item *
config_item_integer (int64_t i)
{
	struct config_item *self = config_item_new (CONFIG_ITEM_INTEGER);
	self->value.integer = i;
	return self;
}

static struct config_item *
config_item_string (const struct str *s)
{
	struct config_item *self = config_item_new (CONFIG_ITEM_STRING);
	self->value.string = str_make ();
	hard_assert (utf8_validate
		(self->value.string.str, self->value.string.len));
	if (s) str_append_str (&self->value.string, s);
	return self;
}

static struct config_item *
config_item_string_from_cstr (const char *s)
{
	struct str tmp = str_make ();
	str_append (&tmp, s);
	struct config_item *self = config_item_string (&tmp);
	str_free (&tmp);
	return self;
}

static struct config_item *
config_item_string_array (const struct str *s)
{
	struct config_item *self = config_item_string (s);
	self->type = CONFIG_ITEM_STRING_ARRAY;
	return self;
}

static struct config_item *
config_item_object (void)
{
	struct config_item *self = config_item_new (CONFIG_ITEM_OBJECT);
	self->value.object = str_map_make ((str_map_free_fn) config_item_destroy);
	return self;
}

static bool
config_schema_accepts_type
	(const struct config_schema *self, enum config_item_type type)
{
	if (self->type == type)
		return true;
	// This is a bit messy but it has its purpose
	if (config_item_type_is_string (self->type)
	 && config_item_type_is_string (type))
		return true;
	return !self->default_ && type == CONFIG_ITEM_NULL;
}

static bool
config_item_validate_by_schema (struct config_item *self,
	const struct config_schema *schema, struct error **e)
{
	struct error *error = NULL;
	if (!config_schema_accepts_type (schema, self->type))
		error_set (e, "invalid type of value, expected: %s%s",
			config_item_type_name (schema->type),
			!schema->default_ ? " (or null)" : "");
	else if (schema->validate && !schema->validate (self, &error))
	{
		error_set (e, "%s: %s", "invalid value", error->message);
		error_free (error);
	}
	else
		return true;
	return false;
}

static bool
config_item_set_from (struct config_item *self, struct config_item *source,
	struct error **e)
{
	const struct config_schema *schema = self->schema;
	if (!schema)
	{
		// Easy, we don't know what this item is
		config_item_move (self, source);
		return true;
	}

	source->user_data = self->user_data;
	if (!config_item_validate_by_schema (source, schema, e))
		return false;

	// Make sure the string subtype fits the schema
	if (config_item_type_is_string (source->type)
	 && config_item_type_is_string (schema->type))
		source->type = schema->type;

	config_item_move (self, source);

	// Notify owner about the change so that they can apply it
	if (schema->on_change)
		schema->on_change (self);
	return true;
}

static struct config_item *
config_item_get (struct config_item *self, const char *path, struct error **e)
{
	hard_assert (self->type == CONFIG_ITEM_OBJECT);

	struct strv v = strv_make ();
	cstr_split (path, ".", false, &v);

	struct config_item *result = NULL;
	size_t i = 0;
	while (true)
	{
		const char *key = v.vector[i];
		if (!*key)
			error_set (e, "empty path element");
		else if (!(self = str_map_find (&self->value.object, key)))
			error_set (e, "`%s' not found in object", key);
		else if (++i == v.len)
			result = self;
		else if (self->type != CONFIG_ITEM_OBJECT)
			error_set (e, "`%s' is not an object", key);
		else
			continue;
		break;
	}
	strv_free (&v);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct config_writer
{
	struct str *output;
	unsigned indent;
};

static void config_item_write_object_innards
	(struct config_writer *self, struct config_item *object);

static void
config_item_write_string (struct str *output, const struct str *s)
{
	str_append_c (output, '"');
	for (size_t i = 0; i < s->len; i++)
	{
		unsigned char c = s->str[i];
		if      (c == '\n')  str_append (output, "\\n");
		else if (c == '\r')  str_append (output, "\\r");
		else if (c == '\t')  str_append (output, "\\t");
		else if (c == '\\')  str_append (output, "\\\\");
		else if (c == '"')   str_append (output, "\\\"");
		else if (iscntrl_ascii (c))
			str_append_printf (output, "\\x%02x", c);
		else
			str_append_c (output, c);
	}
	str_append_c (output, '"');
}

static void
config_item_write_object
	(struct config_writer *self, struct config_item *value)
{
	char indent[self->indent + 1];
	memset (indent, '\t', self->indent);
	indent[self->indent] = 0;

	str_append_c (self->output, '{');
	if (value->value.object.len)
	{
		self->indent++;
		str_append_c (self->output, '\n');
		config_item_write_object_innards (self, value);
		self->indent--;
		str_append (self->output, indent);
	}
	str_append_c (self->output, '}');
}

static void
config_item_write_value (struct config_writer *self, struct config_item *value)
{
	switch (value->type)
	{
	case CONFIG_ITEM_NULL:
		str_append (self->output, "null");
		break;
	case CONFIG_ITEM_BOOLEAN:
		str_append (self->output, value->value.boolean ? "on" : "off");
		break;
	case CONFIG_ITEM_INTEGER:
		str_append_printf (self->output, "%" PRIi64, value->value.integer);
		break;
	case CONFIG_ITEM_STRING:
	case CONFIG_ITEM_STRING_ARRAY:
		config_item_write_string (self->output, &value->value.string);
		break;
	case CONFIG_ITEM_OBJECT:
		config_item_write_object (self, value);
		break;
	default:
		hard_assert (!"invalid item type");
	}
}

// FIXME: shuffle code so that this isn't needed (serializer after the parser)
static bool config_tokenizer_is_word_char (int c);

static void
config_item_write_kv_pair (struct config_writer *self,
	const char *key, struct config_item *value)
{
	char indent[self->indent + 1];
	memset (indent, '\t', self->indent);
	indent[self->indent] = 0;

	if (value->schema && value->schema->comment)
		str_append_printf (self->output,
			"%s# %s\n", indent, value->schema->comment);

	char *end = NULL;
	bool can_use_word = ((void) strtoll (key, &end, 10), end == key);
	for (const char *p = key; *p; p++)
		if (!config_tokenizer_is_word_char (*p))
			can_use_word = false;

	str_append (self->output, indent);
	if (can_use_word)
		str_append (self->output, key);
	else
	{
		struct str s = { .str = (char *) key, .len = strlen (key) };
		config_item_write_string (self->output, &s);
	}

	str_append (self->output, " = ");
	config_item_write_value (self, value);
	str_append_c (self->output, '\n');
}

static void
config_item_write_object_innards
	(struct config_writer *self, struct config_item *object)
{
	hard_assert (object->type == CONFIG_ITEM_OBJECT);

	struct str_map_iter iter = str_map_iter_make (&object->value.object);
	struct config_item *value;
	while ((value = str_map_iter_next (&iter)))
		config_item_write_kv_pair (self, iter.link->key, value);
}

static void
config_item_write (struct config_item *value,
	bool object_innards, struct str *output)
{
	struct config_writer writer = { .output = output, .indent = 0 };
	if (object_innards)
		config_item_write_object_innards (&writer, value);
	else
		config_item_write_value (&writer, value);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum config_token
{
	CONFIG_T_ABORT,                     ///< EOF or error

	CONFIG_T_WORD,                      ///< [a-zA-Z0-9_]+
	CONFIG_T_EQUALS,                    ///< Equal sign
	CONFIG_T_LBRACE,                    ///< Left curly bracket
	CONFIG_T_RBRACE,                    ///< Right curly bracket
	CONFIG_T_NEWLINE,                   ///< New line

	CONFIG_T_NULL,                      ///< CONFIG_ITEM_NULL
	CONFIG_T_BOOLEAN,                   ///< CONFIG_ITEM_BOOLEAN
	CONFIG_T_INTEGER,                   ///< CONFIG_ITEM_INTEGER
	CONFIG_T_STRING                     ///< CONFIG_ITEM_STRING{,_LIST}
};

static const char *
config_token_name (enum config_token token)
{
	switch (token)
	{
	case CONFIG_T_ABORT:    return "end of input";

	case CONFIG_T_WORD:     return "word";
	case CONFIG_T_EQUALS:   return "equal sign";
	case CONFIG_T_LBRACE:   return "left brace";
	case CONFIG_T_RBRACE:   return "right brace";
	case CONFIG_T_NEWLINE:  return "newline";

	case CONFIG_T_NULL:     return "null value";
	case CONFIG_T_BOOLEAN:  return "boolean";
	case CONFIG_T_INTEGER:  return "integer";
	case CONFIG_T_STRING:   return "string";

	default:
		hard_assert (!"invalid token value");
		return NULL;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct config_tokenizer
{
	const char *p;                      ///< Current position in input
	size_t len;                         ///< How many bytes of input are left

	bool report_line;                   ///< Whether to count lines at all
	unsigned line;                      ///< Current line
	unsigned column;                    ///< Current column

	int64_t integer;                    ///< Parsed boolean or integer value
	struct str string;                  ///< Parsed string value
};

/// Input has to be null-terminated anyway
static struct config_tokenizer
config_tokenizer_make (const char *p, size_t len)
{
	return (struct config_tokenizer)
		{ .p = p, .len = len, .report_line = true, .string = str_make () };
}

static void
config_tokenizer_free (struct config_tokenizer *self)
{
	str_free (&self->string);
}

static bool
config_tokenizer_is_word_char (int c)
{
	return isalnum_ascii (c) || c == '_';
}

static int
config_tokenizer_advance (struct config_tokenizer *self)
{
	int c = *self->p++;
	if (c == '\n' && self->report_line)
	{
		self->column = 0;
		self->line++;
	}
	else
		self->column++;

	self->len--;
	return c;
}

static void config_tokenizer_error (struct config_tokenizer *self,
	struct error **e, const char *format, ...) ATTRIBUTE_PRINTF (3, 4);

static void
config_tokenizer_error (struct config_tokenizer *self,
	struct error **e, const char *format, ...)
{
	struct str description = str_make ();

	va_list ap;
	va_start (ap, format);
	str_append_vprintf (&description, format, ap);
	va_end (ap);

	if (self->report_line)
		error_set (e, "near line %u, column %u: %s",
			self->line + 1, self->column + 1, description.str);
	else if (self->len)
		error_set (e, "near character %u: %s",
			self->column + 1, description.str);
	else
		error_set (e, "near end: %s", description.str);

	str_free (&description);
}

static bool
config_tokenizer_hexa_escape (struct config_tokenizer *self, struct str *output)
{
	int i;
	unsigned char code = 0;

	for (i = 0; self->len && i < 2; i++)
	{
		unsigned char c = tolower_ascii (*self->p);
		if (c >= '0' && c <= '9')
			code = (code << 4) | (c - '0');
		else if (c >= 'a' && c <= 'f')
			code = (code << 4) | (c - 'a' + 10);
		else
			break;

		config_tokenizer_advance (self);
	}

	if (!i)
		return false;

	str_append_c (output, code);
	return true;
}

static bool
config_tokenizer_octal_escape
	(struct config_tokenizer *self, struct str *output)
{
	int i;
	unsigned char code = 0;

	for (i = 0; self->len && i < 3; i++)
	{
		unsigned char c = *self->p;
		if (c >= '0' && c <= '7')
			code = (code << 3) | (c - '0');
		else
			break;

		config_tokenizer_advance (self);
	}

	if (!i)
		return false;

	str_append_c (output, code);
	return true;
}

static bool
config_tokenizer_escape_sequence
	(struct config_tokenizer *self, struct str *output, struct error **e)
{
	if (!self->len)
	{
		config_tokenizer_error (self, e, "premature end of escape sequence");
		return false;
	}

	unsigned char c;
	switch ((c = *self->p))
	{
	case '"':              break;
	case '\\':             break;
	case 'a':   c = '\a';  break;
	case 'b':   c = '\b';  break;
	case 'f':   c = '\f';  break;
	case 'n':   c = '\n';  break;
	case 'r':   c = '\r';  break;
	case 't':   c = '\t';  break;
	case 'v':   c = '\v';  break;

	case 'x':
	case 'X':
		config_tokenizer_advance (self);
		if (config_tokenizer_hexa_escape (self, output))
			return true;

		config_tokenizer_error (self, e, "invalid hexadecimal escape");
		return false;

	default:
		if (config_tokenizer_octal_escape (self, output))
			return true;

		config_tokenizer_error (self, e, "unknown escape sequence");
		return false;
	}

	str_append_c (output, c);
	config_tokenizer_advance (self);
	return true;
}

static bool
config_tokenizer_dq_string (struct config_tokenizer *self, struct str *output,
	struct error **e)
{
	unsigned char c = config_tokenizer_advance (self);
	while (self->len)
	{
		if ((c = config_tokenizer_advance (self)) == '"')
			return true;
		if (c != '\\')
			str_append_c (output, c);
		else if (!config_tokenizer_escape_sequence (self, output, e))
			return false;
	}
	config_tokenizer_error (self, e, "premature end of string");
	return false;
}

static bool
config_tokenizer_bt_string (struct config_tokenizer *self, struct str *output,
	struct error **e)
{
	unsigned char c = config_tokenizer_advance (self);
	while (self->len)
	{
		if ((c = config_tokenizer_advance (self)) == '`')
			return true;
		str_append_c (output, c);
	}
	config_tokenizer_error (self, e, "premature end of string");
	return false;
}

static bool
config_tokenizer_string (struct config_tokenizer *self, struct str *output,
	struct error **e)
{
	// Go-like strings, with C/AWK-like automatic concatenation
	while (self->len)
	{
		bool ok = true;
		if (isspace_ascii (*self->p) && *self->p != '\n')
			config_tokenizer_advance (self);
		else if (*self->p == '"')
			ok = config_tokenizer_dq_string (self, output, e);
		else if (*self->p == '`')
			ok = config_tokenizer_bt_string (self, output, e);
		else
			break;

		if (!ok)
			return false;
	}
	return true;
}

static enum config_token
config_tokenizer_next (struct config_tokenizer *self, struct error **e)
{
	// Skip over any whitespace between tokens
	while (self->len && isspace_ascii (*self->p) && *self->p != '\n')
		config_tokenizer_advance (self);
	if (!self->len)
		return CONFIG_T_ABORT;

	switch (*self->p)
	{
	case '\n':  config_tokenizer_advance (self);  return CONFIG_T_NEWLINE;
	case '=':   config_tokenizer_advance (self);  return CONFIG_T_EQUALS;
	case '{':   config_tokenizer_advance (self);  return CONFIG_T_LBRACE;
	case '}':   config_tokenizer_advance (self);  return CONFIG_T_RBRACE;

	case '#':
		// Comments go until newline
		while (self->len)
			if (config_tokenizer_advance (self) == '\n')
				return CONFIG_T_NEWLINE;
		return CONFIG_T_ABORT;

	case '"':
	case '`':
		str_reset (&self->string);
		if (!config_tokenizer_string (self, &self->string, e))
			return CONFIG_T_ABORT;
		if (!utf8_validate (self->string.str, self->string.len))
		{
			config_tokenizer_error (self, e, "not a valid UTF-8 string");
			return CONFIG_T_ABORT;
		}
		return CONFIG_T_STRING;
	}

	// Our input doesn't need to be NUL-terminated but we want to use strtoll()
	char buf[48] = "", *end = buf;
	size_t buf_len = MIN (sizeof buf - 1, self->len);

	errno = 0;
	self->integer = strtoll (strncpy (buf, self->p, buf_len), &end, 10);
	if (errno == ERANGE)
	{
		config_tokenizer_error (self, e, "integer out of range");
		return CONFIG_T_ABORT;
	}
	if (end != buf)
	{
		self->len -= end - buf;
		self->p += end - buf;
		return CONFIG_T_INTEGER;
	}

	if (!config_tokenizer_is_word_char (*self->p))
	{
		config_tokenizer_error (self, e, "invalid input");
		return CONFIG_T_ABORT;
	}

	str_reset (&self->string);
	do
		str_append_c (&self->string, config_tokenizer_advance (self));
	while (self->len && config_tokenizer_is_word_char (*self->p));

	if (!strcmp (self->string.str, "null"))
		return CONFIG_T_NULL;

	bool boolean;
	if (!set_boolean_if_valid (&boolean, self->string.str))
		return CONFIG_T_WORD;

	self->integer = boolean;
	return CONFIG_T_BOOLEAN;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct config_parser
{
	struct config_tokenizer tokenizer;  ///< Tokenizer

	struct error *error;                ///< Tokenizer error
	enum config_token token;            ///< Current token in the tokenizer
	bool replace_token;                 ///< Replace the token
};

static struct config_parser
config_parser_make (const char *script, size_t len)
{
	// As reading in tokens may cause exceptions, we wait for the first peek()
	// to replace the initial CONFIG_T_ABORT.
	return (struct config_parser)
	{
		.tokenizer = config_tokenizer_make (script, len),
		.replace_token = true,
	};
}

static void
config_parser_free (struct config_parser *self)
{
	config_tokenizer_free (&self->tokenizer);
	if (self->error)
		error_free (self->error);
}

static enum config_token
config_parser_peek (struct config_parser *self, jmp_buf out)
{
	if (self->replace_token)
	{
		self->token = config_tokenizer_next (&self->tokenizer, &self->error);
		if (self->error)
			longjmp (out, 1);
		self->replace_token = false;
	}
	return self->token;
}

static bool
config_parser_accept
	(struct config_parser *self, enum config_token token, jmp_buf out)
{
	return self->replace_token = (config_parser_peek (self, out) == token);
}

static void
config_parser_expect
	(struct config_parser *self, enum config_token token, jmp_buf out)
{
	if (config_parser_accept (self, token, out))
		return;

	config_tokenizer_error (&self->tokenizer, &self->error,
		"unexpected `%s', expected `%s'",
		config_token_name (self->token),
		config_token_name (token));
	longjmp (out, 1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// We don't need no generator, but a few macros will come in handy.
// From time to time C just doesn't have the right features.

#define PEEK()         config_parser_peek   (self, err)
#define ACCEPT(token)  config_parser_accept (self, token, err)
#define EXPECT(token)  config_parser_expect (self, token, err)
#define SKIP_NL()      do {} while (ACCEPT (CONFIG_T_NEWLINE))

static struct config_item *config_parser_parse_object
	(struct config_parser *self, jmp_buf out);

static struct config_item *
config_parser_parse_value (struct config_parser *self, jmp_buf out)
{
	struct config_item *volatile result = NULL;
	jmp_buf err;

	if (setjmp (err))
	{
		if (result)
			config_item_destroy (result);
		longjmp (out, 1);
	}

	if (ACCEPT (CONFIG_T_LBRACE))
	{
		result = config_parser_parse_object (self, out);
		SKIP_NL ();
		EXPECT (CONFIG_T_RBRACE);
		return result;
	}
	if (ACCEPT (CONFIG_T_NULL))
		return config_item_null ();
	if (ACCEPT (CONFIG_T_BOOLEAN))
		return config_item_boolean (self->tokenizer.integer);
	if (ACCEPT (CONFIG_T_INTEGER))
		return config_item_integer (self->tokenizer.integer);
	if (ACCEPT (CONFIG_T_STRING))
		return config_item_string (&self->tokenizer.string);

	config_tokenizer_error (&self->tokenizer, &self->error,
		"unexpected `%s', expected a value",
		config_token_name (self->token));
	longjmp (out, 1);
}

/// Parse a single "key = value" assignment into @a object
static bool
config_parser_parse_kv_pair (struct config_parser *self,
	struct config_item *object, jmp_buf out)
{
	char *volatile key = NULL;
	jmp_buf err;

	if (setjmp (err))
	{
		free (key);
		longjmp (out, 1);
	}

	SKIP_NL ();

	// Either this object's closing right brace if called recursively,
	// or end of file when called on a whole configuration file
	if (PEEK () == CONFIG_T_RBRACE
	 || PEEK () == CONFIG_T_ABORT)
		return false;

	// I'm not sure how to feel about arbitrary keys but here they are
	if (!ACCEPT (CONFIG_T_STRING))
		EXPECT (CONFIG_T_WORD);

	key = xstrdup (self->tokenizer.string.str);
	SKIP_NL ();

	EXPECT (CONFIG_T_EQUALS);
	SKIP_NL ();

	str_map_set (&object->value.object, key,
		config_parser_parse_value (self, err));

	free (key);
	key = NULL;

	if (PEEK () == CONFIG_T_RBRACE
	 || PEEK () == CONFIG_T_ABORT)
		return false;

	EXPECT (CONFIG_T_NEWLINE);
	return true;
}

/// Parse the inside of an object definition
static struct config_item *
config_parser_parse_object (struct config_parser *self, jmp_buf out)
{
	struct config_item *volatile object = config_item_object ();
	jmp_buf err;

	if (setjmp (err))
	{
		config_item_destroy (object);
		longjmp (out, 1);
	}

	while (config_parser_parse_kv_pair (self, object, err))
		;
	return object;
}

#undef PEEK
#undef ACCEPT
#undef EXPECT
#undef SKIP_NL

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Parse a configuration snippet either as an object or a bare value.
/// If it's the latter (@a single_value_only), no newlines may follow.
static struct config_item *
config_item_parse (const char *script, size_t len,
	bool single_value_only, struct error **e)
{
	volatile struct config_parser parser = config_parser_make (script, len);
	struct config_parser *volatile self = (struct config_parser *) &parser;

	struct config_item *volatile object = NULL;
	jmp_buf err;

	if (setjmp (err))
	{
		if (object)
		{
			config_item_destroy (object);
			object = NULL;
		}

		error_propagate (e, parser.error);
		parser.error = NULL;
		goto end;
	}

	if (single_value_only)
	{
		// This is really only intended for in-program configuration
		// and telling the line number would look awkward
		parser.tokenizer.report_line = false;
		object = config_parser_parse_value (self, err);
	}
	else
		object = config_parser_parse_object (self, err);
	config_parser_expect (self, CONFIG_T_ABORT, err);
end:
	config_parser_free (self);
	return object;
}

/// Clone an item.  Schema assignments aren't retained.
static struct config_item *
config_item_clone (struct config_item *self)
{
	// Oh well, it saves code
	struct str tmp = str_make ();
	config_item_write (self, false, &tmp);
	struct config_item *result =
		config_item_parse (tmp.str, tmp.len, true, NULL);
	str_free (&tmp);
	return result;
}

static struct config_item *
config_read_from_file (const char *filename, struct error **e)
{
	struct config_item *root = NULL;

	struct str data = str_make ();
	if (!read_file (filename, &data, e))
		goto end;

	struct error *error = NULL;
	if (!(root = config_item_parse (data.str, data.len, false, &error)))
	{
		error_set (e, "parse error in `%s': %s", filename, error->message);
		error_free (error);
	}
end:
	str_free (&data);
	return root;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// "user_data" is passed to allow its immediate use in validation callbacks
static struct config_item *
config_schema_initialize_item (const struct config_schema *schema,
	struct config_item *parent, void *user_data, struct error **warning,
	struct error **e)
{
	hard_assert (parent->type == CONFIG_ITEM_OBJECT);
	struct config_item *item =
		str_map_find (&parent->value.object, schema->name);

	if (item)
	{
		struct error *error = NULL;
		item->user_data = user_data;
		if (config_item_validate_by_schema (item, schema, &error))
			goto keep_current;

		error_set (warning, "resetting configuration item "
			"`%s' to default: %s", schema->name, error->message);
		error_free (error);
	}

	struct error *error = NULL;
	if (schema->default_)
		item = config_item_parse
			(schema->default_, strlen (schema->default_), true, &error);
	else
		item = config_item_null ();

	if (item)
		item->user_data = user_data;

	if (error || !config_item_validate_by_schema (item, schema, &error))
	{
		error_set (e, "invalid default for configuration item `%s': %s",
			schema->name, error->message);
		error_free (error);

		if (item)
			config_item_destroy (item);
		return NULL;
	}

	// This will free the old item if there was any
	str_map_set (&parent->value.object, schema->name, item);

keep_current:
	// Make sure the string subtype fits the schema
	if (config_item_type_is_string (item->type)
	 && config_item_type_is_string (schema->type))
		item->type = schema->type;

	item->schema = schema;
	return item;
}

/// Assign schemas and user_data to multiple items at once;
/// feel free to copy over and modify to suit your particular needs
static void
config_schema_apply_to_object (const struct config_schema *schema_array,
	struct config_item *object, void *user_data)
{
	while (schema_array->name)
	{
		struct error *warning = NULL, *e = NULL;
		config_schema_initialize_item
			(schema_array++, object, user_data, &warning, &e);

		if (warning)
		{
			print_warning ("%s", warning->message);
			error_free (warning);
		}
		if (e)
			exit_fatal ("%s", e->message);
	}
}

static void
config_schema_call_changed (struct config_item *item)
{
	if (item->type == CONFIG_ITEM_OBJECT)
	{
		struct str_map_iter iter = str_map_iter_make (&item->value.object);
		struct config_item *child;
		while ((child = str_map_iter_next (&iter)))
			config_schema_call_changed (child);
	}
	else if (item->schema && item->schema->on_change)
		item->schema->on_change (item);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// XXX: the callbacks may be overdesigned and of little to no practical use

typedef void (*config_module_load_fn)
	(struct config_item *subtree, void *user_data);

struct config_module
{
	char *name;                         ///< Name of the subtree
	config_module_load_fn loader;       ///< Module config subtree loader
	void *user_data;                    ///< User data
};

static void
config_module_destroy (struct config_module *self)
{
	free (self->name);
	free (self);
}

struct config
{
	struct str_map modules;             ///< Toplevel modules
	struct config_item *root;           ///< CONFIG_ITEM_OBJECT
};

static struct config
config_make (void)
{
	return (struct config)
		{ .modules = str_map_make ((str_map_free_fn) config_module_destroy) };
}

static void
config_free (struct config *self)
{
	str_map_free (&self->modules);
	if (self->root)
		config_item_destroy (self->root);
}

static void
config_register_module (struct config *self,
	const char *name, config_module_load_fn loader, void *user_data)
{
	struct config_module *module = xcalloc (1, sizeof *module);
	module->name = xstrdup (name);
	module->loader = loader;
	module->user_data = user_data;

	str_map_set (&self->modules, name, module);
}

static void
config_load (struct config *self, struct config_item *root)
{
	hard_assert (root->type == CONFIG_ITEM_OBJECT);
	if (self->root)
		config_item_destroy (self->root);
	self->root = root;

	struct str_map_iter iter = str_map_iter_make (&self->modules);
	struct config_module *module;
	while ((module = str_map_iter_next (&iter)))
	{
		struct config_item *subtree = str_map_find
			(&root->value.object, module->name);
		// Silently fix inputs that only a lunatic user could create
		if (!subtree || subtree->type != CONFIG_ITEM_OBJECT)
			str_map_set (&root->value.object, module->name,
				(subtree = config_item_object ()));
		if (module->loader)
			module->loader (subtree, module->user_data);
	}
}

// --- Protocol modules --------------------------------------------------------

#include "liberty-proto.c"
