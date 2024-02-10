/*
 * tests/xdg.c
 *
 * Copyright (c) 2024, Přemysl Eric Janouch <p@janouch.name>
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

#include "../liberty.c"
#include "../liberty-xdg.c"

static const char file[] =
	"# This only tests the happy paths\n"
	"[Desktop Entry]\n"
	"Version = 1.0\n"
	"Name=\\s\\n\\t\\r\\\\\n"
	"Name[fr]=Nom\n"
	"Hidden=true\n"
	"Categories=Utility;TextEditor;\n"
	"Number=42";

static void
test_desktop_file (void)
{
	struct desktop_file entry = desktop_file_make (file, sizeof file - 1);
	const char *group = "Desktop Entry";

	char *value = desktop_file_get_string (&entry, group, "Version");
	hard_assert (!strcmp (value, "1.0"));
	cstr_set (&value, desktop_file_get_string (&entry, group, "Name"));
	hard_assert (!strcmp (value, " \n\t\r\\"));
	free (value);

	hard_assert (desktop_file_get_bool (&entry, group, "Hidden"));
	struct strv values = desktop_file_get_stringv (&entry, group, "Categories");
	hard_assert (values.len == 2);
	hard_assert (!strcmp (values.vector[0], "Utility"));
	hard_assert (!strcmp (values.vector[1], "TextEditor"));
	strv_free (&values);
	hard_assert (desktop_file_get_integer (&entry, group, "Number") == 42);

	desktop_file_free (&entry);
}

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);

	test_add_simple (&test, "/desktop-file", NULL, test_desktop_file);

	return test_run (&test);
}
