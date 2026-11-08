#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#include <mach/task_special_ports.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TaskGetSpecialPortId = 3409 };

typedef struct {
  mach_msg_header_t header;
  NDR_record_t ndr;
  int32_t whichPort;
} TaskGetSpecialPortRequest;

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t specialPort;
  mach_msg_trailer_t trailer;
} TaskGetSpecialPortReply;

typedef struct {
  mach_msg_header_t header;
  NDR_record_t ndr;
  kern_return_t returnCode;
  mach_msg_trailer_t trailer;
} TaskGetSpecialPortErrorReply;

typedef union {
  TaskGetSpecialPortRequest request;
  TaskGetSpecialPortReply reply;
  uint8_t bytes[80];
} TaskGetSpecialPortBuffer;

static uint64_t mach_msg2_raw(void *data, uint64_t options,
                              uint64_t bits_and_send_size,
                              uint64_t remote_and_local_port,
                              uint64_t voucher_and_id,
                              uint64_t desc_count_and_receive_name,
                              uint64_t receive_size_and_priority,
                              uint64_t timeout) {
  register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)data;
  register uint64_t x1 __asm__("x1") = options;
  register uint64_t x2 __asm__("x2") = bits_and_send_size;
  register uint64_t x3 __asm__("x3") = remote_and_local_port;
  register uint64_t x4 __asm__("x4") = voucher_and_id;
  register uint64_t x5 __asm__("x5") = desc_count_and_receive_name;
  register uint64_t x6 __asm__("x6") = receive_size_and_priority;
  register uint64_t x7 __asm__("x7") = timeout;
  register uint64_t x16 __asm__("x16") = (uint64_t)-47;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x6), "r"(x7), "r"(x16) : "memory");
  return x0;
}

static uint64_t mach_trap4(uint64_t number, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3) {
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x16) : "memory");
  return x0;
}

static kern_return_t allocate_receive(mach_port_t task, mach_port_t *name) {
  return (kern_return_t)mach_trap4((uint64_t)-16, task, MACH_PORT_RIGHT_RECEIVE,
                                   (uint64_t)(uintptr_t)name, 0);
}

static kern_return_t destroy_port(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-25, task, name, 0, 0);
}

static kern_return_t port_type(mach_port_t task, mach_port_t name,
                               mach_port_type_t *type) {
  return (kern_return_t)mach_trap4((uint64_t)-76, task, name,
                                   (uint64_t)(uintptr_t)type, 0);
}

static kern_return_t deallocate_port(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-18, task, name, 0, 0);
}

static uint64_t get_special_port(mach_port_t task, mach_port_t replyPort,
                                 int32_t whichPort, TaskGetSpecialPortReply *reply) {
  TaskGetSpecialPortBuffer buffer;
  memset(&buffer, 0, sizeof(buffer));
  buffer.request.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                                    MACH_MSG_TYPE_MAKE_SEND_ONCE);
  buffer.request.header.msgh_size = sizeof(buffer.request);
  buffer.request.header.msgh_remote_port = task;
  buffer.request.header.msgh_local_port = replyPort;
  buffer.request.header.msgh_id = TaskGetSpecialPortId;
  buffer.request.ndr = NDR_record;
  buffer.request.whichPort = whichPort;
  const uint64_t result = mach_msg2_raw(&buffer,
      MACH_SEND_MSG | MACH_RCV_MSG | (UINT64_C(2) << 32),
      ((uint64_t)sizeof(buffer.request) << 32) | buffer.request.header.msgh_bits,
      ((uint64_t)replyPort << 32) | task,
      (uint64_t)(uint32_t)TaskGetSpecialPortId << 32,
      (uint64_t)replyPort << 32,
      48, 0);
  memcpy(reply, &buffer.reply, sizeof(*reply));
  return result;
}

static int one_query(mach_port_t task, int32_t whichPort,
                     uint64_t *statusOut, uint32_t *wireBitsOut,
                     uint32_t *wireSizeOut, uint32_t *replyIdOut,
                     uint32_t *returnedNameOut,
                     uint32_t *descriptorOut, uint32_t *rightTypeOut,
                     uint32_t *trailerTypeOut, uint32_t *trailerSizeOut,
                     int32_t *typeStatusOut, int32_t *errorOut) {
  mach_port_t replyPort = MACH_PORT_NULL;
  if (allocate_receive(task, &replyPort) != KERN_SUCCESS) return 0;
  TaskGetSpecialPortReply reply;
  *statusOut = get_special_port(task, replyPort, whichPort, &reply);
  *wireBitsOut = reply.header.msgh_bits;
  *wireSizeOut = reply.header.msgh_size;
  *replyIdOut = (uint32_t)reply.header.msgh_id;
  *returnedNameOut = reply.specialPort.name;
  *descriptorOut = (uint32_t)reply.specialPort.disposition |
      ((uint32_t)reply.specialPort.type << 8) |
      ((uint32_t)reply.body.msgh_descriptor_count << 16);
  const mach_msg_trailer_t *trailer = (const mach_msg_trailer_t *)
      ((const uint8_t *)&reply + reply.header.msgh_size);
  *trailerTypeOut = trailer->msgh_trailer_type;
  *trailerSizeOut = trailer->msgh_trailer_size;
  *errorOut = 0;
  *rightTypeOut = 0;
  *typeStatusOut = 0;
  int valid = 1;
  if (*statusOut == MACH_MSG_SUCCESS &&
      (reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0) {
    mach_port_type_t type = 0;
    const kern_return_t typeStatus = port_type(task, reply.specialPort.name, &type);
    *rightTypeOut = type;
    *typeStatusOut = typeStatus;
    valid = reply.header.msgh_bits == 0x80001200 &&
        reply.header.msgh_size == 40 && reply.header.msgh_id == TaskGetSpecialPortId + 100 &&
        reply.body.msgh_descriptor_count == 1 &&
        reply.specialPort.disposition == MACH_MSG_TYPE_MOVE_SEND &&
        reply.specialPort.type == MACH_MSG_PORT_DESCRIPTOR &&
        *trailerTypeOut == MACH_MSG_TRAILER_FORMAT_0 &&
        *trailerSizeOut == MACH_MSG_TRAILER_MINIMUM_SIZE &&
        typeStatus == KERN_SUCCESS && (type & MACH_PORT_TYPE_SEND) != 0;
    if (reply.specialPort.name != MACH_PORT_NULL &&
        reply.specialPort.name != MACH_PORT_DEAD) {
      valid = deallocate_port(task, reply.specialPort.name) == KERN_SUCCESS && valid;
    }
  } else if (*statusOut == MACH_MSG_SUCCESS && reply.header.msgh_size >= 36) {
    TaskGetSpecialPortErrorReply *errorReply = (TaskGetSpecialPortErrorReply *)&reply;
    *errorOut = errorReply->returnCode;
    valid = reply.header.msgh_bits == 0x1200 && reply.header.msgh_size == 36 &&
        reply.header.msgh_id == TaskGetSpecialPortId + 100 &&
        *trailerTypeOut == MACH_MSG_TRAILER_FORMAT_0 &&
        *trailerSizeOut == MACH_MSG_TRAILER_MINIMUM_SIZE &&
        memcmp(&errorReply->ndr, &NDR_record, sizeof(NDR_record)) == 0;
  }
  valid = destroy_port(task, replyPort) == KERN_SUCCESS && valid;
  return valid;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const int32_t selectors[] = {TASK_BOOTSTRAP_PORT, TASK_KERNEL_PORT, 0, 7, -1};
  int okay = 1;
  for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
    uint64_t status = 0;
    uint32_t bits = 0, size = 0, replyId = 0, name = 0, descriptor = 0, type = 0;
    uint32_t trailerType = 0, trailerSize = 0;
    int32_t typeStatus = 0, error = 0;
    const int wireValid = one_query(task, selectors[i], &status, &bits, &size,
        &replyId, &name, &descriptor, &type, &trailerType, &trailerSize,
        &typeStatus, &error);
    const int expectedError = selectors[i] == TASK_KERNEL_PORT ? 53 :
        (selectors[i] == 0 || selectors[i] == 7 ? 4 :
         (selectors[i] == -1 ? 53 : 0));
    const int valid = wireValid && (expectedError == 0
        ? ((bits & MACH_MSGH_BITS_COMPLEX) != 0 && error == 0 &&
           typeStatus == KERN_SUCCESS && (type & MACH_PORT_TYPE_SEND) != 0)
        : ((bits & MACH_MSGH_BITS_COMPLEX) == 0 && error == expectedError));
    printf("selector=%d valid=%d status=%llu bits=0x%x size=%u reply=%u "
           "descriptor=0x%x right_type=0x%x type_status=%d "
           "trailer=%u/%u error=%d\n",
           selectors[i], valid, (unsigned long long)status, bits, size, replyId,
           descriptor, type, typeStatus, trailerType, trailerSize, error);
    okay &= valid;
  }
  return !okay;
}
