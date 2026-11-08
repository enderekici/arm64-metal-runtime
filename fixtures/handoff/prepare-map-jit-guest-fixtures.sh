#!/bin/sh
set -eu

fixture_dir=$1
build_dir=$2
output="$build_dir/map-jit-guest-fixtures"
mkdir -p "$output"
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  "$fixture_dir/map-jit-guest-main.c" -o "$output/map-jit-guest"
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  "$fixture_dir/map-jit-perthread-fault.c" -o "$output/map-jit-perthread-fault"
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  "$fixture_dir/map-jit-call-guest.c" -o "$output/map-jit-call-guest"

make_bundle() {
  name=$1
  entitlements=${2-}
  executable=${3-map-jit-guest}
  bundle="$output/$name.app"
  executable_name=$(basename "$executable")
  rm -rf "$bundle"
  mkdir -p "$bundle/Contents/MacOS"
  cp "$output/$executable" "$bundle/Contents/MacOS/$executable_name"
  sed "s/map-jit-guest/$executable_name/" \
    "$fixture_dir/map-jit-guest-info.plist" > "$bundle/Contents/Info.plist"
  if [ -n "$entitlements" ]; then
    codesign --force --sign - --options runtime --timestamp=none \
      --entitlements "$entitlements" "$bundle"
  else
    codesign --force --sign - --options runtime --timestamp=none "$bundle"
  fi
  codesign --verify --strict "$bundle"
  shasum -a 256 "$bundle/Contents/MacOS/$executable_name"
}

make_bundle hardened-allow-jit "$fixture_dir/map-jit-guest-allow-jit.plist" map-jit-guest
make_bundle hardened-no-jit "" map-jit-guest
make_bundle hardened-perthread-fault \
  "$fixture_dir/map-jit-guest-allow-jit.plist" map-jit-perthread-fault
make_bundle hardened-jit-call \
  "$fixture_dir/map-jit-guest-allow-jit.plist" map-jit-call-guest
printf 'GPU guest fixtures prepared under %s\n' "$output"
