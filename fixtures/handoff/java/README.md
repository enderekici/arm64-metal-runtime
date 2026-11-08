# Java 26 startup and JIT controls

The fixture programs run on the installed Azul JDK 26 without modifying its
launcher, `libjli`, `libjvm`, or shared cache. `Workload` covers ordinary class
loading, collections, streams, and output. `ThreadedWorkload` exercises
thread start, monitor wait/notify, join, and a hot worker method. `HotWorkload`
makes a non-inlined method hot enough for synchronous C1 and C2 compilation.

Run the native and translated controls with:

```sh
JDK_HOME="$HOME/Library/Java/JavaVirtualMachines/azul-26.0.1/Contents/Home"
make verify-java JDK_HOME="$JDK_HOME" \
  JAVA_VERIFY_RUNTIME="$PWD/.build/cpp/arm64-metal"
```

The gate compares status, stdout, and non-trace stderr for default, `-Xint`,
C1, C2, the threaded fixture, JNI defaults, and `JNI_CreateJavaVM`. It compares
`java -version` stdout and stderr independently because the JDK writes its
version text to stderr. The VM creation case calls `JniSmoke.run()` through JNI
and destroys the VM. It stores the runtime hash and per-process receipts under
`.build/java-verification`.

The C1/C2 controls use `-Xbatch`, `-XX:+PrintCompilation`, and
`-XX:+PrintNMethods`. For the native and guest launches, the gate requires
HotSpot to compile the named non-inlined method at the requested tier and
obtains the emitted nmethod address range. The guest launch sets
`ARM64_METAL_TRACE_JIT=1`. The bounded JIT trace reports a dispatch only when
the PC belongs to an executable dynamic VM mapping that the runtime owns. It
records at most 256 unique mapping-generation and PC pairs, with mapping
bounds, backing identity, generation, MAP_JIT status, and whether the GPU used
translated code or the GPU interpreter. This bounded receipt is independent
of the general progress trace's event cap. The gate requires the dispatch PC
to fall inside HotSpot's emitted nmethod range. Static images and shared-cache
text do not produce JIT-owned events. The hot method is checked in both the
single-threaded and threaded workloads. The mode flags by themselves do not
satisfy that check.

The JNI defaults case uses `fixtures/handoff/jni-entry.c` and checks JNI
argument defaults plus the empty created-VM list. The separate VM creation
case loads `libjvm.dylib`, starts a VM, runs a Java method through JNI, and
destroys the VM.
