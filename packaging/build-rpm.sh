#!/bin/sh
#builds waytator-fedora-<arch>.rpm from the commit checked out here, into the folder given. it installs the build
#dependencies with dnf, so it's meant for a throwaway Fedora container run as root, as in CI
set -eu

mkdir -p "$1"
out=$(cd "$1" && pwd)
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
version=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$repo/meson.build")
commit_time=$(git -C "$repo" log -1 --format=%ct)
arch=$(rpm --eval %_arch)

top=$(mktemp -d)
mkdir -p "$top/SOURCES" "$top/SPECS"
git -C "$repo" archive --prefix="waytator-$version/" -o "$top/SOURCES/waytator-$version.tar.gz" HEAD
{
  printf '%%global waytator_version %s\n' "$version"
  printf '%%global waytator_commit_time %s\n' "$commit_time"
  cat "$repo/packaging/waytator.spec"
} > "$top/SPECS/waytator.spec"

dnf builddep -y "$top/SPECS/waytator.spec"
rpmbuild -bb --define "_topdir $top" "$top/SPECS/waytator.spec"
cp "$top/RPMS/$arch/waytator-$version-1".*".$arch.rpm" "$out/waytator-fedora-$arch.rpm"
