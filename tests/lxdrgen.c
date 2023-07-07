/*
 * tests/lxdrgen.c
 *
 * Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
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
#include "lxdrgen.lxdr.c"

static void
test_ser_deser_free (void)
{
	hard_assert (PROTO_GEN_VERSION == 1);

	enum { CASES = 3 };

	struct proto_gen_struct a = {}, b = {};
	a.u = xcalloc ((a.u_len = CASES + rand () % 100), sizeof *a.u);
	for (size_t i = 0; i < a.u_len; i++)
	{
		union proto_gen_union *u = a.u + i;
		switch (i % CASES)
		{
		case 0:
			u->tag = PROTO_GEN_ENUM_NUMBERS;
			u->numbers.a = rand () % UINT8_MAX;
			u->numbers.b = rand () % UINT16_MAX;
			u->numbers.c = rand () % UINT32_MAX;
			u->numbers.d = rand () % UINT64_MAX;
			u->numbers.e = rand () % UINT8_MAX;
			u->numbers.f = rand () % UINT16_MAX;
			u->numbers.g = rand () % UINT32_MAX;
			u->numbers.h = rand () % UINT64_MAX;
			break;
		case 1:
			u->tag = PROTO_GEN_ENUM_OTHERS;
			u->others.foo = rand () % 2;
			u->others.bar = str_make ();
			for (int i = rand () % 0x30; i > 0; i--)
				str_append_c (&u->others.bar, 0x30 + i);
			u->others.baz_len = rand () % 0x30;
			u->others.baz = xcalloc (1, u->others.baz_len);
			for (uint32_t i = 0; i < u->others.baz_len; i++)
				u->others.baz[i] = 0x30 + i;
			break;
		case 2:
			u->tag = PROTO_GEN_ENUM_NOTHING;
			break;
		default:
			hard_assert (!"unhandled case");
		}
	}

	a.o.tag = PROTO_GEN_ENUM_NOTHING;

	struct str buf = str_make ();
	hard_assert (proto_gen_struct_serialize (&a, &buf));
	struct msg_unpacker r = msg_unpacker_make (buf.str, buf.len);
	hard_assert (proto_gen_struct_deserialize (&b, &r));
	hard_assert (!msg_unpacker_get_available (&r));
	str_free (&buf);

	hard_assert (a.u_len == b.u_len);
	for (size_t i = 0; i < a.u_len; i++)
	{
		union proto_gen_union *ua = a.u + i;
		union proto_gen_union *ub = b.u + i;
		hard_assert (ua->tag == ub->tag);
		switch (ua->tag)
		{
		case PROTO_GEN_ENUM_NUMBERS:
			hard_assert (ua->numbers.a == ub->numbers.a);
			hard_assert (ua->numbers.b == ub->numbers.b);
			hard_assert (ua->numbers.c == ub->numbers.c);
			hard_assert (ua->numbers.d == ub->numbers.d);
			hard_assert (ua->numbers.e == ub->numbers.e);
			hard_assert (ua->numbers.f == ub->numbers.f);
			hard_assert (ua->numbers.g == ub->numbers.g);
			hard_assert (ua->numbers.h == ub->numbers.h);
			break;
		case PROTO_GEN_ENUM_OTHERS:
			hard_assert (ua->others.foo == ub->others.foo);
			hard_assert (ua->others.bar.len == ub->others.bar.len);
			hard_assert (!memcmp (ua->others.bar.str, ub->others.bar.str,
				ua->others.bar.len));
			hard_assert (ua->others.baz_len == ub->others.baz_len);
			hard_assert (!memcmp (ua->others.baz, ub->others.baz,
				ua->others.baz_len));
			break;
		case PROTO_GEN_ENUM_NOTHING:
			break;
		default:
			hard_assert (!"unexpected case");
		}
	}

	hard_assert (a.o.tag == b.o.tag);

	// Emulate partially deserialized data to test disposal of that.
	for (size_t i = b.u_len - CASES; i < b.u_len; i++)
	{
		proto_gen_union_free (&b.u[i]);
		memset (&b.u[i], 0, sizeof b.u[i]);
	}

	proto_gen_struct_free (&a);
	proto_gen_struct_free (&b);
}

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);

	test_add_simple (&test, "/ser-deser-free", NULL, test_ser_deser_free);

	return test_run (&test);
}
