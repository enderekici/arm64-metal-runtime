#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [ -n "${OUT_DIR:-}" ]; then
  out=$OUT_DIR
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/dlopen-from-marker.XXXXXX")
  trap 'rm -rf "$out"' EXIT HUP INT TERM
fi
mkdir -p "$out/rpaths"

xcrun clang -std=c11 -Wall -Wextra -Werror -pedantic -dynamiclib \
  -DMARKER_VALUE=41 -Wl,-install_name,@rpath/libloader-marker-target.dylib \
  -o "$out/libloader-marker-target.dylib" \
  "$repo_root/fixtures/handoff/dlopen-from-target.c"
xcrun clang -std=c11 -Wall -Wextra -Werror -pedantic -dynamiclib \
  -DMARKER_VALUE=73 -Wl,-install_name,@rpath/librpath-marker-target.dylib \
  -o "$out/rpaths/librpath-marker-target.dylib" \
  "$repo_root/fixtures/handoff/dlopen-from-target.c"
xcrun clang -std=c11 -Wall -Wextra -Werror -pedantic -dynamiclib \
  -Wl,-install_name,@rpath/libdlopen-from-caller.dylib \
  -Wl,-rpath,@loader_path/rpaths \
  -o "$out/libdlopen-from-caller.dylib" \
  "$repo_root/fixtures/handoff/dlopen-from-caller.c"
xcrun clang -std=c11 -Wall -Wextra -Werror -pedantic \
  -Wl,-rpath,@executable_path \
  -o "$out/dlopen-from-marker" \
  "$repo_root/fixtures/handoff/dlopen-from-marker.c" \
  "$out/libdlopen-from-caller.dylib"
"$out/dlopen-from-marker" "$out/libdlopen-from-caller.dylib"
