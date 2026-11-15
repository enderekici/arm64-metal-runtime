#!/bin/sh
# usage: dump_images.sh DSC_DUMP_BIN OUTDIR   (images used by Azul java / plutil / tar closure)
exec "$1" "$2" /usr/lib/libSystem.B.dylib /usr/lib/system/libsystem_kernel.dylib /usr/lib/system/libsystem_c.dylib \
 /usr/lib/system/libsystem_pthread.dylib /usr/lib/system/libdispatch.dylib /usr/lib/system/libdyld.dylib \
 /usr/lib/system/libsystem_platform.dylib /usr/lib/system/libsystem_malloc.dylib /usr/lib/system/libsystem_info.dylib \
 /usr/lib/libobjc.A.dylib /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation \
 /usr/lib/system/libxpc.dylib /usr/lib/libc++.1.dylib /usr/lib/system/libsystem_trace.dylib \
 /usr/lib/system/libsystem_notify.dylib /usr/lib/system/libsystem_darwin.dylib /usr/lib/system/libsystem_blocks.dylib \
 /usr/lib/system/libcorecrypto.dylib /usr/lib/system/libsystem_featureflags.dylib /usr/lib/system/libsystem_sandbox.dylib \
 /usr/lib/system/libsystem_containermanager.dylib /usr/lib/system/libsystem_configuration.dylib \
 /usr/lib/system/libsystem_coreservices.dylib /usr/lib/system/libsystem_asl.dylib /usr/lib/libarchive.2.dylib \
 /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation /usr/lib/libc++abi.dylib \
 /usr/lib/system/libunwind.dylib /usr/lib/system/libcompiler_rt.dylib /usr/lib/system/libremovefile.dylib \
 /usr/lib/system/libcache.dylib /usr/lib/system/libcommonCrypto.dylib /usr/lib/system/libmacho.dylib \
 /usr/lib/system/libkeymgr.dylib /usr/lib/system/libquarantine.dylib
