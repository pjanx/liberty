/*
 * tests/fuzz.c
 *
 * Copyright (c) 2020, Přemysl Eric Janouch <p@janouch.name>
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

#define PROGRAM_NAME "fuzz"
#define PROGRAM_VERSION "0"

#define LIBERTY_WANT_SSL
// The MPD client is a full wrapper and needs the network
#define LIBERTY_WANT_POLLER
#define LIBERTY_WANT_ASYNC

#define LIBERTY_WANT_PROTO_IRC
#define LIBERTY_WANT_PROTO_HTTP
#define LIBERTY_WANT_PROTO_SCGI
#define LIBERTY_WANT_PROTO_FASTCGI
#define LIBERTY_WANT_PROTO_WS
#define LIBERTY_WANT_PROTO_MPD

#include "../liberty.c"

// --- UTF-8 -------------------------------------------------------------------

static void
test_utf8_validate (const uint8_t *data, size_t size)
{
	utf8_validate ((const char *) data, size);
}

// --- Base 64 -----------------------------------------------------------------

static void
test_base64_decode (const uint8_t *data, size_t size)
{
	struct str wrap = str_make ();
	str_append_data (&wrap, data, size);

	struct str out = str_make ();
	base64_decode (wrap.str, true /* ignore_ws */, &out);
	str_free (&out);

	str_free (&wrap);
}

// --- IRC ---------------------------------------------------------------------

static void
test_irc_parse_message (const uint8_t *data, size_t size)
{
	struct str wrap = str_make ();
	str_append_data (&wrap, data, size);

	struct irc_message msg;
	irc_parse_message (&msg, wrap.str);
	irc_free_message (&msg);

	str_free (&wrap);
}

// --- HTTP --------------------------------------------------------------------

static void
test_http_parse_media_type (const uint8_t *data, size_t size)
{
	struct str wrap = str_make ();
	str_append_data (&wrap, data, size);

	char *type = NULL;
	char *subtype = NULL;
	struct str_map parameters = str_map_make (free);
	http_parse_media_type (wrap.str, &type, &subtype, &parameters);
	free (type);
	free (subtype);
	str_map_free (&parameters);

	str_free (&wrap);
}

static void
test_http_parse_upgrade (const uint8_t *data, size_t size)
{
	struct str wrap = str_make ();
	str_append_data (&wrap, data, size);

	struct http_protocol *protocols = NULL;
	http_parse_upgrade (wrap.str, &protocols);
	LIST_FOR_EACH (struct http_protocol, iter, protocols)
		http_protocol_destroy (iter);

	str_free (&wrap);
}

// --- SCGI --------------------------------------------------------------------

static bool
test_scgi_parser_on_headers_read (void *user_data)
{
	(void) user_data;
	return true;
}

static bool
test_scgi_parser_on_content (void *user_data, const void *data, size_t len)
{
	(void) user_data;
	(void) data;
	(void) len;
	return true;
}

static void
test_scgi_parser_push (const uint8_t *data, size_t size)
{
	struct scgi_parser parser = scgi_parser_make ();
	parser.on_headers_read = test_scgi_parser_on_headers_read;
	parser.on_content      = test_scgi_parser_on_content;

	scgi_parser_push (&parser, data, size, NULL);
	scgi_parser_free (&parser);
}

// --- WebSockets --------------------------------------------------------------

static bool
test_ws_parser_on_frame_header (void *user_data, const struct ws_parser *self)
{
	(void) user_data;
	(void) self;
	return true;
}

static bool
test_ws_parser_on_frame (void *user_data, const struct ws_parser *self)
{
	(void) user_data;
	(void) self;
	return true;
}

static void
test_ws_parser_push (const uint8_t *data, size_t size)
{
	struct ws_parser parser = ws_parser_make ();
	parser.on_frame_header = test_ws_parser_on_frame_header;
	parser.on_frame        = test_ws_parser_on_frame;

	ws_parser_push (&parser, data, size);
	ws_parser_free (&parser);
}

// --- FastCGI -----------------------------------------------------------------

static bool
test_fcgi_parser_on_message (const struct fcgi_parser *parser, void *user_data)
{
	(void) parser;
	(void) user_data;
	return true;
}

static void
test_fcgi_parser_push (const uint8_t *data, size_t size)
{
	struct fcgi_parser parser = fcgi_parser_make ();
	parser.on_message = test_fcgi_parser_on_message;
	fcgi_parser_push (&parser, data, size);
	fcgi_parser_free (&parser);
}

static void
test_fcgi_nv_parser_push (const uint8_t *data, size_t size)
{
	struct str_map values = str_map_make (free);
	struct fcgi_nv_parser nv_parser = fcgi_nv_parser_make ();
	nv_parser.output = &values;

	fcgi_nv_parser_push (&nv_parser, data, size);
	fcgi_nv_parser_free (&nv_parser);
	str_map_free (&values);
}

// --- Config ------------------------------------------------------------------

static void
test_config_item_parse (const uint8_t *data, size_t size)
{
	struct config_item *item =
		config_item_parse ((const char *) data, size, false, NULL);
	if (item)
		config_item_destroy (item);
}

// --- MPD ---------------------------------------------------------------------

static void
test_mpd_client_process_input (const uint8_t *data, size_t size)
{
	struct poller poller;
	poller_init (&poller);

	struct mpd_client mpd = mpd_client_make (&poller);
	str_append_data (&mpd.read_buffer, data, size);
	mpd_client_process_input (&mpd);
	mpd_client_free (&mpd);

	poller_free (&poller);
}

// --- Main --------------------------------------------------------------------

typedef void (*fuzz_test_fn) (const uint8_t *data, size_t size);
static fuzz_test_fn generator = NULL;

void
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
	generator (data, size);
}

int
LLVMFuzzerInitialize (int *argcp, char ***argvp)
{
	struct str_map targets = str_map_make (NULL);
#define REGISTER(name) str_map_set (&targets, #name, test_ ## name);
	REGISTER (utf8_validate)
	REGISTER (base64_decode)
	REGISTER (irc_parse_message)
	REGISTER (http_parse_media_type)
	REGISTER (http_parse_upgrade)
	REGISTER (scgi_parser_push)
	REGISTER (ws_parser_push)
	REGISTER (fcgi_parser_push)
	REGISTER (fcgi_nv_parser_push)
	REGISTER (config_item_parse)
	REGISTER (mpd_client_process_input)

	char **argv = *argvp, *option = "-test=", *name = NULL;
	for (int i = 1; i < *argcp; i++)
		if (!strncmp (argv[i], option, strlen (option)))
		{
			name = argv[i] + strlen (option);
			memmove (argv + i, argv + i + 1, (*argcp - i) * sizeof *argv);
			(*argcp)--;
		}

	if (!name)
	{
		struct str_map_iter iter = str_map_iter_make (&targets);
		while (str_map_iter_next (&iter))
			printf ("%s\n", iter.link->key);
		exit (EXIT_FAILURE);
	}

	if (!(generator = str_map_find (&targets, name)))
	{
		fprintf (stderr, "Unknown test: %s\n", name);
		exit (EXIT_FAILURE);
	}

	str_map_free (&targets);
	return 0;
}
