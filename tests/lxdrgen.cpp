/*
 * tests/lxdrgen.cpp
 *
 * Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
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

#include "lxdrgen.lxdr.cpp"

#include <cstdlib>

static void
hard_assert (bool condition, const char *description)
{
	if (!condition)
	{
		fprintf (stderr, "assertion failed: %s\n", description);
		abort ();
	}
}

#define hard_assert(condition) hard_assert (condition, #condition)

int
main (int argc, char *argv[])
{
	hard_assert (ProtoGen::VERSION == 1);

	enum { CASES = 3 };

	ProtoGen::Struct a = {}, b = {};
	a.u.resize (CASES + rand () % 100);
	for (size_t i = 0; i < a.u.size (); i++)
	{
		std::unique_ptr<ProtoGen::Union> &u = a.u[i];
		switch (i % CASES)
		{
		case 0:
		{
			auto numbers = new ProtoGen::Union_Numbers ();
			numbers->a = rand () % UINT8_MAX;
			numbers->b = rand () % UINT16_MAX;
			numbers->c = rand () % UINT32_MAX;
			numbers->d = rand () % UINT64_MAX;
			numbers->e = rand () % UINT8_MAX;
			numbers->f = rand () % UINT16_MAX;
			numbers->g = rand () % UINT32_MAX;
			numbers->h = rand () % UINT64_MAX;
			u.reset (numbers);
			break;
		}
		case 1:
		{
			auto others = new ProtoGen::Union_Others ();
			others->foo = rand () % 2;
			for (int i = rand () % 0x30; i > 0; i--)
				others->bar += 0x30 + i;
			for (int i = rand () % 0x30; i > 0; i--)
				others->baz.push_back (0x30 + i);
			u.reset (others);
			break;
		}
		case 2:
			u.reset (new ProtoGen::Union_Nothing ());
			break;
		default:
			hard_assert (!"unhandled case");
		}
	}

	a.o.reset (new ProtoGen::Onion_Nothing ());

	LibertyXDR::Writer buf;
	hard_assert (a.serialize (buf));
	LibertyXDR::Reader r;
	r.data = buf.data.data ();
	r.length = buf.data.size ();
	hard_assert (b.deserialize (r));
	hard_assert (!r.length);

	hard_assert (a.u.size () == b.u.size ());
	for (size_t i = 0; i < a.u.size (); i++)
	{
		ProtoGen::Union *ua = a.u[i].get ();
		ProtoGen::Union *ub = b.u[i].get ();
		hard_assert (ua->tag == ub->tag);
		switch (ua->tag)
		{
		case ProtoGen::Enum::NUMBERS:
		{
			auto a = dynamic_cast<ProtoGen::Union_Numbers *> (ua);
			auto b = dynamic_cast<ProtoGen::Union_Numbers *> (ub);
			hard_assert (a->a == b->a);
			hard_assert (a->b == b->b);
			hard_assert (a->c == b->c);
			hard_assert (a->d == b->d);
			hard_assert (a->e == b->e);
			hard_assert (a->f == b->f);
			hard_assert (a->g == b->g);
			hard_assert (a->h == b->h);
			break;
		}
		case ProtoGen::Enum::OTHERS:
		{
			auto a = dynamic_cast<ProtoGen::Union_Others *> (ua);
			auto b = dynamic_cast<ProtoGen::Union_Others *> (ub);
			hard_assert (a->foo == b->foo);
			hard_assert (a->bar == b->bar);
			hard_assert (a->baz == b->baz);
			break;
		}
		case ProtoGen::Enum::NOTHING:
			break;
		default:
			hard_assert (!"unexpected case");
		}
	}

	hard_assert (a.o->tag == b.o->tag);
	return 0;
}
