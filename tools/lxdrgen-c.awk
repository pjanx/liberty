# lxdrgen-c.awk: C backend for lxdrgen.awk.
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Neither *_new() nor *_destroy() functions are provided, because they'd only
# be useful for top-levels, and are merely extra malloc()/free() calls.
# Users are expected to reuse buffers.
#
# Similarly, no constructors are produced--those are easy to write manually.
#
# All arrays are deserialized zero-terminated, so u8<> and i8<> can be directly
# used as C strings.
#
# All types must be able to dispose partially zero values going from the back,
# i.e., in the reverse order of deserialization.

function define_internal(name, ctype) {
	Types[name] = "internal"
	CodegenCType[name] = ctype
}

function define_int(shortname, ctype) {
	define_internal(shortname, ctype)
	CodegenSerialize[shortname] = \
		"\tstr_pack_" shortname "(w, %s);\n"
	CodegenDeserialize[shortname] = \
		"\tif (!msg_unpacker_" shortname "(r, &%s))\n" \
		"\t\treturn false;\n"
}

function define_sint(size) { define_int("i" size, "int" size "_t") }
function define_uint(size) { define_int("u" size, "uint" size "_t") }

function codegen_begin() {
	define_sint("8")
	define_sint("16")
	define_sint("32")
	define_sint("64")
	define_uint("8")
	define_uint("16")
	define_uint("32")
	define_uint("64")

	define_internal("string", "struct str")
	CodegenDispose["string"] = "\tstr_free(&%s);\n"
	CodegenSerialize["string"] = \
		"\tif (!proto_string_serialize(&%s, w))\n" \
		"\t\treturn false;\n"
	CodegenDeserialize["string"] = \
		"\tif (!proto_string_deserialize(&%s, r))\n" \
		"\t\treturn false;\n"

	define_internal("bool", "bool")
	CodegenSerialize["bool"] = \
		"\tstr_pack_u8(w, !!%s);\n"
	CodegenDeserialize["bool"] = \
		"\t{\n" \
		"\t\tuint8_t v = 0;\n" \
		"\t\tif (!msg_unpacker_u8(r, &v))\n" \
		"\t\t\treturn false;\n" \
		"\t\t%s = !!v;\n" \
		"\t}\n"

	print "// Code generated from " FILENAME ". DO NOT EDIT."
	print "// This file directly depends on liberty.c, but doesn't include it."
	print ""
	print "static bool"
	print "proto_string_serialize(const struct str *s, struct str *w) {"
	print "\tif (s->len > UINT32_MAX)"
	print "\t\treturn false;"
	print "\tstr_pack_u32(w, s->len);"
	print "\tstr_append_str(w, s);"
	print "\treturn true;"
	print "}"
	print ""
	print "static bool"
	print "proto_string_deserialize(struct str *s, struct msg_unpacker *r) {"
	print "\tuint32_t len = 0;"
	print "\tif (!msg_unpacker_u32(r, &len))"
	print "\t\treturn false;"
	print "\tif (msg_unpacker_get_available(r) < len)"
	print "\t\treturn false;"
	print "\t*s = str_make();"
	print "\tstr_append_data(s, r->data + r->offset, len);"
	print "\tr->offset += len;"
	print "\tif (!utf8_validate (s->str, s->len))"
	print "\t\treturn false;"
	print "\treturn true;"
	print "}"
}

function codegen_constant(name, value) {
	print ""
	print "enum { " PrefixUpper name " = " value " };"
}

function codegen_enum_value(name, subname, value, cg) {
	append(cg, "fields",
		"\t" PrefixUpper toupper(cameltosnake(name)) "_" subname \
		" = " value ",\n")
}

function codegen_enum(name, cg,    ctype) {
	ctype = "enum " PrefixLower cameltosnake(name)
	print ""
	print ctype " {"
	print cg["fields"] "};"

	# XXX: This should also check if it isn't out-of-range for any reason,
	# but our usage of sprintf() stands in the way a bit.
	CodegenSerialize[name] = "\tstr_pack_i8(w, %s);\n"
	CodegenDeserialize[name] = \
		"\t{\n" \
		"\t\tint8_t v = 0;\n" \
		"\t\tif (!msg_unpacker_i8(r, &v) || !v)\n" \
		"\t\t\treturn false;\n" \
		"\t\t%s = v;\n" \
		"\t}\n"

	CodegenCType[name] = ctype
	for (i in cg)
		delete cg[i]
}

function codegen_struct_tag(d, cg,    f) {
	f = "self->" d["name"]
	append(cg, "fields", "\t" CodegenCType[d["type"]] " " d["name"] ";\n")
	append(cg, "dispose", sprintf(CodegenDispose[d["type"]], f))
	append(cg, "serialize", sprintf(CodegenSerialize[d["type"]], f))
	# Do not deserialize here, that would be out of order.
}

function codegen_struct_field(d, cg,    f, dispose, serialize, deserialize) {
	f = "self->" d["name"]
	dispose = CodegenDispose[d["type"]]
	serialize = CodegenSerialize[d["type"]]
	deserialize = CodegenDeserialize[d["type"]]
	if (!d["isarray"]) {
		append(cg, "fields", "\t" CodegenCType[d["type"]] " " d["name"] ";\n")
		append(cg, "dispose", sprintf(dispose, f))
		append(cg, "serialize", sprintf(serialize, f))
		append(cg, "deserialize", sprintf(deserialize, f))
		return
	}

	append(cg, "fields",
		"\t" CodegenCType["u32"] " " d["name"] "_len;\n" \
		"\t" CodegenCType[d["type"]] " *" d["name"] ";\n")

	if (dispose)
		append(cg, "dispose", "\tif (" f ")\n" \
			"\t\tfor (size_t i = 0; i < " f "_len; i++)\n" \
			indent(indent(sprintf(dispose, f "[i]"))))
	append(cg, "dispose", "\tfree(" f ");\n")

	append(cg, "serialize", sprintf(CodegenSerialize["u32"], f "_len"))
	if (d["type"] == "u8" || d["type"] == "i8") {
		append(cg, "serialize",
			"\tstr_append_data(w, " f ", " f "_len);\n")
	} else if (serialize) {
		append(cg, "serialize",
			"\tfor (size_t i = 0; i < " f "_len; i++)\n" \
			indent(sprintf(serialize, f "[i]")))
	}

	append(cg, "deserialize", sprintf(CodegenDeserialize["u32"], f "_len") \
		"\tif (!(" f " = calloc(" f "_len + 1, sizeof *" f ")))\n" \
		"\t\treturn false;\n")
	if (d["type"] == "u8" || d["type"] == "i8") {
		append(cg, "deserialize",
			"\tif (msg_unpacker_get_available(r) < " f "_len)\n" \
			"\t\treturn false;\n" \
			"\tmemcpy(" f ", r->data + r->offset, " f "_len);\n" \
			"\tr->offset += " f "_len;\n")
	} else if (deserialize) {
		append(cg, "deserialize",
			"\tfor (size_t i = 0; i < " f "_len; i++)\n" \
			indent(sprintf(deserialize, f "[i]")))
	}
}

function codegen_struct(name, cg,    ctype, funcname) {
	ctype = "struct " PrefixLower cameltosnake(name)
	print ""
	print ctype " {"
	print cg["fields"] "};"

	if (cg["dispose"]) {
		funcname = PrefixLower cameltosnake(name) "_free"
		print ""
		print "static void\n" funcname "(" ctype " *self) {"
		print cg["dispose"] "}"

		CodegenDispose[name] = "\t" funcname "(&%s);\n"
	}
	if (cg["serialize"]) {
		funcname = PrefixLower cameltosnake(name) "_serialize"
		print ""
		print "static bool\n" \
			  funcname "(\n\t\tconst " ctype " *self, struct str *w) {"
		print cg["serialize"] "\treturn true;"
		print "}"

		CodegenSerialize[name] = "\tif (!" funcname "(&%s, w))\n" \
			"\t\treturn false;\n"
	}
	if (cg["deserialize"]) {
		funcname = PrefixLower cameltosnake(name) "_deserialize"
		print ""
		print "static bool\n" \
			  funcname "(\n\t\t" ctype " *self, struct msg_unpacker *r) {"
		print cg["deserialize"] "\treturn true;"
		print "}"

		CodegenDeserialize[name] = "\tif (!" funcname "(&%s, r))\n" \
			"\t\treturn false;\n"
	}

	CodegenCType[name] = ctype
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(name, d, cg) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = d["name"]
	append(cg, "fields", "\t" CodegenCType[d["type"]] " " d["name"] ";\n")
}

function codegen_union_struct( \
		name, casename, cg, scg,     structname, fieldname, fullcasename) {
	# Don't generate obviously useless structs.
	fullcasename = toupper(cameltosnake(cg["tagtype"])) "_" casename
	if (!scg["dispose"] && !scg["deserialize"]) {
		append(cg, "structless", "\tcase " PrefixUpper fullcasename ":\n")
		for (i in scg)
			delete scg[i]
		return
	}

	# And thus not all generated structs are present in Types.
	structname = name "_" casename
	fieldname = tolower(casename)
	codegen_struct(structname, scg)

	append(cg, "fields", "\t" CodegenCType[structname] " " fieldname ";\n")
	if (CodegenDispose[structname])
		append(cg, "dispose", "\tcase " PrefixUpper fullcasename ":\n" \
			indent(sprintf(CodegenDispose[structname], "self->" fieldname)) \
			"\t\tbreak;\n")

	# With no de/serialization code, this will simply recognize the tag.
	append(cg, "serialize", "\tcase " PrefixUpper fullcasename ":\n" \
		indent(sprintf(CodegenSerialize[structname], "self->" fieldname)) \
		"\t\tbreak;\n")
	append(cg, "deserialize", "\tcase " PrefixUpper fullcasename ":\n" \
		indent(sprintf(CodegenDeserialize[structname], "self->" fieldname)) \
		"\t\tbreak;\n")
}

function codegen_union(name, cg, exhaustive,    f, ctype, funcname) {
	ctype = "union " PrefixLower cameltosnake(name)
	print ""
	print ctype " {"
	print cg["fields"] "};"

	f = "self->" cg["tagname"]
	if (cg["dispose"]) {
		funcname = PrefixLower cameltosnake(name) "_free"
		print ""
		print "static void\n" funcname "(" ctype " *self) {"
		print "\tswitch (" f ") {"
		if (cg["structless"])
			print cg["structless"] \
				indent(sprintf(CodegenDispose[cg["tagtype"]], f)) "\t\tbreak;"
		print cg["dispose"] "\tdefault:"
		print "\t\tbreak;"
		print "\t}"
		print "}"

		CodegenDispose[name] = "\t" funcname "(&%s);\n"
	}
	{
		funcname = PrefixLower cameltosnake(name) "_serialize"
		print ""
		print "static bool\n" \
			  funcname "(\n\t\tconst " ctype " *self, struct str *w) {"
		print "\tswitch (" f ") {"
		if (cg["structless"])
			print cg["structless"] \
				indent(sprintf(CodegenSerialize[cg["tagtype"]], f)) "\t\tbreak;"
		print cg["serialize"] "\tdefault:"
		print "\t\treturn false;"
		print "\t}"
		print "\treturn true;"
		print "}"

		CodegenSerialize[name] = "\tif (!" funcname "(&%s, w))\n" \
			"\t\treturn false;\n"
	}
	{
		funcname = PrefixLower cameltosnake(name) "_deserialize"
		print ""
		print "static bool\n" \
			  funcname "(\n\t\t" ctype " *self, struct msg_unpacker *r) {"
		print sprintf(CodegenDeserialize[cg["tagtype"]], f)
		print "\tswitch (" f ") {"
		if (cg["structless"])
			print cg["structless"] "\t\tbreak;"
		print cg["deserialize"] "\tdefault:"
		print "\t\treturn false;"
		print "\t}"
		print "\treturn true;"
		print "}"

		CodegenDeserialize[name] = "\tif (!" funcname "(&%s, r))\n" \
			"\t\treturn false;\n"
	}

	CodegenCType[name] = ctype
	for (i in cg)
		delete cg[i]
}
