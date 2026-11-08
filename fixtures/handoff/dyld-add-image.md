# Add-image notification probe

Build and run the native differential from the repository root.

```sh
out=.build/handoff-probes
mkdir -p "$out"
xcrun clang -Wall -Wextra -Werror -dynamiclib fixtures/handoff/dep.c \
  -Wl,-install_name,@rpath/libdyld-add-image-dep.dylib \
  -o "$out/libdyld-add-image-dep.dylib"
xcrun clang -Wall -Wextra -Werror -dynamiclib fixtures/handoff/new_image.c \
  -L"$out" -ldyld-add-image-dep -Wl,-rpath,@loader_path \
  -Wl,-install_name,@rpath/libdyld-add-image-new.dylib \
  -o "$out/libdyld-add-image-new.dylib"
xcrun clang -Wall -Wextra -Werror fixtures/handoff/probe.c -ldl \
  -o "$out/dyld-add-image-probe"
xcrun clang -Wall -Wextra -Werror -dynamiclib fixtures/handoff/dyld-add-image-nested.c \
  -Wl,-install_name,@rpath/libdyld-add-image-nested.dylib \
  -o "$out/libdyld-add-image-nested.dylib"
xcrun clang -Wall -Wextra -Werror fixtures/handoff/dyld-add-image-reentrant.c -ldl \
  -o "$out/dyld-add-image-reentrant"
"$out/dyld-add-image-probe" "$out/libdyld-add-image-new.dylib" \
  "$out/missing.dylib"
"$out/dyld-add-image-reentrant" "$out/libdyld-add-image-new.dylib" \
  "$out/missing.dylib" "$out/libdyld-add-image-nested.dylib"
```

The probe verifies that registration synchronously visits every currently
loaded image, a failed `dlopen` emits no add callback, and a successful
`dlopen` emits one callback for the requested image and one for its dependency.
On the inspected native host, the newly requested image arrives before its
dependency. Every callback header matches `dladdr`, and every received slide matches the
Mach-O `__TEXT` segment mapping. The output normalizes away ASLR.

Native result on 8 November 2026:

```
initial=81 failed=0 added=2 order=1 headers_slides=81/81 value=42
```

The libraries' constructors print `C_R` and `C_D`; add callbacks print `A_R`
and `A_D`. Native order is `A_R`, `A_D`, `C_D`, `C_R`, so notifications
precede dependency-first initializers.

The reentrant probe is built with the same two libraries plus
`dyld-add-image-nested.c` and `dyld-add-image-reentrant.c`. Its callback opens
the nested library while handling the requested root image. The native output
order is `A_R`, `A_N`, `A_D`, `C_D`, `C_R` and event sequence is `RNrDO`.
The callback synchronously completes the nested load before the outer load
continues with its dependency notification.

These native controls establish the guest contract for synchronous replay,
failed-load silence, root-before-dependency notifications, pre-initializer
delivery, and reentrant `dlopen`. The runtime uses `GuestImageRegistry` for the
ordered image snapshot and callback frames for guest delivery. GPU parity is a
separate gate.
