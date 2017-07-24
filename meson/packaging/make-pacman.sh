#!/bin/sh -e
cd "$MESON_BUILD_ROOT"
. "$MESON_SUBDIR/meta"
wd="`pwd`/`mktemp -d pacman.XXXXXX`"
trap "rm -rf '$wd'" INT QUIT TERM EXIT

target="$name-$version-$arch.tar.xz"
fakeroot sh -e <<-EOF
	DESTDIR="$wd" ninja install
	cat > "$wd/.PKGINFO" <<END
	pkgname = $name
	pkgver = $version-1
	pkgdesc = $summary
	url = $url
	builddate = \`date -u +%s\`
	packager = $author
	size = \`du -sb | cut -f1\`
	arch = $arch
	END
	cd "$wd" && tar cJf "../$target" .PKGINFO *
	echo Written $target
EOF
