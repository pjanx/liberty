/*
 * tests/liberty.c
 *
 * Copyright (c) 2015, Přemysl Janouch <p.janouch@gmail.com>
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

#define PROGRAM_NAME "test"
#define PROGRAM_VERSION "0"

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
	LIST_UNLINK (list, a[0]); a[0] = NULL;
	LIST_UNLINK (list, a[3]); a[3] = NULL;
	LIST_UNLINK (list, a[4]); a[4] = NULL;
	LIST_UNLINK (list, a[6]); a[6] = NULL;

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
	LIST_UNLINK_WITH_TAIL (list, tail, a[0]); a[0] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[3]); a[3] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[4]); a[4] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[6]); a[6] = NULL;
	LIST_UNLINK_WITH_TAIL (list, tail, a[9]); a[9] = NULL;

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
test_str_vector (void)
{
	struct str_vector v;
	str_vector_init (&v);

	str_vector_add_owned (&v, xstrdup ("xkcd"));
	str_vector_reset (&v);

	const char *a[] =
		{ "123", "456", "a", "bc", "def", "ghij", "klmno", "pqrstu" };

	// Add the first two items via another vector
	struct str_vector w;
	str_vector_init (&w);
	str_vector_add_args (&w, a[0], a[1], NULL);
	str_vector_add_vector (&v, w.vector);
	str_vector_free (&w);

	// Add an item and delete it right after
	str_vector_add (&v, "test");
	str_vector_remove (&v, v.len - 1);

	// Add the rest of the list properly
	for (int i = 2; i < (int) N_ELEMENTS (a); i++)
		str_vector_add (&v, a[i]);

	// Check the contents
	soft_assert (v.len == N_ELEMENTS (a));
	for (int i = 0; i < (int) N_ELEMENTS (a); i++)
		soft_assert (!strcmp (v.vector[i], a[i]));
	soft_assert (v.vector[v.len] == NULL);

	str_vector_free (&v);
}

static void
test_str (void)
{
	uint8_t x[] = { 0x12, 0x34, 0x56, 0x78, 0x11, 0x22, 0x33, 0x44 };

	struct str s;
	str_init (&s);
	str_ensure_space (&s, MEGA);
	str_append_data (&s, x, sizeof x);
	str_remove_slice (&s, 4, 4);
	soft_assert (s.len == 4);

	struct str t;
	str_init (&t);
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
	struct str_map m;
	str_map_init (&m);
	m.key_xfrm = tolower_ascii_strxfrm;
	m.free = free_counter;

	int *a = make_counter ();
	int *b = make_counter ();

	str_map_set (&m, "abc", ref_counter (a));
	soft_assert (str_map_find (&m, "ABC") == a);
	soft_assert (!str_map_find (&m, "DEFghi"));

	str_map_set (&m, "defghi", ref_counter (b));
	soft_assert (str_map_find (&m, "ABC") == a);
	soft_assert (str_map_find (&m, "DEFghi") == b);

	// Check that we can iterate over both of them
	struct str_map_iter iter;
	str_map_iter_init (&iter, &m);

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
	str_map_init (&m);
	m.free = free;

	for (size_t i = 0; i < 100 * 100; i++)
	{
		char *x = xstrdup_printf ("%zu", i);
		str_map_set (&m, x, x);
	}

	struct str_map_unset_iter unset_iter;
	str_map_unset_iter_init (&unset_iter, &m);
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
	const char valid  [] = "2H₂ + O₂ ⇌ 2H₂O, R = 4.7 kΩ, ⌀ 200 mm";
	const char invalid[] = "\xf0\x90\x28\xbc";
	soft_assert ( utf8_validate (valid,   sizeof valid));
	soft_assert (!utf8_validate (invalid, sizeof invalid));
}

static void
test_base64 (void)
{
	char data[65];
	for (size_t i = 0; i < N_ELEMENTS (data); i++)
		data[i] = i;

	struct str encoded;  str_init (&encoded);
	struct str decoded;  str_init (&decoded);

	base64_encode (data, sizeof data, &encoded);
	soft_assert (base64_decode (encoded.str, false, &decoded));
	soft_assert (decoded.len == sizeof data);
	soft_assert (!memcmp (decoded.str, data, sizeof data));

	str_free (&encoded);
	str_free (&decoded);
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
	test_add_simple (&test, "/str-vector",     NULL, test_str_vector);
	test_add_simple (&test, "/str",            NULL, test_str);
	test_add_simple (&test, "/error",          NULL, test_error);
	test_add_simple (&test, "/str-map",        NULL, test_str_map);
	test_add_simple (&test, "/utf-8",          NULL, test_utf8);
	test_add_simple (&test, "/base64",         NULL, test_base64);

	// TODO: write tests for the rest of the library

	return test_run (&test);
}
