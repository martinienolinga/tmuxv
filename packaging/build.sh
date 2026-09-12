#!/bin/sh
# Construit le paquet Debian tmuxv a partir de l'arbre source courant.
#
#   sh packaging/build.sh            # construit packaging/tmuxv_<version>_<arch>.deb
#   sudo dpkg -i packaging/tmuxv_*.deb
#
# Le binaire doit avoir ete compile (make) au prealable. Les proprietaires des
# fichiers doivent etre root:root - sans fakeroot (absent ici) on passe par
# sudo, sinon /usr/bin/tmuxv serait modifiable sans privileges.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
STAGE=$HERE/build
ARCH=$(dpkg --print-architecture)
VERSION=$(sed -n 's/^Version: //p' "$HERE/src/control")
DEB=$HERE/tmuxv_${VERSION}_${ARCH}.deb

# Compilation hors arbre (build/) par defaut ; on accepte aussi un binaire
# construit a la racine, pour ne pas casser un arbre configure a l'ancienne.
if   [ -x "$SRC/build/tmux" ]; then BIN=$SRC/build/tmux
elif [ -x "$SRC/tmux" ];       then BIN=$SRC/tmux
else echo "binaire absent : mkdir build && cd build && ../configure && make" >&2; exit 1
fi
echo "binaire : $BIN"
# Le binaire doit porter LA version du paquet (inscrite a la compilation,
# lue dans src/control) : changer la version sans recompiler donnerait un
# paquet dont la boite "A propos" ment.
if ! grep -aqF "@(#)tmuxv $VERSION @" "$BIN"; then
	echo "binaire perime : il ne porte pas la version $VERSION" >&2
	echo "  recompiler d'abord : make -C $SRC/build" >&2
	exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/share/man/man1" \
         "$STAGE/usr/share/doc/tmuxv/examples"

install -m 0755 "$BIN" "$STAGE/usr/bin/tmuxv"
strip --strip-unneeded "$STAGE/usr/bin/tmuxv"

# Meme page de manuel, renommee pour ne pas ecraser celle du paquet tmux.
sed -e 's/^\.Dt TMUX 1$/.Dt TMUXV 1/' -e 's/^\.Nm tmux$/.Nm tmuxv/' "$SRC/tmux.1" \
    | gzip -9n > "$STAGE/usr/share/man/man1/tmuxv.1.gz"

install -m 0644 "$HERE/src/control"   "$STAGE/DEBIAN/control"
install -m 0644 "$HERE/src/copyright" "$STAGE/usr/share/doc/tmuxv/copyright"
install -m 0644 "$SRC/example_tmux.conf" \
                "$STAGE/usr/share/doc/tmuxv/examples/example_tmuxv.conf"
# Version sans revision Debian = paquet natif : le changelog s'appelle
# changelog.gz, pas changelog.Debian.gz (lintian).
gzip -9nc "$HERE/src/changelog"    > "$STAGE/usr/share/doc/tmuxv/changelog.gz"
gzip -9nc "$HERE/src/README.tmuxv" > "$STAGE/usr/share/doc/tmuxv/README.tmuxv.gz"
chmod 0644 "$STAGE/usr/share/doc/tmuxv/changelog.gz" \
           "$STAGE/usr/share/doc/tmuxv/README.tmuxv.gz"

# Dependances calculees, jamais ecrites a la main.
if command -v dpkg-shlibdeps >/dev/null; then
	tmp=$(mktemp -d); mkdir -p "$tmp/debian"
	printf 'Source: tmuxv\n\nPackage: tmuxv\nArchitecture: %s\n' "$ARCH" \
	    > "$tmp/debian/control"
	: > "$tmp/debian/tmuxv.substvars"
	deps=$(cd "$tmp" && dpkg-shlibdeps -O "$STAGE/usr/bin/tmuxv" 2>/dev/null \
	    | sed -n 's/^shlibs:Depends=//p')
	rm -rf "$tmp"
	if [ -n "$deps" ]; then
		sed -i "s|^Depends: .*|Depends: $deps|" "$STAGE/DEBIAN/control"
		echo "dependances : $deps"
	fi
fi

(cd "$STAGE" && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
chmod 0644 "$STAGE/DEBIAN/md5sums"

sudo chown -R root:root "$STAGE"
sudo chmod -R go-w "$STAGE"
rm -f "$DEB"
sudo dpkg-deb --build "$STAGE" "$DEB"
sudo chown "$(id -u):$(id -g)" "$DEB"
sudo rm -rf "$STAGE"

echo
echo "paquet : $DEB"
command -v lintian >/dev/null && lintian "$DEB" || true
echo "installer : sudo dpkg -i $DEB"
