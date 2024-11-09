# lxdrgen-go.awk: Go backend for lxdrgen.awk.
#
# Copyright (c) 2022 - 2024, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# This backend also enables proxying to other endpoints using JSON.

function define_internal(name, gotype) {
	Types[name] = "internal"
	CodegenGoType[name] = gotype
}

function define_sint(size,    shortname, gotype) {
	shortname = "i" size
	gotype = "int" size
	define_internal(shortname, gotype)

	CodegenAppendJSON[shortname] = \
		"\tb = strconv.AppendInt(b, int64(%s), 10)\n"
	if (size == 8) {
		CodegenSerialize[shortname] = "\tdata = append(data, uint8(%s))\n"
		CodegenDeserialize[shortname] = \
			"\tif len(data) >= 1 {\n" \
			"\t\t%s, data = int8(data[0]), data[1:]\n" \
			"\t} else {\n" \
			"\t\treturn nil, false\n" \
			"\t}\n"
		return
	}

	CodegenSerialize[shortname] = \
		"\tdata = binary.BigEndian.AppendUint" size "(data, uint" size "(%s))\n"
	CodegenDeserialize[shortname] = \
		"\tif len(data) >= " (size / 8) " {\n" \
		"\t\t%s = " gotype "(binary.BigEndian.Uint" size "(data))\n" \
		"\t\tdata = data[" (size / 8) ":]\n" \
		"\t} else {\n" \
		"\t\treturn nil, false\n" \
		"\t}\n"
}

function define_uint(size,    shortname, gotype) {
	# Both []byte and []uint8 luckily marshal as base64-encoded JSON strings,
	# so there's no need to rename the type as an exception.
	shortname = "u" size
	gotype = "uint" size
	define_internal(shortname, gotype)

	CodegenAppendJSON[shortname] = \
		"\tb = strconv.AppendUint(b, uint64(%s), 10)\n"
	if (size == 8) {
		CodegenSerialize[shortname] = "\tdata = append(data, %s)\n"
		CodegenDeserialize[shortname] = \
			"\tif len(data) >= 1 {\n" \
			"\t\t%s, data = data[0], data[1:]\n" \
			"\t} else {\n" \
			"\t\treturn nil, false\n" \
			"\t}\n"
		return
	}

	CodegenSerialize[shortname] = \
		"\tdata = binary.BigEndian.AppendUint" size "(data, %s)\n"
	CodegenDeserialize[shortname] = \
		"\tif len(data) >= " (size / 8) " {\n" \
		"\t\t%s = binary.BigEndian.Uint" size "(data)\n" \
		"\t\tdata = data[" (size / 8) ":]\n" \
		"\t} else {\n" \
		"\t\treturn nil, false\n" \
		"\t}\n"
}

# Currently two outputs cannot coexist within the same package.
function codegen_private(name) {
	return "proto" name
}

function codegen_begin(    funcname) {
	define_sint("8")
	define_sint("16")
	define_sint("32")
	define_sint("64")
	define_uint("8")
	define_uint("16")
	define_uint("32")
	define_uint("64")
	define_internal("bool", "bool")
	define_internal("string", "string")

	# Cater to "go generate", for what it's worth.
	CodegenPackage = ENV["GOPACKAGE"]
	if (!CodegenPackage)
		CodegenPackage = "main"

	print "// Code generated from " FILENAME ". DO NOT EDIT."
	print ""
	print "package " CodegenPackage
	print ""
	print "import ("
	print "\t`encoding/base64`"
	print "\t`encoding/binary`"
	print "\t`encoding/json`"
	print "\t`errors`"
	print "\t`math`"
	print "\t`strconv`"
	print "\t`unicode/utf8`"
	print ")"
	print ""
	print "// This is a hack to always use the base64 import."
	print "var _ = base64.StdEncoding"
	print ""

	CodegenAppendJSON["bool"] = \
		"\tb = strconv.AppendBool(b, %s)\n"
	CodegenSerialize["bool"] = \
		"\tif %s {\n" \
		"\t\tdata = append(data, 1)\n" \
		"\t} else {\n" \
		"\t\tdata = append(data, 0)\n" \
		"\t}\n"

	funcname = codegen_private("ConsumeBoolFrom")
	print "// " funcname " tries to deserialize a boolean value"
	print "// from the beginning of a byte stream. When successful,"
	print "// it returns a subslice with any data that might follow."
	print "func " funcname "(data []byte, b *bool) ([]byte, bool) {"
	print "\tif len(data) < 1 {"
	print "\t\treturn nil, false"
	print "\t}"
	print "\tif data[0] != 0 {"
	print "\t\t*b = true"
	print "\t} else {"
	print "\t\t*b = false"
	print "\t}"
	print "\treturn data[1:], true"
	print "}"
	print ""

	CodegenDeserialize["bool"] = \
		"\tif data, ok = " funcname "(data, &%s); !ok {\n" \
		"\t\treturn nil, ok\n" \
		"\t}\n"

	funcname = codegen_private("AppendStringTo")
	print "// " funcname " tries to serialize a string value,"
	print "// appending it to the end of a byte stream."
	print "func " funcname "(data []byte, s string) ([]byte, bool) {"
	print "\tif int64(len(s)) > math.MaxUint32 {"
	print "\t\treturn nil, false"
	print "\t}"
	print "\tdata = binary.BigEndian.AppendUint32(data, uint32(len(s)))"
	print "\treturn append(data, s...), true"
	print "}"
	print ""

	CodegenSerialize["string"] = \
		"\tif data, ok = " funcname "(data, %s); !ok {\n" \
		"\t\treturn nil, ok\n" \
		"\t}\n"

	funcname = codegen_private("ConsumeStringFrom")
	print "// " funcname " tries to deserialize a string value"
	print "// from the beginning of a byte stream. When successful,"
	print "// it returns a subslice with any data that might follow."
	print "func " funcname "(data []byte, s *string) ([]byte, bool) {"
	print "\tif len(data) < 4 {"
	print "\t\treturn nil, false"
	print "\t}"
	print "\tlength := binary.BigEndian.Uint32(data)"
	print "\tif data = data[4:]; uint64(len(data)) < uint64(length) {"
	print "\t\treturn nil, false"
	print "\t}"
	print "\t*s = string(data[:length])"
	print "\tif !utf8.ValidString(*s) {"
	print "\t\treturn nil, false"
	print "\t}"
	print "\treturn data[length:], true"
	print "}"
	print ""

	CodegenDeserialize["string"] = \
		"\tif data, ok = " funcname "(data, &%s); !ok {\n" \
		"\t\treturn nil, ok\n" \
		"\t}\n"

	funcname = codegen_private("UnmarshalEnumJSON")
	print "// " funcname " converts a JSON fragment to an integer,"
	print "// ensuring that it's within the expected range of enum values."
	print "func " funcname "(data []byte) (int64, error) {"
	print "\tvar n int64"
	print "\tif err := json.Unmarshal(data, &n); err != nil {"
	print "\t\treturn 0, err"
	print "\t} else if n > math.MaxInt8 || n < math.MinInt8 {"
	print "\t\treturn 0, errors.New(`integer out of range`)"
	print "\t} else {"
	print "\t\treturn n, nil"
	print "\t}"
	print "}"
	print ""
}

function codegen_constant(name, value) {
	print "const " PrefixCamel snaketocamel(name) " = " value
	print ""
}

function codegen_enum_value(name, subname, value, cg,    goname) {
	goname = PrefixCamel name snaketocamel(subname)
	append(cg, "fields",
		"\t" goname " = " value "\n")
	append(cg, "stringer",
		"\tcase " goname ":\n" \
		"\t\treturn `" snaketocamel(subname) "`\n")
	append(cg, "marshal",
		goname ",\n")
	append(cg, "unmarshal",
		"\tcase `" snaketocamel(subname) "`:\n" \
		"\t\t*v = " goname "\n")
}

function codegen_enum(name, cg,    gotype, fields, funcname) {
	gotype = PrefixCamel name
	print "type " gotype " int8"
	print ""

	print "const ("
	print cg["fields"] ")"
	print ""

	print "func (v " gotype ") String() string {"
	print "\tswitch v {"
	print cg["stringer"] "\tdefault:"
	print "\t\treturn strconv.Itoa(int(v))"
	print "\t}"
	print "}"
	print ""

	CodegenIsMarshaler[name] = 1
	fields = cg["marshal"]
	sub(/,\n$/, ":", fields)
	gsub(/\n/, "\n\t", fields)
	print "func (v " gotype ") MarshalJSON() ([]byte, error) {"
	print "\tswitch v {"
	print indent("case " fields)
	print "\t\treturn []byte(`\"` + v.String() + `\"`), nil"
	print "\t}"
	print "\treturn json.Marshal(int(v))"
	print "}"
	print ""

	funcname = codegen_private("UnmarshalEnumJSON")
	print "func (v *" gotype ") UnmarshalJSON(data []byte) error {"
	print "\tvar s string"
	print "\tif json.Unmarshal(data, &s) == nil {"
	print "\t\t// Handled below."
	print "\t} else if n, err := " funcname "(data); err != nil {"
	print "\t\treturn err"
	print "\t} else {"
	print "\t\t*v = " gotype "(n)"
	print "\t\treturn nil"
	print "\t}"
	print ""
	print "\tswitch s {"
	print cg["unmarshal"] "\tdefault:"
	print "\t\treturn errors.New(`unrecognized value: ` + s)"
	print "\t}"
	print "\treturn nil"
	print "}"
	print ""

	# XXX: This should also check if it isn't out-of-range for any reason,
	# but our usage of sprintf() stands in the way a bit.
	CodegenSerialize[name] = "\tdata = append(data, uint8(%s))\n"
	CodegenDeserialize[name] = \
		"\tif len(data) >= 1 {\n" \
		"\t\t%s, data = " gotype "(data[0]), data[1:]\n" \
		"\t} else {\n" \
		"\t\treturn nil, false\n" \
		"\t}\n"

	CodegenGoType[name] = gotype
	for (i in cg)
		delete cg[i]
}

function codegen_marshal(type, f,    marshal) {
	if (CodegenAppendJSON[type])
		return sprintf(CodegenAppendJSON[type], f)

	# Complex types are json.Marshalers, there's no need to json.Marshal(&f).
	if (CodegenIsMarshaler[type])
		marshal = f ".MarshalJSON()"
	else
		marshal = "json.Marshal(" f ")"

	return \
		"\tif j, err := " marshal "; err != nil {\n" \
		"\t\treturn nil, err\n" \
		"\t} else {\n" \
		"\t\tb = append(b, j...)\n" \
		"\t}\n"
}

function codegen_struct_field_marshal(d, cg, isaccessor,    camel, f, marshal) {
	camel = snaketocamel(d["name"])
	f = "s." camel
	if (isaccessor)
		f = f "()"
	if (!d["isarray"]) {
		append(cg, "marshal",
			"\tb = append(b, `,\"" decapitalize(camel) "\":`...)\n" \
			codegen_marshal(d["type"], f))
		return
	}

	# Note that we do not produce `null` for nil slices, unlike encoding/json.
	# And arrays never get deserialized as such.
	if (d["type"] == "u8") {
		append(cg, "marshal",
			"\tb = append(b, `,\"" decapitalize(camel) "\":\"`...)\n" \
			"\tb = append(b, base64.StdEncoding.EncodeToString(" f ")...)\n" \
			"\tb = append(b, '\"')\n")
		return
	}

	append(cg, "marshal",
		"\tb = append(b, `,\"" decapitalize(camel) "\":[`...)\n" \
		"\tfor i := 0; i < len(" f "); i++ {\n" \
		"\t\tif i > 0 {\n" \
		"\t\t\tb = append(b, ',')\n" \
		"\t\t}\n" \
		indent(codegen_marshal(d["type"], f "[i]")) \
		"\t}\n" \
		"\tb = append(b, ']')\n")
}

function codegen_struct_field(d, cg,    camel, f, serialize, deserialize) {
	codegen_struct_field_marshal(d, cg, 0)

	camel = snaketocamel(d["name"])
	f = "s." camel
	serialize = CodegenSerialize[d["type"]]
	deserialize = CodegenDeserialize[d["type"]]
	if (!d["isarray"]) {
		append(cg, "fields", "\t" camel " " CodegenGoType[d["type"]] \
			" `json:\"" decapitalize(camel) "\"`\n")
		append(cg, "serialize", sprintf(serialize, f))
		append(cg, "deserialize", sprintf(deserialize, f))
		return
	}

	append(cg, "fields", "\t" camel " []" CodegenGoType[d["type"]] \
		" `json:\"" decapitalize(camel) "\"`\n")

	# XXX: This should also check if it isn't out-of-range for any reason.
	append(cg, "serialize",
		sprintf(CodegenSerialize["u32"], "uint32(len(" f "))"))
	if (d["type"] == "u8") {
		append(cg, "serialize",
			"\tdata = append(data, " f "...)\n")
	} else {
		append(cg, "serialize",
			"\tfor i := 0; i < len(" f "); i++ {\n" \
			indent(sprintf(serialize, f "[i]")) \
			"\t}\n")
	}

	append(cg, "deserialize",
		"\t{\n" \
		"\t\tvar length uint32\n" \
		indent(sprintf(CodegenDeserialize["u32"], "length")))
	if (d["type"] == "u8") {
		append(cg, "deserialize",
			"\t\tif uint64(len(data)) < uint64(length) {\n" \
			"\t\t\treturn nil, false\n" \
			"\t\t}\n" \
			"\t\t" f ", data = data[:length], data[length:]\n" \
			"\t}\n")
	} else {
		append(cg, "deserialize",
			"\t\t" f " = make([]" CodegenGoType[d["type"]] ", length)\n" \
			"\t}\n" \
			"\tfor i := 0; i < len(" f "); i++ {\n" \
			indent(sprintf(deserialize, f "[i]")) \
			"\t}\n")
	}
}

function codegen_struct_tag(d, cg) {
	codegen_struct_field_marshal(d, cg, 1)

	# Do not serialize or deserialize here,
	# that is already done by the containing union.
}

function codegen_struct(name, cg,    gotype) {
	gotype = PrefixCamel name
	print "type " gotype " struct {\n" cg["fields"] "}\n"

	if (cg["marshal"]) {
		CodegenIsMarshaler[name] = 1
		print "func (s *" gotype ") MarshalJSON() ([]byte, error) {"
		print "\tb := []byte{}"
		print cg["marshal"] "\tb[0] = '{'"
		print "\treturn append(b, '}'), nil"
		print "}"
		print ""
	}

	if (cg["serialize"]) {
		print "func (s *" gotype ") AppendTo(data []byte) ([]byte, bool) {"
		print "\tok := true"
		print cg["serialize"] "\treturn data, ok"
		print "}"
		print ""

		CodegenSerialize[name] = \
			"\tif data, ok = %s.AppendTo(data); !ok {\n" \
			"\t\treturn nil, ok\n" \
			"\t}\n"
	}
	if (cg["deserialize"]) {
		print "func (s *" gotype ") ConsumeFrom(data []byte) ([]byte, bool) {"
		print "\tok := true"
		print cg["deserialize"] "\treturn data, ok"
		print "}"
		print ""

		CodegenDeserialize[name] = \
			"\tif data, ok = %s.ConsumeFrom(data); !ok {\n" \
			"\t\treturn nil, ok\n" \
			"\t}\n"
	}

	CodegenGoType[name] = gotype
	for (i in cg)
		delete cg[i]
}

function codegen_union_tag(name, d, cg) {
	cg["tagtype"] = d["type"]
	cg["tagname"] = snaketocamel(d["name"])
	# The tag is implied from the type of struct stored in the interface.
}

function codegen_union_struct(name, casename, cg, scg,     structname, init) {
	# And thus not all generated structs are present in Types.
	structname = name snaketocamel(casename)
	codegen_struct(structname, scg)

	print "func (u *" CodegenGoType[structname] ") " cg["tagname"] "() " \
		CodegenGoType[cg["tagtype"]] " {"
	print "\treturn " CodegenGoType[cg["tagtype"]] snaketocamel(casename)
	print "}"
	print ""

	init = CodegenGoType[structname] "{}"
	append(cg, "unmarshal",
		"\tcase " CodegenGoType[cg["tagtype"]] snaketocamel(casename) ":\n" \
		"\t\ts := " init "\n" \
		"\t\terr = json.Unmarshal(data, &s)\n" \
		"\t\tu.Variant = &s\n")
	append(cg, "serialize",
		"\tcase *" CodegenGoType[structname] ":\n" \
		indent(sprintf(CodegenSerialize[structname], "union")))
	append(cg, "deserialize",
		"\tcase " CodegenGoType[cg["tagtype"]] snaketocamel(casename) ":\n" \
		"\t\ts := " init "\n" \
		indent(sprintf(CodegenDeserialize[structname], "s")) \
		"\t\tu.Variant = &s\n")
}

function codegen_union(name, cg, exhaustive,    gotype, tagvar) {
	gotype = PrefixCamel name
	# This must be a struct, so that UnmarshalJSON can create concrete types.
	print "type " gotype " struct {"
	print "\tVariant interface {"
	print "\t\t" cg["tagname"] "() " CodegenGoType[cg["tagtype"]]
	print "\t}"
	print "}"
	print ""

	# This cannot be a pointer method, it wouldn't work recursively.
	CodegenIsMarshaler[name] = 1
	print "func (u " gotype ") MarshalJSON() ([]byte, error) {"
	print "\treturn u.Variant.(json.Marshaler).MarshalJSON()"
	print "}"
	print ""

	tagvar = decapitalize(cg["tagname"])
	print "func (u *" gotype ") UnmarshalJSON(data []byte) (err error) {"
	print "\tvar t struct {"
	print "\t\t" cg["tagname"] " " CodegenGoType[cg["tagtype"]] \
		" `json:\"" tagvar "\"`"
	print "\t}"
	print "\tif err := json.Unmarshal(data, &t); err != nil {"
	print "\t\treturn err"
	print "\t}"
	print ""
	print "\tswitch " tagvar " := t." cg["tagname"] "; " tagvar " {"
	print cg["unmarshal"] "\tdefault:"
	print "\t\terr = errors.New(`unsupported value: ` + " tagvar ".String())"
	print "\t}"
	print "\treturn err"
	print "}"
	print ""

	# XXX: Consider rather testing the type for having an AppendTo method,
	# which would eliminate this type case switch entirely.
	print "func (u *" gotype ") AppendTo(data []byte) ([]byte, bool) {"
	print "\tok := true"
	print sprintf(CodegenSerialize[cg["tagtype"]],
		"u.Variant." cg["tagname"] "()") \
		"\tswitch union := u.Variant.(type) {"
	print cg["serialize"] "\tdefault:"
	print "\t\t_ = union"
	print "\t\treturn nil, false"
	print "\t}"
	print "\treturn data, ok"
	print "}"
	print ""

	CodegenSerialize[name] = \
		"\tif data, ok = %s.AppendTo(data); !ok {\n" \
		"\t\treturn nil, ok\n" \
		"\t}\n"

	print "func (u *" gotype ") ConsumeFrom(data []byte) ([]byte, bool) {"
	print "\tok := true"
	print "\tvar " tagvar " " CodegenGoType[cg["tagtype"]]
	print sprintf(CodegenDeserialize[cg["tagtype"]], tagvar)
	print "\tswitch " tagvar " {"
	print cg["deserialize"] "\tdefault:"
	print "\t\treturn nil, false"
	print "\t}"
	print "\treturn data, ok"
	print "}"
	print ""

	CodegenDeserialize[name] = \
		"\tif data, ok = %s.ConsumeFrom(data); !ok {\n" \
		"\t\treturn nil, ok\n" \
		"\t}\n"

	CodegenGoType[name] = gotype
	for (i in cg)
		delete cg[i]
}
