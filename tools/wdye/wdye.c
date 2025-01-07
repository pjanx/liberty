/*
 * wdye.c: what did you expect: Lua-based Expect tool
 *
 * Copyright (c) 2025, Přemysl Eric Janouch <p@janouch.name>
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

#include "config.h"
#include "../../liberty.c"

#include <math.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <sys/ioctl.h>
#include <termios.h>
#ifdef SOLARIS
#include <stropts.h>
#endif

#ifdef WITH_CURSES
#include <curses.h>
#include <term.h>
#endif

static int64_t
clock_msec (void)
{
#ifdef _POSIX_TIMERS
	struct timespec tp;
	hard_assert (clock_gettime (CLOCK_BEST, &tp) != -1);
	return (int64_t) tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
#else
	struct timeval tp;
	hard_assert (gettimeofday (&tp, NULL) != -1);
	return (int64_t) tp.tv_sec * 1000 + *msec = tp.tv_usec / 1000;
#endif
}

// execvpe is a GNU extension, reimplement it.
static int
execvpe (const char *file, char *const argv[], char *const envp[])
{
	const char *path = getenv ("PATH");
	if (strchr (file, '/') || !path)
		return execve (file, argv, envp);

	struct strv dirs = strv_make ();
	cstr_split (path, ":", false, &dirs);
	char *name = NULL;
	for (size_t i = 0; i < dirs.len; i++)
	{
		cstr_set (&name, xstrdup_printf ("%s/%s",
			*dirs.vector[i] ? dirs.vector[i] : ".", file));
		execve (name, argv, envp);
	}
	strv_free (&dirs);
	return -1;
}

// This is a particularly inefficient algorithm, but it can match binary data.
static const char *
str_memmem (const struct str *haystack, const struct str *needle, bool nocase)
{
	if (haystack->len < needle->len)
		return NULL;

	char *xhaystack = xmalloc (haystack->len + 1);
	char *xneedle = xmalloc (needle->len + 1);
	if (nocase)
	{
		for (size_t i = 0; i <= haystack->len; i++)
			xhaystack[i] = tolower ((uint8_t) haystack->str[i]);
		for (size_t i = 0; i <= needle->len; i++)
			xneedle[i] = tolower ((uint8_t) needle->str[i]);
	}
	else
	{
		memcpy (xhaystack, haystack->str, haystack->len + 1);
		memcpy (xneedle, needle->str, needle->len + 1);
	}

	const char *result = NULL;
	for (size_t i = 0, end = haystack->len - needle->len; i <= end; i++)
		if (!memcmp (xhaystack + i, xneedle, needle->len))
		{
			result = haystack->str + i;
			break;
		}

	free (xhaystack);
	free (xneedle);
	return result;
}

// --- Pseudoterminal ----------------------------------------------------------
// This is largely taken from Advanced Programming in the UNIX® Environment,
// just without a bunch of bugs.

static int
ptym_open (char **pts_name)
{
	int fdm = -1, err = 0;
	if ((fdm = posix_openpt (O_RDWR | O_NOCTTY)) < 0)
		return -1;
	if (grantpt (fdm) < 0
	 || unlockpt (fdm) < 0)
		goto errout;

	char *ptr = NULL;
	if ((ptr = ptsname (fdm)) == NULL)
		goto errout;

	cstr_set (pts_name, xstrdup (ptr));
	return fdm;

errout:
	err = errno;
	xclose (fdm);
	errno = err;
	return -1;
}

static int
ptys_open (const char *pts_name)
{
	int fds = -1;
#ifdef SOLARIS
	int err = 0, setup = 0;
#endif
	if ((fds = open (pts_name, O_RDWR)) < 0)
		return -1;
#ifdef SOLARIS
	if ((setup = ioctl (fds, I_FIND, "ldterm")) < 0)
		goto errout;
	if (setup == 0)
	{
		if (ioctl (fds, I_PUSH, "ptem") < 0
		 || ioctl (fds, I_PUSH, "ldterm") < 0)
			goto errout;

		if (ioctl (fds, I_PUSH, "ttcompat") < 0)
		{
errout:
			err = errno;
			xclose (fds);
			errno = err;
			return -1;
		}
	}
#endif
	return fds;
}

static pid_t
pty_fork (int *ptrfdm, char **slave_name,
	const struct termios *slave_termios, const struct winsize *slave_winsize,
	struct error **e)
{
	int fdm = -1, fds = -1;

	char *pts_name = NULL;
	if ((fdm = ptym_open (&pts_name)) < 0)
	{
		error_set (e, "can't open master pty: %s", strerror (errno));
		return -1;
	}
	if (slave_name != NULL)
		cstr_set (slave_name, xstrdup (pts_name));

	pid_t pid = fork ();
	if (pid < 0)
	{
		error_set (e, "fork: %s", strerror (errno));
		xclose (fdm);
	}
	else if (pid != 0)
		*ptrfdm = fdm;
	else
	{
		if (setsid () < 0)
			exit_fatal ("setsid: %s", strerror (errno));
		if ((fds = ptys_open (pts_name)) < 0)
			exit_fatal ("can't open slave pty: %s", strerror (errno));
		xclose (fdm);

#if defined BSD
		if (ioctl (fds, TIOCSCTTY, (char *) 0) < 0)
			exit_fatal ("TIOCSCTTY: %s", strerror (errno));
#endif

		if (slave_termios != NULL
		 && tcsetattr (fds, TCSANOW, slave_termios) < 0)
			exit_fatal ("tcsetattr error on slave pty: %s", strerror (errno));
		if (slave_winsize != NULL
		 && ioctl (fds, TIOCSWINSZ, slave_winsize) < 0)
			exit_fatal ("TIOCSWINSZ error on slave pty: %s", strerror (errno));

		if (dup2 (fds, STDIN_FILENO) != STDIN_FILENO)
			exit_fatal ("dup2 error to stdin");
		if (dup2 (fds, STDOUT_FILENO) != STDOUT_FILENO)
			exit_fatal ("dup2 error to stdout");
		if (dup2 (fds, STDERR_FILENO) != STDERR_FILENO)
			exit_fatal ("dup2 error to stderr");
		if (fds != STDIN_FILENO && fds != STDOUT_FILENO && fds != STDERR_FILENO)
			xclose (fds);
	}
	free (pts_name);
	return pid;
}

// --- JSON --------------------------------------------------------------------

static void
write_json_string (FILE *output, const char *s, size_t len)
{
	fputc ('"', output);
	for (const char *last = s, *end = s + len; s != end; last = s)
	{
		// Here is where you realize the asciicast format is retarded for using
		// JSON at all.  (Consider multibyte characters at read() boundaries.)
		int32_t codepoint = utf8_decode (&s, end - s);
		if (codepoint < 0)
		{
			s++;
			fprintf (output, "\\uFFFD");
			continue;
		}

		switch (codepoint)
		{
		break; case '"':  fprintf (output, "\\\"");
		break; case '\\': fprintf (output, "\\\\");
		break; case '\b': fprintf (output, "\\b");
		break; case '\f': fprintf (output, "\\f");
		break; case '\n': fprintf (output, "\\n");
		break; case '\r': fprintf (output, "\\r");
		break; case '\t': fprintf (output, "\\t");
		break; default:
			if (!utf8_validate_cp (codepoint))
				fprintf (output, "\\uFFFD");
			else if (codepoint < 32)
				fprintf (output, "\\u%04X", codepoint);
			else
				fwrite (last, 1, s - last, output);
		}
	}
	fputc ('"', output);
}

// --- Global state ------------------------------------------------------------

static struct
{
	lua_State *L;                       ///< Lua state
	lua_Number default_timeout;         ///< Default expect timeout (s)
}
g =
{
	.default_timeout = 10.,
};

static int
xlua_error_handler (lua_State *L)
{
	// Don't add tracebacks when there's already one, and pass nil through.
	const char *string = luaL_optstring (L, 1, NULL);
	if (string && !strchr (string, '\n'))
	{
		luaL_traceback (L, L, string, 1);
		lua_remove (L, 1);
	}
	return 1;
}

static bool
xlua_getfield (lua_State *L, int idx, const char *name,
	int expected, bool optional)
{
	int found = lua_getfield (L, idx, name);
	if (found == expected)
		return true;
	if (optional && found == LUA_TNIL)
		return false;

	const char *message = optional
		? "invalid field \"%s\" (found: %s, expected: %s or nil)"
		: "invalid or missing field \"%s\" (found: %s, expected: %s)";
	return luaL_error (L, message, name,
		lua_typename (L, found), lua_typename (L, expected));
}

static void
xlua_newtablecopy (lua_State *L, int idx, int first, int last)
{
	int len = last - first + 1;
	lua_createtable (L, len, 0);
	if (idx < 0)
		idx--;

	for (lua_Integer i = 0; i < len; i++)
	{
		lua_rawgeti (L, idx, first + i);
		lua_rawseti (L, -2, 1 + i);
	}
}

// --- Patterns ----------------------------------------------------------------

#define XLUA_PATTERN_METATABLE "pattern"

enum pattern_kind
{
	PATTERN_REGEX,                      ///< Regular expression match
	PATTERN_EXACT,                      ///< Literal string match
	PATTERN_TIMEOUT,                    ///< Timeout
	PATTERN_EOF,                        ///< EOF condition
	PATTERN_DEFAULT,                    ///< Either timeout or EOF condition
};

struct pattern
{
	enum pattern_kind kind;             ///< Tag
	int ref_process;                    ///< Process for all except TIMEOUT
	struct process *process;            ///< Weak pointer to the process
	regex_t *regex;                     ///< Regular expression for REGEX
	struct str exact;                   ///< Exact match literal for EXACT
	lua_Number timeout;                 ///< Timeout for TIMEOUT/DEFAULT (s)
	bool nocase;                        ///< Case insensitive search
	bool notransfer;                    ///< Do not consume process buffer
	int ref_values;                     ///< Return values as a table reference

	// Patterns are constructed in place, used once, and forgotten,
	// so we can just shove anything extra in here.

	struct error *e;                    ///< Error buffer
	struct str input;                   ///< Matched input
	regmatch_t *matches;                ///< Match indexes within the input
	bool eof;                           ///< End of file seen
};

static struct pattern *
pattern_new (lua_State *L, enum pattern_kind kind, int idx_process)
{
	struct pattern *self = lua_newuserdata (L, sizeof *self);
	luaL_setmetatable (L, XLUA_PATTERN_METATABLE);
	memset (self, 0, sizeof *self);

	self->kind = kind;
	self->ref_process = LUA_NOREF;
	self->exact = str_make ();
	self->timeout = -1.;
	self->ref_values = LUA_NOREF;
	self->input = str_make ();

	if (idx_process)
	{
		lua_pushvalue (L, idx_process);
		self->process = lua_touserdata (L, -1);
		self->ref_process = luaL_ref (L, LUA_REGISTRYINDEX);
	}
	return self;
}

static int
xlua_pattern_gc (lua_State *L)
{
	struct pattern *self = luaL_checkudata (L, 1, XLUA_PATTERN_METATABLE);
	luaL_unref (L, LUA_REGISTRYINDEX, self->ref_process);
	if (self->regex)
		regex_free (self->regex);
	str_free (&self->exact);
	luaL_unref (L, LUA_REGISTRYINDEX, self->ref_values);
	if (self->e)
		error_free (self->e);
	str_free (&self->input);
	free (self->matches);
	return 0;
}

static int
xlua_pattern_index (lua_State *L)
{
	struct pattern *self = luaL_checkudata (L, 1, XLUA_PATTERN_METATABLE);
	if (!lua_isinteger (L, 2))
	{
		const char *key = luaL_checkstring (L, 2);
		if (!strcmp (key, "process"))
			lua_rawgeti (L, LUA_REGISTRYINDEX, self->ref_process);
		else
			return luaL_error (L, "not a readable property: %s", key);
		return 1;
	}

	lua_Integer group = lua_tointeger (L, 2);
	switch (self->kind)
	{
	case PATTERN_REGEX:
	{
		const regmatch_t *m = self->matches + group;
		if (group < 0 || (size_t) group > self->regex->re_nsub
		 || m->rm_so < 0 || m->rm_eo < 0 || (size_t) m->rm_eo > self->input.len)
			lua_pushnil (L);
		else
			lua_pushlstring (L,
				self->input.str + m->rm_so, m->rm_eo - m->rm_so);
		return 1;
	}
	case PATTERN_EXACT:
	case PATTERN_EOF:
	case PATTERN_DEFAULT:
		if (group != 0)
			lua_pushnil (L);
		else
			lua_pushlstring (L, self->input.str, self->input.len);
		return 1;
	default:
		return luaL_argerror (L, 1, "indexing unavailable for this pattern");
	}
}

static bool
pattern_readtimeout (struct pattern *self, lua_State *L, int idx)
{
	lua_rawgeti (L, idx, 1);
	bool ok = lua_isnumber (L, -1);
	lua_Number v = lua_tonumber (L, -1);
	lua_pop (L, 1);
	if (v != v)
		luaL_error (L, "timeout is not a number");
	if (ok)
		self->timeout = v;
	return ok;
}

static void
pattern_readflags (struct pattern *self, lua_State *L, int idx)
{
	lua_getfield (L, idx, "nocase");
	self->nocase = lua_toboolean (L, -1);
	lua_getfield (L, idx, "notransfer");
	self->notransfer = lua_toboolean (L, -1);
	lua_pop (L, 2);
}

static luaL_Reg xlua_pattern_table[] =
{
	{ "__gc",       xlua_pattern_gc       },
	{ "__index",    xlua_pattern_index    },
	{ NULL,         NULL                  }
};

// --- Process -----------------------------------------------------------------

#define XLUA_PROCESS_METATABLE "process"

struct process
{
	int terminal_fd;                    ///< Process stdin/stdout/stderr
	pid_t pid;                          ///< Process ID or -1 if collected
	int ref_term;                       ///< Terminal information
	struct str buffer;                  ///< Terminal input buffer
	int status;                         ///< Process status iff pid is -1

	int64_t start;                      ///< Start timestamp (Unix msec)
	FILE *asciicast;                    ///< asciicast script dump
};

static struct process *
process_new (lua_State *L)
{
	struct process *self = lua_newuserdata (L, sizeof *self);
	luaL_setmetatable (L, XLUA_PROCESS_METATABLE);
	memset (self, 0, sizeof *self);

	self->terminal_fd = -1;
	self->pid = -1;
	self->ref_term = LUA_NOREF;
	self->buffer = str_make ();
	return self;
}

static int
xlua_process_gc (lua_State *L)
{
	struct process *self = luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	if (self->terminal_fd != -1)
		xclose (self->terminal_fd);
	if (self->pid != -1)
		// The slave is in its own process group.
		kill (-self->pid, SIGKILL);
	luaL_unref (L, LUA_REGISTRYINDEX, self->ref_term);
	str_free (&self->buffer);
	if (self->asciicast)
		fclose (self->asciicast);
	return 0;
}

static int
xlua_process_index (lua_State *L)
{
	struct process *self = luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	const char *key = luaL_checkstring (L, 2);
	if (*key != '_' && luaL_getmetafield (L, 1, key))
		return 1;

	if (!strcmp (key, "buffer"))
		lua_pushlstring (L, self->buffer.str, self->buffer.len);
	else if (!strcmp (key, "pid"))
		lua_pushinteger (L, self->pid);
	else if (!strcmp (key, "term"))
		lua_rawgeti (L, LUA_REGISTRYINDEX, self->ref_term);
	else
		return luaL_error (L, "not a readable property: %s", key);
	return 1;
}

static int
xlua_process_send (lua_State *L)
{
	struct process *self = luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	int nargs = lua_gettop (L);
	for (int i = 2; i <= nargs; i++)
		if (!lua_isstring (L, i))
			return luaL_argerror (L, i, "need string arguments");

	for (int i = 2; i <= nargs; i++)
	{
		size_t len = 0;
		const char *arg = lua_tolstring (L, i, &len);
		ssize_t written = write (self->terminal_fd, arg, len);
		if (written == -1)
			return luaL_error (L, "write failed: %s", strerror (errno));
		else if (written != (ssize_t) len)
			return luaL_error (L, "write failed: %s", "short write");

		if (self->asciicast)
		{
			double timestamp = (clock_msec () - self->start) / 1000.;
			fprintf (self->asciicast, "[%f, \"i\", ", timestamp);
			write_json_string (self->asciicast, arg, len);
			fprintf (self->asciicast, "]\n");
		}
	}
	lua_pushvalue (L, 1);
	return 1;
}

static int
xlua_process_regex (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	luaL_checktype (L, 2, LUA_TTABLE);
	if (lua_gettop (L) != 2)
		return luaL_error (L, "too many arguments");

	struct pattern *pattern = pattern_new (L, PATTERN_REGEX, 1);
	pattern_readflags (pattern, L, 2);

	int flags = REG_EXTENDED;
	if (pattern->nocase)
		flags |= REG_ICASE;

	lua_rawgeti (L, 2, 1);
	if (!lua_isstring (L, -1))
		return luaL_error (L, "expected regular expression");

	size_t len = 0;
	const char *re = lua_tolstring (L, -1, &len);
	if (!(pattern->regex = regex_compile (re, flags, &pattern->e)))
		return luaL_error (L, "%s", pattern->e->message);
	lua_pop (L, 1);

	pattern->matches =
		xcalloc (pattern->regex->re_nsub + 1, sizeof *pattern->matches);

	xlua_newtablecopy (L, 2, 2, lua_rawlen (L, 2));
	pattern->ref_values = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}

static int
xlua_process_exact (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	luaL_checktype (L, 2, LUA_TTABLE);
	if (lua_gettop (L) != 2)
		return luaL_error (L, "too many arguments");

	struct pattern *pattern = pattern_new (L, PATTERN_EXACT, 1);
	pattern_readflags (pattern, L, 2);

	lua_rawgeti (L, 2, 1);
	if (!lua_isstring (L, -1))
		return luaL_error (L, "expected string literal");

	size_t len = 0;
	const char *literal = lua_tolstring (L, -1, &len);
	str_append_data (&pattern->exact, literal, len);
	lua_pop (L, 1);

	xlua_newtablecopy (L, 2, 2, lua_rawlen (L, 2));
	pattern->ref_values = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}

static int
xlua_process_eof (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	luaL_checktype (L, 2, LUA_TTABLE);
	if (lua_gettop (L) != 2)
		return luaL_error (L, "too many arguments");

	struct pattern *pattern = pattern_new (L, PATTERN_EOF, 1);
	pattern_readflags (pattern, L, 2);

	xlua_newtablecopy (L, 2, 1, lua_rawlen (L, 2));
	pattern->ref_values = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}

static int
xlua_process_default (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	luaL_checktype (L, 2, LUA_TTABLE);
	if (lua_gettop (L) != 2)
		return luaL_error (L, "too many arguments");

	struct pattern *pattern = pattern_new (L, PATTERN_DEFAULT, 1);
	pattern_readflags (pattern, L, 2);

	int first = 1, last = lua_rawlen (L, 2);
	if (pattern_readtimeout (pattern, L, 2))
		first++;

	xlua_newtablecopy (L, 2, first, last);
	pattern->ref_values = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}

static int
xlua_process_wait (lua_State *L)
{
	struct process *self = luaL_checkudata (L, 1, XLUA_PROCESS_METATABLE);
	bool nowait = luaL_opt(L, lua_toboolean, 2, false);
	if (lua_gettop (L) > 2)
		return luaL_error (L, "too many arguments");

	int status = self->status;
restart:
	if (self->pid != -1)
	{
		int options = 0;
		if (nowait)
			options |= WNOHANG;

		pid_t pid = waitpid (self->pid, &status, options);
		if (!pid)
			return 0;

		if (pid < 0)
		{
			if (errno == EINTR)
				goto restart;
			return luaL_error (L, "waitpid: %s", strerror (errno));
		}

		// We lose the ability to reliably kill the whole process group.
		self->status = status;
		self->pid = -1;
	}
	if (WIFEXITED (status))
	{
		lua_pushinteger (L, WEXITSTATUS (status));
		lua_pushinteger (L, WEXITSTATUS (status));
		lua_pushnil (L);
		return 3;
	}
	if (WIFSIGNALED (status))
	{
		lua_pushinteger (L, 128 + WTERMSIG (status));
		lua_pushnil (L);
		lua_pushinteger (L, WTERMSIG (status));
		return 3;
	}
	return 0;
}

static bool
process_feed (struct process *self)
{
	// Let's do this without O_NONBLOCK for now.
	char buf[BUFSIZ] = "";
	ssize_t n = read (self->terminal_fd, buf, sizeof buf);
	if (n < 0)
	{
		if (errno == EINTR)
			return true;
#ifdef __linux__
		// https://unix.stackexchange.com/a/538271
		if (errno == EIO)
			return false;
#endif

		print_warning ("read: %s", strerror (errno));
		return false;
	}

	if (self->asciicast)
	{
		double timestamp = (clock_msec () - self->start) / 1000.;
		fprintf (self->asciicast, "[%f, \"o\", ", timestamp);
		write_json_string (self->asciicast, buf, n);
		fprintf (self->asciicast, "]\n");
	}

	// TODO(p): Add match_max processing, limiting the buffer size.
	str_append_data (&self->buffer, buf, n);
	return n > 0;
}

static luaL_Reg xlua_process_table[] =
{
	{ "__gc",       xlua_process_gc       },
	{ "__index",    xlua_process_index    },
	{ "send",       xlua_process_send     },
	{ "regex",      xlua_process_regex    },
	{ "exact",      xlua_process_exact    },
	{ "eof",        xlua_process_eof      },
	{ "default",    xlua_process_default  },
	{ "wait",       xlua_process_wait     },
	{ NULL,         NULL                  }
};

// --- Terminal ----------------------------------------------------------------

struct terminfo_entry
{
	enum { TERMINFO_BOOLEAN, TERMINFO_NUMERIC, TERMINFO_STRING } kind;
	unsigned numeric;
	char string[];
};

#ifdef WITH_CURSES

static bool
load_terminfo (const char *term, struct str_map *strings)
{
	// Neither ncurses nor NetBSD curses need an actual terminal FD passed.
	// We don't want them to read out the winsize, we just read the database.
	int err = 0;
	TERMINAL *saved_term = set_curterm (NULL);
	if (setupterm ((char *) term, -1, &err) != OK)
	{
		set_curterm (saved_term);
		return false;
	}

	for (size_t i = 0; boolfnames[i]; i++)
	{
		int flag = tigetflag (boolnames[i]);
		if (flag <= 0)
			continue;

		struct terminfo_entry *entry = xcalloc (1, sizeof *entry + 1);
		*entry = (struct terminfo_entry) { TERMINFO_BOOLEAN, true };
		str_map_set (strings, boolfnames[i], entry);
	}
	for (size_t i = 0; numfnames[i]; i++)
	{
		int num = tigetnum (numnames[i]);
		if (num < 0)
			continue;

		struct terminfo_entry *entry = xcalloc (1, sizeof *entry + 1);
		*entry = (struct terminfo_entry) { TERMINFO_NUMERIC, num };
		str_map_set (strings, numfnames[i], entry);
	}
	for (size_t i = 0; strfnames[i]; i++)
	{
		const char *str = tigetstr (strnames[i]);
		if (!str || str == (const char *) -1)
			continue;

		size_t len = strlen (str) + 1;
		struct terminfo_entry *entry = xcalloc (1, sizeof *entry + len);
		*entry = (struct terminfo_entry) { TERMINFO_STRING, 0 };
		memcpy (entry + 1, str, len);
		str_map_set (strings, strfnames[i], entry);
	}
	del_curterm (set_curterm (saved_term));
	return true;
}

#endif

// --- Library -----------------------------------------------------------------

struct spawn_context
{
	struct str_map env;                 ///< Subprocess environment map
	struct str_map term;                ///< terminfo database
	struct strv envv;                   ///< Subprocess environment vector
	struct strv argv;                   ///< Subprocess argument vector

	struct error *error;                ///< Error
};

static struct spawn_context
spawn_context_make (void)
{
	struct spawn_context self = {};
	self.env = str_map_make (free);
	self.term = str_map_make (free);

	// XXX: It might make sense to enable starting from an empty environment.
	for (char **p = environ; *p; p++)
	{
		const char *equals = strchr (*p, '=');
		if (!equals)
			continue;

		char *key = xstrndup (*p, equals - *p);
		str_map_set (&self.env, key, xstrdup (equals + 1));
		free (key);
	}
	self.envv = strv_make ();
	self.argv = strv_make ();
	return self;
}

static void
spawn_context_free (struct spawn_context *self)
{
	str_map_free (&self->env);
	str_map_free (&self->term);
	strv_free (&self->envv);
	strv_free (&self->argv);

	if (self->error)
		error_free (self->error);
}

// -0, +0, e
static void
environ_map_update (struct str_map *env, lua_State *L)
{
	lua_pushnil (L);
	while (lua_next (L, -2))
	{
		if (lua_type (L, -2) != LUA_TSTRING)
			luaL_error (L, "environment maps must be keyed by strings");

		const char *value = lua_tostring (L, -1);
		str_map_set (env, lua_tostring (L, -2),
			value ? xstrdup (value) : NULL);
		lua_pop (L, 1);
	}
}

// The environment will get pseudo-randomly reordered,
// which is fine by POSIX.
static void
environ_map_serialize (struct str_map *env, struct strv *envv)
{
	struct str_map_iter iter = str_map_iter_make (env);
	const char *value;
	while ((value = str_map_iter_next (&iter)))
		strv_append_owned (envv,
			xstrdup_printf ("%s=%s", iter.link->key, value));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int
spawn_protected (lua_State *L)
{
	struct spawn_context *ctx = lua_touserdata (L, 1);

	// Step 1: Prepare process environment.
	if (xlua_getfield (L, 2, "environ", LUA_TTABLE, true))
	{
		environ_map_update (&ctx->env, L);
		lua_pop (L, 1);
	}
	char *term = str_map_find (&ctx->env, "TERM");
	if (!term)
	{
		print_debug ("setting a default TERM");
		str_map_set (&ctx->env, "TERM", (term = xstrdup ("dumb")));
	}
	environ_map_serialize (&ctx->env, &ctx->envv);

#ifdef WITH_CURSES
	// Step 2: Load terminal information.
	if (!load_terminfo (term, &ctx->term))
		luaL_error (L, "failed to initialize terminfo for %s", term);
#endif

	// Step 3: Prepare process command line.
	size_t argc = lua_rawlen (L, 2);
	for (size_t i = 1; i <= argc; i++)
	{
		lua_pushinteger (L, i);
		lua_rawget (L, 2);
		const char *arg = lua_tostring (L, -1);
		if (!arg)
			return luaL_error (L, "spawn arguments must be strings");

		strv_append (&ctx->argv, arg);
		lua_pop (L, 1);
	}
	if (ctx->argv.len < 1)
		return luaL_error (L, "missing argument");

	// Step 4: Create a process object.
	// This will get garbage collected as appropriate on failure.
	struct process *process = process_new (L);

	// This could be made into an object that can adjust winsize/termios.
	lua_createtable (L, 0, ctx->term.len);
	struct str_map_iter iter = str_map_iter_make (&ctx->term);
	const struct terminfo_entry *entry = NULL;
	while ((entry = str_map_iter_next (&iter)))
	{
		lua_pushstring (L, iter.link->key);
		switch (entry->kind)
		{
		break; case TERMINFO_BOOLEAN: lua_pushboolean (L, true);
		break; case TERMINFO_NUMERIC: lua_pushinteger (L, entry->numeric);
		break; case TERMINFO_STRING:  lua_pushstring (L, entry->string);
		break; default:               lua_pushnil (L);
		}
		lua_settable (L, -3);
	}
	process->ref_term = luaL_ref (L, LUA_REGISTRYINDEX);

	struct winsize ws = { .ws_row = 24, .ws_col = 80 };
	if ((entry = str_map_find (&ctx->term, "lines"))
	 && entry->kind == TERMINFO_NUMERIC)
		ws.ws_row = entry->numeric;
	if ((entry = str_map_find (&ctx->term, "columns"))
	 && entry->kind == TERMINFO_NUMERIC)
		ws.ws_col = entry->numeric;

	// Step 5: Spawn the process, which gets a new process group.
	process->pid =
		pty_fork (&process->terminal_fd, NULL, NULL, &ws, &ctx->error);
	if (process->pid < 0)
	{
		return luaL_error (L, "failed to spawn %s: %s",
			ctx->argv.vector[0], ctx->error->message);
	}
	if (!process->pid)
	{
		execvpe (ctx->argv.vector[0], ctx->argv.vector, ctx->envv.vector);
		print_error ("failed to spawn %s: %s",
			ctx->argv.vector[0], strerror (errno));
		// Or we could figure out when exactly to use statuses 126 and 127.
		_exit (EXIT_FAILURE);
	}

	// Step 6: Create a log file.
	if (getenv ("WDYE_LOGGING"))
	{
		const char *name = ctx->argv.vector[0];
		const char *last_slash = strrchr (name, '/');
		if (last_slash)
			name = last_slash + 1;

		char *path = xstrdup_printf ("%s-%s.%d.cast",
			PROGRAM_NAME, name, (int) process->pid);
		if (!(process->asciicast = fopen (path, "w")))
			print_warning ("%s: %s", path, strerror (errno));
		free (path);
	}
	process->start = clock_msec ();
	if (process->asciicast)
	{
		fprintf (process->asciicast, "{\"version\": 2, "
			"\"width\": %u, \"height\": %u, \"env\": {\"TERM\": \"%s\"}}\n",
			ws.ws_col, ws.ws_row, term);
	}

	set_cloexec (process->terminal_fd);
	return 1;
}

static int
xlua_spawn (lua_State *L)
{
	luaL_checktype (L, 1, LUA_TTABLE);

	lua_pushcfunction (L, xlua_error_handler);
	lua_pushcfunction (L, spawn_protected);

	// There are way too many opportunities for Lua to throw,
	// so maintain a context to clean up in one go.
	struct spawn_context ctx = spawn_context_make ();
	lua_pushlightuserdata (L, &ctx);
	lua_rotate (L, 1, -1);
	int result = lua_pcall (L, 2, 1, -4);
	spawn_context_free (&ctx);
	if (result)
		return lua_error (L);

	// Remove the error handler ("good programming practice").
	lua_remove (L, -2);
	return 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct expect_context
{
	size_t patterns_len;                ///< Number of patterns
	struct pattern **patterns;          ///< Pattern array
	size_t pfds_len;                    ///< Number of distinct poll FDs
	struct pollfd *pfds;                ///< Distinct poll FDs

	lua_Number first_timeout;           ///< Nearest timeout value
	lua_Number timeout;                 ///< Actually used timeout value
};

static void
expect_context_free (struct expect_context *self)
{
	free (self->patterns);
	free (self->pfds);
}

static bool
expect_has_fd (struct expect_context *ctx, int fd)
{
	for (size_t i = 0; i < ctx->pfds_len; i++)
		if (ctx->pfds[i].fd == fd)
			return true;
	return false;
}

static struct process *
expect_fd_to_process (struct expect_context *ctx, int fd)
{
	for (size_t i = 0; i < ctx->patterns_len; i++)
	{
		struct pattern *p = ctx->patterns[i];
		if (p->process
		 && p->process->terminal_fd == fd)
			return p->process;
	}
	return NULL;
}

static void
expect_set_fd_eof (struct expect_context *ctx, int fd)
{
	for (size_t i = 0; i < ctx->patterns_len; i++)
	{
		struct pattern *p = ctx->patterns[i];
		if (p->process
		 && p->process->terminal_fd == fd)
			p->eof = true;
	}
}

static void
expect_prepare_pattern (struct expect_context *ctx, struct pattern *p)
{
	str_reset (&p->input);
	if (p->kind == PATTERN_REGEX)
		for (size_t i = 0; i <= p->regex->re_nsub; i++)
			p->matches[i] = (regmatch_t) { .rm_so = -1, .rm_eo = -1 };

	if (p->kind == PATTERN_REGEX
	 || p->kind == PATTERN_EXACT
	 || p->kind == PATTERN_EOF
	 || p->kind == PATTERN_DEFAULT)
	{
		p->eof = false;
		if (!expect_has_fd (ctx, p->process->terminal_fd))
			ctx->pfds[ctx->pfds_len++] = (struct pollfd)
				{ .fd = p->process->terminal_fd, .events = POLLIN };
	}
	if (p->kind == PATTERN_TIMEOUT
	 || p->kind == PATTERN_DEFAULT)
	{
		lua_Number v = p->timeout >= 0 ? p->timeout : g.default_timeout;
		if (ctx->first_timeout != ctx->first_timeout)
			ctx->first_timeout = v;
		else
			ctx->first_timeout = MIN (ctx->first_timeout, v);
	}
}

static void
expect_prepare (struct expect_context *ctx)
{
	// The liberty poller is not particularly appropriate for this use case.
	ctx->pfds_len = 0;
	ctx->pfds = xcalloc (ctx->patterns_len, sizeof *ctx->pfds);

	ctx->first_timeout = NAN;
	for (size_t i = 0; i < ctx->patterns_len; i++)
		expect_prepare_pattern (ctx, ctx->patterns[i]);

	// There is always at least a default timeout.
	ctx->timeout = g.default_timeout;
	if (ctx->first_timeout == ctx->first_timeout)
		ctx->timeout = ctx->first_timeout;
}

static struct pattern *
expect_match_timeout (struct expect_context *ctx)
{
	for (size_t i = 0; i < ctx->patterns_len; i++)
	{
		struct pattern *p = ctx->patterns[i];
		if (p->kind != PATTERN_TIMEOUT
		 && p->kind != PATTERN_DEFAULT)
			continue;

		if (p->timeout <= ctx->first_timeout)
			return p;
	}
	return NULL;
}

static bool
pattern_match (struct pattern *self)
{
	struct process *process = self->process;
	struct str *buffer = process ? &process->buffer : NULL;

	str_reset (&self->input);
	switch (self->kind)
	{
	case PATTERN_EOF:
	case PATTERN_DEFAULT:
	{
		if (!self->eof)
			return false;

		str_append_str (&self->input, &process->buffer);
		if (!self->notransfer)
			str_reset (&process->buffer);
		return true;
	}
	case PATTERN_REGEX:
	{
		int flags = 0;
#ifdef REG_STARTEND
		self->matches[0] = (regmatch_t) { .rm_so = 0, .rm_eo = buffer->len };
		flags |= REG_STARTEND;
#endif
		if (regexec (self->regex, buffer->str,
			self->regex->re_nsub + 1, self->matches, flags))
		{
			for (size_t i = 0; i <= self->regex->re_nsub; i++)
				self->matches[i] = (regmatch_t) { .rm_so = -1, .rm_eo = -1 };
			return false;
		}

		str_append_data (&self->input, buffer->str, self->matches[0].rm_eo);
		if (!self->notransfer)
			str_remove_slice (buffer, 0, self->matches[0].rm_eo);
		return true;
	}
	case PATTERN_EXACT:
	{
		const char *match = str_memmem (buffer, &self->exact, self->nocase);
		if (!match)
			return false;

		str_append_data (&self->input, match, self->exact.len);
		if (!self->notransfer)
			str_remove_slice (buffer, 0, match - buffer->str + self->exact.len);
		return true;
	}
	default:
		return false;
	}
}

static struct pattern *
expect_match_data (struct expect_context *ctx)
{
	for (size_t i = 0; i < ctx->patterns_len; i++)
	{
		struct pattern *p = ctx->patterns[i];
		if (pattern_match (p))
			return p;
	}
	return NULL;
}

static int
expect_protected (lua_State *L)
{
	struct expect_context *ctx = lua_touserdata (L, lua_upvalueindex (1));
	ctx->patterns_len = lua_gettop (L);
	ctx->patterns = xcalloc (ctx->patterns_len, sizeof *ctx->patterns);
	for (size_t i = 0; i < ctx->patterns_len; i++)
		ctx->patterns[i] = luaL_checkudata (L, i + 1, XLUA_PATTERN_METATABLE);
	expect_prepare (ctx);

	int64_t deadline = 0;
	struct pattern *match = NULL;
restart:
	// A "continue" statement means we start anew with a new timeout.
	// TODO(p): We should detect deadline > INT64_MAX, and wait indefinitely.
	deadline = clock_msec () + ctx->timeout * 1000;

	// First, check if anything matches already,
	// so that we don't need to wait for /even more/ data.
	match = expect_match_data (ctx);

	while (!match)
	{
		int64_t until_deadline = deadline - clock_msec ();
		int n = poll (ctx->pfds, ctx->pfds_len, MAX (0, until_deadline));
		if (n < 0)
			return luaL_error (L, "poll: %s", strerror (errno));

		for (int i = 0; i < n; i++)
		{
			struct pollfd *pfd = ctx->pfds + i;
			hard_assert (!(pfd->revents & POLLNVAL));
			if (!(pfd->revents & (POLLIN | POLLHUP | POLLERR)))
				continue;

			struct process *process = expect_fd_to_process (ctx, pfd->fd);
			hard_assert (process != NULL);
			if (!process_feed (process))
			{
				expect_set_fd_eof (ctx, pfd->fd);
				// Otherwise we would loop around this descriptor.
				pfd->fd = -1;
			}
		}

		if (n > 0)
			match = expect_match_data (ctx);
		else if (!(match = expect_match_timeout (ctx)))
			return 0;
	}

	// Resolve the matching pattern back to its Lua full userdata.
	int match_idx = 0;
	for (size_t i = 0; i < ctx->patterns_len; i++)
		if (ctx->patterns[i] == match)
			match_idx = i + 1;

	// Filter the values table by executing any functions with the pattern.
	lua_rawgeti (L, LUA_REGISTRYINDEX, match->ref_values);
	int values_idx = lua_gettop (L);
	int values_len = lua_rawlen (L, values_idx);
	lua_checkstack (L, values_len);

	lua_pushcfunction (L, xlua_error_handler);
	int handler_idx = lua_gettop (L);
	for (int i = 1; i <= values_len; i++)
	{
		lua_rawgeti (L, values_idx, i);
		if (!lua_isfunction (L, -1))
			continue;

		lua_pushvalue (L, match_idx);
		if (!lua_pcall (L, 1, LUA_MULTRET, handler_idx))
			continue;
		if (!lua_isnil (L, -1))
			return lua_error (L);

		lua_pop (L, lua_gettop (L) - values_idx + 1);
		goto restart;
	}
	return lua_gettop (L) - handler_idx;
}

static int
xlua_expect (lua_State *L)
{
	lua_pushcfunction (L, xlua_error_handler);
	lua_insert (L, 1);

	struct expect_context ctx = {};
	lua_pushlightuserdata (L, &ctx);
	lua_pushcclosure (L, expect_protected, 1);
	lua_insert (L, 2);

	int result = lua_pcall (L, lua_gettop (L) - 2, LUA_MULTRET, 1);
	expect_context_free (&ctx);
	if (result)
		return lua_error (L);

	// Remove the error handler ("good programming practice").
	lua_remove (L, 1);
	return lua_gettop (L);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int
xlua_timeout (lua_State *L)
{
	luaL_checktype (L, 1, LUA_TTABLE);
	if (lua_gettop (L) != 1)
		return luaL_error (L, "too many arguments");

	struct pattern *pattern = pattern_new (L, PATTERN_TIMEOUT, 0);

	int first = 1, last = lua_rawlen (L, 1);
	if (pattern_readtimeout (pattern, L, 1))
		first++;

	xlua_newtablecopy (L, 1, first, last);
	pattern->ref_values = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}

static int
xlua_continue (lua_State *L)
{
	// xlua_expect() handles this specially.
	lua_pushnil (L);
	return lua_error (L);
}

static luaL_Reg xlua_library[] =
{
	{ "spawn",    xlua_spawn    },
	{ "expect",   xlua_expect   },
	{ "timeout",  xlua_timeout  },
	{ "continue", xlua_continue },
	{ NULL,       NULL          }
};

// --- Initialisation, event handling ------------------------------------------

static void *
xlua_alloc (void *ud, void *ptr, size_t o_size, size_t n_size)
{
	(void) ud;
	(void) o_size;

	if (n_size)
		return realloc (ptr, n_size);

	free (ptr);
	return NULL;
}

static int
xlua_panic (lua_State *L)
{
	print_fatal ("Lua panicked: %s", lua_tostring (L, -1));
	lua_close (L);
	exit (EXIT_FAILURE);
	return 0;
}

int
main (int argc, char *argv[])
{
	if (argc != 2)
	{
		fprintf (stderr, "Usage: %s program.lua\n", argv[0]);
		return 1;
	}

	if (!(g.L = lua_newstate (xlua_alloc, NULL)))
		exit_fatal ("Lua initialization failed");
	lua_atpanic (g.L, xlua_panic);
	luaL_openlibs (g.L);
	luaL_checkversion (g.L);

	luaL_newlib (g.L, xlua_library);
	lua_setglobal (g.L, PROGRAM_NAME);

	luaL_newmetatable (g.L, XLUA_PROCESS_METATABLE);
	luaL_setfuncs (g.L, xlua_process_table, 0);
	lua_pop (g.L, 1);

	luaL_newmetatable (g.L, XLUA_PATTERN_METATABLE);
	luaL_setfuncs (g.L, xlua_pattern_table, 0);
	lua_pop (g.L, 1);

	const char *path = argv[1];
	lua_pushcfunction (g.L, xlua_error_handler);
	if (luaL_loadfile (g.L, path)
	 || lua_pcall (g.L, 0, 0, -2))
	{
		print_error ("%s", lua_tostring (g.L, -1));
		lua_pop (g.L, 1);
		lua_close (g.L);
		return 1;
	}
	lua_close (g.L);
	return 0;
}
