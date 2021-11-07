/*
 * tests/pulse.c
 *
 * Copyright (c) 2021, Přemysl Eric Janouch <p@janouch.name>
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

#include "../liberty.c"
#include "../liberty-pulse.c"

// --- Tests -------------------------------------------------------------------

enum
{
	EVENT_IO    =  1 << 0,
	EVENT_TIME  =  1 << 1,
	EVENT_DEFER =  1 << 2,
	EVENT_ALL   = (1 << 3) - 1
};

static intptr_t g_events = 0;
static intptr_t g_destroys = 0;

static void
io_event_cb (pa_mainloop_api *a,
	pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata)
{
	(void) a; (void) e; (void) fd; (void) events;
	g_events |= (intptr_t) userdata;
}

static void
io_event_destroy_cb (pa_mainloop_api *a, pa_io_event *e, void *userdata)
{
	(void) a; (void) e;
	g_destroys += (intptr_t) userdata;
}

static void
time_event_cb (pa_mainloop_api *a,
	pa_time_event *e, const struct timeval *tv, void *userdata)
{
	(void) a; (void) e; (void) tv;
	g_events |= (intptr_t) userdata;
}

static void
time_event_destroy_cb (pa_mainloop_api *a, pa_time_event *e, void *userdata)
{
	(void) a; (void) e;
	g_destroys += (intptr_t) userdata;
}

static void
defer_event_cb (pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	(void) a; (void) e;
	g_events |= (intptr_t) userdata;
}

static void
defer_event_destroy_cb (pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	(void) a; (void) e;
	g_destroys += (intptr_t) userdata;
}

static void
test_pulse (void)
{
	struct poller poller;
	poller_init (&poller);

	// Let's just get this over with, not aiming for high test coverage here
	pa_mainloop_api *api = poller_pa_new (&poller);

	pa_io_event *ie = api->io_new (api, STDOUT_FILENO, PA_IO_EVENT_OUTPUT,
		io_event_cb, (void *) EVENT_IO);
	api->io_set_destroy (ie, io_event_destroy_cb);

	const struct timeval tv = poller_pa_get_current_time ();
	pa_time_event *te = api->time_new (api, &tv,
		time_event_cb, (void *) EVENT_TIME);
	api->time_set_destroy (te, time_event_destroy_cb);
	api->time_restart (te, &tv);

	pa_defer_event *de = api->defer_new (api,
		defer_event_cb, (void *) EVENT_DEFER);
	api->defer_set_destroy (de, defer_event_destroy_cb);
	api->defer_enable (api->defer_new (api,
		defer_event_cb, (void *) EVENT_DEFER), false);

	alarm (1);
	while (g_events != EVENT_ALL)
		poller_run (&poller);

	poller_pa_destroy (api);
	soft_assert (g_destroys == EVENT_ALL);
	poller_free (&poller);
}

// --- Main --------------------------------------------------------------------

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);
	test_add_simple (&test, "/pulse", NULL, test_pulse);
	return test_run (&test);
}
