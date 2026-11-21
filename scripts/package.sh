#!/usr/bin/env bash
# Package every built LV2 bundle (*/*.lv2) into release tarballs.
#
#   scripts/package.sh <version> <arch>     e.g. scripts/package.sh v0.1.0 x86_64
#
# Run `make` first. Output goes to dist/:
#   <plugin>-<version>-linux-<arch>.tar.gz      one bundle each
#   plugins-lv2-<version>-linux-<arch>.tar.gz   all bundles
# Each tarball extracts to ready-to-use bundle folders: put them in ~/.lv2.
set -euo pipefail

version=${1:?version}
arch=${2:?arch}
root=$(cd "$(dirname "$0")/.." && pwd)
dist="$root/dist"
stage="$dist/stage-$arch"

rm -rf "$stage"
mkdir -p "$stage"

bundles=()
for b in "$root"/*/*.lv2; do
	name=$(basename "$b")
	so=("$b"/*.so)
	if [ ! -f "${so[0]}" ]; then
		echo "missing binary in $b: run make first" >&2
		exit 1
	fi
	mkdir "$stage/$name"
	cp "$b"/*.ttl "$b"/*.so "$stage/$name/"
	strip --strip-unneeded "$stage/$name"/*.so
	tar -C "$stage" -czf "$dist/${name%.lv2}-$version-linux-$arch.tar.gz" "$name"
	bundles+=("$name")
done

tar -C "$stage" -czf "$dist/plugins-lv2-$version-linux-$arch.tar.gz" "${bundles[@]}"
rm -rf "$stage"
ls -1 "$dist"/*-"$version"-linux-"$arch".tar.gz
