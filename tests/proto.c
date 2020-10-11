/*
 * tests/proto.c
 *
 * Copyright (c) 2015, Přemysl Eric Janouch <p@janouch.name>
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

#define PROGRAM_NAME "test"
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

// --- Tests -------------------------------------------------------------------

static void
test_irc (void)
{
	struct irc_message msg;
	irc_parse_message (&msg, "@first=a\\:\\s\\r\\n\\\\;2nd "
		":srv hi there :good m8 :how are you?");

	struct str_map_iter iter = str_map_iter_make (&msg.tags);
	soft_assert (msg.tags.len == 2);

	char *value;
	while ((value = str_map_iter_next (&iter)))
	{
		if (!strcmp (iter.link->key, "first"))
			soft_assert (!strcmp (value, "a; \r\n\\"));
		else if (!strcmp (iter.link->key, "2nd"))
			soft_assert (!strcmp (value, ""));
		else
			soft_assert (!"found unexpected message tag");
	}

	soft_assert (!strcmp (msg.prefix, "srv"));
	soft_assert (!strcmp (msg.command, "hi"));
	soft_assert (msg.params.len == 2);
	soft_assert (!strcmp (msg.params.vector[0], "there"));
	soft_assert (!strcmp (msg.params.vector[1], "good m8 :how are you?"));

	irc_free_message (&msg);

	const char *n[2] = { "[fag]^", "{FAG}~" };
	soft_assert (!irc_strcmp (n[0], n[1]));

	char a[irc_strxfrm (NULL, n[0], 0) + 1]; irc_strxfrm (a, n[0], sizeof a);
	char b[irc_strxfrm (NULL, n[1], 0) + 1]; irc_strxfrm (b, n[1], sizeof b);
	soft_assert (!strcmp (a, b));

	// TODO: more tests
}

static void
test_http_parser (void)
{
	struct str_map parameters = str_map_make (free);
	parameters.key_xfrm = tolower_ascii_strxfrm;

	char *type = NULL;
	char *subtype = NULL;
	soft_assert (http_parse_media_type ("TEXT/html; CHARset=\"utf\\-8\"",
		&type, &subtype, &parameters));
	soft_assert (!strcasecmp_ascii (type, "text"));
	soft_assert (!strcasecmp_ascii (subtype, "html"));
	soft_assert (parameters.len == 1);
	soft_assert (!strcmp (str_map_find (&parameters, "charset"), "utf-8"));
	free (type);
	free (subtype);
	str_map_free (&parameters);

	struct http_protocol *protocols = NULL;
	soft_assert (http_parse_upgrade ("websocket, HTTP/2.0, , ", &protocols));

	soft_assert (!strcmp (protocols->name, "websocket"));
	soft_assert (!protocols->version);

	soft_assert (!strcmp (protocols->next->name, "HTTP"));
	soft_assert (!strcmp (protocols->next->version, "2.0"));

	soft_assert (!protocols->next->next);

	LIST_FOR_EACH (struct http_protocol, iter, protocols)
		http_protocol_destroy (iter);
}

struct scgi_fixture
{
	struct scgi_parser parser;
	bool seen_headers;
	bool seen_content;
};

static bool
test_scgi_parser_on_headers_read (void *user_data)
{
	struct scgi_fixture *fixture = user_data;
	struct scgi_parser *parser = &fixture->parser;
	fixture->seen_headers = true;

	soft_assert (parser->headers.len == 4);
	soft_assert (!strcmp (str_map_find (&parser->headers,
		"CONTENT_LENGTH"), "27"));
	soft_assert (!strcmp (str_map_find (&parser->headers,
		"SCGI"), "1"));
	soft_assert (!strcmp (str_map_find (&parser->headers,
		"REQUEST_METHOD"), "POST"));
	soft_assert (!strcmp (str_map_find (&parser->headers,
		"REQUEST_URI"), "/deepthought"));
	return true;
}

static bool
test_scgi_parser_on_content (void *user_data, const void *data, size_t len)
{
	struct scgi_fixture *fixture = user_data;
	fixture->seen_content = true;

	soft_assert (!strncmp (data, "What is the answer to life?", len));
	return true;
}

static void
test_scgi_parser (void)
{
	struct scgi_fixture fixture = { scgi_parser_make(), false, false };
	struct scgi_parser *parser = &fixture.parser;

	parser->on_headers_read = test_scgi_parser_on_headers_read;
	parser->on_content      = test_scgi_parser_on_content;
	parser->user_data       = &fixture;

	// This is an example straight from the specification
	const char example[] =
		"70:"
			"CONTENT_LENGTH" "\0" "27" "\0"
			"SCGI" "\0" "1" "\0"
			"REQUEST_METHOD" "\0" "POST" "\0"
			"REQUEST_URI" "\0" "/deepthought" "\0"
		","
		"What is the answer to life?";

	soft_assert (scgi_parser_push (parser, example, sizeof example, NULL));
	soft_assert (fixture.seen_headers && fixture.seen_content);
	scgi_parser_free (parser);
}

static bool
test_websockets_on_frame_header (void *user_data, const struct ws_parser *self)
{
	(void) user_data;
	soft_assert (self->is_fin);
	soft_assert (self->is_masked);
	soft_assert (self->opcode == WS_OPCODE_TEXT);
	return true;
}

static bool
test_websockets_on_frame (void *user_data, const struct ws_parser *self)
{
	(void) user_data;
	soft_assert (self->input.len == self->payload_len);
	soft_assert (!strncmp (self->input.str, "Hello", self->input.len));
	return true;
}

static void
test_websockets (void)
{
	char *accept = ws_encode_response_key ("dGhlIHNhbXBsZSBub25jZQ==");
	soft_assert (!strcmp (accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	free (accept);

	struct ws_parser parser = ws_parser_make ();
	parser.on_frame_header = test_websockets_on_frame_header;
	parser.on_frame        = test_websockets_on_frame;
	parser.user_data       = &parser;

	const char frame[] = "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58";
	soft_assert (ws_parser_push (&parser, frame, sizeof frame - 1));
	ws_parser_free (&parser);
}

// --- Main --------------------------------------------------------------------

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);

	test_add_simple (&test, "/irc",            NULL, test_irc);
	test_add_simple (&test, "/http-parser",    NULL, test_http_parser);
	test_add_simple (&test, "/scgi-parser",    NULL, test_scgi_parser);
	test_add_simple (&test, "/websockets",     NULL, test_websockets);
	// TODO: test FastCGI and MPD

	return test_run (&test);
}
