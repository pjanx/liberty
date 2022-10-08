# asciiman.awk: simplified AsciiDoc to manual page converter
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# This is not intended to produce great output, merely useful output.
# As such, input documents should restrict themselves as follows:
#
#  - In-line formatting sequences must not overlap,
#    cannot be escaped, and cannot span lines.
#  - Heading underlines must match in byte length exactly.
#  - Only a small subset of syntax is supported overall.
#
# Also beware that the output has only been tested with GNU troff and mandoc.
# Attributes can be passed via environment variables starting with "asciidoc-".

function fatal(message) {
	print ".\\\" " FILENAME ":" FNR ": fatal error: " message
	print FILENAME ":" FNR ": fatal error: " message > "/dev/stderr"
	exit 1
}

function haveattribute(name) {
	return name in Attrs || ("asciidoc-" name) in ENVIRON
}

function getattribute(name) {
	if (!(name in Attrs) && ("asciidoc-" name) in ENVIRON)
		Attrs[name] = ENVIRON["asciidoc-" name]
	return Attrs[name]
}

function expand(s,   attr, v) {
	while (match(s, /[{][^{}]*[}]/)) {
		attr = substr(s, RSTART + 1, RLENGTH - 2)
		if (haveattribute(attr))
			v = v substr(s, 1, RSTART - 1) getattribute(attr)
		else
			v = v substr(s, 1, RSTART + RLENGTH - 1)
		s = substr(s, RSTART + RLENGTH)
	}
	return v s
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
	print "'\\\" t"
	printf ".TH \"%s\" \"%s\" \"\" \"%s\"",
		toupper(name), section, getattribute("mansource")
	if (getattribute("manmanual"))
		printf " \"%s\"", getattribute("manmanual")
	print ""

	# Hyphenation is indeed rather annoying, in particular with long links.
	print ".nh"
}

function format(line,    v) {
	# Pass-through, otherwise useful for hacks, is a bit of a lie here,
	# and formatting doesn't fully respect word boundaries.
	while (line) {
		if (match(line, /^[+][+][+][^+]+[+][+][+]/)) {
			v = v substr(line, RSTART + 3, RLENGTH - 6)
		} else if (match(line, /^__[^_]+__/)) {
			v = v "\\fI" substr(line, RSTART + 2, RLENGTH - 4) "\\fP"
		} else if (match(line, /^[*][*][^*]+[*][*]/)) {
			v = v "\\fB" substr(line, RSTART + 2, RLENGTH - 4) "\\fP"
		} else if (match(line, /^_[^_]+_/) &&
			substr(line, RSTART + RLENGTH) !~ /^[[:alnum:]]/) {
			v = v "\\fI" substr(line, RSTART + 1, RLENGTH - 2) "\\fP"
		} else if (match(line, /^[*][^*]+[*]/) &&
			substr(line, RSTART + RLENGTH) !~ /^[[:alnum:]]/) {
			v = v "\\fB" substr(line, RSTART + 1, RLENGTH - 2) "\\fP"
		} else {
			v = v substr(line, 1, 1)
			line = substr(line, 2)
			continue
		}
		line = substr(line, RSTART + RLENGTH)
	}
	return v
}

function flushspace() {
	if (NeedSpace) {
		print ".sp"
		NeedSpace = 0
	}
}

function inline(line) {
	if (!line) {
		NeedSpace = 1
		return
	}

	flushspace()
	line = format(escape(expand(line)))

	# Strip empty URL descriptions, otherwise useful for demarking the end.
	while (match(line, /[^[:space:]]+\[\]/)) {
		line = substr(line, 1, RSTART + RLENGTH - 3) \
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

	# mandoc(1) automatically precedes section headers with blank lines.
	if (length(firstline) == length($0) && /^-+$/) {
		print ".SH \"" escape(toupper(expand(firstline))) "\""
		NeedSpace = 0
		return 0
	}
	if (length(firstline) == length($0) && /^~+$/) {
		print ".SS \"" escape(expand(firstline)) "\""
		NeedSpace = 0
		return 0
	}

	if (firstline ~ /^(-{4,}|[.]{4,})$/) {
		flushspace()

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
		print ".\\\"" substr(firstline, RSTART + RLENGTH)
		return 1
	}

	# We generally assume these block end with a blank line.
	if (match(firstline, /^[[:space:]]*[*][[:space:]]+/)) {
		flushspace()

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
		NeedSpace = 1
		return !!$0
	}
	if (match(firstline, /^[[:space:]]+/)) {
		flushspace()

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
		NeedSpace = 1
		return !!$0
	}
	inline(firstline)
	return 1
}

{
	while (process($0)) {}
}
