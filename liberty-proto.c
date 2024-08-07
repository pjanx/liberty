/*
 * liberty-proto.c: the ultimate C unlibrary: protocols
 *
 * Copyright (c) 2014 - 2016, Přemysl Eric Janouch <p@janouch.name>
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

// Mostly parsers and various utilities relating to various protocols

#ifdef LIBERTY_WANT_PROTO_IRC

// --- IRC utilities -----------------------------------------------------------

struct irc_message
{
	struct str_map tags;                ///< IRC 3.2 message tags
	char *prefix;                       ///< Message prefix
	char *command;                      ///< IRC command
	struct strv params;                 ///< Command parameters
};

static char *
irc_unescape_message_tag (const char *value)
{
	struct str s = str_make ();
	bool escape = false;
	for (const char *p = value; *p; p++)
	{
		if (escape)
		{
			switch (*p)
			{
			case ':': str_append_c (&s, ';');  break;
			case 's': str_append_c (&s, ' ');  break;
			case 'r': str_append_c (&s, '\r'); break;
			case 'n': str_append_c (&s, '\n'); break;
			default:  str_append_c (&s, *p);
			}
			escape = false;
		}
		else if (*p == '\\')
			escape = true;
		else
			str_append_c (&s, *p);
	}
	return str_steal (&s);
}

static void
irc_parse_message_tags (const char *tags, struct str_map *out)
{
	struct strv v = strv_make ();
	cstr_split (tags, ";", true, &v);

	for (size_t i = 0; i < v.len; i++)
	{
		char *key = v.vector[i], *equal_sign = strchr (key, '=');
		if (equal_sign)
		{
			*equal_sign = '\0';
			str_map_set (out, key, irc_unescape_message_tag (equal_sign + 1));
		}
		else
			str_map_set (out, key, xstrdup (""));
	}
	strv_free (&v);
}

static void
irc_parse_message (struct irc_message *msg, const char *line)
{
	msg->tags = str_map_make (free);
	msg->prefix = NULL;
	msg->command = NULL;
	msg->params = strv_make ();

	// IRC 3.2 message tags
	if (*line == '@')
	{
		size_t tags_len = strcspn (++line, " ");
		char *tags = xstrndup (line, tags_len);
		irc_parse_message_tags (tags, &msg->tags);
		free (tags);

		line += tags_len;
		while (*line == ' ')
			line++;
	}

	// Prefix
	if (*line == ':')
	{
		size_t prefix_len = strcspn (++line, " ");
		msg->prefix = xstrndup (line, prefix_len);
		line += prefix_len;
	}

	// Command name
	{
		while (*line == ' ')
			line++;

		size_t cmd_len = strcspn (line, " ");
		msg->command = xstrndup (line, cmd_len);
		line += cmd_len;
	}

	// Arguments
	while (true)
	{
		while (*line == ' ')
			line++;

		if (*line == ':')
		{
			strv_append (&msg->params, ++line);
			break;
		}

		size_t param_len = strcspn (line, " ");
		if (!param_len)
			break;

		strv_append_owned (&msg->params, xstrndup (line, param_len));
		line += param_len;
	}
}

static void
irc_free_message (struct irc_message *msg)
{
	str_map_free (&msg->tags);
	free (msg->prefix);
	free (msg->command);
	strv_free (&msg->params);
}

static void
irc_process_buffer (struct str *buf,
	void (*callback) (const struct irc_message *, const char *, void *),
	void *user_data)
{
	char *start = buf->str, *end = start + buf->len;
	for (char *p = start; p + 1 < end; p++)
	{
		// Split the input on newlines
		if (p[0] != '\r' || p[1] != '\n')
			continue;

		*p = 0;

		struct irc_message msg;
		irc_parse_message (&msg, start);
		callback (&msg, start, user_data);
		irc_free_message (&msg);

		start = p + 2;
	}

	// XXX: we might want to just advance some kind of an offset to avoid
	//   moving memory around unnecessarily.
	str_remove_slice (buf, 0, start - buf->str);
}

static int
irc_tolower (int c)
{
	if (c == '[')   return '{';
	if (c == ']')   return '}';
	if (c == '\\')  return '|';
	if (c == '~')   return '^';
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int
irc_tolower_strict (int c)
{
	if (c == '[')   return '{';
	if (c == ']')   return '}';
	if (c == '\\')  return '|';
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

TRIVIAL_STRXFRM (irc_strxfrm,        irc_tolower)
TRIVIAL_STRXFRM (irc_strxfrm_strict, irc_tolower_strict)

static int
irc_strcmp (const char *a, const char *b)
{
	int x;
	while (*a || *b)
		if ((x = irc_tolower (*a++) - irc_tolower (*b++)))
			return x;
	return 0;
}

static int
irc_fnmatch (const char *pattern, const char *string)
{
	size_t pattern_size = strlen (pattern) + 1;
	size_t string_size  = strlen (string)  + 1;
	char x_pattern[pattern_size], x_string[string_size];
	irc_strxfrm (x_pattern, pattern, pattern_size);
	irc_strxfrm (x_string,  string,  string_size);
	// FIXME: this supports [], which is not mentioned in RFC 2812
	return fnmatch (x_pattern, x_string, 0);
}

#endif

#ifdef LIBERTY_WANT_PROTO_HTTP

// --- HTTP parsing ------------------------------------------------------------

// Basic tokenizer for HTTP header field values, to be used in various parsers.
// The input should already be unwrapped.

// Recommended literature:
//   http://tools.ietf.org/html/rfc7230#section-3.2.6
//   http://tools.ietf.org/html/rfc7230#appendix-B
//   http://tools.ietf.org/html/rfc5234#appendix-B.1

#define HTTP_TOKENIZER_CLASS(name, definition)                                 \
	static inline bool                                                         \
	http_tokenizer_is_ ## name (int c)                                         \
	{                                                                          \
		return (definition);                                                   \
	}

HTTP_TOKENIZER_CLASS (vchar, c >= 0x21 && c <= 0x7E)
HTTP_TOKENIZER_CLASS (delimiter, !!strchr ("\"(),/:;<=>?@[\\]{}", c))
HTTP_TOKENIZER_CLASS (whitespace, c == '\t' || c == ' ')
HTTP_TOKENIZER_CLASS (obs_text, c >= 0x80 && c <= 0xFF)

HTTP_TOKENIZER_CLASS (tchar,
	http_tokenizer_is_vchar (c) && !http_tokenizer_is_delimiter (c))

HTTP_TOKENIZER_CLASS (qdtext,
	c == '\t' || c == ' ' || c == '!'
	|| (c >= 0x23 && c <= 0x5B)
	|| (c >= 0x5D && c <= 0x7E)
	|| http_tokenizer_is_obs_text (c))

HTTP_TOKENIZER_CLASS (quoted_pair,
	c == '\t' || c == ' '
	|| http_tokenizer_is_vchar (c)
	|| http_tokenizer_is_obs_text (c))

#undef HTTP_TOKENIZER_CLASS

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum http_tokenizer_token
{
	HTTP_T_EOF,                         ///< Input error
	HTTP_T_ERROR,                       ///< End of input

	HTTP_T_TOKEN,                       ///< "token"
	HTTP_T_QUOTED_STRING,               ///< "quoted-string"
	HTTP_T_DELIMITER,                   ///< "delimiters"
	HTTP_T_WHITESPACE                   ///< RWS/OWS/BWS
};

struct http_tokenizer
{
	const unsigned char *input;         ///< The input string
	size_t input_len;                   ///< Length of the input
	size_t offset;                      ///< Position in the input

	char delimiter;                     ///< The delimiter character
	struct str string;                  ///< "token" / "quoted-string" content
};

static struct http_tokenizer
http_tokenizer_make (const char *input, size_t len)
{
	return (struct http_tokenizer)
	{
		.input = (const unsigned char *) input,
		.input_len = len,
		.string = str_make (),
	};
}

static void
http_tokenizer_free (struct http_tokenizer *self)
{
	str_free (&self->string);
}

static enum http_tokenizer_token
http_tokenizer_quoted_string (struct http_tokenizer *self)
{
	bool quoted_pair = false;
	while (self->offset < self->input_len)
	{
		int c = self->input[self->offset++];
		if (quoted_pair)
		{
			if (!http_tokenizer_is_quoted_pair (c))
				return HTTP_T_ERROR;

			str_append_c (&self->string, c);
			quoted_pair = false;
		}
		else if (c == '\\')
			quoted_pair = true;
		else if (c == '"')
			return HTTP_T_QUOTED_STRING;
		else if (http_tokenizer_is_qdtext (c))
			str_append_c (&self->string, c);
		else
			return HTTP_T_ERROR;
	}

	// Premature end of input
	return HTTP_T_ERROR;
}

static enum http_tokenizer_token
http_tokenizer_next (struct http_tokenizer *self, bool skip_ows)
{
	str_reset (&self->string);
	if (self->offset >= self->input_len)
		return HTTP_T_EOF;

	int c = self->input[self->offset++];

	if (skip_ows)
		while (http_tokenizer_is_whitespace (c))
		{
			if (self->offset >= self->input_len)
				return HTTP_T_EOF;
			c = self->input[self->offset++];
		}

	if (c == '"')
		return http_tokenizer_quoted_string (self);

	if (http_tokenizer_is_delimiter (c))
	{
		self->delimiter = c;
		return HTTP_T_DELIMITER;
	}

	// Simple variable-length tokens
	enum http_tokenizer_token result;
	bool (*eater) (int c) = NULL;
	if (http_tokenizer_is_whitespace (c))
	{
		eater = http_tokenizer_is_whitespace;
		result = HTTP_T_WHITESPACE;
	}
	else if (http_tokenizer_is_tchar (c))
	{
		eater = http_tokenizer_is_tchar;
		result = HTTP_T_TOKEN;
	}
	else
		return HTTP_T_ERROR;

	str_append_c (&self->string, c);
	while (self->offset < self->input_len)
	{
		if (!eater (c = self->input[self->offset]))
			break;

		str_append_c (&self->string, c);
		self->offset++;
	}
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
http_parse_media_type_parameter
	(struct http_tokenizer *t, struct str_map *parameters)
{
	bool result = false;
	char *attribute = NULL;

	if (http_tokenizer_next (t, true) != HTTP_T_TOKEN)
		goto end;
	attribute = xstrdup (t->string.str);

	if (http_tokenizer_next (t, false) != HTTP_T_DELIMITER
	 || t->delimiter != '=')
		goto end;

	switch (http_tokenizer_next (t, false))
	{
	case HTTP_T_TOKEN:
	case HTTP_T_QUOTED_STRING:
		if (parameters)
			str_map_set (parameters, attribute, xstrdup (t->string.str));
		result = true;
	default:
		break;
	}

end:
	free (attribute);
	return result;
}

/// Parser for "Content-Type".  @a type and @a subtype may end up non-NULL
/// even if the function fails.  @a parameters should be case-insensitive,
/// and may be NULL for validation only.
static bool
http_parse_media_type (const char *media_type,
	char **type, char **subtype, struct str_map *parameters)
{
	bool result = false;
	struct http_tokenizer t =
		http_tokenizer_make (media_type, strlen (media_type));

	if (http_tokenizer_next (&t, true) != HTTP_T_TOKEN)
		goto end;
	*type = xstrdup (t.string.str);

	if (http_tokenizer_next (&t, false) != HTTP_T_DELIMITER
	 || t.delimiter != '/')
		goto end;

	if (http_tokenizer_next (&t, false) != HTTP_T_TOKEN)
		goto end;
	*subtype = xstrdup (t.string.str);

	while (true)
	switch (http_tokenizer_next (&t, true))
	{
	case HTTP_T_DELIMITER:
		if (t.delimiter != ';')
			goto end;
		if (!http_parse_media_type_parameter (&t, parameters))
			goto end;
		break;
	case HTTP_T_EOF:
		result = true;
	default:
		goto end;
	}

end:
	http_tokenizer_free (&t);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct http_protocol
{
	LIST_HEADER (struct http_protocol)

	char *name;                         ///< The protocol to upgrade to
	char *version;                      ///< Version of the protocol, if any
};

static void
http_protocol_destroy (struct http_protocol *self)
{
	free (self->name);
	free (self->version);
	free (self);
}

static bool
http_parse_upgrade (const char *upgrade, struct http_protocol **out)
{
	// HTTP grammar makes this more complicated than it should be

	bool result = false;
	struct http_protocol *list = NULL;
	struct http_protocol *tail = NULL;

	struct http_tokenizer t = http_tokenizer_make (upgrade, strlen (upgrade));

	enum {
		STATE_PROTOCOL_NAME,
		STATE_SLASH,
		STATE_PROTOCOL_VERSION,
		STATE_EXPECT_COMMA
	} state = STATE_PROTOCOL_NAME;
	struct http_protocol *proto = NULL;

	while (true)
	switch (state)
	{
	case STATE_PROTOCOL_NAME:
		switch (http_tokenizer_next (&t, false))
		{
		case HTTP_T_DELIMITER:
			if (t.delimiter != ',')
				goto end;
		case HTTP_T_WHITESPACE:
			break;
		case HTTP_T_TOKEN:
			proto = xcalloc (1, sizeof *proto);
			proto->name = xstrdup (t.string.str);
			LIST_APPEND_WITH_TAIL (list, tail, proto);
			state = STATE_SLASH;
			break;
		case HTTP_T_EOF:
			result = true;
		default:
			goto end;
		}
		break;
	case STATE_SLASH:
		switch (http_tokenizer_next (&t, false))
		{
		case HTTP_T_DELIMITER:
			if (t.delimiter == '/')
				state = STATE_PROTOCOL_VERSION;
			else if (t.delimiter == ',')
				state = STATE_PROTOCOL_NAME;
			else
				goto end;
			break;
		case HTTP_T_WHITESPACE:
			state = STATE_EXPECT_COMMA;
			break;
		case HTTP_T_EOF:
			result = true;
		default:
			goto end;
		}
		break;
	case STATE_PROTOCOL_VERSION:
		switch (http_tokenizer_next (&t, false))
		{
		case HTTP_T_TOKEN:
			proto->version = xstrdup (t.string.str);
			state = STATE_EXPECT_COMMA;
			break;
		default:
			goto end;
		}
		break;
	case STATE_EXPECT_COMMA:
		switch (http_tokenizer_next (&t, false))
		{
		case HTTP_T_DELIMITER:
			if (t.delimiter != ',')
				goto end;
			state = STATE_PROTOCOL_NAME;
		case HTTP_T_WHITESPACE:
			break;
		case HTTP_T_EOF:
			result = true;
		default:
			goto end;
		}
	}

end:
	if (result)
		*out = list;
	else
		LIST_FOR_EACH (struct http_protocol, iter, list)
			http_protocol_destroy (iter);

	http_tokenizer_free (&t);
	return result;
}

#endif

#ifdef LIBERTY_WANT_PROTO_SCGI

// --- SCGI --------------------------------------------------------------------

enum scgi_parser_state
{
	SCGI_READING_NETSTRING_LENGTH,      ///< The length of the header netstring
	SCGI_READING_NAME,                  ///< Header name
	SCGI_READING_VALUE,                 ///< Header value
	SCGI_READING_CONTENT                ///< Incoming data
};

struct scgi_parser
{
	enum scgi_parser_state state;       ///< Parsing state
	struct str input;                   ///< Input buffer

	struct str_map headers;             ///< Headers parsed

	size_t headers_len;                 ///< Length of the netstring contents
	struct str name;                    ///< Header name so far
	struct str value;                   ///< Header value so far

	/// Finished parsing request headers.
	/// Return false to abort further processing of input.
	bool (*on_headers_read) (void *user_data);

	/// Content available; len == 0 means end of file.
	/// Return false to abort further processing of input.
	bool (*on_content) (void *user_data, const void *data, size_t len);

	void *user_data;                    ///< User data passed to callbacks
};

static struct scgi_parser
scgi_parser_make (void)
{
	return (struct scgi_parser)
	{
		.input = str_make (),
		.headers = str_map_make (free),
		.name = str_make (),
		.value = str_make (),
	};
}

static void
scgi_parser_free (struct scgi_parser *self)
{
	str_free (&self->input);
	str_map_free (&self->headers);
	str_free (&self->name);
	str_free (&self->value);
}

static bool
scgi_parser_push (struct scgi_parser *self,
	const void *data, size_t len, struct error **e)
{
	if (!len)
	{
		if (self->state != SCGI_READING_CONTENT)
			return error_set (e, "premature EOF");

		// Indicate end of file
		return self->on_content (self->user_data, NULL, 0);
	}

	// Notice that this madness is significantly harder to parse than FastCGI;
	// this procedure could also be optimized significantly
	str_append_data (&self->input, data, len);

	bool keep_running = true;
	while (keep_running)
	switch (self->state)
	{
	case SCGI_READING_NETSTRING_LENGTH:
	{
		if (self->input.len < 1)
			return true;

		char digit = *self->input.str;
		// XXX: this allows for omitting the netstring length altogether
		if (digit == ':')
		{
			self->state = SCGI_READING_NAME;
			str_remove_slice (&self->input, 0, 1);
			break;
		}

		if (digit < '0' || digit > '9')
			return error_set (e, "invalid header netstring");

		size_t new_len = self->headers_len * 10 + (digit - '0');
		if (new_len < self->headers_len)
			return error_set (e, "header netstring is too long");

		self->headers_len = new_len;
		str_remove_slice (&self->input, 0, 1);
		break;
	}
	case SCGI_READING_NAME:
	{
		if (self->input.len < 1)
			return true;

		char c = *self->input.str;
		if (!self->headers_len)
		{
			// The netstring is ending but we haven't finished parsing it,
			// or the netstring doesn't end with a comma
			if (self->name.len || c != ',')
				return error_set (e, "invalid header netstring");

			self->state = SCGI_READING_CONTENT;
			keep_running = self->on_headers_read (self->user_data);
		}
		else if (c != '\0')
			str_append_c (&self->name, c);
		else
			self->state = SCGI_READING_VALUE;

		str_remove_slice (&self->input, 0, 1);
		self->headers_len--;
		break;
	}
	case SCGI_READING_VALUE:
	{
		if (self->input.len < 1)
			return true;

		char c = *self->input.str;
		if (!self->headers_len)
		{
			// The netstring is ending but we haven't finished parsing it
			return error_set (e, "invalid header netstring");
		}
		else if (c != '\0')
			str_append_c (&self->value, c);
		else
		{
			// We've got a name-value pair, let's put it in the map
			str_map_set (&self->headers,
				self->name.str, str_steal (&self->value));

			str_reset (&self->name);
			self->value = str_make ();

			self->state = SCGI_READING_NAME;
		}

		str_remove_slice (&self->input, 0, 1);
		self->headers_len--;
		break;
	}
	case SCGI_READING_CONTENT:
		keep_running = self->on_content
			(self->user_data, self->input.str, self->input.len);
		str_remove_slice (&self->input, 0, self->input.len);
		return keep_running;
	}
	return false;
}

#endif

#ifdef LIBERTY_WANT_PROTO_FASTCGI

// --- FastCGI -----------------------------------------------------------------

// Constants from the FastCGI specification document

#define FCGI_HEADER_LEN       8

#define FCGI_VERSION_1        1
#define FCGI_NULL_REQUEST_ID  0
#define FCGI_KEEP_CONN        1

enum fcgi_type
{
	FCGI_BEGIN_REQUEST     =  1,
	FCGI_ABORT_REQUEST     =  2,
	FCGI_END_REQUEST       =  3,
	FCGI_PARAMS            =  4,
	FCGI_STDIN             =  5,
	FCGI_STDOUT            =  6,
	FCGI_STDERR            =  7,
	FCGI_DATA              =  8,
	FCGI_GET_VALUES        =  9,
	FCGI_GET_VALUES_RESULT = 10,
	FCGI_UNKNOWN_TYPE      = 11,
	FCGI_MAXTYPE           = FCGI_UNKNOWN_TYPE
};

enum fcgi_role
{
	FCGI_RESPONDER         =  1,
	FCGI_AUTHORIZER        =  2,
	FCGI_FILTER            =  3
};

enum fcgi_protocol_status
{
	FCGI_REQUEST_COMPLETE  =  0,
	FCGI_CANT_MPX_CONN     =  1,
	FCGI_OVERLOADED        =  2,
	FCGI_UNKNOWN_ROLE      =  3
};

#define FCGI_MAX_CONNS   "FCGI_MAX_CONNS"
#define FCGI_MAX_REQS    "FCGI_MAX_REQS"
#define FCGI_MPXS_CONNS  "FCGI_MPXS_CONNS"

// - - Message stream parser - - - - - - - - - - - - - - - - - - - - - - - - - -

struct fcgi_parser;

/// Message handler, returns false if further processing should be stopped
typedef bool (*fcgi_message_fn)
	(const struct fcgi_parser *parser, void *user_data);

enum fcgi_parser_state
{
	FCGI_READING_HEADER,                ///< Reading the fixed header portion
	FCGI_READING_CONTENT,               ///< Reading the message content
	FCGI_READING_PADDING                ///< Reading the padding
};

struct fcgi_parser
{
	enum fcgi_parser_state state;       ///< Parsing state
	struct str input;                   ///< Input buffer

	// The next block of fields is considered public:

	uint8_t version;                    ///< FastCGI protocol version
	uint8_t type;                       ///< FastCGI record type
	uint16_t request_id;                ///< FastCGI request ID
	struct str content;                 ///< Message data

	uint16_t content_length;            ///< Message content length
	uint8_t padding_length;             ///< Message padding length

	fcgi_message_fn on_message;         ///< Callback on message
	void *user_data;                    ///< User data
};

static struct fcgi_parser
fcgi_parser_make (void)
{
	return (struct fcgi_parser)
		{ .input = str_make (), .content = str_make () };
}

static void
fcgi_parser_free (struct fcgi_parser *self)
{
	str_free (&self->input);
	str_free (&self->content);
}

static void
fcgi_parser_unpack_header (struct fcgi_parser *self)
{
	struct msg_unpacker unpacker =
		msg_unpacker_make (self->input.str, self->input.len);

	bool success = true;
	uint8_t reserved;
	success &= msg_unpacker_u8  (&unpacker, &self->version);
	success &= msg_unpacker_u8  (&unpacker, &self->type);
	success &= msg_unpacker_u16 (&unpacker, &self->request_id);
	success &= msg_unpacker_u16 (&unpacker, &self->content_length);
	success &= msg_unpacker_u8  (&unpacker, &self->padding_length);
	success &= msg_unpacker_u8  (&unpacker, &reserved);
	hard_assert (success);

	str_remove_slice (&self->input, 0, unpacker.offset);
}

static bool
fcgi_parser_push (struct fcgi_parser *self, const void *data, size_t len)
{
	// This could be made considerably faster for high-throughput applications
	// if we use a circular buffer instead of constantly calling memmove()
	str_append_data (&self->input, data, len);

	while (true)
	switch (self->state)
	{
	case FCGI_READING_HEADER:
		if (self->input.len < FCGI_HEADER_LEN)
			return true;

		fcgi_parser_unpack_header (self);
		self->state = FCGI_READING_CONTENT;
		break;
	case FCGI_READING_CONTENT:
		if (self->input.len < self->content_length)
			return true;

		// Move an appropriate part of the input buffer to the content buffer
		str_reset (&self->content);
		str_append_data (&self->content, self->input.str, self->content_length);
		str_remove_slice (&self->input, 0, self->content_length);
		self->state = FCGI_READING_PADDING;
		break;
	case FCGI_READING_PADDING:
		if (self->input.len < self->padding_length)
			return true;

		// Call the callback to further process the message
		if (!self->on_message (self, self->user_data))
			return false;

		// Remove the padding from the input buffer
		str_remove_slice (&self->input, 0, self->padding_length);
		self->state = FCGI_READING_HEADER;
		break;
	}
}

// - - Name-value pair parser  - - - - - - - - - - - - - - - - - - - - - - - - -

enum fcgi_nv_parser_state
{
	FCGI_NV_PARSER_NAME_LEN,            ///< The first name length octet
	FCGI_NV_PARSER_NAME_LEN_FULL,       ///< Remaining name length octets
	FCGI_NV_PARSER_VALUE_LEN,           ///< The first value length octet
	FCGI_NV_PARSER_VALUE_LEN_FULL,      ///< Remaining value length octets
	FCGI_NV_PARSER_NAME,                ///< Reading the name
	FCGI_NV_PARSER_VALUE                ///< Reading the value
};

struct fcgi_nv_parser
{
	struct str_map *output;             ///< Where the pairs will be stored

	enum fcgi_nv_parser_state state;    ///< Parsing state
	struct str input;                   ///< Input buffer

	uint32_t name_len;                  ///< Length of the name
	uint32_t value_len;                 ///< Length of the value

	char *name;                         ///< The current name, 0-terminated
	char *value;                        ///< The current value, 0-terminated
};

static struct fcgi_nv_parser
fcgi_nv_parser_make (void)
{
	return (struct fcgi_nv_parser) { .input = str_make () };
}

static void
fcgi_nv_parser_free (struct fcgi_nv_parser *self)
{
	str_free (&self->input);
	free (self->name);
	free (self->value);
}

static void
fcgi_nv_parser_push (struct fcgi_nv_parser *self, const void *data, size_t len)
{
	// This could be optimized significantly; I'm not even trying
	str_append_data (&self->input, data, len);

	while (true)
	{
		struct msg_unpacker unpacker =
			msg_unpacker_make (self->input.str, self->input.len);

	switch (self->state)
	{
		uint8_t len;
		uint32_t len_full;

	case FCGI_NV_PARSER_NAME_LEN:
		if (!msg_unpacker_u8 (&unpacker, &len))
			return;

		if (len >> 7)
			self->state = FCGI_NV_PARSER_NAME_LEN_FULL;
		else
		{
			self->name_len = len;
			str_remove_slice (&self->input, 0, unpacker.offset);
			self->state = FCGI_NV_PARSER_VALUE_LEN;
		}
		break;
	case FCGI_NV_PARSER_NAME_LEN_FULL:
		if (!msg_unpacker_u32 (&unpacker, &len_full))
			return;

		self->name_len = len_full & ~(1U << 31);
		str_remove_slice (&self->input, 0, unpacker.offset);
		self->state = FCGI_NV_PARSER_VALUE_LEN;
		break;
	case FCGI_NV_PARSER_VALUE_LEN:
		if (!msg_unpacker_u8 (&unpacker, &len))
			return;

		if (len >> 7)
			self->state = FCGI_NV_PARSER_VALUE_LEN_FULL;
		else
		{
			self->value_len = len;
			str_remove_slice (&self->input, 0, unpacker.offset);
			self->state = FCGI_NV_PARSER_NAME;
		}
		break;
	case FCGI_NV_PARSER_VALUE_LEN_FULL:
		if (!msg_unpacker_u32 (&unpacker, &len_full))
			return;

		self->value_len = len_full & ~(1U << 31);
		str_remove_slice (&self->input, 0, unpacker.offset);
		self->state = FCGI_NV_PARSER_NAME;
		break;
	case FCGI_NV_PARSER_NAME:
		if (self->input.len < self->name_len)
			return;

		self->name = xmalloc (self->name_len + 1);
		self->name[self->name_len] = '\0';
		memcpy (self->name, self->input.str, self->name_len);
		str_remove_slice (&self->input, 0, self->name_len);
		self->state = FCGI_NV_PARSER_VALUE;
		break;
	case FCGI_NV_PARSER_VALUE:
		if (self->input.len < self->value_len)
			return;

		self->value = xmalloc (self->value_len + 1);
		self->value[self->value_len] = '\0';
		memcpy (self->value, self->input.str, self->value_len);
		str_remove_slice (&self->input, 0, self->value_len);
		self->state = FCGI_NV_PARSER_NAME_LEN;

		// The map takes ownership of the value
		str_map_set (self->output, self->name, self->value);
		free (self->name);

		self->name  = NULL;
		self->value = NULL;
		break;
	}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fcgi_nv_convert_len (size_t len, struct str *output)
{
	if (len < 0x80)
		str_pack_u8 (output, len);
	else
	{
		len |= (uint32_t) 1 << 31;
		str_pack_u32 (output, len);
	}
}

static void
fcgi_nv_convert (struct str_map *map, struct str *output)
{
	struct str_map_iter iter = str_map_iter_make (map);
	while (str_map_iter_next (&iter))
	{
		const char *name  = iter.link->key;
		const char *value = iter.link->data;
		size_t name_len   = iter.link->key_length;
		size_t value_len  = strlen (value);

		fcgi_nv_convert_len (name_len,  output);
		fcgi_nv_convert_len (value_len, output);
		str_append_data (output, name,  name_len);
		str_append_data (output, value, value_len);
	}
}

#endif

#ifdef LIBERTY_WANT_PROTO_WS

// --- WebSockets --------------------------------------------------------------

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define SEC_WS_KEY         "Sec-WebSocket-Key"
#define SEC_WS_ACCEPT      "Sec-WebSocket-Accept"
#define SEC_WS_PROTOCOL    "Sec-WebSocket-Protocol"
#define SEC_WS_EXTENSIONS  "Sec-WebSocket-Extensions"
#define SEC_WS_VERSION     "Sec-WebSocket-Version"

#define WS_MAX_CONTROL_PAYLOAD_LEN  125

static char *
ws_encode_response_key (const char *key)
{
	char *response_key = xstrdup_printf ("%s" WS_GUID, key);
	unsigned char hash[SHA_DIGEST_LENGTH];
	SHA1 ((unsigned char *) response_key, strlen (response_key), hash);
	free (response_key);

	struct str base64 = str_make ();
	base64_encode (hash, sizeof hash, &base64);
	return str_steal (&base64);
}

enum ws_status
{
	// Named according to the meaning specified in RFC 6455, section 11.7

	WS_STATUS_NORMAL_CLOSURE         = 1000,
	WS_STATUS_GOING_AWAY             = 1001,
	WS_STATUS_PROTOCOL_ERROR         = 1002,
	WS_STATUS_UNSUPPORTED_DATA       = 1003,
	WS_STATUS_INVALID_PAYLOAD_DATA   = 1007,
	WS_STATUS_POLICY_VIOLATION       = 1008,
	WS_STATUS_MESSAGE_TOO_BIG        = 1009,
	WS_STATUS_MANDATORY_EXTENSION    = 1010,
	WS_STATUS_INTERNAL_SERVER_ERROR  = 1011,

	// Reserved for internal usage
	WS_STATUS_NO_STATUS_RECEIVED     = 1005,
	WS_STATUS_ABNORMAL_CLOSURE       = 1006,
	WS_STATUS_TLS_HANDSHAKE          = 1015
};

// - - Frame parser  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum ws_parser_state
{
	WS_PARSER_FIXED,                    ///< Parsing fixed length part
	WS_PARSER_PAYLOAD_LEN_16,           ///< Parsing extended payload length
	WS_PARSER_PAYLOAD_LEN_64,           ///< Parsing extended payload length
	WS_PARSER_MASK,                     ///< Parsing masking-key
	WS_PARSER_PAYLOAD                   ///< Parsing payload
};

enum ws_opcode
{
	// Non-control
	WS_OPCODE_CONT   =  0,
	WS_OPCODE_TEXT   =  1,
	WS_OPCODE_BINARY =  2,

	// Control
	WS_OPCODE_CLOSE  =  8,
	WS_OPCODE_PING   =  9,
	WS_OPCODE_PONG   = 10
};

static bool
ws_is_control_frame (int opcode)
{
	return opcode >= WS_OPCODE_CLOSE;
}

struct ws_parser
{
	struct str input;                   ///< External input buffer
	enum ws_parser_state state;         ///< Parsing state

	unsigned is_fin     : 1;            ///< Final frame of a message?
	unsigned is_masked  : 1;            ///< Is the frame masked?
	unsigned reserved_1 : 1;            ///< Reserved
	unsigned reserved_2 : 1;            ///< Reserved
	unsigned reserved_3 : 1;            ///< Reserved
	enum ws_opcode opcode;              ///< Opcode
	uint32_t mask;                      ///< Frame mask
	uint64_t payload_len;               ///< Payload length

	bool (*on_frame_header) (void *user_data, const struct ws_parser *self);

	/// Callback for when a message is successfully parsed.
	/// The actual payload is stored in "input", of length "payload_len".
	bool (*on_frame) (void *user_data, const struct ws_parser *self);

	void *user_data;                    ///< User data for callbacks
};

static struct ws_parser
ws_parser_make (void)
{
	return (struct ws_parser) { .input = str_make () };
}

static void
ws_parser_free (struct ws_parser *self)
{
	str_free (&self->input);
}

static void
ws_parser_unmask (char *payload, uint64_t len, uint32_t mask)
{
	// This could be made faster.  For example by reading the mask in
	// native byte ordering and applying it directly here.

	uint64_t end = len & ~(uint64_t) 3;
	for (uint64_t i = 0; i < end; i += 4)
	{
		payload[i + 3] ^=  mask        & 0xFF;
		payload[i + 2] ^= (mask >>  8) & 0xFF;
		payload[i + 1] ^= (mask >> 16) & 0xFF;
		payload[i    ] ^= (mask >> 24) & 0xFF;
	}

	switch (len - end)
	{
	case 3:
		payload[end + 2] ^= (mask >>  8) & 0xFF;
		// Fall-through
	case 2:
		payload[end + 1] ^= (mask >> 16) & 0xFF;
		// Fall-through
	case 1:
		payload[end    ] ^= (mask >> 24) & 0xFF;
	}
}

static bool
ws_parser_push (struct ws_parser *self, const void *data, size_t len)
{
	bool success = false;
	str_append_data (&self->input, data, len);

	struct msg_unpacker unpacker =
		msg_unpacker_make (self->input.str, self->input.len);

	while (true)
	switch (self->state)
	{
		uint8_t u8;
		uint16_t u16;

	case WS_PARSER_FIXED:
		if (unpacker.len - unpacker.offset < 2)
			goto need_data;

		(void) msg_unpacker_u8 (&unpacker, &u8);
		self->is_fin      = (u8 >> 7) &   1;
		self->reserved_1  = (u8 >> 6) &   1;
		self->reserved_2  = (u8 >> 5) &   1;
		self->reserved_3  = (u8 >> 4) &   1;
		self->opcode      =  u8       &  15;

		(void) msg_unpacker_u8 (&unpacker, &u8);
		self->is_masked   = (u8 >> 7) &   1;
		self->payload_len =  u8       & 127;

		if (self->payload_len == 127)
			self->state = WS_PARSER_PAYLOAD_LEN_64;
		else if (self->payload_len == 126)
			self->state = WS_PARSER_PAYLOAD_LEN_16;
		else
			self->state = WS_PARSER_MASK;
		break;

	case WS_PARSER_PAYLOAD_LEN_16:
		if (!msg_unpacker_u16 (&unpacker, &u16))
			goto need_data;
		self->payload_len = u16;

		self->state = WS_PARSER_MASK;
		break;

	case WS_PARSER_PAYLOAD_LEN_64:
		if (!msg_unpacker_u64 (&unpacker, &self->payload_len))
			goto need_data;

		self->state = WS_PARSER_MASK;
		break;

	case WS_PARSER_MASK:
		if (!self->is_masked)
			goto end_of_header;
		if (!msg_unpacker_u32 (&unpacker, &self->mask))
			goto need_data;

	end_of_header:
		self->state = WS_PARSER_PAYLOAD;
		if (!self->on_frame_header (self->user_data, self))
			goto fail;
		break;

	case WS_PARSER_PAYLOAD:
		// Move the buffer so that payload data is at the front
		str_remove_slice (&self->input, 0, unpacker.offset);
		unpacker = msg_unpacker_make (self->input.str, self->input.len);

		if (self->input.len < self->payload_len)
			goto need_data;
		if (self->is_masked)
			ws_parser_unmask (self->input.str, self->payload_len, self->mask);
		if (!self->on_frame (self->user_data, self))
			goto fail;

		// And continue unpacking frames past the payload
		unpacker.offset = self->payload_len;
		self->state = WS_PARSER_FIXED;
		break;
	}

need_data:
	success = true;
fail:
	str_remove_slice (&self->input, 0, unpacker.offset);
	return success;
}

#endif

#ifdef LIBERTY_WANT_PROTO_MPD

#include <sys/un.h>

// --- MPD client interface ----------------------------------------------------

// This is a rather thin MPD client interface intended for basic tasks

#define MPD_SUBSYSTEM_TABLE(XX)                 \
	XX (DATABASE,         0, "database")        \
	XX (UPDATE,           1, "update")          \
	XX (STORED_PLAYLIST,  2, "stored_playlist") \
	XX (PLAYLIST,         3, "playlist")        \
	XX (PLAYER,           4, "player")          \
	XX (MIXER,            5, "mixer")           \
	XX (OUTPUT,           6, "output")          \
	XX (OPTIONS,          7, "options")         \
	XX (STICKER,          8, "sticker")         \
	XX (SUBSCRIPTION,     9, "subscription")    \
	XX (MESSAGE,         10, "message")

enum mpd_subsystem
{
#define XX(a, b, c) MPD_SUBSYSTEM_ ## a = (1 << b),
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
};

static const char *mpd_subsystem_names[] =
{
#define XX(a, b, c) [b] = c,
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum mpd_client_state
{
	MPD_DISCONNECTED,                   ///< Not connected
	MPD_CONNECTING,                     ///< Currently connecting
	MPD_CONNECTED                       ///< Connected
};

struct mpd_response
{
	bool success;                       ///< OK or ACK

	// ACK-only fields:

	int error;                          ///< Numeric error value (ack.h)
	int list_offset;                    ///< Offset of command in list
	char *current_command;              ///< Name of the erroring command
	char *message_text;                 ///< Error message
};

/// Task completion callback; on connection abortion most fields are 0
typedef void (*mpd_client_task_cb) (const struct mpd_response *response,
	const struct strv *data, void *user_data);

struct mpd_client_task
{
	LIST_HEADER (struct mpd_client_task)

	mpd_client_task_cb callback;        ///< Callback on completion
	void *user_data;                    ///< User data
};

struct mpd_client
{
	struct poller *poller;              ///< Poller

	// Connection:

	enum mpd_client_state state;        ///< Connection state
	struct connector *connector;        ///< Connection establisher

	int socket;                         ///< MPD socket
	struct str read_buffer;             ///< Input yet to be processed
	struct str write_buffer;            ///< Outut yet to be be sent out
	struct poller_fd socket_event;      ///< We can read from the socket

	struct poller_timer timeout_timer;  ///< Connection seems to be dead

	// Protocol:

	char *got_hello;                    ///< Version from OK MPD hello message

	bool idling;                        ///< Sent idle as the last command
	unsigned idling_subsystems;         ///< Subsystems we're idling for
	bool in_list;                       ///< We're inside a command list

	struct mpd_client_task *tasks;      ///< Task queue
	struct mpd_client_task *tasks_tail; ///< Tail of task queue
	struct strv data;                   ///< Data from last command

	// User configuration:

	void *user_data;                    ///< User data for callbacks

	/// Callback after connection has been successfully established
	void (*on_connected) (void *user_data);

	/// Callback for general failures or even normal disconnection;
	/// the interface is reinitialized
	void (*on_failure) (void *user_data);

	/// Callback to receive "idle" updates.
	/// Remember to restart the idle if needed.
	void (*on_event) (unsigned subsystems, void *user_data);

	/// Callback to trace protocol I/O
	void (*on_io_hook) (void *user_data, bool outgoing, const char *line);
};

static void mpd_client_reset (struct mpd_client *self);
static void mpd_client_destroy_connector (struct mpd_client *self);

static struct mpd_client
mpd_client_make (struct poller *poller)
{
	return (struct mpd_client)
	{
		.poller = poller,
		.socket = -1,
		.read_buffer = str_make (),
		.write_buffer = str_make (),
		.data = strv_make (),
		.socket_event = poller_fd_make (poller, -1),
		.timeout_timer = poller_timer_make (poller),
	};
}

static void
mpd_client_free (struct mpd_client *self)
{
	// So that we don't have to repeat most of the stuff
	mpd_client_reset (self);

	str_free (&self->read_buffer);
	str_free (&self->write_buffer);

	strv_free (&self->data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_dispatch (struct mpd_client *self, struct mpd_response *response)
{
	struct mpd_client_task *task;
	if (!(task = self->tasks))
		return;

	if (task->callback)
		task->callback (response, &self->data, task->user_data);
	strv_reset (&self->data);

	LIST_UNLINK_WITH_TAIL (self->tasks, self->tasks_tail, task);
	free (task);
}

/// Reinitialize the interface so that you can reconnect anew
static void
mpd_client_reset (struct mpd_client *self)
{
	// Get rid of all pending tasks to release resources etc.
	strv_reset (&self->data);
	struct mpd_response aborted = { .message_text = "Disconnected" };
	while (self->tasks)
		mpd_client_dispatch (self, &aborted);

	if (self->state == MPD_CONNECTING)
		mpd_client_destroy_connector (self);

	if (self->socket != -1)
		xclose (self->socket);
	self->socket = -1;

	// FIXME: this is not robust wrt. forking
	self->socket_event.closed = true;
	poller_fd_reset (&self->socket_event);
	poller_timer_reset (&self->timeout_timer);

	str_reset (&self->read_buffer);
	str_reset (&self->write_buffer);

	cstr_set (&self->got_hello, NULL);
	self->idling = false;
	self->idling_subsystems = 0;
	self->in_list = false;

	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_fail (struct mpd_client *self)
{
	mpd_client_reset (self);
	if (self->on_failure)
		self->on_failure (self->user_data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_parse_response (const char *p, struct mpd_response *response)
{
	if (!strcmp (p, "OK"))
		return response->success = true;

	char *end = NULL;
	if (*p++ != 'A' || *p++ != 'C' || *p++ != 'K' || *p++ != ' ' || *p++ != '[')
		return false;

	errno = 0;
	response->error = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != '@')
		return false;

	errno = 0;
	response->list_offset = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != ']' || *p++ != ' ' || *p++ != '{' || !(end = strchr (p, '}')))
		return false;

	response->current_command = xstrndup (p, end - p);
	p = end + 1;

	if (*p++ != ' ')
		return false;

	response->message_text = xstrdup (p);
	response->success = false;
	return true;
}

static bool
mpd_client_parse_hello (struct mpd_client *self, const char *line)
{
	const char hello[] = "OK MPD ";
	if (strncmp (line, hello, sizeof hello - 1))
	{
		print_debug ("invalid MPD hello message");
		return false;
	}

	// TODO: call "on_connected" now.  We should however also set up a timer
	//   so that we don't wait on this message forever.
	cstr_set (&self->got_hello, xstrdup (line + sizeof hello - 1));
	return true;
}

static bool
mpd_client_parse_line (struct mpd_client *self, const char *line)
{
	if (self->on_io_hook)
		self->on_io_hook (self->user_data, false, line);

	if (!self->got_hello)
		return mpd_client_parse_hello (self, line);

	struct mpd_response response;
	memset (&response, 0, sizeof response);
	if (!strcmp (line, "list_OK"))
		strv_append_owned (&self->data, NULL);
	else if (mpd_client_parse_response (line, &response))
		mpd_client_dispatch (self, &response);
	else
		strv_append (&self->data, line);

	free (response.current_command);
	free (response.message_text);
	return true;
}

/// All output from MPD commands seems to be in a trivial "key: value" format
static char *
mpd_client_parse_kv (char *line, char **value)
{
	char *sep;
	if (!(sep = strstr (line, ": ")))
		return NULL;

	*sep = 0;
	*value = sep + 2;
	return line;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_update_poller (struct mpd_client *self)
{
	poller_fd_set (&self->socket_event,
		self->write_buffer.len ? (POLLIN | POLLOUT) : POLLIN);
}

static bool
mpd_client_process_input (struct mpd_client *self)
{
	// Split socket input at newlines and process them separately
	struct str *rb = &self->read_buffer;
	char *start = rb->str, *end = start + rb->len;
	for (char *p = start; p < end; p++)
	{
		if (*p != '\n')
			continue;

		*p = 0;
		if (!mpd_client_parse_line (self, start))
			return false;
		start = p + 1;
	}

	str_remove_slice (rb, 0, start - rb->str);
	return true;
}

static void
mpd_client_on_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;

	struct mpd_client *self = user_data;
	if (socket_io_try_read  (self->socket, &self->read_buffer)  != SOCKET_IO_OK
	 || !mpd_client_process_input (self)
	 || socket_io_try_write (self->socket, &self->write_buffer) != SOCKET_IO_OK)
		mpd_client_fail (self);
	else
		mpd_client_update_poller (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_must_quote (const char *s)
{
	if (!*s)
		return true;
	for (; *s; s++)
		if ((unsigned char) *s <= ' ' || *s == '"' || *s == '\'')
			return true;
	return false;
}

static bool
mpd_client_must_escape_in_quote (char c)
{
	return c == '"' || c == '\'' || c == '\\';
}

static void
mpd_client_quote (const char *s, struct str *output)
{
	str_append_c (output, '"');
	for (; *s; s++)
	{
		if (mpd_client_must_escape_in_quote (*s))
			str_append_c (output, '\\');
		str_append_c (output, *s);
	}
	str_append_c (output, '"');
}

/// Beware that delivery of the event isn't deferred and you musn't make
/// changes to the interface while processing the event!
static void
mpd_client_add_task
	(struct mpd_client *self, mpd_client_task_cb cb, void *user_data)
{
	// This only has meaning with command_list_ok_begin, and then it requires
	// special handling (all in-list tasks need to be specially marked and
	// later flushed if an early ACK or OK arrives).
	hard_assert (!self->in_list);

	struct mpd_client_task *task = xcalloc (1, sizeof *self);
	task->callback = cb;
	task->user_data = user_data;
	LIST_APPEND_WITH_TAIL (self->tasks, self->tasks_tail, task);
}

/// Send a command.  Remember to call mpd_client_add_task() to handle responses,
/// unless the command is being sent in a list.
static void mpd_client_send_command
	(struct mpd_client *self, const char *command, ...) ATTRIBUTE_SENTINEL;

/// Avoid calling this method directly if you don't want things to explode
static void
mpd_client_send_command_raw (struct mpd_client *self, const char *raw)
{
	// Automatically interrupt idle mode
	if (self->idling)
	{
		poller_timer_reset (&self->timeout_timer);

		self->idling = false;
		self->idling_subsystems = 0;
		mpd_client_send_command (self, "noidle", NULL);
	}

	if (self->on_io_hook)
		self->on_io_hook (self->user_data, true, raw);

	str_append (&self->write_buffer, raw);
	str_append_c (&self->write_buffer, '\n');

	mpd_client_update_poller (self);
}

static void
mpd_client_send_commandv (struct mpd_client *self, char **fields)
{
	struct str line = str_make ();
	for (; *fields; fields++)
	{
		if (line.len)
			str_append_c (&line, ' ');

		if (mpd_client_must_quote (*fields))
			mpd_client_quote (*fields, &line);
		else
			str_append (&line, *fields);
	}
	mpd_client_send_command_raw (self, line.str);
	str_free (&line);
}

static void
mpd_client_send_command (struct mpd_client *self, const char *command, ...)
{
	struct strv v = strv_make ();

	va_list ap;
	va_start (ap, command);
	for (; command; command = va_arg (ap, const char *))
		strv_append (&v, command);
	va_end (ap);

	mpd_client_send_commandv (self, v.vector);
	strv_free (&v);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// "On success for all commands, OK is returned.  If a command fails, no more
/// commands are executed and the appropriate ACK error is returned"
static void
mpd_client_list_begin (struct mpd_client *self)
{
	hard_assert (!self->in_list);
	mpd_client_send_command (self, "command_list_begin", NULL);
	self->in_list = true;
}

/// Beware that "list_OK" turns into NULL values in the output vector
static void
mpd_client_list_ok_begin (struct mpd_client *self)
{
	hard_assert (!self->in_list);
	mpd_client_send_command (self, "command_list_ok_begin", NULL);
	self->in_list = true;
}

/// End a list of commands.  Remember to call mpd_client_add_task()
/// to handle the summary response.
static void
mpd_client_list_end (struct mpd_client *self)
{
	hard_assert (self->in_list);
	mpd_client_send_command (self, "command_list_end", NULL);
	self->in_list = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_resolve_subsystem (const char *name, unsigned *output)
{
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (!strcasecmp_ascii (name, mpd_subsystem_names[i]))
		{
			*output |= 1 << i;
			return true;
		}
	return false;
}

static void
mpd_client_on_idle_return (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) response;

	struct mpd_client *self = user_data;
	unsigned subsystems = 0;
	for (size_t i = 0; i < data->len; i++)
	{
		char *value, *key;
		if (!(key = mpd_client_parse_kv (data->vector[i], &value)))
			print_debug ("%s: %s", "erroneous MPD output", data->vector[i]);
		else if (strcasecmp_ascii (key, "changed"))
			print_debug ("%s: %s", "unexpected idle key", key);
		else if (!mpd_resolve_subsystem (value, &subsystems))
			print_debug ("%s: %s", "unknown subsystem", value);
	}

	// Not resetting "idling" here, we may send an extra "noidle" no problem
	if (self->on_event && subsystems)
		self->on_event (subsystems, self->user_data);
}

static void mpd_client_idle (struct mpd_client *self, unsigned subsystems);

static void
mpd_client_on_timeout (void *user_data)
{
	struct mpd_client *self = user_data;

	// Abort and immediately restore the current idle so that MPD doesn't
	// disconnect us, even though the documentation says this won't happen.
	// Just sending this out should bring a dead connection down over TCP.
	// TODO: set another timer to make sure we get a reply
	mpd_client_idle (self, self->idling_subsystems);
}

/// When not expecting to send any further commands, you should call this
/// in order to keep the connection alive.  Or to receive updates.
static void
mpd_client_idle (struct mpd_client *self, unsigned subsystems)
{
	hard_assert (!self->in_list);

	struct strv v = strv_make ();
	strv_append (&v, "idle");
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (subsystems & (1 << i))
			strv_append (&v, mpd_subsystem_names[i]);

	mpd_client_send_commandv (self, v.vector);
	strv_free (&v);

	self->timeout_timer.dispatcher = mpd_client_on_timeout;
	self->timeout_timer.user_data = self;
	poller_timer_set (&self->timeout_timer, 5 * 60 * 1000);

	mpd_client_add_task (self, mpd_client_on_idle_return, self);
	self->idling = true;
	self->idling_subsystems = subsystems;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_finish_connection (struct mpd_client *self, int socket)
{
	set_blocking (socket, false);
	self->socket = socket;
	self->state = MPD_CONNECTED;

	self->socket_event = poller_fd_make (self->poller, self->socket);
	self->socket_event.dispatcher = mpd_client_on_ready;
	self->socket_event.user_data = self;

	mpd_client_update_poller (self);

	if (self->on_connected)
		self->on_connected (self->user_data);
}

static void
mpd_client_destroy_connector (struct mpd_client *self)
{
	if (self->connector)
		connector_free (self->connector);
	free (self->connector);
	self->connector = NULL;

	// Not connecting anymore
	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_on_connector_failure (void *user_data)
{
	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_fail (self);
}

static void
mpd_client_on_connector_connected
	(void *user_data, int socket, const char *host)
{
	(void) host;

	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_finish_connection (self, socket);
}

static bool
mpd_client_connect_unix (struct mpd_client *self, const char *address,
	struct error **e)
{
	int fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		return error_set (e, "%s: %s", "socket", strerror (errno));

	// Expand tilde if needed
	char *expanded = resolve_filename (address, xstrdup);

	struct sockaddr_un sau;
	sau.sun_family = AF_UNIX;
	strncpy (sau.sun_path, expanded, sizeof sau.sun_path);
	sau.sun_path[sizeof sau.sun_path - 1] = 0;

	free (expanded);

	if (connect (fd, (struct sockaddr *) &sau, sizeof sau))
	{
		error_set (e, "%s: %s", "connect", strerror (errno));
		xclose (fd);
		return false;
	}

	mpd_client_finish_connection (self, fd);
	return true;
}

static bool
mpd_client_connect (struct mpd_client *self, const char *address,
	const char *service, struct error **e)
{
	hard_assert (self->state == MPD_DISCONNECTED);

	// If it looks like a path, assume it's a UNIX socket
	if (strchr (address, '/'))
		return mpd_client_connect_unix (self, address, e);

	struct connector *connector = xmalloc (sizeof *connector);
	connector_init (connector, self->poller);
	self->connector = connector;

	connector->user_data    = self;
	connector->on_connected = mpd_client_on_connector_connected;
	connector->on_failure   = mpd_client_on_connector_failure;

	connector_add_target (connector, address, service);
	self->state = MPD_CONNECTING;
	return true;
}

#endif
