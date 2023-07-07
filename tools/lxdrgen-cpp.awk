# lxdrgen-cpp.awk: C++ backend for lxdrgen.awk.
#
# This backend is intended for Windows, it just happens to have a fallback
# that will probably work on Unices, of which we make use in tests.
#
# Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD

function define_internal(name, ctype) {
	Types[name] = "internal"
	CodegenCType[name] = ctype
	CodegenSerialize[name] = \
		"\tw.append(%s);\n"
	CodegenDeserialize[name] = \
		"\tif (!r.read(%s))\n" \
		"\t\treturn false;\n"
}

function define_int(shortname, ctype) {
	define_internal(shortname, ctype)
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

	define_internal("string", "std::wstring")
	define_internal("bool", "bool")

	CodegenSerialize["string"] = \
		"\tif (!w.append(%s))\n" \
		"\t\treturn false;\n"

	print "// Code generated from " FILENAME ". DO NOT EDIT."
	print ""
	print "#include <cstdint>"
	print "#include <memory>"
	print "#include <string>"
	print "#include <vector>"
	print ""
	print "namespace LibertyXDR {"
	print ""
	print "bool utf8_to_wstring("
	print "\tconst uint8_t *utf8, size_t length, std::wstring &wide);"
	print "bool wstring_to_utf8("
	print "\tconst std::wstring &wide, std::string &utf8);"
	print ""
	print "struct Reader {"
	print "\tconst uint8_t *data = {};"
	print "\tsize_t length = {};"
	print ""
	print "\ttemplate<typename T> bool read(T &number) {"
	print "\t\tif (length < sizeof number)"
	print "\t\t\treturn false;"
	print ""
	print "\t\tnumber = 0;"
	print "\t\tfor (size_t i = 0; i < sizeof number; i++) {"
	print "\t\t\tnumber = number << 8 | *data++;"
	print "\t\t\tlength--;"
	print "\t\t}"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool read(bool &boolean) {"
	print "\t\tuint8_t number = 0;"
	print "\t\tif (!read(number))"
	print "\t\t\treturn false;"
	print ""
	print "\t\tboolean = number != 0;"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool read(std::wstring &string) {"
	print "\t\tuint32_t size = 0;"
	print "\t\tif (!read(size) || size > length)"
	print "\t\t\treturn false;"
	print "\t\tif (!utf8_to_wstring(data, size, string))"
	print "\t\t\treturn false;"
	print ""
	print "\t\tdata += size;"
	print "\t\tlength -= size;"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool read(std::vector<uint8_t> &vector) {"
	print "\t\tuint32_t size = 0;"
	print "\t\tif (!read(size) || size > length)"
	print "\t\t\treturn false;"
	print "\t\tvector.assign(data, data + size);"
	print ""
	print "\t\tdata += size;"
	print "\t\tlength -= size;"
	print "\t\treturn true;"
	print "\t}"
	print "};"
	print ""
	print "struct Writer {"
	print "\tstd::vector<uint8_t> data;"
	print ""
	print "\ttemplate<typename T> bool append(T number) {"
	print "\t\tuint8_t buffer[sizeof number], *p = buffer + sizeof buffer;"
	print "\t\twhile (p != buffer) {"
	print "\t\t\t*--p = number;"
	print "\t\t\tnumber >>= 8;"
	print "\t\t}"
	print "\t\tdata.insert(data.end(), buffer, buffer + sizeof buffer);"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool append(int8_t number) {"
	print "\t\tdata.push_back(number);"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool append(uint8_t number) {"
	print "\t\tdata.push_back(number);"
	print "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool append(bool boolean) {"
	print "\t\treturn append(uint8_t(boolean));"
	print "\t}"
	print ""
	print "\tbool append(const std::wstring &string) {"
	print "\t\tif (string.size() > UINT32_MAX)"
	print "\t\t\treturn false;"
	print ""
	print "\t\tstd::string utf8;"
	print "\t\tif (!wstring_to_utf8(string, utf8))"
	print "\t\t\treturn false;"
	print ""
	print "\t\tappend<uint32_t>(utf8.size());"
	print "\t\tdata.insert(data.end(), utf8.begin(), utf8.end());"
	print "\t\treturn true;"
	print "\t}"
	print "};"
	print ""
	print "} // namespace LibertyXDR"
	print "namespace " PrefixCamel " {"
}

END {
	print ""
	print "} // namespace " PrefixCamel
}

function codegen_constant(name, value) {
	print ""
	print "enum { " name " = " value " };"
}

function codegen_enum_value(name, subname, value, cg) {
	append(cg, "fields", "\t" subname " = " value ",\n")
}

function codegen_enum(name, cg) {
	print ""
	print "enum struct " name " : int8_t {"
	print cg["fields"] "};"

	# XXX: This should also check if it isn't out-of-range for any reason,
	# but our usage of sprintf() stands in the way a bit.
	CodegenSerialize[name] = \
		"\tw.append(static_cast<int8_t>(%s));\n"
	CodegenDeserialize[name] = \
		"\t{\n" \
		"\t\tint8_t v = 0;\n" \
		"\t\tif (!r.read(v) || !v)\n" \
		"\t\t\treturn false;\n" \
		"\t\t%s = static_cast<" name ">(v);\n" \
		"\t}\n"

	CodegenCType[name] = name
	for (i in cg)
		delete cg[i]
}

# Some identifiers do not pose a problem in C, but do in our C++.
function codegen_struct_sanitize(name) {
	if (name ~ /^(serialize|deserialize)_*$/ ||
		name ~ /^(catch|class|delete|except|finally|friend|new|operator)_*$/ ||
		name ~ /^(private|protected|public|template|this|throw|try|virtual)_*$/)
		return name "_"
	return name
}

function codegen_struct_tag(d, cg,    name, f) {
	name = codegen_struct_sanitize(d["name"])
	f = "this->" name

	append(cg, "serialize", sprintf(CodegenSerialize[d["type"]], f))
	# Do not deserialize here, that would be out of order.
}

function codegen_struct_field(d, cg,    name, f, serialize, deserialize) {
	name = codegen_struct_sanitize(d["name"])
	f = "this->" name

	serialize = CodegenSerialize[d["type"]]
	deserialize = CodegenDeserialize[d["type"]]
	if (!d["isarray"]) {
		append(cg, "fields",
			"\t" CodegenCType[d["type"]] " " name " = {};\n")
		append(cg, "serialize", sprintf(serialize, f))
		append(cg, "deserialize", sprintf(deserialize, f))
		return
	}

	append(cg, "fields",
		"\tstd::vector<" CodegenCType[d["type"]] "> " name ";\n")

	# XXX: We should probably pedantically check for overflows.
	append(cg, "serialize",
		sprintf(CodegenSerialize["u32"], "uint32_t(" f ".size())") \
		"\tfor (const auto &it : " f ")\n" \
		indent(sprintf(serialize, "it")))

	if (d["type"] == "u8") {
		append(cg, "deserialize",
			"\tif (!r.read(" f "))\n" \
			"\t\treturn false;\n")
	} else if (deserialize) {
		append(cg, "deserialize",
			"\t{\n" \
			"\t\tuint32_t size = 0;\n" \
			indent(sprintf(CodegenDeserialize["u32"], "size")) \
			"\t\t" f ".resize(size);\n" \
			"\t}\n" \
			"\tfor (auto &it : " f ")\n" \
			indent(sprintf(deserialize, "it")))
	}
}

function codegen_struct(name, cg) {
	print ""
	print "struct " name " {"
	print cg["fields"]
	print "\tbool serialize(LibertyXDR::Writer &w) const {"
	print indent(cg["serialize"]) "\t\treturn true;"
	print "\t}"
	print ""
	print "\tbool deserialize([[maybe_unused]] LibertyXDR::Reader &r) {"
	print indent(cg["deserialize"]) "\t\treturn true;"
	print "\t}"
	print "};"

	CodegenSerialize[name] = "\tif (!%s->serialize(w))\n" \
		"\t\treturn false;\n"
	CodegenDeserialize[name] = "\tif (!%s->deserialize(r))\n" \
		"\t\treturn false;\n"

	CodegenCType[name] = name
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(name, d, cg,    tagname) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = tagname = codegen_struct_sanitize(d["name"])

	print ""
	print "struct " name " {"
	print "\t" CodegenCType[d["type"]] " " tagname " = {};"
	print "\tvirtual ~" name "() = 0;"
	print "\tvirtual bool serialize(LibertyXDR::Writer &w) const = 0;"
	print "\tvirtual bool deserialize(LibertyXDR::Reader &r) = 0;"
	print "};"
	print ""
	print name "::~" name "() {}"
}

function codegen_union_struct(name, casename, cg, scg,     structname) {
	# And thus not all generated structs are present in Types.
	structname = name "_" snaketocamel(casename)

	print ""
	print "struct " structname " : virtual public " name " {"
	print scg["fields"]
	print "\t" structname "() {"
	print "\t\tthis->" cg["tagname"] " = " \
		CodegenCType[cg["tagtype"]] "::" casename ";"
	print "\t}"
	print ""
	print "\tvirtual bool serialize(LibertyXDR::Writer &w) const {"
	print indent(scg["serialize"]) "\t\treturn true;"
	print "\t}"
	print ""
	print "\tvirtual bool deserialize([[maybe_unused]] LibertyXDR::Reader &r) {"
	print indent(scg["deserialize"]) "\t\treturn true;"
	print "\t}"
	print "};"

	append(cg, "deserialize",
		"\tcase " CodegenCType[cg["tagtype"]] "::" casename ":\n" \
		"\t\treturn new " structname "();\n")

	CodegenSerialize[structname] = "\tif (!%s->serialize(w))\n" \
		"\t\treturn false;\n"
	CodegenDeserialize[structname] = "\tif (!%s->deserialize(r))\n" \
		"\t\treturn false;\n"

	CodegenCType[structname] = structname
	for (i in scg)
		delete scg[i]
}

function codegen_union(name, cg, exhaustive,    ctype) {
	CodegenSerialize[name] = "\tif (!%s->serialize(w))\n" \
		"\t\treturn false;\n"

	ctype = "std::unique_ptr<" name ">"
	if (cg["deserialize"]) {
		print ""
		print "static " name " *read" name "(" \
			CodegenCType[cg["tagtype"]] " " cg["tagname"] ") {"
		print "\tswitch (" cg["tagname"] ") {"
		print cg["deserialize"] "\tdefault:"
		print "\t\treturn nullptr;"
		print "\t}"
		print "}"
		print ""
		print "static " ctype " read" name "(LibertyXDR::Reader &r) {"
		print "\tint8_t v = 0;"
		print "\tif (!r.read(v) || !v)"
		print "\t\treturn nullptr;"
		print ""
		print "\t" ctype " result(read" name "(static_cast<" \
			CodegenCType[cg["tagtype"]] ">(v)));"
		print "\tif (!result || !result->deserialize(r))"
		print "\t\treturn nullptr;"
		print "\treturn result;"
		print "}"

		CodegenDeserialize[name] = "\tif (!(%s = read" name "(r)))\n" \
			"\t\treturn false;\n"
	}

	CodegenCType[name] = ctype
	for (i in cg)
		delete cg[i]
}
