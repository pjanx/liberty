# lxdrgen-mjs.awk: Javascript backend for lxdrgen.awk.
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# This backend is currently for decoding the binary format only.
# (JSON is way too expensive to process and transfer.)
#
# Import the resulting script as a Javascript module.
# Identifiers intentionally aren't prefixed.

function define_internal(name) {
	Types[name] = "internal"
}

function define_sint(size,    shortname) {
	shortname = "i" size
	define_internal(shortname)
	CodegenDeserialize[shortname] = "\t%s = r." shortname "()\n"

	print ""
	print "\t" shortname "() {"
	if (size == "64") {
		# XXX: 2^53 - 1 must be enough for anyone.  BigInts are a PITA.
		print "\t\tconst " shortname \
			" = Number(this.getBigInt" size "(this.offset))"
	} else {
		print "\t\tconst " shortname " = this.getInt" size "(this.offset)"
	}
	print "\t\tthis.offset += " (size / 8)
	print "\t\treturn " shortname
	print "\t}"
}

function define_uint(size,    shortname) {
	shortname = "u" size
	define_internal(shortname)
	CodegenDeserialize[shortname] = "\t%s = r." shortname "()\n"

	print ""
	print "\t" shortname "() {"
	if (size == "64") {
		# XXX: 2^53 - 1 must be enough for anyone.  BigInts are a PITA.
		print "\t\tconst " shortname \
			" = Number(this.getBigUint" size "(this.offset))"
	} else {
		print "\t\tconst " shortname " = this.getUint" size "(this.offset)"
	}
	print "\t\tthis.offset += " (size / 8)
	print "\t\treturn " shortname
	print "\t}"
}

function codegen_begin() {
	print "// Code generated from " FILENAME ". DO NOT EDIT."
	print ""
	print "export class Reader extends DataView {"
	print "\tconstructor() {"
	print "\t\tsuper(...arguments)"
	print "\t\tthis.offset = 0"
	print "\t\tthis.decoder = new TextDecoder('utf-8', {fatal: true})"
	print "\t}"
	print ""
	print "\tget empty() {"
	print "\t\treturn this.byteLength <= this.offset"
	print "\t}"
	print ""
	print "\trequire(len) {"
	print "\t\tif (this.byteLength - this.offset < len)"
	print "\t\t\tthrow `Premature end of data`"
	print "\t\treturn this.byteOffset + this.offset"
	print "\t}"

	define_internal("string")
	CodegenDeserialize["string"] = "\t%s = r.string()\n"

	print ""
	print "\tstring() {"
	print "\t\tconst len = this.getUint32(this.offset)"
	print "\t\tthis.offset += 4"
	print "\t\tconst array = new Uint8Array("
	print "\t\t\tthis.buffer, this.require(len), len)"
	print "\t\tthis.offset += len"
	print "\t\treturn this.decoder.decode(array)"
	print "\t}"

	define_internal("bool")
	CodegenDeserialize["bool"] = "\t%s = r.bool()\n"

	print ""
	print "\tbool() {"
	print "\t\tconst u8 = this.getUint8(this.offset)"
	print "\t\tthis.offset += 1"
	print "\t\treturn u8 != 0"
	print "\t}"

	define_sint("8")
	define_sint("16")
	define_sint("32")
	define_sint("64")
	define_uint("8")
	define_uint("16")
	define_uint("32")
	define_uint("64")

	print "}"
}

function codegen_constant(name, value) {
	print ""
	print "export const " decapitalize(snaketocamel(name)) " = " value
}

function codegen_enum_value(name, subname, value, cg) {
	append(cg, "fields", "\t" snaketocamel(subname) ": " value ",\n")
}

function codegen_enum(name, cg) {
	print ""
	print "export const " name " = Object.freeze({"
	print cg["fields"] "})"

	CodegenDeserialize[name] = "\t%s = r.i8()\n"
	for (i in cg)
		delete cg[i]
}

function codegen_struct_field(d, cg,    camel, f, deserialize) {
	camel = decapitalize(snaketocamel(d["name"]))
	f = "s." camel
	append(cg, "fields", "\t" camel "\n")

	deserialize = CodegenDeserialize[d["type"]]
	if (!d["isarray"]) {
		append(cg, "deserialize", sprintf(deserialize, f))
		return
	}

	append(cg, "deserialize",
		"\t{\n" \
		indent(sprintf(CodegenDeserialize["u32"], "const len")))
	if (d["type"] == "u8") {
		append(cg, "deserialize",
			"\t\t" f " = new Uint8Array(\n" \
			"\t\t\tr.buffer, r.require(len), len)\n" \
			"\t\tr.offset += len\n" \
			"\t}\n")
		return
	}
	if (d["type"] == "i8") {
		append(cg, "deserialize",
			"\t\t" f " = new Int8Array(\n" \
			"\t\t\tr.buffer, r.require(len), len)\n" \
			"\t\tr.offset += len\n" \
			"\t}\n")
		return
	}

	append(cg, "deserialize",
		"\t\t" f " = new Array(len)\n" \
		"\t}\n" \
		"\tfor (let i = 0; i < " f ".length; i++)\n" \
		indent(sprintf(deserialize, f "[i]")))
}

function codegen_struct_tag(d, cg) {
	append(cg, "fields", "\t" decapitalize(snaketocamel(d["name"])) "\n")
	# Do not deserialize here, that is already done by the containing union.
}

function codegen_struct(name, cg) {
	print ""
	print "export class " name " {"
	print cg["fields"] cg["methods"]
	print "\tstatic deserialize(r) {"
	print "\t\tconst s = new " name "()"
	print indent(cg["deserialize"]) "\t\treturn s"
	print "\t}"
	print "}"

	CodegenDeserialize[name] = "\t%s = " name ".deserialize(r)\n"
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(name, d, cg) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = decapitalize(snaketocamel(d["name"]))
}

function codegen_union_struct(name, casename, cg, scg,     structname) {
	append(scg, "methods",
		"\n" \
		"\tconstructor() {\n" \
		"\t\tthis." cg["tagname"] \
			" = " cg["tagtype"] "." snaketocamel(casename) "\n" \
		"\t}\n")

	# And thus not all generated structs are present in Types.
	structname = name snaketocamel(casename)
	codegen_struct(structname, scg)

	append(cg, "deserialize",
		"\tcase " cg["tagtype"] "." snaketocamel(casename) ":\n" \
		"\t{\n" \
		indent(sprintf(CodegenDeserialize[structname], "const s")) \
		"\t\treturn s\n" \
		"\t}\n")
}

function codegen_union(name, cg, exhaustive,    tagvar) {
	tagvar = cg["tagname"]

	print ""
	print "export function deserialize" name "(r) {"
	print sprintf(CodegenDeserialize[cg["tagtype"]], "const " tagvar) \
		"\tswitch (" tagvar ") {"
	print cg["deserialize"] "\tdefault:"
	print "\t\tthrow `Unknown " cg["tagtype"] " (${tagvar})`"
	print "\t}"
	print "}"

	CodegenDeserialize[name] = "\t%s = deserialize" name "(r)\n"
	for (i in cg)
		delete cg[i]
}
