# cmake-parser.awk: rudimentary CMake script parser
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Implemented roughly according to the grammar described in cmake-language(7),
# which is self-conflicting, and not an accurate description.
#
# The result of parsing is stored in the case-normalized Command variable,
# and the Args array.  These can be used by subsequent scripts.

function warning(message) {
	print FILENAME ":" FNR ": warning: " message > "/dev/stderr"
}

function fatal(message) {
	print FILENAME ":" FNR ": fatal error: " message > "/dev/stderr"
	exit 1
}

function expect(v) {
	if (!v && v == 0)
		fatal("broken expectations at `" $0 "'")
	return v
}

function literal(v) {
	if (substr($0, 1, length(v)) != v)
		return 0
	$0 = substr($0, length(v) + 1)
	return 1
}

function regexp(re) {
	if (!match($0, "^" re))
		return 0
	$0 = substr($0, RLENGTH + 1)
	return 1
}

function space() {
	return regexp("[ \t]+")
}

function unbracket(len,    v) {
	do {
		if (match($0, "]={" len "}]")) {
			v = v substr($0, 1, RSTART - 1)
			$0 = substr($0, RSTART + RLENGTH)
			return v
		}
		v = v $0 RS
	} while (getline > 0)
	fatal("unterminated bracket")
}

function bracket_comment() {
	if (!match($0, /^#\[=*\[/))
		return 0
	$0 = substr($0, RSTART + RLENGTH)
	unbracket(RLENGTH - 3)
	return 1
}

function line_ending() {
	while (space() || bracket_comment()) {}
	if (/^#/)
		$0 = ""
	return !$0
}

# ------------------------------------------------------------------------------

# While elementary expansion of previously set variables is implementable,
# it doesn't seem to be worth the effort.
function expand(s,    v) {
	v = s
	while (match(v, /\\*[$](ENV|CACHE)?[{]/)) {
		if (index(substr(v, RSTART), "$") % 2 != 0) {
			warning("variable expansion is not supported: " s)
			return s
		}
		v = substr(v, RSTART + RLENGTH)
	}
	return s
}

function escape_sequence(    v) {
	if (!literal("\\"))
		return 0

	if (literal("t")) return "\t"
	if (literal("r")) return "\r"
	if (literal("n")) return "\n"

	# escape_semicolon isn't treated any specially here.
	if (regexp("[A-Za-z0-9]"))
		fatal("unsupported escape sequence")

	if ($0) {
		v = substr($0, 1, 1)
		$0 = substr($0, 2)
		return v
	}
	if (getline > 0)
		return ""
	fatal("premature end of file")
}

function quoted_argument(    v, unescaped) {
	if (!literal("\""))
		return 0

	v = ""
	while (!literal("\"")) {
		if (!$0) {
			if (getline <= 0)
				fatal("premature end of file")
			v = v RS
		} else if ((unescaped = escape_sequence())) {
			if (unescaped == "\\" || unescaped == "$")
				v = v "\\"
			else if (unescaped == ";")
				v = v "\\\\"
			v = v unescaped
		} else if (unescaped == "") {
			# quoted_continuation
		} else {
			v = v substr($0, 1, 1)
			$0 = substr($0, 2)
		}
	}
	return v
}

function finalize_quoted(expanded,    v) {
	while (match(expanded, /\\./)) {
		v = v substr(expanded, 1, RSTART - 1) \
			substr(expanded, RSTART + 1, 1)
		expanded = substr(expanded, RSTART + RLENGTH)
	}
	Args[++N] = v expanded
}

function unquoted_argument(    v, unescaped) {
	while (1) {
		if (match($0, /^[^[:space:]()#"\\]+/)) {
			v = v substr($0, RSTART, RLENGTH)
			$0 = substr($0, RSTART + RLENGTH)
		} else if ((unescaped = escape_sequence())) {
			if (unescaped == "\\" || unescaped == "$" || unescaped == ";")
				v = v "\\"
			v = v unescaped
		} else if (unescaped == "") {
			fatal("unexpected backslash in an unquoted argument")
		} else {
			# unquoted_legacy is not supported.
			return v
		}
	}
}

function finalize_unquoted(expanded,    v) {
	while (expanded) {
		if (expanded ~ /^;/) {
			if (v)
				Args[++N] = v
			v = ""
			expanded = substr(expanded, 2)
		} else if (expanded ~ /^\\./) {
			v = v substr(expanded, 2, 1)
			expanded = substr(expanded, 3)
		} else {
			v = v substr(expanded, 1, 1)
			expanded = substr(expanded, 2)
		}
	}
	if (v)
		Args[++N] = v
}

# We keep and reprocess some escape sequences in here.
function argument(    arg, expanded, v) {
	if (regexp("\\[=*\\["))
		Args[++N] = unbracket(RLENGTH - 2)
	else if ((arg = quoted_argument()) || arg == "")
		finalize_quoted(expand(arg))
	else if ((arg = unquoted_argument()))
		finalize_unquoted(expand(arg))
	else
		return 0
	return 1
}

# ------------------------------------------------------------------------------

function identifier(    v) {
	if (!match($0, /^[A-Za-z_][A-Za-z0-9_]*/))
		return 0
	v = substr($0, 1, RLENGTH)
	$0 = substr($0, RLENGTH + 1)
	return v
}

function separation() {
	if (space() || bracket_comment())
		return 1

	if (!line_ending())
		return 0
	if (getline > 0)
		return 1
	fatal("premature end of file")
}

function command_invocation(    level) {
	while (space()) {}
	Command = identifier()
	if (!Command)
		return 0
	while (space()) {}

	Command = tolower(Command)
	for (N in Args)
		delete Args[N]

	N = 0
	expect(literal("("))
	while (1) {
		while (separation()) {}
		if (literal(")")) {
			if (!level--)
				break
			Args[++N] = ")"
			continue
		}
		if (literal("(")) {
			level++
			Args[++N] = "("
			continue
		}
		expect(argument())
		if (!/^[()]/)
			expect(separation())
	}
	return 1
}

{
	command_invocation()
	expect(line_ending())
}
