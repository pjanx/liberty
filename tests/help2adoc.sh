#!/bin/sh -e
# This test very exactly matches the output,
# but help2adoc is more or less feature-complete already.
self=$(realpath "$0")
help2adoc=$(realpath "$(dirname "$0")/../tools/help2adoc.awk")

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

test_oneline_help() {
cat <<END
Usage: $self [--brightness [+-]BRIGHTNESS] [--input NAME] [--restart]
END
}

test_oneline_version() {
cat <<'END'
eizoctl 1.0
END
}

test_oneline_out() {
cat <<'END'
eizoctl(1)
==========
:doctype: manpage
:manmanual: eizoctl Manual
:mansource: eizoctl 1.0

Name
----
eizoctl - manual page for eizoctl 1.0

Synopsis
--------
*eizoctl* [**--brightness** [+-]__BRIGHTNESS__] [**--input** __NAME__] [**--restart**]
END
}

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

test_simple_help() {
cat <<'END'
Usage: elksmart-comm [OPTION]... [COMMAND...]
Usage: elksmart-comm
Transmit or receive infrared commands.

  -d, --debug                 elksmart-comm will run in debug mode
 -f, --frequency HZ          frequency (38000 Hz by default)
   -n, --nec                   use the NEC transmission format
  -h, --help                  display this help and exit
 -V, --version               output version information and exit
END
}

test_simple_version() {
cat <<'END'
elksmart-comm (usb-drivers) dev
END
}

test_simple_out() {
cat <<'END'
elksmart-comm(1)
================
:doctype: manpage
:manmanual: elksmart-comm Manual
:mansource: usb-drivers dev

Name
----
elksmart-comm - manual page for elksmart-comm dev

Synopsis
--------
*elksmart-comm* [__OPTION__]... [__COMMAND__...]

*elksmart-comm*


Description
-----------
Transmit or receive infrared commands.


*-d*, **--debug**::
	**elksmart-comm** will run in debug mode

*-f*, **--frequency** __HZ__::
	frequency (38000 Hz by default)

*-n*, **--nec**::
	use the NEC transmission format

*-h*, **--help**::
	display this help and exit

*-V*, **--version**::
	output version information and exit
END
}

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

test_wild_help() {
cat <<'END'
Usage:
 wild [option]… <command>… — Be wild
What's happening?
  -f, --frequency hz-2-foo  frequency to --foo at
      --foo=bar
  Foobar.
  Boo far.

 Subsection:
  --help
  --version
  Oh my.

Major section:
And now for something completely different.
  Very wild
END
}

test_wild_version() {
cat <<'END'
wild 1
Copies left and right.
END
}

test_wild_out() {
cat <<'END'
wild(1)
=======
:doctype: manpage
:manmanual: wild Manual
:mansource: wild 1

Name
----
wild - manual page for wild 1

Synopsis
--------
*wild* [__option__]... <__command__>...


Description
-----------
Be **wild**

What's happening?

*-f*, **--frequency** __hz-2-foo__::
	frequency to **--foo** at

*--foo*=__bar__::
	Foobar.
	Boo far.


Subsection
~~~~~~~~~~

*--help*::

*--version*::
	Oh my.


Major section
-------------
And now for something completely different.
  Very wild
END
}

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

run() {
	echo "-- help2adoc/$1"
	local selfquoted=$(echo "$self" | sed 's/\\/&&/g')
	local output=$(TEST=$1 awk -f "$help2adoc" -v Target="$selfquoted")
	local expect="$($1_out)"
	if [ "$output" = "$expect" ]
	then return
	fi

	echo "== Expected"
	sed 's/^/   /' <<-END
	$expect
	END
	echo "== Received"
	sed 's/^/   /' <<-END
	$output
	END
	exit 1
}

if [ -z "$TEST" ]
then
	run test_oneline
	run test_simple
	run test_wild
	echo "-- OK"
elif [ "$1" = "--help" ]
then ${TEST}_help
elif [ "$1" = "--version" ]
then ${TEST}_version
else
	echo "Wrong usage"
	exit 1
fi
