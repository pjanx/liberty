# asciiman.awk: stupid AsciiDoc to manual page converter
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# This is not intended to produce great output, merely useful output.
# As such, input documents should restrict themselves as follows:
#
#  - Attributes cannot be passed on the command line.
#  - In-line formatting sequences must not overlap,
#    cannot be escaped, and cannot span lines.
#  - Heading underlines must match in byte length exactly.
#  - Only a small subset of syntax is supported overall.
#
# Also beware that the output has only been tested with GNU troff.

function fatal(message) {
	print ".\\\" " FILENAME ":" FNR ": fatal error: " message
	print FILENAME ":" FNR ": fatal error: " message > "/dev/stderr"
	exit 1
}

function expand(s,   attr) {
	# TODO: This should not expand unknown attribute names.
	while (match(s, /[{][^{}]*[}]/)) {
		attr = substr(s, RSTART + 1, RLENGTH - 2)
		s = substr(s, 1, RSTART - 1) Attrs[attr] substr(s, RSTART + RLENGTH)
	}
	return s
}

function escape(s) {
	gsub(/\\/, "\\\\", s)
	gsub(/-/, "\\-", s)
	sub(/^[.']/, "\\\\\\&&", s)
	return s
}

function readattribute(line,    attrname, attrvalue) {
	if (match(line, /^:[^:]*: /)) {
		attrname = substr(line, RSTART + 1, RLENGTH - 3)
		attrvalue = substr(line, RSTART + RLENGTH)
		Attrs[attrname] = expand(attrvalue)
		return 1
	}
}

NR == 1 {
	nameline = $0
	if (match(nameline, /[(][[:digit:]][)]$/)) {
		name = substr(nameline, 1, RSTART - 1)
		section = substr(nameline, RSTART + 1, RLENGTH - 2)
	} else {
		fatal("invalid header line")
	}

	getline
	if (length(nameline) != length($0) || /[^=]/)
		fatal("invalid header underline")

	getline
	while (readattribute($0))
		getline
	if ($0)
		fatal("expected an empty line after the header")

	# Requesting tbl(1), even though we currently do not support tables.
	print "'\\\\"" t"
	print ".TH \"" toupper(name) "\" \"" section "\""

	# Hyphenation is indeed rather annoying, in particular with long links.
	print ".nh"
}

function inline(line) {
	if (!line) {
		print ".sp"
		return
	}

	line = escape(expand(line))

	# Strip empty URL descriptions, otherwise useful for demarking the end.
	while (match(line, /[^[:space:]]+\[\]/)) {
		line = substr(line, 1, RSTART + RLENGTH - 3) \
			 substr(line, RSTART + RLENGTH)
	}

	# Pass-through, otherwise useful for hacks, is a lie here.
	while (match(line, /[+][+][+][^+]+[+][+][+]/)) {
		line = substr(line, 1, RSTART - 1) \
			 substr(line, RSTART + 3, RLENGTH - 6) \
			 substr(line, RSTART + RLENGTH)
	}

	# Italic and bold formatting doesn't respect any word boundaries.
	while (match(line, /__[^_]+__/)) {
		line = substr(line, 1, RSTART - 1) \
			 "\\fI" substr(line, RSTART + 2, RLENGTH - 4) "\\fP" \
			 substr(line, RSTART + RLENGTH)
	}
	while (match(line, /_[^_]+_/)) {
		line = substr(line, 1, RSTART - 1) \
			 "\\fI" substr(line, RSTART + 1, RLENGTH - 2) "\\fP" \
			 substr(line, RSTART + RLENGTH)
	}
	while (match(line, /[*][*][^*]+[*][*]/)) {
		line = substr(line, 1, RSTART - 1) \
			 "\\fB" substr(line, RSTART + 2, RLENGTH - 4) "\\fP" \
			 substr(line, RSTART + RLENGTH)
	}
	while (match(line, /[*][^*]+[*]/)) {
		line = substr(line, 1, RSTART - 1) \
			 "\\fB" substr(line, RSTART + 1, RLENGTH - 2) "\\fP" \
			 substr(line, RSTART + RLENGTH)
	}

	# Enable double-spacing after the end of a sentence.
	gsub(/[.][[:space:]]+/, ".\n", line)
	gsub(/[!][[:space:]]+/, "!\n", line)
	gsub(/[?][[:space:]]+/, "?\n", line)

	# Quote commands resulting from that, as well as from expand().
	gsub(/\n[.]/, "\n\\\\\\&.", line)
	gsub(/\n[']/, "\n\\\\\\&'", line)

	sub(/[[:space:]]+[+]$/, "\n.br", line)
	print line
}

# Returns 1 iff the left-over $0 should be processed further.
function process(firstline) {
	if (readattribute(firstline))
		return 0
	if (getline <= 0) {
		inline(firstline)
		return 0
	}

	if (length(firstline) == length($0) && /^-+$/) {
		print ".SH \"" escape(toupper(expand(firstline))) "\""
		return 0
	}
	if (length(firstline) == length($0) && /^~+$/) {
		print ".SS \"" escape(expand(firstline)) "\""
		return 0
	}
	if (firstline ~ /^(-{4,}|[.]{4,})$/) {
		print ".if n .RS 4"
		print ".nf"
		print ".fam C"
		do {
			print escape($0)
		} while (getline > 0 && $0 != firstline)
		print ".fam"
		print ".fi"
		print ".if n .RE"
		return 0
	}
	if (firstline ~ /^\/{4,}$/) {
		do {
			print ".\\\" " $0
		} while (getline > 0 && $0 != firstline)
		return 0
	}
	if (match(firstline, /^\/\//)) {
		print ".\\\" " firstline
		return 1
	}

	# We generally assume these block end with a blank line.
	if (match(firstline, /^[[:space:]]*[*][[:space:]]+/)) {
		# Bullet magic copied over from AsciiDoc/Asciidoctor generators.
		print ".RS 4"
		print ".ie n \\{\\"
		print "\\h'-04'\\(bu\\h'+03'\\c"
		print ".\\}"
		print ".el \\{\\"
		print ".sp -1"
		print ".IP \\(bu 2.3"
		print ".\\}"

		inline(substr(firstline, RSTART + RLENGTH))
		while ($0) {
			sub(/^[[:space:]]+/, "")
			sub(/^[+]$/, "")
			if (!process($0) && getline <= 0)
				fatal("unexpected EOF")
			if (match($0, /^[[:space:]]*[*][[:space:]]+/))
				break
		}
		print ".RE"
		print ".sp"
		return !!$0
	}
	if (match(firstline, /^[[:space:]]+/)) {
		print ".if n .RS 4"
		print ".nf"
		print ".fam C"
		do {
			print escape(substr(firstline, RLENGTH + 1))
			firstline = $0
		} while ($0 && getline > 0)
		print ".fam"
		print ".fi"
		print ".if n .RE"
		return 1
	}
	if (match(firstline, /::$/)) {
		inline(substr(firstline, 1, RSTART - 1))
		while (match($0, /::$/)) {
			print ".br"
			inline(substr($0, 1, RSTART - 1))
			if (getline <= 0)
				fatal("unexpected EOF")
		}

		print ".RS 4"
		while ($0) {
			sub(/^[[:space:]]+/, "")
			sub(/^[+]$/, "")
			if (!process($0) && getline <= 0)
				fatal("unexpected EOF")
			if (match($0, /::$/))
				break
		}
		print ".RE"
		print ".sp"
		return !!$0
	}
	inline(firstline)
	return 1
}

{
	while (process($0)) {}
}
