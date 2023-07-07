# lxdrgen-swift.awk: Swift backend for lxdrgen.awk.
#
# Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD

function define_internal(name, swifttype) {
	Types[name] = "internal"
	CodegenSwiftType[name] = swifttype
	CodegenDeserialize[name] = "%s.read()"
}

function define_sint(size,    shortname, swifttype) {
	shortname = "i" size
	swifttype = "Int" size
	define_internal(shortname, swifttype)
}

function define_uint(size,    shortname, swifttype) {
	shortname = "u" size
	swifttype = "UInt" size
	define_internal(shortname, swifttype)
}

function codegen_begin() {
	define_sint("8")
	define_sint("16")
	define_sint("32")
	define_sint("64")
	define_uint("8")
	define_uint("16")
	define_uint("32")
	define_uint("64")
	define_internal("bool", "Bool")
	define_internal("string", "String")

	print "// Code generated from " FILENAME ". DO NOT EDIT."
	print "import Foundation"
	print ""
	print "public struct " PrefixCamel "Reader {"
	print "\tpublic var data: Data"
	print ""
	print "\tpublic enum ReadError: Error {"
	print "\t\tcase unexpectedEOF"
	print "\t\tcase invalidEncoding"
	print "\t\tcase overflow"
	print "\t\tcase unexpectedValue"
	print "\t}"
	print ""
	print "\tpublic mutating func read<T: FixedWidthInteger>() throws -> T {"
	print "\t\tlet size = MemoryLayout<T>.size"
	print "\t\tguard data.count >= size else {"
	print "\t\t\tthrow ReadError.unexpectedEOF"
	print "\t\t}"
	print "\t\tvar acc: T = 0"
	print "\t\tdata.prefix(size).forEach { acc = acc << 8 | T($0) }"
	print "\t\tdata = data.dropFirst(size)"
	print "\t\treturn acc"
	print "\t}"
	print ""
	print "\tpublic mutating func read() throws -> Bool {"
	print "\t\ttry read() != UInt8(0)"
	print "\t}"
	print ""
	print "\tpublic mutating func read() throws -> String {"
	print "\t\tlet size: UInt32 = try self.read()"
	print "\t\tguard let count = Int(exactly: size) else {"
	print "\t\t\tthrow ReadError.overflow"
	print "\t\t}"
	print "\t\tguard data.count >= count else {"
	print "\t\t\tthrow ReadError.unexpectedEOF"
	print "\t\t}"
	print "\t\tdefer {"
	print "\t\t\tdata = data.dropFirst(count)"
	print "\t\t}"
	print "\t\tif let s = String(data: data.prefix(count), encoding: .utf8) {"
	print "\t\t\treturn s"
	print "\t\t} else {"
	print "\t\t\tthrow ReadError.invalidEncoding"
	print "\t\t}"
	print "\t}"
	print ""
	print "\tpublic mutating func read<" \
		"T: RawRepresentable<Int8>>() throws -> T {"
	print "\t\tguard let value = T(rawValue: try read()) else {"
	print "\t\t\tthrow ReadError.unexpectedValue"
	print "\t\t}"
	print "\t\treturn value"
	print "\t}"
	print ""
	print "\tpublic mutating func read<T>("
	print "\t\t\t_ read: (inout Self) throws -> T) throws -> [T] {"
	print "\t\tlet size: UInt32 = try self.read()"
	print "\t\tguard let count = Int(exactly: size) else {"
	print "\t\t\tthrow ReadError.overflow"
	print "\t\t}"
	print "\t\tvar array = [T]()"
	print "\t\tarray.reserveCapacity(count)"
	print "\t\tfor _ in 0..<count {"
	print "\t\t\tarray.append(try read(&self))"
	print "\t\t}"
	print "\t\treturn array"
	print "\t}"
	print "}"
	print ""
	print "public struct " PrefixCamel "Writer {"
	print "\tpublic var data = Data()"
	print ""
	print "\tpublic mutating func append<T: FixedWidthInteger>(_ number: T) {"
	print "\t\tvar n = number.byteSwapped"
	print "\t\tfor _ in 0..<MemoryLayout<T>.size {"
	print "\t\t\tdata.append(UInt8(truncatingIfNeeded: n))"
	print "\t\t\tn >>= 8"
	print "\t\t}"
	print "\t}"
	print ""
	print "\tpublic mutating func append(_ bool: Bool) {"
	print "\t\tappend(UInt8(bool ? 1 : 0))"
	print "\t}"
	print ""
	print "\tpublic mutating func append(_ string: String) {"
	print "\t\tlet bytes = string.data(using: .utf8)!"
	print "\t\tappend(UInt32(bytes.count))"
	print "\t\tdata.append(bytes)"
	print "\t}"
	print ""
	print "\tpublic mutating func append<T: " \
		"RawRepresentable<Int8>>(_ value: T) {"
	print "\t\tappend(value.rawValue)"
	print "\t}"
	print ""
	print "\tpublic mutating func append<T>("
	print "\t\t\t_ array: Array<T>, _ write: (inout Self, T) -> ()) {"
	print "\t\tappend(UInt32(array.count))"
	print "\t\tfor i in 0..<array.count {"
	print "\t\t\twrite(&self, array[i])"
	print "\t\t}"
	print "\t}"
	print ""
	print "\tpublic mutating func append<T: " \
		PrefixCamel "Encodable>(_ value: T) {"
	print "\t\tvalue.encode(to: &self)"
	print "\t}"
	print "}"
	print ""
	print "public protocol " PrefixCamel "Encodable { " \
		"func encode(to: inout " PrefixCamel "Writer) }"
}

function codegen_constant(name, value) {
	print ""
	print "public let " decapitalize(PrefixCamel snaketocamel(name)) " = " value
}

function codegen_enum_value(name, subname, value, cg) {
	append(cg, "fields",
		"\tcase " decapitalize(snaketocamel(subname)) " = " value "\n")
}

function codegen_enum(name, cg,    swifttype) {
	swifttype = PrefixCamel name
	print ""
	print "public enum " swifttype ": Int8 {"
	print cg["fields"] "}"

	CodegenSwiftType[name] = swifttype
	CodegenDeserialize[name] = "%s.read()"
	for (i in cg)
		delete cg[i]
}

function codegen_struct_field(d, cg,    camel) {
	camel = decapitalize(snaketocamel(d["name"]))
	if (!d["isarray"]) {
		append(cg, "fields",
			"\tpublic var " camel ": " CodegenSwiftType[d["type"]] "\n")
		append(cg, "deserialize",
			"\t\tself." camel " = try " \
				sprintf(CodegenDeserialize[d["type"]], "from") "\n")
		append(cg, "serialize",
			"\t\tto.append(self." camel ")\n")
		return
	}

	append(cg, "fields",
		"\tpublic var " camel ": [" CodegenSwiftType[d["type"]] "]\n")
	append(cg, "deserialize",
		"\t\tself." camel " = try from.read() { r in try " \
			sprintf(CodegenDeserialize[d["type"]], "r") " }\n")
	append(cg, "serialize",
		"\t\tto.append(self." camel ") { (w, value) in w.append(value) }\n")
}

function codegen_struct_tag(d, cg,    camel) {
	camel = decapitalize(snaketocamel(d["name"]))
	append(cg, "serialize",
		"\t\tto.append(self." camel ")\n")
}

function codegen_struct(name, cg,    swifttype) {
	swifttype = PrefixCamel name
	print ""
	print "public struct " swifttype " {\n" cg["fields"] "}"
	print ""
	print "extension " swifttype ": " PrefixCamel "Encodable {"
	print "\tpublic init(from: inout " PrefixCamel "Reader) throws {"
	print cg["deserialize"] "\t}"
	print ""
	print "\tpublic func encode(to: inout " PrefixCamel "Writer) {"
	print cg["serialize"] "\t}"
	print "}"

	CodegenSwiftType[name] = swifttype
	CodegenDeserialize[name] = "%s.read()"
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(name, d, cg) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = decapitalize(snaketocamel(d["name"]))
}

function codegen_union_struct(name, casename, cg, scg,     swifttype) {
	# And thus not all generated structs are present in Types.
	swifttype = PrefixCamel name snaketocamel(casename)
	casename = decapitalize(snaketocamel(casename))
	print ""
	print "public struct " swifttype ": " PrefixCamel name " {"
	print "\tpublic var " cg["tagname"] \
		": " CodegenSwiftType[cg["tagtype"]] " { ." casename " }"
	print scg["fields"] "}"
	print ""
	print "extension " swifttype ": " PrefixCamel "Encodable {"
	print "\tfileprivate init(from: inout " PrefixCamel "Reader) throws {"
	print scg["deserialize"] "\t}"
	print ""
	print "\tpublic func encode(to: inout " PrefixCamel "Writer) {"
	print scg["serialize"] "\t}"
	print "}"

	append(cg, "cases", "\tcase ." casename ":\n" \
		"\t\treturn try " swifttype "(from: &from)\n")

	CodegenSwiftType[name] = swifttype
	CodegenDeserialize[name] = "%s.read()"
	for (i in scg)
		delete scg[i]
}

function codegen_union(name, cg, exhaustive,    swifttype, init) {
	# Classes don't have automatic member-wise initializers,
	# thus using structs and protocols.
	swifttype = PrefixCamel name
	print ""
	print "public protocol " swifttype ": " PrefixCamel "Encodable {"
	print "\tvar " cg["tagname"] ": " CodegenSwiftType[cg["tagtype"]] " { get }"
	print "}"

	if (!exhaustive)
		append(cg, "cases", "\tdefault:\n" \
			"\t\tthrow " PrefixCamel "Reader.ReadError.unexpectedValue\n")

	init = decapitalize(swifttype)
	print ""
	print "public func " init \
		"(from: inout " PrefixCamel "Reader) throws -> " swifttype " {"
	print "\tlet " cg["tagname"] ": " CodegenSwiftType[cg["tagtype"]] \
		" = try from.read()"
	print "\tswitch " cg["tagname"] " {"
	print cg["cases"] "\t}"
	print "}"

	CodegenSwiftType[name] = swifttype
	CodegenDeserialize[name] = init "(from: &%s)"
	for (i in cg)
		delete cg[i]
}
