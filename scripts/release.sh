#!/usr/bin/env bash
# Build, check and package Resolume OMT's macOS release on this Mac.
#
#     scripts/release.sh              # -> out/release/resolume-omt-macos-universal.zip
#                                     #    and resolume-omt-<version>-macos-universal.dmg
#
# Local rather than CI because libomt is a Native AOT .NET library from outside
# this repo (OMT_RUNTIME_DIR, default ~/.local/lib/omt) and nothing here builds it.
#
# Both artefacts come out AD HOC on purpose. Publish them with `gh release
# create`, and the fleet's autosign agent (stoatworks-backend
# release/auto-sign-watch.sh) signs them inside out with the Developer ID:
# every dylib in each bundle's Contents/Frameworks first, then the bundle,
# then notarises, staples and re-uploads. That is the path every other FFGL
# plugin's release takes. release-lib would Developer-ID sign by itself here
# (rl_init reads ~/.config/stoatworks/release-signing.env), so the signing
# variables are cleared after it.
#
# OMTTEST=0 skips the harness (it needs a GL context and the loopback network).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$here/build"
out="$here/out/release"
runtime="${OMT_RUNTIME_DIR:-$HOME/.local/lib/omt}"
version="$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$here/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || { echo "no VERSION in CMakeLists.txt" >&2; exit 1; }
bundles=("SW OMT In" "SW OMT Out")

if [[ -n "$(git -C "$here" status --porcelain --untracked-files=no)" ]]; then
  echo "warning: the tree has uncommitted changes; a release should be built from its tag" >&2
fi

echo "==> build $version (universal, Release)"
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DOMT_RUNTIME_DIR="$runtime" >/dev/null
cmake --build "$build" -j8 >/dev/null

if [[ "${OMTTEST:-1}" != "0" ]]; then
  echo "==> omttest"
  ( cd "$here" && "$build/omttest" ) | tail -3
fi

echo "==> check the bundles"
for name in "${bundles[@]}"; do
  b="$build/$name.bundle"
  for f in "$b/Contents/MacOS/$name" "$b/Contents/Frameworks/libomt.dylib" "$b/Contents/Frameworks/libvmx.dylib"; do
    [[ -f "$f" ]] || { echo "missing: ${f#$build/}" >&2; exit 1; }
    archs="$(lipo -archs "$f")"
    case "$archs" in
      *x86_64*arm64* | *arm64*x86_64*) printf '    %-55s %s\n' "${f#$build/}" "$archs" ;;
      *) echo "${f#$build/}: expected x86_64 and arm64, got: $archs" >&2; exit 1 ;;
    esac
  done
  nm -gU "$b/Contents/MacOS/$name" | grep -q '_plugMain' || { echo "$name: no _plugMain" >&2; exit 1; }
  plist="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$b/Contents/Info.plist")"
  [[ "$plist" == "$version" ]] || { echo "$name: Info.plist says $plist, not $version" >&2; exit 1; }
  codesign --verify --strict --deep "$b" || { echo "$name: signature does not verify" >&2; exit 1; }
done

echo "==> stage"
rm -rf "$out"; mkdir -p "$out"
stage="$out/stage"; mkdir -p "$stage"
for name in "${bundles[@]}"; do
  ditto "$build/$name.bundle" "$stage/$name.bundle"
done
# The FFGL SDK, libomt, libvmx and the .NET runtime inside libomt are all in
# the download, so their notices travel with it.
cp "$here/ATTRIBUTIONS.md" "$stage/"
cp "$here/third_party/omt/LICENSE.txt" "$stage/LICENSE-libomt.txt"

echo "==> zip"
( cd "$stage" && COPYFILE_DISABLE=1 zip -qry "$out/resolume-omt-macos-universal.zip" . )

echo "==> dmg"
# shellcheck source=release-lib.sh
source "$here/scripts/release-lib.sh"
rl_init "Resolume OMT" resolume-omt "$version" com.stoatworks.ffgl.omt "$out"
unset RL_MAC_SIGN_IDENTITY RL_MAC_INSTALLER_IDENTITY RL_NOTARY_PROFILE APPLE_SIGNING_IDENTITY \
      RL_NOTARY_KEY RL_NOTARY_KEY_ID RL_NOTARY_ISSUER
dmgstage="$out/dmg"; ditto "$stage" "$dmgstage"
cp "$here/README.md" "$dmgstage/"
rl_dmg macos-universal "$dmgstage"
rm -rf "$dmgstage" "$stage"

ls -l "$out"
