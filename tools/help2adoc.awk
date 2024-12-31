# help2adoc.awk: convert --version/--help to AsciiDoc manual pages
#
# Copyright (c) 2024, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Usage: awk -f help2adoc.awk -v Target=cat
#
# This is not intended to produce great output, merely useful output,
# if only because there is no real standard of what the input should look like.
#
# The only target that needs to work is liberty's own opt_handler.
# The expected input format is roughly that of GNU utilites.

function fatal(message) {
	print "// " message
	print "fatal error: " message > "/dev/stderr"
	exit 1
}

# The input model of this script is that function take the next line on $0,
# read further lines as necessary, and leave the next line in $0 again.
function readline(    ok) {
	if ((ok = (Command | getline)) < 0)
		fatal("read error")
	if (!ok)
		exit
}

function emboldenoptions(line) {
	# -N, --newer=DATE-OR-FILE, --after-date=DATE-OR-FILE
	sub(/^-[^-=,[:space:]{[<]/, "*&*", line)
	while (match(line, /[^-_[:alnum:]*'+]-[^-=,[:space:]{[<]/)) {
		line = substr(line, 1, RSTART) \
			"**" substr(line, RSTART + 1, RLENGTH - 1) "**" \
			substr(line, RSTART + RLENGTH)
	}
	sub(/^--[-_[:alnum:]]+/, "*&*", line)
	while (match(line, /[^-_[:alnum:]*'+]--[-_[:alnum:]]+/)) {
		line = substr(line, 1, RSTART) \
			"**" substr(line, RSTART + 1, RLENGTH - 1) "**" \
			substr(line, RSTART + RLENGTH)
	}
	return line
}

function formatinline(line,    programname, last, i) {
	# Go the extra step of emboldening the program name at word boundaries.
	programname = ProgramName
	gsub(/[][\\.^$(){}|*+?]/, "\\\\&", programname)
	if (match(line, "^" programname "[^-_[:alnum:]*'+/]")) {
		line = "**" substr(line, RSTART, RLENGTH - 1) "**" \
			substr(line, RSTART + RLENGTH - 1)
	}
	while (match(line, "[^-_[:alnum:]*'+/]" programname "[^-_[:alnum:]*'+/]")) {
		line = substr(line, 1, RSTART) \
			"**" substr(line, RSTART + 1, RLENGTH - 2) "**" \
			substr(line, RSTART + RLENGTH - 1)
	}
	if (match(line, "[^-_[:alnum:]*'+/]" programname "$")) {
		line = substr(line, 1, RSTART) \
			"**" substr(line, RSTART + 1, RLENGTH - 1) "**"
	}
	return emboldenoptions(line)
}

function printusage(usage,    description) {
	gsub(/…/, "...", usage)
	gsub(/—|–/, "-", usage)

	# --help output will more likely than not simply include argv[0],
	# or perhaps program_invocation_short_name (not addressed here).
	if (substr(usage, 1, length(Target) + 1) == Target " ")
		usage = ProgramName substr(usage, length(Target) + 1)

	# A lot of GNOME software includes the description here.
	if (match(usage, / +- +/) && usage !~ / - [^[:alnum:]]/) {
		description = substr(usage, RSTART + RLENGTH)
		usage = substr(usage, 1, RSTART - 1)
	}

	while (match(usage, /[^-_[:alnum:]*'+.][[:alnum:]][-_[:alnum:]]+/)) {
		usage = substr(usage, 1, RSTART) \
			"__" substr(usage, RSTART + 1, RLENGTH - 1) "__" \
			substr(usage, RSTART + RLENGTH)
	}
	sub(/^[^[:space:]]+/, "*&*", usage)
	print emboldenoptions(usage)
	print ""

	if (description) {
		flushsections()
		print formatinline(description)
		print ""
	}
}

# We're going with Setext headers, because that's what asciiman.awk supports.
function printheader(text, underline) {
	print text
	gsub(/./, underline, text)
	print text
}

BEGIN {
	if (!Target)
		fatal("missing Target")

	TargetQuoted = Target
	gsub(/'/, "'\\''", TargetQuoted)
	TargetQuoted = "'" TargetQuoted "'"

	# Remaining --version lines could be about copyright (GNU),
	# or something else entirely.
	Command = TargetQuoted " --version"
	if ((Command | getline) > 0) {
		# GNU --version output can place the package name in parentheses.
		Package = $0
		if (match($0, /[[:space:]][(][^)]*[)]/)) {
			Package = substr($0, RSTART + 2, RLENGTH - 3) \
				substr($0, RSTART + RLENGTH)
			sub(/[[:space:]]+[(][^)]*[)]/, "")
		}

		Version = $0
		sub(/[[:space:]]+[^[:space:]]+$/, "")
		Name = $0
	} else {
		fatal("failed to get --version output")
	}

	if (Name !~ /[[:space:]]/)
		ProgramName = Name
	else if (match(Target, /[^\/]+$/))
		ProgramName = substr(Target, RSTART, RLENGTH)

	printheader(ProgramName "(1)", "=")
	print ":doctype: manpage"
	print ":manmanual: " Name " Manual"
	print ":mansource: " Package
	print ""
	printheader("Name", "-")
	print ProgramName " - manual page for " Version
	print ""

	close(Command)
	Command = TargetQuoted " --help"
	if ((Command | getline) <= 0)
		fatal("failed to get --help output")

	NextSection = "Description"
	NextSubsection = ""

	# The SYNOPSIS section is mandatory, so just put it there.
	printheader("Synopsis", "-")
	while (1) {
		if (match($0, /^[Uu]sage:[[:space:]]*/)) {
			if (($0 = substr($0, RSTART + RLENGTH)))
				printusage($0)
		} else if (match($0, /^[[:space:]]+/) && !/^[[:space:]]*-/) {
			if (($0 = substr($0, RSTART + RLENGTH)))
				printusage($0)
		} else if ($0) {
			break
		}
		readline()
	}
	while (1) {
		if (match($0, /^[[:alpha:]][-[:alnum:][:space:]]+:$/)) {
			# We don't flush sections here,
			# so that we don't unnecessarily enforce DESCRIPTION first.
			NextSection = substr($0, RSTART, RLENGTH - 1)
		} else if (match($0, /^ [[:alpha:]][-[:alnum:][:space:]]+:$/)) {
			flushsections()
			NextSubsection = substr($0, RSTART + 1, RLENGTH - 2)
		} else if (match($0, /^ +-/)) {
			flushsections()
			parseoption(substr($0, RSTART + RLENGTH - 1))
			continue
		} else if ($0) {
			flushsections()

			# That will be probably interpreted as a literal block.
			if (!/^[[:space:]]/)
				$0 = formatinline($0)
			print
		} else {
			print
		}
		readline()
	}
}

function flushsections() {
	if (NextSection) {
		print ""
		printheader(NextSection, "-")
		NextSection = ""
	}
	if (NextSubsection) {
		print ""
		printheader(NextSubsection, "~")
		NextSubsection = ""
	}
}

function parseoption(line,    usage) {
	# Often enough you will see it separated with only one space,
	# which will simply not work for us.
	if (match(line, /[[:space:]]{2,}/)) {
		usage = substr(line, 1, RSTART - 1)
		line = substr(line, RSTART + RLENGTH)
	} else {
		usage = line
		line = ""
	}

	usage = emboldenoptions(usage)
	while (match(usage, /[=<, ][[:alnum:]][-_[:alnum:]]*/)) {
		usage = substr(usage, 1, RSTART) \
			"__" substr(usage, RSTART + 1, RLENGTH - 1) "__" \
			substr(usage, RSTART + RLENGTH)
	}

	print ""
	print usage "::"
	if (line)
		print "\t" formatinline(line)

	readline()
	while (match($0, /^ +[^-[:space:]]|^ {7,}./)) {
		print "\t" formatinline(substr($0, RSTART + RLENGTH - 1))
		readline()
	}
}
