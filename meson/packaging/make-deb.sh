#!/bin/sh -e
cd "$MESON_BUILD_ROOT"
. "$MESON_SUBDIR/meta"
wd="`pwd`/`mktemp -d deb.XXXXXX`"
trap "rm -rf '$wd'" INT QUIT TERM EXIT

[ "$arch" = x86    ] && arch=i386
[ "$arch" = x86_64 ] && arch=amd64
target="$name-$version-$system-$arch.deb"

echo 2.0 > "$wd/debian-binary"
cat > "$wd/control" <<-EOF
	Package: $name
	Version: $version
	Section: misc
	Priority: optional
	Architecture: $arch
	Maintainer: $author
	Description: $summary
EOF
fakeroot sh -e <<-EOF
	DESTDIR="$wd/pkg" ninja install
	cd "$wd/pkg" && tar cJf ../data.tar.xz .
EOF

(cd "$wd" && tar czf control.tar.gz ./control)
ar rc "$target" "$wd/debian-binary" "$wd/control.tar.gz" "$wd/data.tar.xz"
echo Written $target
