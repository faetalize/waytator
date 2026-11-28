#!/bin/sh
#builds waytator-<distro>-<arch>.deb from the commit checked out here, into the folder given, for the Debian or
#Ubuntu it runs on. it installs the build dependencies with apt, so it's meant for a throwaway container run as
#root, as in CI
set -eu

mkdir -p "$1"
out=$(cd "$1" && pwd)
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
version=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$repo/meson.build")
commit_time=$(git -C "$repo" log -1 --format=%ct)
arch=$(dpkg --print-architecture)
distro=$(. /etc/os-release && echo "$ID")
codename=$(. /etc/os-release && echo "$VERSION_CODENAME")

work=$(mktemp -d)
src="$work/waytator-$version"
mkdir -p "$src"
git -C "$repo" archive HEAD | tar -x -C "$src"
cp -r "$repo/packaging/debian" "$src/debian"

#dated to the commit, which makes it the build's SOURCE_DATE_EPOCH and so waytator's commit_time
maintainer=$(sed -n 's/^Maintainer: //p' "$src/debian/control")
cat > "$src/debian/changelog" <<EOF
waytator ($version-1) $codename; urgency=medium

  * Release $version.

 -- $maintainer  $(date -u -R -d "@$commit_time")
EOF

apt-get update
apt-get build-dep -y "$src"

cd "$src"
DEB_BUILD_OPTIONS="parallel=$(nproc)" dpkg-buildpackage -b -us -uc
cp "$work/waytator_${version}-1_$arch.deb" "$out/waytator-$distro-$arch.deb"
