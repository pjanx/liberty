# asciiman.awk: simplified AsciiDoc to manual page converter
#
# Copyright (c) 2022 - 2024, Přemysl Eric Janouch <p@janouch.name>
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

BEGIN {
	for (name in ENVIRON)
		if (match(name, /^asciidoc-/))
			Attrs[substr(name, RSTART + RLENGTH)] = ENVIRON[name]
}

function expand(s,   attrname, v) {
	while (match(s, /[{][^{}]+[}]/)) {
		attrname = substr(s, RSTART + 1, RLENGTH - 2)
		if (attrname in Attrs)
			v = v substr(s, 1, RSTART - 1) Attrs[attrname]
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

function readattribute(line,    attrname) {
	if (match(line, /^:[^:]+:$/)) {
		Attrs[substr(line, RSTART + 1, RLENGTH - 2)] = ""
	} else if (match(line, /^:[^:]+!:$/)) {
		delete Attrs[substr(line, RSTART + 1, RLENGTH - 3)]
	} else if (match(line, /^:![^:]+:$/)) {
		delete Attrs[substr(line, RSTART + 2, RLENGTH - 3)]
	} else if (match(line, /^:[^:]+: /)) {
		attrname = substr(line, RSTART + 1, RLENGTH - 3)
		Attrs[attrname] = expand(substr(line, RSTART + RLENGTH))
	} else {
		return 0
	}
	return 1
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
		toupper(name), section, Attrs["mansource"]
	if ("manmanual" in Attrs)
		printf " \"%s\"", Attrs["manmanual"]
	print ""

	# Hyphenation is indeed rather annoying, in particular with long links.
	print ".nh"
}

function readattrlist(line, posattrs, namedattrs,    name, value, n) {
	if (!match(line, /^\[.*\]$/))
		return 0

	line = expand(substr(line, RSTART + 1, RLENGTH - 2))
	while (line) {
		name = ""
		if (match(line, /^[[:alnum:]][[:alnum:]-]*/)) {
			value = substr(line, RSTART, RLENGTH)
			if (match(substr(line, RSTART + RLENGTH),
					/^[[:space:]]*=[[:space:]]*/)) {
				name = value
				line = substr(line, 1 + length(name) + RLENGTH)
			}
		}

		# The quoting syntax actually is awful like this.
		if (match(line, /^"(\\.|[^"\\])*"/)) {
			value = substr(line, RSTART + 1, RLENGTH - 2)
			gsub(/\\"/, "\"", value)
		} else if (match(line, /^'(\\.|[^'\\])*'/)) {
			value = substr(line, RSTART + 1, RLENGTH - 2)
			gsub(/\\'/, "'", value)
		} else {
			match(line, /^[^,]*/)
			value = substr(line, RSTART, RLENGTH)
			sub(/[[:space:]]*$/, "", value)
		}

		line = substr(line, RSTART + RLENGTH)
		sub(/^[[:space:]]*,[[:space:]]*/, "", line)
		if (!name)
			posattrs[++n] = value
		else if (value == "None")
			delete namedattrs[name]
		else
			namedattrs[name] = value
	}
	return 1
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
		} else if (match(line, /^`[^`]+`/) &&
			substr(line, RSTART + RLENGTH) !~ /^[[:alnum:]]/) {
			# Manual pages are usually already rendered in monospace;
			# follow others, and render this in boldface.
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
function process(firstline,     posattrs, namedattrs, ok) {
	if (readattribute(firstline))
		return 0
	if (getline <= 0) {
		inline(firstline)
		return 0
	}

	# Block attribute list lines.
	delete posattrs[0]
	delete namedattrs[0]
	while (readattrlist(firstline, posattrs, namedattrs)) {
		firstline = $0
		if (getline <= 0) {
			inline(firstline)
			return 0
		}
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

	if (firstline ~ /^--$/) {
		flushspace()

		# For now, recognize, but do not process open block delimiters.
		InOpenBlock = !InOpenBlock
		return 1
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

	# We generally assume these blocks end with a blank line.
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
			if (!process($0) && (ok = getline) <= 0) {
				if (ok < 0)
					fatal("getline failed")
				$0 = ""
			} else if (match($0, /^[[:space:]]*[*][[:space:]]+/))
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
			if (!process($0) && (ok = getline) <= 0) {
				if (ok < 0)
					fatal("getline failed")
				$0 = ""
			} else if (match($0, /::$/))
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
