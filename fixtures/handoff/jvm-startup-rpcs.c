#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/thread_act.h>
#include <notify.h>
#include <os/log.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void out(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write(1, buf, n);
}

static void tick(CFRunLoopTimerRef t, void *info) {
  (*(int *)info)++;
  CFRunLoopStop(CFRunLoopGetCurrent());
}

int main(void) {
  mach_port_t port = MACH_PORT_NULL;
  out("look_up missing=%d\n", bootstrap_look_up(bootstrap_port, "com.example.no-such", &port));
  out("look_up existing=%d\n", bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &port));
  host_basic_info_data_t hi; mach_msg_type_number_t n = HOST_BASIC_INFO_COUNT;
  out("host_info=%d ncpu_ok=%d\n", host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t)&hi, &n), hi.max_cpus > 0);
  vm_size_t ps = 0; out("host_page_size=%d ps=%lu\n", host_page_size(mach_host_self(), &ps), (unsigned long)ps);
  host_priv_t hp = MACH_PORT_NULL; out("host_get_host_priv_port=%d\n", host_get_host_priv_port(mach_host_self(), &hp));
  mach_port_t pset = MACH_PORT_NULL; out("host_processor_set_priv_default=%d\n", processor_set_default(mach_host_self(), &pset));
  task_basic_info_data_t ti; n = TASK_BASIC_INFO_COUNT;
  out("task_info basic=%d\n", task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&ti, &n));
  task_vm_info_data_t vi; n = TASK_VM_INFO_COUNT;
  out("task_info vm=%d\n", task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n));
  task_dyld_info_data_t di; n = TASK_DYLD_INFO_COUNT;
  out("task_info dyld=%d\n", task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&di, &n));
  task_events_info_data_t ei; n = TASK_EVENTS_INFO_COUNT;
  out("task_info events=%d\n", task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ei, &n));
  task_thread_times_info_data_t tt; n = TASK_THREAD_TIMES_INFO_COUNT;
  out("task_info thread_times=%d\n", task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t)&tt, &n));
  thread_basic_info_data_t thi; n = THREAD_BASIC_INFO_COUNT;
  out("thread_info basic=%d\n", thread_info(mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t)&thi, &n));
  thread_identifier_info_data_t tid; n = THREAD_IDENTIFIER_INFO_COUNT;
  out("thread_info identifier=%d\n", thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO, (thread_info_t)&tid, &n));
  thread_extended_info_data_t tei; n = THREAD_EXTENDED_INFO_COUNT;
  out("thread_info extended=%d\n", thread_info(mach_thread_self(), THREAD_EXTENDED_INFO, (thread_info_t)&tei, &n));
  thread_policy_flavor_t pf = THREAD_PRECEDENCE_POLICY; thread_precedence_policy_data_t pp; n = THREAD_PRECEDENCE_POLICY_COUNT; boolean_t def = 0;
  out("thread_policy_get=%d\n", thread_policy_get(mach_thread_self(), pf, (thread_policy_t)&pp, &n, &def));
  mach_port_t p2 = MACH_PORT_NULL;
  out("port_allocate=%d\n", mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p2));
  mach_port_type_t ty = 0; out("port_type=%d ty=0x%x\n", mach_port_type(mach_task_self(), p2, &ty), ty);
  mach_port_status_t st; n = MACH_PORT_RECEIVE_STATUS_COUNT;
  out("port_get_attributes=%d\n", mach_port_get_attributes(mach_task_self(), p2, MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&st, &n));
  mach_port_limits_t lim = {.mpl_qlimit = 8};
  out("port_set_attributes=%d\n", mach_port_set_attributes(mach_task_self(), p2, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&lim, MACH_PORT_LIMITS_INFO_COUNT));
  mach_port_context_t ctx = 0x1234; out("port_set_context=%d\n", mach_port_set_context(mach_task_self(), p2, ctx));
  mach_port_context_t ctx2 = 0; out("port_get_context=%d ctx=%llx\n", mach_port_get_context(mach_task_self(), p2, &ctx2), (unsigned long long)ctx2);
  { mach_port_limits_t big = {.mpl_qlimit = 2000};
    out("set_attributes qlimit=2000 -> %d\n", mach_port_set_attributes(mach_task_self(), p2, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&big, MACH_PORT_LIMITS_INFO_COUNT));
    out("set_attributes bad name -> %d\n", mach_port_set_attributes(mach_task_self(), 0x12345, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&lim, MACH_PORT_LIMITS_INFO_COUNT));
    out("set_attributes send right -> %d\n", mach_port_set_attributes(mach_task_self(), mach_host_self(), MACH_PORT_LIMITS_INFO, (mach_port_info_t)&lim, MACH_PORT_LIMITS_INFO_COUNT));
    mach_port_context_t cx = 0;
    out("set_context bad name -> %d\n", mach_port_set_context(mach_task_self(), 0x12345, 1));
    out("get_context bad name -> %d\n", mach_port_get_context(mach_task_self(), 0x12345, &cx));
    out("get_context send right -> %d\n", mach_port_get_context(mach_task_self(), mach_host_self(), &cx));
    out("set_context send right -> %d\n", mach_port_set_context(mach_task_self(), mach_host_self(), 1)); }
  mach_port_t prev = MACH_PORT_NULL;
  out("port_request_notification=%d\n", mach_port_request_notification(mach_task_self(), p2, MACH_NOTIFY_NO_SENDERS, 0, p2, MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev));
  { mach_port_t pv = MACH_PORT_NULL;
    out("request_notification bad name -> %d\n", mach_port_request_notification(mach_task_self(), 0x12345, MACH_NOTIFY_NO_SENDERS, 0, p2, MACH_MSG_TYPE_MAKE_SEND_ONCE, &pv));
    out("request_notification send right -> %d\n", mach_port_request_notification(mach_task_self(), mach_host_self(), MACH_NOTIFY_NO_SENDERS, 0, p2, MACH_MSG_TYPE_MAKE_SEND_ONCE, &pv));
    out("request_notification null notify -> %d prev=%u\n", mach_port_request_notification(mach_task_self(), p2, MACH_NOTIFY_NO_SENDERS, 0, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND_ONCE, &pv), pv != MACH_PORT_NULL);
    out("request_notification again -> %d prev=%u\n", mach_port_request_notification(mach_task_self(), p2, MACH_NOTIFY_NO_SENDERS, 0, p2, MACH_MSG_TYPE_MAKE_SEND_ONCE, &pv), pv != MACH_PORT_NULL); }
  out("port_insert_right=%d\n", mach_port_insert_right(mach_task_self(), p2, p2, MACH_MSG_TYPE_MAKE_SEND));
  mach_port_urefs_t refs = 0; out("port_get_refs=%d refs=%u\n", mach_port_get_refs(mach_task_self(), p2, MACH_PORT_RIGHT_SEND, &refs), refs);
  out("port_mod_refs=%d\n", mach_port_mod_refs(mach_task_self(), p2, MACH_PORT_RIGHT_SEND, -1));
  out("port_construct=%d\n", ({ mach_port_options_t o = {.flags = MPO_INSERT_SEND_RIGHT}; mach_port_t q = 0; kern_return_t k = mach_port_construct(mach_task_self(), &o, 0x55, &q); if (!k) mach_port_destruct(mach_task_self(), q, -1, 0x55); k; }));
  mach_port_name_array_t names; mach_msg_type_number_t nn, tn; mach_port_type_array_t types;
  out("port_names=%d\n", mach_port_names(mach_task_self(), &names, &nn, &types, &tn));
  out("port_destroy=%d\n", mach_port_destroy(mach_task_self(), p2));
  exception_mask_t masks[EXC_TYPES_COUNT]; mach_msg_type_number_t cnt = EXC_TYPES_COUNT; mach_port_t handlers[EXC_TYPES_COUNT]; exception_behavior_t beh[EXC_TYPES_COUNT]; thread_state_flavor_t fl[EXC_TYPES_COUNT];
  out("task_get_exception_ports=%d\n", task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, masks, &cnt, handlers, beh, fl));
  mach_port_t sp = MACH_PORT_NULL;
  out("task_get_special_port=%d\n", task_get_special_port(mach_task_self(), TASK_ACCESS_PORT, &sp));
  out("task_suspend_count_ok=%d\n", task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&pp, &n, &def));
  out("thread_get_state=%d\n", ({ arm_thread_state64_t s; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT; thread_get_state(mach_thread_self(), ARM_THREAD_STATE64, (thread_state_t)&s, &c); }));
  uint32_t tok = 0; int nst = notify_register_check("com.example.arm64-metal.probe", &tok);
  out("notify_register_check=%d\n", nst);
  CFTypeRef pref = CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), CFSTR("kCFPreferencesAnyApplication"));
  out("CFPreferencesCopyAppValue=%d\n", pref != NULL);
  os_log_t log = os_log_create("com.example.probe", "startup"); os_log(log, "probe message"); out("os_log=1\n");
  int ticks = 0; CFRunLoopTimerContext tc = {0, &ticks, NULL, NULL, NULL};
  CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.05, 0, 0, 0, tick, &tc);
  CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
  out("CFRunLoop ticks=%d\n", ticks);
  pthread_attr_t at; pthread_attr_init(&at);
  out("pthread_attr_set_qos=%d\n", pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INITIATED, 0));
  qos_class_t q; int rp; out("pthread_get_qos=%d q=0x%x\n", pthread_get_qos_class_np(pthread_self(), &q, &rp), q);
  out("pthread_set_qos=%d\n", pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0));
  out("done\n");
  return 0;
}
