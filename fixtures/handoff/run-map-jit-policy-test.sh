#!/bin/sh
set -eu

test_binary=$1
fixture_binary=$2
fixture_dir=$3
build_dir=$4
work_dir=$(mktemp -d "$build_dir/map-jit-policy.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

make_bundle() {
  name=$1
  entitlements=${2-}
  bundle="$work_dir/$name.app"
  mkdir -p "$bundle/Contents/MacOS"
  cp "$fixture_binary" "$bundle/Contents/MacOS/fixture"
  cp "$fixture_dir/map-jit-policy-info.plist" "$bundle/Contents/Info.plist"
  if [ -n "$entitlements" ]; then
    codesign --force --sign - --options runtime --timestamp=none \
      --entitlements "$entitlements" "$bundle"
  else
    codesign --force --sign - --options runtime --timestamp=none "$bundle"
  fi
  codesign --verify --strict "$bundle"
  printf '%s\n' "$bundle/Contents/MacOS/fixture"
}

without_jit=$(make_bundle hardened-no-jit)
with_jit=$(make_bundle hardened-allow-jit "$fixture_dir/map-jit-policy-allow-jit.plist")
"$test_binary" "$without_jit" "$with_jit"
