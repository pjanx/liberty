/*
 * liberty-xdg.c: the ultimate C unlibrary: freedesktop.org specifications
 *
 * Copyright (c) 2023 - 2024, Přemysl Eric Janouch <p@janouch.name>
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

// This files assumes you've already included liberty.c.

#ifdef LIBERTY_XDG_WANT_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#ifdef LIBERTY_XDG_WANT_ICONS
#include <png.h>
#endif

// --- XSettings ---------------------------------------------------------------

#ifdef LIBERTY_XDG_WANT_X11

struct xdg_xsettings_setting
{
	enum xdg_xsettings_type
	{
		XDG_XSETTINGS_INTEGER,
		XDG_XSETTINGS_STRING,
		XDG_XSETTINGS_COLOR,
	}
	type;                               ///< What's stored in the union
	uint32_t serial;                    ///< Serial of the last change
	union
	{
		int32_t integer;
		struct str string;
		struct { uint16_t red, green, blue, alpha; } color;
	};
};

static void
xdg_xsettings_setting_destroy (struct xdg_xsettings_setting *self)
{
	if (self->type == XDG_XSETTINGS_STRING)
		str_free (&self->string);
	free (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct xdg_xsettings
{
	struct str_map settings;            ///< Name -> xdg_xsettings_setting
};

static void
xdg_xsettings_free (struct xdg_xsettings *self)
{
	str_map_free (&self->settings);
}

static struct xdg_xsettings
xdg_xsettings_make (void)
{
	return (struct xdg_xsettings)
	{
		.settings =
			str_map_make ((str_map_free_fn) xdg_xsettings_setting_destroy),
	};
}

static void
xdg_xsettings_update (struct xdg_xsettings *self, Display *dpy)
{
	// TODO: We're supposed to lock the server.
	// TODO: We're supposed to trap X errors.
	char *selection = xstrdup_printf ("_XSETTINGS_S%d", DefaultScreen (dpy));
	Window owner
		= XGetSelectionOwner (dpy, XInternAtom (dpy, selection, True));
	free (selection);
	if (!owner)
		return;

	Atom actual_type = None;
	int actual_format = 0;
	unsigned long nitems = 0, bytes_after = 0;
	unsigned char *buffer = NULL;
	Atom xsettings = XInternAtom (dpy, "_XSETTINGS_SETTINGS", True);
	int status = XGetWindowProperty (dpy,
		owner,
		xsettings,
		0L,
		LONG_MAX,
		False,
		xsettings,
		&actual_type,
		&actual_format,
		&nitems,
		&bytes_after,
		&buffer);
	if (status != Success || !buffer)
		return;

	if (actual_type != xsettings
	 || actual_format != 8
	 || nitems < 12)
		goto fail;

	const struct peeker *peeker = NULL;
	if (buffer[0] == LSBFirst)
		peeker = &peeker_le;
	else if (buffer[0] == MSBFirst)
		peeker = &peeker_be;
	else
		goto fail;

	// We're ignoring the serial for now.
	uint32_t n_settings = peeker->u32 (buffer + 8);
	size_t offset = 12;
	struct str name = str_make ();
	struct xdg_xsettings_setting *setting = xcalloc (1, sizeof *setting);
	while (n_settings--)
	{
		if (nitems < offset + 4)
			goto fail_item;

		setting->type = buffer[offset];
		uint16_t name_len = peeker->u16 (buffer + offset + 2);
		offset += 4;
		if (nitems < offset + name_len)
			goto fail_item;

		str_append_data (&name, buffer + offset, name_len);
		offset += ((name_len + 3) & ~3);
		if (nitems < offset + 4)
			goto fail_item;

		setting->serial = peeker->u32 (buffer + offset);
		offset += 4;
		switch (setting->type)
		{
		case XDG_XSETTINGS_INTEGER:
			if (nitems < offset + 4)
				goto fail_item;

			setting->integer = (int32_t) peeker->u32 (buffer + offset);
			offset += 4;
			break;
		case XDG_XSETTINGS_STRING:
		{
			if (nitems < offset + 4)
				goto fail_item;

			uint32_t value_len = peeker->u32 (buffer + offset);
			offset += 4;
			if (nitems < offset + value_len)
				goto fail_item;

			setting->string = str_make ();
			str_append_data (&setting->string, buffer + offset, value_len);
			offset += ((value_len + 3) & ~3);
			break;
		}
		case XDG_XSETTINGS_COLOR:
			if (nitems < offset + 8)
				goto fail_item;

			setting->color.red   = peeker->u16 (buffer + offset);
			setting->color.green = peeker->u16 (buffer + offset + 2);
			setting->color.blue  = peeker->u16 (buffer + offset + 4);
			setting->color.alpha = peeker->u16 (buffer + offset + 6);
			offset += 8;
			break;
		default:
			goto fail_item;
		}

		// TODO(p): Change detection, by comparing existence and serials.
		str_map_set (&self->settings, name.str, setting);
		setting = xcalloc (1, sizeof *setting);
		str_reset (&name);
	}
fail_item:
	xdg_xsettings_setting_destroy (setting);
	str_free (&name);
fail:
	XFree (buffer);
}

#endif // LIBERTY_XDG_WANT_X11

// --- Desktop file parser -----------------------------------------------------

// Useful for parsing desktop-entry-spec, icon-theme-spec, trash-spec,
// mime-apps-spec.  This code is not designed for making changes to the files.

struct desktop_file
{
	struct str_map groups;              ///< Group name → Key → Value
};

static void
desktop_file_free_group (void *value)
{
	str_map_free (value);
	free (value);
}

static void
desktop_file_free (struct desktop_file *self)
{
	str_map_free (&self->groups);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
desktop_file_parse_line (struct desktop_file *self,
	char **group_name, const char *line, const char *end)
{
	struct str_map *group = NULL;
	if (*group_name)
		group = str_map_find (&self->groups, *group_name);

	if (*line == '[')
	{
		bool ok = *--end == ']';
		for (const char *p = ++line; ok && p != end; p++)
			ok = (unsigned char) *p >= 32 && (unsigned char) *p <= 127
				&& *p != '[' && *p != ']';
		if (!ok)
		{
			cstr_set (group_name, NULL);
			print_debug ("invalid desktop file group header");
			return;
		}

		cstr_set (group_name, xstrndup (line, end - line));
		if (str_map_find (&self->groups, *group_name))
		{
			print_debug ("duplicate desktop file group: %s", *group_name);
			return;
		}

		group = xcalloc (1, sizeof *group);
		*group = str_map_make (free);
		str_map_set (&self->groups, *group_name, group);
		return;
	}
	if (!group)
	{
		print_debug ("unexpected desktop file entry outside of a group");
		return;
	}

	const char *key_end = line;
	while (key_end != end && (isalnum_ascii (*key_end) || *key_end == '-'))
		key_end++;

	// We could validate these further, but we just search in them anyway.
	if (key_end != end && *key_end == '[')
	{
		while (++key_end != end && *key_end != ']')
			;
		if (key_end != end && *key_end == ']')
			key_end++;
	}

	const char *value = key_end;
	while (value != end && *value == ' ')
		value++;
	if (value == end || *value++ != '=')
	{
		print_debug ("invalid desktop file entry");
		return;
	}
	while (value != end && *value == ' ')
		value++;

	char *key = xstrndup (line, key_end - line);
	if (str_map_find (group, key))
		print_debug ("duplicate desktop file entry for: %s", key);
	else
		str_map_set (group, key, xstrndup (value, end - value));
	free (key);
}

static struct desktop_file
desktop_file_make (const char *data, size_t len)
{
	struct desktop_file self = (struct desktop_file)
		{ .groups = str_map_make (desktop_file_free_group) };

	char *group_name = NULL;
	const char *p = data, *data_end = p + len;
	while (p != data_end)
	{
		const char *line = p, *line_end = line;
		while (line_end != data_end && *line_end != '\n')
			line_end++;
		if ((p = line_end) != data_end && *p == '\n')
			p++;

		if (line != line_end && *line != '#')
			desktop_file_parse_line (&self, &group_name, line, line_end);
	}
	free (group_name);
	return self;
}

static const char *
desktop_file_get (struct desktop_file *self, const char *group, const char *key)
{
	// TODO(p): Ideally, also implement localised keys.
	struct str_map *group_map = str_map_find (&self->groups, group);
	if (!group_map)
		return NULL;

	return str_map_find (group_map, key);
}

static struct strv
desktop_file_unescape (const char *value, bool is_list)
{
	struct strv result = strv_make ();
	struct str s = str_make ();

	// XXX: The unescaping behaviour is underspecified.
	//   It might make sense to warn about unrecognised escape sequences.
	bool escape = false;
	for (const char *p = value; *p; p++)
	{
		if (escape)
		{
			switch (*p)
			{
			break; case 's': str_append_c (&s, ' ');
			break; case 'n': str_append_c (&s, '\n');
			break; case 't': str_append_c (&s, '\t');
			break; case 'r': str_append_c (&s, '\r');
			break; default:  str_append_c (&s, *p);
			}
			escape = false;
		}
		else if (*p == '\\' && p[1])
			escape = true;
		else if (*p == ';' && is_list)
		{
			strv_append_owned (&result, str_steal (&s));
			s = str_make ();
		}
		else
			str_append_c (&s, *p);
	}

	if (!is_list || s.len != 0)
		strv_append_owned (&result, str_steal (&s));
	else
		str_free (&s);
	return result;
}

static char *
desktop_file_get_string (struct desktop_file *self,
	const char *group, const char *key)
{
	const char *value = desktop_file_get (self, group, key);
	if (!value)
		return NULL;

	struct strv values = desktop_file_unescape (value, false /* is_list */);
	char *unescaped = strv_steal (&values, 0);
	strv_free (&values);
	return unescaped;
}

static struct strv
desktop_file_get_stringv (struct desktop_file *self,
	const char *group, const char *key)
{
	const char *value = desktop_file_get (self, group, key);
	if (!value)
		return strv_make ();

	return desktop_file_unescape (value, true /* is_list */);
}

static bool
desktop_file_get_bool (struct desktop_file *self,
	const char *group, const char *key)
{
	const char *value = desktop_file_get (self, group, key);
	if (!value)
		return false;

	// Let's be compatible with pre-1.0 files when it costs us so little.
	if (!strcmp (value, "true")
	 || !strcmp (value, "1"))
		return true;
	if (!strcmp (value, "false")
	 || !strcmp (value, "0"))
		return false;

	print_debug ("invalid desktop file boolean for '%s': %s", key, value);
	return false;
}

// Nothing uses the "numeric" type.
// "icon-theme-spec" uses "integer" and doesn't say what it is.
static long
desktop_file_get_integer (struct desktop_file *self,
	const char *group, const char *key)
{
	const char *value = desktop_file_get (self, group, key);
	if (!value)
		return 0;

	char *end = NULL;
	long parsed = (errno = 0, strtol (value, &end, 10));
	if (errno != 0 || *end)
		print_debug ("invalid desktop file integer for '%s': %s", key, value);
	return parsed;
}

// --- Icon themes -------------------------------------------------------------

// This implements part of the Icon Theme Specification.

#ifdef LIBERTY_XDG_WANT_ICONS

struct icon_theme_icon
{
	uint32_t width;                     ///< Width of argb in pixels
	uint32_t height;                    ///< Height of argb in pixels
	uint32_t argb[];                    ///< ARGB32 data, unassociated alpha
};

static void
icon_theme_open_on_error (png_structp pngp, const char *error)
{
	print_debug ("%s: %s", (const char *) png_get_error_ptr (pngp), error);
	png_longjmp (pngp, 1);
}

static void
icon_theme_open_on_warning (png_structp pngp, const char *warning)
{
	(void) pngp;
	(void) warning;
	// Fuck your "gamma value does not match libpng estimate".
}

// For simplicity, only support PNG icons, using the most popular library.
static struct icon_theme_icon *
icon_theme_open (const char *path)
{
	volatile png_bytep buffer = NULL;
	volatile png_bytepp row_pointers = NULL;
	struct icon_theme_icon *volatile result = NULL;
	FILE *fp = fopen (path, "rb");
	if (!fp)
	{
		if (errno != ENOENT)
			print_debug ("%s: %s", path, strerror (errno));
		return NULL;
	}

	// The simplified and high-level APIs aren't powerful enough.
	png_structp pngp = png_create_read_struct (PNG_LIBPNG_VER_STRING,
		(png_voidp) path, icon_theme_open_on_error, icon_theme_open_on_warning);
	png_infop infop = png_create_info_struct (pngp);
	if (!infop)
	{
		print_debug ("%s: %s", path, strerror (errno));
		goto fail;
	}
	if (setjmp (png_jmpbuf (pngp)))
		goto fail;

	png_init_io (pngp, fp);
	png_read_info (pngp, infop);

	// Asking for at least 8-bit channels.  This call is a superset of:
	//  - png_set_palette_to_rgb(),
	//  - png_set_tRNS_to_alpha(),
	//  - png_set_expand_gray_1_2_4_to_8().
	png_set_expand (pngp);

	// Reduce the possibilities further to RGB or RGBA...
	png_set_gray_to_rgb (pngp);

	// ...and /exactly/ 8-bit channels.
	// Alternatively, use png_set_expand_16() above to obtain 16-bit channels.
	png_set_scale_16 (pngp);

	// PNG uses RGBA order, let's change that to ARGB (both in memory order).
	// This doesn't change a row's `color_type` in png_do_read_filler(),
	// and the following transformation thus ignores it.
	png_set_add_alpha (pngp, 0xFFFF, PNG_FILLER_BEFORE);
	png_set_swap_alpha (pngp);

	(void) png_set_interlace_handling (pngp);

	png_read_update_info (pngp, infop);
	if (png_get_bit_depth (pngp, infop) != 8
	 || png_get_channels (pngp, infop) != 4
	 || png_get_color_type (pngp, infop) != PNG_COLOR_TYPE_RGB_ALPHA)
		png_error (pngp, "result not A8R8G8B8");

	size_t row_bytes = png_get_rowbytes (pngp, infop);
	size_t height = png_get_image_height (pngp, infop);
	buffer = xcalloc (row_bytes, height);
	row_pointers = xcalloc (height, sizeof buffer);
	for (size_t y = 0; y < height; y++)
		row_pointers[y] = buffer + y * row_bytes;

	png_read_image (pngp, row_pointers);

	result = xcalloc (1, sizeof *result + row_bytes * height);
	result->width = png_get_image_width (pngp, infop);
	result->height = height;

	uint32_t *dst = (uint32_t *) result->argb, *src = (uint32_t *) buffer;
	for (size_t pixels = result->width * result->height; pixels--; )
		*dst++ = ntohl (*src++);

fail:
	free (buffer);
	free (row_pointers);
	png_destroy_read_struct (&pngp, &infop, NULL);
	fclose (fp);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct icon_theme_find_context
{
	struct strv base;                   ///< Base directories
	struct str_map visited;             ///< Cycle prevention

	ARRAY (struct icon_theme_icon *, icons)
};

static void
icon_theme_find__fallback (struct icon_theme_find_context *ctx,
	const char *name)
{
	for (size_t i = 0; i < ctx->base.len; i++)
	{
		char *path = xstrdup_printf ("%s/%s.png", ctx->base.vector[i], name);
		struct icon_theme_icon *icon = icon_theme_open (path);
		free (path);
		if (icon)
		{
			ARRAY_RESERVE (ctx->icons, 1);
			ctx->icons[ctx->icons_len++] = icon;
			return;
		}
	}
}

static struct desktop_file
icon_theme_find__index (struct icon_theme_find_context *ctx, const char *theme)
{
	struct str data = str_make ();
	for (size_t i = 0; i < ctx->base.len; i++)
	{
		struct error *e = NULL;
		char *path = xstrdup_printf ("%s/%s/index.theme",
			ctx->base.vector[i], theme);
		read_file (path, &data, &e);
		free (path);
		if (!e)
			break;

		if (errno != ENOENT)
			print_debug ("%s", e->message);
		error_free (e);
	}

	struct desktop_file index = desktop_file_make (data.str, data.len);
	str_free (&data);
	return index;
}

static void
icon_theme_find__named (struct icon_theme_find_context *ctx,
	const char *theme, const char *name)
{
	// Either a cycle, or a common ancestor of inherited themes, which is valid.
	if (str_map_find (&ctx->visited, theme))
		return;

	str_map_set (&ctx->visited, theme, (void *) (intptr_t) 1);
	struct desktop_file index = icon_theme_find__index (ctx, theme);

	char *directories =
		desktop_file_get_string (&index, "Icon Theme", "Directories");
	if (!directories)
		goto out;

	// NOTE: The sizes are not deduplicated, and priorities are uncertain.
	struct strv dirs = strv_make ();
	cstr_split (directories, ",", true, &dirs);
	free (directories);
	for (size_t d = 0; d < dirs.len; d++)
	{
		// The hicolor icon theme stuffs everything in Directories.
		if (desktop_file_get (&index, dirs.vector[d], "Scale")
		 && desktop_file_get_integer (&index, dirs.vector[d], "Scale") != 1)
			continue;

		for (size_t i = 0; i < ctx->base.len; i++)
		{
			char *path = xstrdup_printf ("%s/%s/%s/%s.png",
				ctx->base.vector[i], theme, dirs.vector[d], name);
			struct icon_theme_icon *icon = icon_theme_open (path);
			free (path);
			if (icon)
			{
				ARRAY_RESERVE (ctx->icons, 1);
				ctx->icons[ctx->icons_len++] = icon;
				break;
			}
		}
	}
	strv_free (&dirs);
	if (ctx->icons_len)
		goto out;

	char *inherits =
		desktop_file_get_string (&index, "Icon Theme", "Inherits");
	if (inherits)
	{
		struct strv parents = strv_make ();
		cstr_split (inherits, ",", true, &parents);
		free (inherits);
		for (size_t i = 0; i < parents.len; i++)
		{
			icon_theme_find__named (ctx, parents.vector[i], name);
			if (ctx->icons_len)
				break;
		}
		strv_free (&parents);
	}

out:
	desktop_file_free (&index);
}

/// Return all base directories appropriate for icon search.
static struct strv
icon_theme_get_base_directories (void)
{
	struct strv dirs = strv_make ();
	struct str icons = str_make ();
	(void) str_append_env_path (&icons, "HOME", false);
	str_append (&icons, "/.icons");
	strv_append_owned (&dirs, str_steal (&icons));

	// Note that we use XDG_CONFIG_HOME as well, which might be intended.
	struct strv xdg = strv_make ();
	get_xdg_data_dirs (&xdg);
	for (size_t i = 0; i < xdg.len; i++)
		strv_append_owned (&dirs, xstrdup_printf ("%s/icons", xdg.vector[i]));
	strv_free (&xdg);

	strv_append (&dirs, "/usr/share/pixmaps");
	return dirs;
}

static int
icon_theme_find__compare (const void *a, const void *b)
{
	const struct icon_theme_icon **ia = (const struct icon_theme_icon **) a;
	const struct icon_theme_icon **ib = (const struct icon_theme_icon **) b;
	double pa = (double) (*ia)->width * (*ia)->height;
	double pb = (double) (*ib)->width * (*ib)->height;
	return (pa > pb) - (pa < pb);
}

/// Return all sizes of the named icon.  When the theme name is not NULL,
/// use it as the preferred theme.  Always consult fallbacks locations.
/// Ignore icon scales other than 1.
static struct icon_theme_icon **
icon_theme_find (const char *theme, const char *name, size_t *len)
{
	struct icon_theme_find_context ctx = {};
	ctx.base = icon_theme_get_base_directories ();
	ctx.visited = str_map_make (NULL);
	ARRAY_INIT (ctx.icons);

	if (theme)
		icon_theme_find__named (&ctx, theme, name);
	if (!ctx.icons_len)
		icon_theme_find__named (&ctx, "hicolor", name);
	if (!ctx.icons_len)
		icon_theme_find__fallback (&ctx, name);

	strv_free (&ctx.base);
	str_map_free (&ctx.visited);

	ARRAY_RESERVE (ctx.icons, 1);
	ctx.icons[ctx.icons_len] = NULL;
	if (!ctx.icons_len)
	{
		free (ctx.icons);
		return NULL;
	}

	qsort (ctx.icons,
		ctx.icons_len, sizeof *ctx.icons, icon_theme_find__compare);
	*len = ctx.icons_len;
	return ctx.icons;
}

static void
icon_theme_free (struct icon_theme_icon **icons)
{
	for (struct icon_theme_icon **p = icons; *p; p++)
		free (*p);
	free (icons);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifdef LIBERTY_XDG_WANT_X11

static void
icon_theme_set_window_icon (Display *dpy,
	Window window, const char *theme, const char *name)
{
	size_t icons_len = 0;
	struct icon_theme_icon **icons = icon_theme_find (theme, name, &icons_len);
	if (!icons)
		return;

	size_t n = 0;
	for (size_t i = 0; i < icons_len; i++)
		n += 2 + icons[i]->width * icons[i]->height;

	unsigned long *data = xcalloc (n, sizeof *data), *p = data;
	for (size_t i = 0; i < icons_len; i++)
	{
		*p++ = icons[i]->width;
		*p++ = icons[i]->height;

		uint32_t *q = icons[i]->argb;
		for (size_t k = icons[i]->width * icons[i]->height; k--; )
			*p++ = *q++;
	}

	XChangeProperty (dpy, window, XInternAtom (dpy, "_NET_WM_ICON", False),
		XA_CARDINAL, 32, PropModeReplace, (const unsigned char *) data, n);
	free (data);
	icon_theme_free (icons);
}

#endif // LIBERTY_XDG_WANT_X11
#endif // LIBERTY_XDG_WANT_ICONS
