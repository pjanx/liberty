/*
 * tests/liberty.c
 *
 * Copyright (c) 2015 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

#define LIBERTY_WANT_POLLER
#define LIBERTY_WANT_ASYNC

#include "../liberty.c"

// --- Memory ------------------------------------------------------------------

#define KILO 1024
#define MEGA 1048576

static void
test_memory (void)
{
	void *m = xmalloc (MEGA);
	memset (m, 0, MEGA);

	void *n = xcalloc (KILO, KILO);
	soft_assert (!memcmp (n, m, MEGA));

	m = xrealloc (m, 1024);
	n = xreallocarray (n, KILO, 1);
	soft_assert (!memcmp (n, m, KILO));

	free (m);
	free (n);

	char *s = xstrdup ("test");
	char *t = xstrndup ("testing", 4);
	soft_assert (!strcmp (s, t));

	free (s);
	free (t);
}

// --- Linked lists ------------------------------------------------------------

struct my_link
{
	LIST_HEADER (struct my_link)
	int n;
};

static struct my_link *
make_link (int value)
{
	struct my_link *link = xcalloc (1, sizeof *link);
	link->n = value;
	return link;
}

static void
check_linked_list (struct my_link *list, struct my_link **a, int n)
{
	// The linked list must contain items from the array, in that order
	struct my_link *iter = list;
	for (int i = 0; i < n; i++)
	{
		if (!a[i])
			continue;

		hard_assert (iter != NULL);
		soft_assert (iter->n == i);
		iter = iter->next;
	}

	// And nothing more
	soft_assert (iter == NULL);
}

static void
test_list (void)
{
	struct my_link *list = NULL;
	struct my_link *a[10];

	// Prepare a linked list
	for (int i = N_ELEMENTS (a); i--; )
	{
		a[i] = make_link (i);
		LIST_PREPEND (list, a[i]);
	}

	// Remove a few entries
	LIST_UNLINK (list, a[0]); free (a[0]); a[0] = NULL;
	LIST_UNLINK (list, a[3]); free (a[3]); a[3] = NULL;
	LIST_UNLINK (list, a[4]); free (a[4]); a[4] = NULL;
	LIST_UNLINK (list, a[6]); free (a[6]); a[6] = NULL;

	// Prepend one more item
	a[0] = make_link (0);
	LIST_PREPEND (list, a[0]);

	// Check the contents
	check_linked_list (list, a, N_ELEMENTS (a));

	// Destroy the linked list
	LIST_FOR_EACH (struct my_link, iter, list)
		free (iter);
}

static void
test_list_with_tail (void)
{
	struct my_link *list = NULL;
	struct my_link *tail = NULL;
	struct my_link *a[10];

	// Prepare a linked list
	for (int i = 0; i < (int) N_ELEMENTS (a); i++)
	{
		a[i] = make_link (i);
		LIST_APPEND_WITH_TAIL (list, tail, a[i]);
	}

	// Remove a few entries
	LIST_UNLINK_WITH_TAIL (list, tail, a[0]); free (a[0]); a[0] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[3]); free (a[3]); a[3] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[4]); free (a[4]); a[4] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[6]); free (a[6]); a[6] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[9]); free (a[9]); a[9] = NULL;

	// Append one more item
	a[9] = make_link (9);
	LIST_APPEND_WITH_TAIL (list, tail, a[9]);

	// Check the contents
	check_linked_list (list, a, N_ELEMENTS (a));

	// Destroy the linked list
	LIST_FOR_EACH (struct my_link, iter, list)
		free (iter);
}

// --- Strings -----------------------------------------------------------------

static void
test_strv (void)
{
	struct strv v = strv_make ();
	strv_append_owned (&v, xstrdup ("xkcd"));
	strv_reset (&v);

	const char *a[] =
		{ "123", "456", "a", "bc", "def", "ghij", "klmno", "pqrstu" };

	// Add the first two items via another vector
	struct strv w = strv_make ();
	strv_append_args (&w, a[0], a[1], NULL);
	strv_append_vector (&v, w.vector);
	strv_free (&w);

	// Add an item and delete it right after
	strv_append (&v, "test");
	strv_remove (&v, v.len - 1);

	// Add the rest of the list properly
	for (int i = 2; i < (int) N_ELEMENTS (a); i++)
		strv_append (&v, a[i]);

	// Check the contents
	soft_assert (v.len == N_ELEMENTS (a));
	for (int i = 0; i < (int) N_ELEMENTS (a); i++)
		soft_assert (!strcmp (v.vector[i], a[i]));
	soft_assert (v.vector[v.len] == NULL);

	strv_free (&v);
}

static void
test_str (void)
{
	uint8_t x[] = { 0x12, 0x34, 0x56, 0x78, 0x11, 0x22, 0x33, 0x44 };

	struct str s = str_make ();
	str_reserve (&s, MEGA);
	str_append_data (&s, x, sizeof x);
	str_remove_slice (&s, 4, 4);
	soft_assert (s.len == 4);

	struct str t = str_make ();
	str_append_str (&t, &s);
	str_append (&t, "abc");
	str_append_c (&t, 'd');
	str_append_printf (&t, "efg");

	char *y = str_steal (&t);
	soft_assert (!strcmp (y, "\x12\x34\x56\x78" "abcdefg"));
	free (y);

	str_reset (&s);
	str_free (&s);
}

// --- Errors ------------------------------------------------------------------

static void
test_error (void)
{
	const char *m = "something fucked up";

	struct error *e = NULL;
	error_set (&e, "%s", m);

	struct error *f = NULL;
	error_propagate (&f, e);

	soft_assert (f != NULL);
	soft_assert (!strcmp (f->message, m));
	error_free (f);
}

// --- Hash map ----------------------------------------------------------------

static void
free_counter (void *data)
{
	int *counter = data;
	if (!--*counter)
		free (data);
}

static int *
make_counter (void)
{
	int *counter = xmalloc (sizeof *counter);
	*counter = 1;
	return counter;
}

static int *
ref_counter (int *counter)
{
	(*counter)++;
	return counter;
}

static void
test_str_map (void)
{
	// Put two reference counted objects in the map under case-insensitive keys
	struct str_map m = str_map_make (free_counter);
	m.key_xfrm = tolower_ascii_strxfrm;

	int *a = make_counter ();
	int *b = make_counter ();

	str_map_set (&m, "abc", ref_counter (a));
	soft_assert (str_map_find (&m, "ABC") == a);
	soft_assert (!str_map_find (&m, "DEFghi"));

	str_map_set (&m, "defghi", ref_counter (b));
	soft_assert (str_map_find (&m, "ABC") == a);
	soft_assert (str_map_find (&m, "DEFghi") == b);

	// Check that we can iterate over both of them
	struct str_map_iter iter = str_map_iter_make (&m);

	bool met_a = false;
	bool met_b = false;
	void *iter_data;
	while ((iter_data = str_map_iter_next (&iter)))
	{
		if (iter_data == a) { soft_assert (!met_a); met_a = true; }
		if (iter_data == b) { soft_assert (!met_b); met_b = true; }
		soft_assert (met_a || met_b);
	}
	soft_assert (met_a && met_b);

	// Remove one of the keys
	str_map_set (&m, "abc", NULL);
	soft_assert (!str_map_find (&m, "ABC"));
	soft_assert (str_map_find (&m, "DEFghi") == b);

	str_map_free (&m);

	// Check that the objects have been destroyed exactly once
	soft_assert (*a == 1);
	soft_assert (*b == 1);
	free_counter (a);
	free_counter (b);

	// Iterator test with a high number of items
	m = str_map_make (free);

	for (size_t i = 0; i < 100 * 100; i++)
	{
		char *x = xstrdup_printf ("%zu", i);
		str_map_set (&m, x, x);
	}

	struct str_map_unset_iter unset_iter = str_map_unset_iter_make (&m);
	while ((str_map_unset_iter_next (&unset_iter)))
	{
		unsigned long x;
		hard_assert (xstrtoul (&x, unset_iter.link->key, 10));
		if (x >= 100)
			str_map_set (&m, unset_iter.link->key, NULL);
	}
	str_map_unset_iter_free (&unset_iter);

	soft_assert (m.len == 100);
	str_map_free (&m);
}

static void
test_utf8 (void)
{
	const char *full = "\xc5\x99", *partial = full, *empty = full;
	soft_assert (utf8_decode (&full,    2) == 0x0159);
	soft_assert (utf8_decode (&partial, 1) == -2);
	soft_assert (utf8_decode (&empty,   0) == -1);

	const char valid_1[] = "2H₂ + O₂ ⇌ 2H₂O, R = 4.7 kΩ, ⌀ 200 mm";
	const char valid_2[] = "\xf0\x93\x82\xb9";
	const char invalid_1[] = "\xf0\x90\x28\xbc";
	const char invalid_2[] = "\xc0\x80";
	soft_assert ( utf8_validate (valid_1,   sizeof valid_1));
	soft_assert ( utf8_validate (valid_2,   sizeof valid_2));
	soft_assert (!utf8_validate (invalid_1, sizeof invalid_1));
	soft_assert (!utf8_validate (invalid_2, sizeof invalid_2));

	struct utf8_iter iter = utf8_iter_make ("fóọ");
	size_t ch_len;
	hard_assert (utf8_iter_next (&iter, &ch_len) == 'f'    && ch_len == 1);
	hard_assert (utf8_iter_next (&iter, &ch_len) == 0x00F3 && ch_len == 2);
	hard_assert (utf8_iter_next (&iter, &ch_len) == 0x1ECD && ch_len == 3);
}

static void
test_base64 (void)
{
	char data[65];
	for (size_t i = 0; i < N_ELEMENTS (data); i++)
		data[i] = i;

	struct str encoded = str_make ();
	struct str decoded = str_make ();

	base64_encode (data, sizeof data, &encoded);
	soft_assert (base64_decode (encoded.str, false, &decoded));
	soft_assert (decoded.len == sizeof data);
	soft_assert (!memcmp (decoded.str, data, sizeof data));

	str_free (&encoded);
	str_free (&decoded);
}

// --- Asynchronous jobs -------------------------------------------------------

struct test_async_data
{
	struct async_manager manager;       ///< Async manager
	struct async_getaddrinfo *gai;      ///< Address resolution job
	struct async_getnameinfo *gni;      ///< Name resolution job

	struct async busyloop;              ///< Busy job for cancellation
	bool finished;                      ///< End of test indicator
};

static void
on_getnameinfo (int err, char *host, char *service, void *user_data)
{
	(void) host;
	(void) service;

	hard_assert (!err);
	struct test_async_data *data = user_data;
	data->gni = NULL;

	async_cancel (&data->busyloop);
}

static void
on_getaddrinfo (int err, struct addrinfo *results, void *user_data)
{
	hard_assert (!err);
	struct test_async_data *data = user_data;
	data->gai = NULL;

	data->gni = async_getnameinfo
		(&data->manager, results->ai_addr, results->ai_addrlen, 0);
	data->gni->dispatcher = on_getnameinfo;
	data->gni->user_data = data;

	freeaddrinfo (results);
}

static void
on_busyloop_execute (struct async *async)
{
	(void) async;

	while (true)
		sleep (1);
}

static void
on_busyloop_destroy (struct async *async)
{
	CONTAINER_OF (async, struct test_async_data, busyloop)->finished = true;
}

static void
test_async (void)
{
	struct test_async_data data;
	memset (&data, 0, sizeof data);
	data.manager = async_manager_make ();

	data.busyloop = async_make (&data.manager);
	data.busyloop.execute = on_busyloop_execute;
	data.busyloop.destroy = on_busyloop_destroy;
	async_run (&data.busyloop);

	struct addrinfo hints;
	memset (&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;

	// Localhost should be network-independent and instantaneous
	data.gai = async_getaddrinfo (&data.manager, "127.0.0.1", "22", &hints);
	data.gai->dispatcher = on_getaddrinfo;
	data.gai->user_data = &data;

	struct pollfd pfd =
		{ .events = POLLIN, .fd = data.manager.finished_pipe[0] };

	// Eventually the busyloop should get cancelled and stop the loop
	while (!data.finished)
	{
		hard_assert (poll (&pfd, 1, 1000) == 1);
		async_manager_dispatch (&data.manager);
	}

	soft_assert (!data.gai);
	soft_assert (!data.gni);
	async_manager_free (&data.manager);
}

// --- Connector ---------------------------------------------------------------

// This also happens to test a large part of the poller implementation

#include <arpa/inet.h>

struct test_connector_fixture
{
	const char *host;                   ///< The host we're listening on
	int port;                           ///< The port we're listening on

	int listening_fd;                   ///< Listening FD

	struct poller poller;               ///< Poller
	struct poller_fd listening_event;   ///< Listening event
	bool quitting;                      ///< Quit signal for the event loop
};

static void
test_connector_on_client (const struct pollfd *pfd, void *user_data)
{
	(void) user_data;

	int fd = accept (pfd->fd, NULL, NULL);
	if (fd == -1)
	{
		if (errno == EAGAIN
		 || errno == EINTR
		 || errno == ECONNABORTED)
			return;

		exit_fatal ("%s: %s", "accept", strerror (errno));
	}

	const char message[] = "Hello!\n";
	(void) write (fd, message, strlen (message));
	xclose (fd);
}

static bool
test_connector_try_bind
	(struct test_connector_fixture *self, const char *host, int port)
{
	struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_port = htons ((self->port = port));
	sin.sin_addr.s_addr = inet_addr ((self->host = host));

	int fd = socket (AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return true;

	int yes = 1;
	(void) setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

	if (bind (fd, (struct sockaddr *) &sin, sizeof sin)
	 || listen (fd, 10))
	{
		xclose (fd);
		return false;
	}

	self->listening_fd = fd;
	return true;
}

static void
test_connector_fixture_init
	(const void *user_data, struct test_connector_fixture *self)
{
	(void) user_data;

	// Find a free port on localhost in the user range and bind to it
	for (int i = 0; i < 1024; i++)
		if (test_connector_try_bind (self, "127.0.0.1", 1024 + i))
			break;
	if (!self->listening_fd)
		exit_fatal ("cannot bind to localhost");

	// Make it so that we immediately accept all connections
	poller_init (&self->poller);
	self->listening_event = poller_fd_make (&self->poller, self->listening_fd);
	self->listening_event.dispatcher = test_connector_on_client;
	self->listening_event.user_data = (poller_fd_fn) self;
	poller_fd_set (&self->listening_event, POLLIN);
}

static void
test_connector_fixture_free
	(const void *user_data, struct test_connector_fixture *self)
{
	(void) user_data;

	poller_free (&self->poller);
	xclose (self->listening_fd);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
test_connector_on_connected (void *user_data, int socket, const char *hostname)
{
	struct test_connector_fixture *self = user_data;
	hard_assert (!strcmp (hostname, self->host));
	xclose (socket);

	self->quitting = true;
}

static void
test_connector_on_failure (void *user_data)
{
	(void) user_data;
	exit_fatal ("failed to connect to the prepared port");
}

static void
test_connector_on_connecting (void *user_data, const char *address)
{
	(void) user_data;
	print_debug ("connecting to %s", address);
}

static void
test_connector_on_error (void *user_data, const char *error)
{
	(void) user_data;
	print_debug ("%s: %s", "connecting failed", error);
}

static void
test_connector (const void *user_data, struct test_connector_fixture *self)
{
	(void) user_data;
	print_debug ("final target is %s:%d", self->host, self->port);

	struct connector connector;
	connector_init (&connector, &self->poller);
	connector.on_connecting = test_connector_on_connecting;
	connector.on_error      = test_connector_on_error;
	connector.on_connected  = test_connector_on_connected;
	connector.on_failure    = test_connector_on_failure;
	connector.user_data     = self;

	connector_add_target (&connector, ":D", "nonsense");

	char *port = xstrdup_printf ("%d", self->port);
	connector_add_target (&connector, self->host, port);
	free (port);

	while (!self->quitting)
		poller_run (&self->poller);

	connector_free (&connector);
}

// --- Configuration -----------------------------------------------------------

static void
on_test_config_foo_change (struct config_item *item)
{
	*(bool *) item->user_data = item->value.boolean;
}

static bool
test_config_validate_nonnegative
	(const struct config_item *item, struct error **e)
{
	if (item->type == CONFIG_ITEM_NULL)
		return true;

	hard_assert (item->type == CONFIG_ITEM_INTEGER);
	if (item->value.integer >= 0)
		return true;

	error_set (e, "must be non-negative");
	return false;
}

static const struct config_schema g_config_test[] =
{
	{ .name      = "foo",
	  .comment   = "baz",
	  .type      = CONFIG_ITEM_BOOLEAN,
	  .default_  = "off",
	  .on_change = on_test_config_foo_change },
	{ .name      = "bar",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = test_config_validate_nonnegative,
	  .default_  = "1" },
	{ .name      = "123",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "\"qux\\x01`\" \"\"`a`" },
	{}
};

static void
test_config_load (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_test, subtree, user_data);
}

static void
test_config (void)
{
	struct config config = config_make ();

	bool b = true;
	config_register_module (&config, "top", test_config_load, &b);
	config_load (&config, config_item_object ());
	config_schema_call_changed (config.root);
	hard_assert (b == false);

	struct config_item *invalid = config_item_integer (-1);
	hard_assert (!config_item_set_from (config_item_get (config.root,
		"top.bar", NULL), invalid, NULL));
	config_item_destroy (invalid);

	hard_assert (!strcmp ("qux\001`a",
		config_item_get (config.root, "top.123", NULL)->value.string.str));

	struct str s = str_make ();
	config_item_write (config.root, true, &s);
	print_debug ("%s", s.str);
	struct config_item *parsed = config_item_parse (s.str, s.len, false, NULL);
	hard_assert (parsed);
	config_item_destroy (parsed);
	str_free (&s);

	config_free (&config);
}

// --- Main --------------------------------------------------------------------

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);

	test_add_simple (&test, "/memory",         NULL, test_memory);
	test_add_simple (&test, "/list",           NULL, test_list);
	test_add_simple (&test, "/list-with-tail", NULL, test_list_with_tail);
	test_add_simple (&test, "/strv",           NULL, test_strv);
	test_add_simple (&test, "/str",            NULL, test_str);
	test_add_simple (&test, "/error",          NULL, test_error);
	test_add_simple (&test, "/str-map",        NULL, test_str_map);
	test_add_simple (&test, "/utf-8",          NULL, test_utf8);
	test_add_simple (&test, "/base64",         NULL, test_base64);
	test_add_simple (&test, "/async",          NULL, test_async);
	test_add_simple (&test, "/config",         NULL, test_config);

	test_add (&test, "/connector", struct test_connector_fixture, NULL,
		test_connector_fixture_init,
		test_connector,
		test_connector_fixture_free);

	// TODO: write tests for the rest of the library

	return test_run (&test);
}
