# Mach port deallocation probes

mach-port-deallocate.c is the guest/runtime parity fixture. It uses raw ARM64 traps for task_self (-28), host_self (-29), and mach_port_deallocate (-18), plus the runtime's modeled receive-port construct/destruct path. It emits normalized booleans and does not print task-assigned port names.

Expected parity output on the qualified host:

    task_stable=1 task_releases=1 host_stable=1 host_releases=1 host_final_invalid=1 target_invalid_dest=1 unknown_name_invalid=1 sentinels_noop=1 receive_only_rejected=1 receive_cleanup=1

Build and run the native fixture with xcrun clang -Wall -Wextra -Werror -O2 mach-port-deallocate.c -o mach-port-deallocate && ./mach-port-deallocate. For the guest runtime, use the frozen C++ executable with ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 and leave cache bootstrap/initializers unset so this fixture reaches main before libSystem startup Mach messages.

mach-port-deallocate-native.c is the wider native-only oracle. It also probes send-once and dead-name transitions, which are outside the guest fixture's supported scope. Its observed host results were: repeated host_self returned one stable name and increased send urefs from 1 to 2; mach_port_deallocate reduced that count one per call, and a third deallocation returned KERN_INVALID_NAME. A dead name was created when its receive right was destroyed while a send right remained, then deallocated successfully. A send-once right transferred in a message was released successfully; repeat deallocation returned KERN_INVALID_RIGHT while mach_port_type reported the name removed. MACH_PORT_NULL and MACH_PORT_DEAD are successful no-ops. A receive-only right returns KERN_INVALID_RIGHT. The full output and exit status are asserted by the oracle.

The raw trap ABI comes from XNU syscall_map.lua and syscall_sw.c. Trap 18 is _kernelrpc_mach_port_deallocate_trap(target, name), trap 28 is task_self_trap, and trap 29 is host_self_trap.
