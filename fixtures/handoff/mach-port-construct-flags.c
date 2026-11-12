#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <stdint.h>
#include <stdio.h>

#define EXPECT_SEND ((mach_port_type_t)0x00010000U)
#define EXPECT_RECEIVE ((mach_port_type_t)0x00020000U)

static int check(uint32_t flags, uint32_t qlimit, int expect_send) {
  const mach_port_t task = mach_task_self();
  const mach_port_context_t context = UINT64_C(0x12345678abcdef01);
  const int guarded = (flags & MPO_CONTEXT_AS_GUARD) != 0;
  mach_port_options_t options = {0};
  options.flags = flags;
  options.mpl.mpl_qlimit = qlimit;
  mach_port_name_t name = MACH_PORT_NULL;
  if (_kernelrpc_mach_port_construct_trap(task, &options, guarded ? context : 0, &name) !=
      KERN_SUCCESS) {
    return 0;
  }
  mach_port_type_t type = 0;
  mach_port_urefs_t refs = 0xdeadbeefU;
  const int type_ok = _kernelrpc_mach_port_type_trap(task, name, &type) == KERN_SUCCESS &&
      type == (expect_send ? (EXPECT_SEND | EXPECT_RECEIVE) : EXPECT_RECEIVE);
  const int refs_ok = !expect_send ||
      (mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
  const int destroyed = _kernelrpc_mach_port_destruct_trap(
      task, name, expect_send ? -1 : 0, guarded ? context : 0) == KERN_SUCCESS;
  return type_ok && refs_ok && destroyed;
}

int main(void) {
  const int qlimit_only = check(MPO_QLIMIT, 1, 0);
  const int tempowner = check(MPO_TEMPOWNER, 0, 0);
  const int importance = check(MPO_IMPORTANCE_RECEIVER, 0, 0);
  const int send_only = check(MPO_INSERT_SEND_RIGHT, 0, 1);
  const int strict_only = check(MPO_STRICT, 0, 0);
  const int azul = check(MPO_CONTEXT_AS_GUARD | MPO_QLIMIT | MPO_INSERT_SEND_RIGHT | MPO_STRICT,
      1, 1);
  const int zero_limit_send = check(MPO_QLIMIT | MPO_INSERT_SEND_RIGHT, 0, 1);

  mach_port_options_t big = {0};
  big.flags = MPO_QLIMIT;
  big.mpl.mpl_qlimit = 1025;
  mach_port_name_t name = MACH_PORT_DEAD;
  const kern_return_t big_result =
      _kernelrpc_mach_port_construct_trap(mach_task_self(), &big, 0, &name);
  const int big_ok = big_result == KERN_INVALID_VALUE && name == MACH_PORT_DEAD;

  const int pass = qlimit_only && tempowner && importance && send_only && strict_only && azul &&
      zero_limit_send && big_ok;
  printf("qlimit=%d tempowner=%d importance=%d send=%d strict=%d azul=%d zero=%d big=%d "
         "result=%s\n", qlimit_only, tempowner, importance, send_only, strict_only, azul,
      zero_limit_send, big_ok, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
