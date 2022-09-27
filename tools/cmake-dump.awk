# cmake-dump.awk: dump parsed CMake scripts as tables
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Parsed scripts are output in a table, with commands separated using ASCII
# Record Separators, and arguments using Unit Separators.
#
# Example usage: awk -f cmake-parser.awk -f cmake-dump.awk CMakeLists.txt \
#  | sed 'y/\x1F\x1E\t\n/\t\n  /' \
#  | sed -n '/^project\t\([^\t]*\).*\tVERSION\t\([^\t]*\).*/{s//\1 \2/p;q;}'

function sanitize(s) {
	if (s ~ /[\x1E\x1F]/)
		fatal("conflicting ASCII control characters found in source")
	return s
}

Command {
	out = sanitize(Command)
	for (i in Args)
		out = out "\x1F" sanitize(Args[i])
	printf "%s\x1E", out
}
