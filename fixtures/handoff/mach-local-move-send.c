#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { RequestId = 0x7152, ReceiveCapacity = 48 };

struct Message {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
  mach_msg_trailer_t trailer;
};

struct MessageTwo {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t ports[2];
  mach_msg_trailer_t trailer;
};

struct MessageThree {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t ports[3];
  mach_msg_trailer_t trailer;
};

struct PortPair {
  mach_port_t destination;
  mach_port_t source;
};

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

static kern_return_t create_port(mach_port_t task, mach_port_t *name) {
  const kern_return_t allocated = (kern_return_t)mach_trap4(
      (uint64_t)-16, task, MACH_PORT_RIGHT_RECEIVE, (uint64_t)(uintptr_t)name, 0);
  if (allocated != KERN_SUCCESS) return allocated;
  return (kern_return_t)mach_trap4((uint64_t)-21, task, *name, *name,
                                   MACH_MSG_TYPE_MAKE_SEND);
}

static kern_return_t deallocate_send(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-18, task, name, 0, 0);
}

static kern_return_t release_receive(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-19, task, name,
                                   MACH_PORT_RIGHT_RECEIVE, (uint64_t)-1);
}

static kern_return_t port_type(mach_port_t task, mach_port_t name,
                               mach_port_type_t *type) {
  return (kern_return_t)mach_trap4((uint64_t)-76, task, name,
                                   (uint64_t)(uintptr_t)type, 0);
}

static int create_pair(mach_port_t task, struct PortPair *pair) {
  pair->destination = MACH_PORT_NULL;
  pair->source = MACH_PORT_NULL;
  return create_port(task, &pair->destination) == KERN_SUCCESS &&
      create_port(task, &pair->source) == KERN_SUCCESS;
}

static uint64_t send_port(mach_port_t destination, mach_port_t source,
                          mach_msg_type_name_t disposition) {
  struct Message message;
  memset(&message, 0, sizeof(message));
  message.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.header.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
      sizeof(mach_msg_port_descriptor_t);
  message.header.msgh_remote_port = destination;
  message.header.msgh_id = RequestId;
  message.body.msgh_descriptor_count = 1;
  message.port.name = source;
  message.port.disposition = disposition;
  message.port.type = MACH_MSG_PORT_DESCRIPTOR;
  return mach_msg2_raw(&message.header, MACH_SEND_MSG | (UINT64_C(8) << 32),
      ((uint64_t)message.header.msgh_size << 32) | message.header.msgh_bits,
      destination, (uint64_t)RequestId << 32, 1, 0, 0);
}

static uint64_t receive_port(mach_port_t destination, void *buffer,
                             uint32_t capacity, int timeout) {
  const uint64_t options = MACH_RCV_MSG | (timeout ? MACH_RCV_TIMEOUT : 0);
  return mach_msg2_raw(buffer, options, 0, 0, 0,
      (uint64_t)destination << 32, capacity, timeout ? 1 : 0);
}

static uint64_t send_two_ports_with_second_type(mach_port_t destination,
                                                mach_port_t first,
                                                mach_msg_type_name_t firstDisposition,
                                                mach_port_t second,
                                                mach_msg_type_name_t secondDisposition,
                                                uint8_t secondType) {
  struct MessageTwo message;
  memset(&message, 0, sizeof(message));
  message.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.header.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
      2 * sizeof(mach_msg_port_descriptor_t);
  message.header.msgh_remote_port = destination;
  message.header.msgh_id = RequestId;
  message.body.msgh_descriptor_count = 2;
  message.ports[0].name = first;
  message.ports[0].disposition = firstDisposition;
  message.ports[0].type = MACH_MSG_PORT_DESCRIPTOR;
  message.ports[1].name = second;
  message.ports[1].disposition = secondDisposition;
  message.ports[1].type = secondType;
  return mach_msg2_raw(&message.header, MACH_SEND_MSG | (UINT64_C(8) << 32),
      ((uint64_t)message.header.msgh_size << 32) | message.header.msgh_bits,
      destination, (uint64_t)RequestId << 32, 2, 0, 0);
}

static uint64_t send_two_ports(mach_port_t destination,
                               mach_port_t first,
                               mach_msg_type_name_t firstDisposition,
                               mach_port_t second,
                               mach_msg_type_name_t secondDisposition) {
  return send_two_ports_with_second_type(destination, first, firstDisposition, second,
                                         secondDisposition, MACH_MSG_PORT_DESCRIPTOR);
}

static uint64_t send_three_ports(mach_port_t destination, const mach_port_t sources[3],
                                 const mach_msg_type_name_t dispositions[3],
                                 uint32_t declaredSize, uint32_t sendSize) {
  struct MessageThree message;
  memset(&message, 0, sizeof(message));
  message.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.header.msgh_size = declaredSize;
  message.header.msgh_remote_port = destination;
  message.header.msgh_id = RequestId;
  message.body.msgh_descriptor_count = 3;
  for (size_t index = 0; index < 3; ++index) {
    message.ports[index].name = sources[index];
    message.ports[index].disposition = dispositions[index];
    message.ports[index].type = MACH_MSG_PORT_DESCRIPTOR;
  }
  return mach_msg2_raw(&message.header, MACH_SEND_MSG | (UINT64_C(8) << 32),
      ((uint64_t)sendSize << 32) | message.header.msgh_bits, destination,
      (uint64_t)RequestId << 32, 3, 0, 0);
}

static uint64_t receive_three_ports(mach_port_t destination, struct MessageThree *buffer,
                                    uint32_t capacity, int timeout) {
  memset(buffer, 0, sizeof(*buffer));
  const uint64_t options = MACH_RCV_MSG | (timeout ? MACH_RCV_TIMEOUT : 0);
  return mach_msg2_raw(&buffer->header, options, 0, 0, 0,
      (uint64_t)destination << 32, capacity, timeout ? 1 : 0);
}

static int received_three_ports_valid(const struct MessageThree *message,
                                      mach_port_t destination,
                                      const mach_port_t expectedNames[3]) {
  if (message->header.msgh_id != RequestId ||
      message->header.msgh_size != sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
          3 * sizeof(mach_msg_port_descriptor_t) ||
      message->header.msgh_bits !=
          (MACH_MSGH_BITS_COMPLEX | (MACH_MSG_TYPE_MOVE_SEND << 8)) ||
      message->header.msgh_remote_port != MACH_PORT_NULL ||
      message->header.msgh_local_port != destination ||
      message->header.msgh_voucher_port != MACH_PORT_NULL ||
      message->body.msgh_descriptor_count != 3) return 0;
  for (size_t index = 0; index < 3; ++index) {
    if (message->ports[index].name != expectedNames[index] ||
        message->ports[index].disposition != MACH_MSG_TYPE_MOVE_SEND ||
        message->ports[index].type != MACH_MSG_PORT_DESCRIPTOR ||
        message->ports[index].pad1 != 0 || message->ports[index].pad2 != 0) return 0;
  }
  return 1;
}

static uint64_t receive_two_ports(mach_port_t destination, struct MessageTwo *buffer,
                                  uint32_t capacity, int timeout) {
  memset(buffer, 0, sizeof(*buffer));
  const uint64_t options = MACH_RCV_MSG | (timeout ? MACH_RCV_TIMEOUT : 0);
  return mach_msg2_raw(&buffer->header, options, 0, 0, 0,
      (uint64_t)destination << 32, capacity, timeout ? 1 : 0);
}

static int received_two_ports_valid(const struct MessageTwo *message,
                                    mach_port_t destination,
                                    mach_port_t first, mach_port_t second) {
  return message->header.msgh_id == RequestId &&
      message->header.msgh_size == sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
          2 * sizeof(mach_msg_port_descriptor_t) &&
      message->header.msgh_bits ==
          (MACH_MSGH_BITS_COMPLEX | (MACH_MSG_TYPE_MOVE_SEND << 8)) &&
      message->header.msgh_remote_port == MACH_PORT_NULL &&
      message->header.msgh_local_port == destination &&
      message->header.msgh_voucher_port == MACH_PORT_NULL &&
      message->body.msgh_descriptor_count == 2 &&
      message->ports[0].name == first && message->ports[1].name == second &&
      message->ports[0].disposition == MACH_MSG_TYPE_MOVE_SEND &&
      message->ports[1].disposition == MACH_MSG_TYPE_MOVE_SEND &&
      message->ports[0].type == MACH_MSG_PORT_DESCRIPTOR &&
      message->ports[1].type == MACH_MSG_PORT_DESCRIPTOR &&
      message->ports[0].pad1 == 0 && message->ports[0].pad2 == 0 &&
      message->ports[1].pad1 == 0 && message->ports[1].pad2 == 0;
}

static int send_present(mach_port_t task, mach_port_t name) {
  mach_port_type_t type = MACH_PORT_TYPE_NONE;
  return port_type(task, name, &type) == KERN_SUCCESS &&
      (type & MACH_PORT_TYPE_SEND) != 0;
}

static int cleanup_port(mach_port_t task, mach_port_t name,
                        unsigned sendRefs, int hasReceive) {
  int okay = 1;
  for (unsigned i = 0; i < sendRefs; ++i) {
    okay = deallocate_send(task, name) == KERN_SUCCESS && okay;
  }
  if (hasReceive) okay = release_receive(task, name) == KERN_SUCCESS && okay;
  return okay;
}

static int received_descriptor_valid(const struct Message *message,
                                     mach_port_t destination,
                                     mach_port_t expectedName,
                                     mach_msg_type_name_t expectedDisposition) {
  return message->header.msgh_id == RequestId &&
      message->header.msgh_size == sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
          sizeof(mach_msg_port_descriptor_t) &&
      message->header.msgh_bits ==
          (MACH_MSGH_BITS_COMPLEX | (MACH_MSG_TYPE_MOVE_SEND << 8)) &&
      message->header.msgh_remote_port == MACH_PORT_NULL &&
      message->header.msgh_local_port == destination &&
      message->header.msgh_voucher_port == MACH_PORT_NULL &&
      message->body.msgh_descriptor_count == 1 &&
      message->port.pad1 == 0 && message->port.pad2 == 0 &&
      message->port.disposition == MACH_MSG_TYPE_MOVE_SEND &&
      message->port.type == MACH_MSG_PORT_DESCRIPTOR &&
      message->port.name == expectedName &&
      (expectedDisposition == MACH_MSG_TYPE_MOVE_SEND ||
       expectedDisposition == MACH_MSG_TYPE_COPY_SEND);
}

int main(void) {
  const mach_port_t task = mach_task_self();
  struct PortPair moved, copied, tooLarge, badBuffer, destroyedMove, destroyedCopy;
  mach_port_t invalidMoveSource = MACH_PORT_NULL;
  int setup = 1;
  setup &= create_pair(task, &moved);
  setup &= create_pair(task, &copied);
  setup &= create_pair(task, &tooLarge);
  setup &= create_pair(task, &badBuffer);
  setup &= create_pair(task, &destroyedMove);
  setup &= create_pair(task, &destroyedCopy);
  if (!setup) {
    puts("setup_failed");
    return 1;
  }

  const uint64_t moveSend = send_port(moved.destination, moved.source,
                                      MACH_MSG_TYPE_MOVE_SEND);
  mach_port_type_t pendingMoveType = MACH_PORT_TYPE_NONE;
  const kern_return_t pendingMoveStatus = port_type(task, moved.source, &pendingMoveType);
  struct Message moveReply;
  memset(&moveReply, 0, sizeof(moveReply));
  const uint64_t moveReceive = receive_port(moved.destination, &moveReply,
                                             sizeof(moveReply), 1);
  const int moveDescriptor = moveReceive == MACH_MSG_SUCCESS &&
      received_descriptor_valid(&moveReply, moved.destination, moved.source,
                                MACH_MSG_TYPE_MOVE_SEND) && send_present(task, moved.source);

  const uint64_t copySend = send_port(copied.destination, copied.source,
                                      MACH_MSG_TYPE_COPY_SEND);
  const int pendingCopySend = send_present(task, copied.source);
  struct Message copyReply;
  memset(&copyReply, 0, sizeof(copyReply));
  const uint64_t copyReceive = receive_port(copied.destination, &copyReply,
                                             sizeof(copyReply), 1);
  const int copyDescriptor = copyReceive == MACH_MSG_SUCCESS &&
      received_descriptor_valid(&copyReply, copied.destination, copied.source,
                                MACH_MSG_TYPE_COPY_SEND) && send_present(task, copied.source);

  const uint64_t tooLargeSend = send_port(tooLarge.destination, tooLarge.source,
                                          MACH_MSG_TYPE_MOVE_SEND);
  struct Message tooLargeReply;
  const uint64_t tooLargeReceive = receive_port(tooLarge.destination, &tooLargeReply,
                                                 sizeof(mach_msg_header_t) +
                                                     sizeof(mach_msg_body_t), 1);
  const int tooLargeDroppedSend = !send_present(task, tooLarge.source);
  const uint64_t tooLargeRetry = receive_port(tooLarge.destination, &tooLargeReply,
                                               sizeof(tooLargeReply), 1);

  const uint64_t badBufferSend = send_port(badBuffer.destination, badBuffer.source,
                                           MACH_MSG_TYPE_MOVE_SEND);
  const uint64_t badBufferReceive = receive_port(badBuffer.destination,
      (void *)(uintptr_t)1, sizeof(struct Message), 1);
  const int badBufferRestoredSend = send_present(task, badBuffer.source);
  struct Message badBufferReply;
  const uint64_t badBufferRetry = receive_port(badBuffer.destination, &badBufferReply,
                                                sizeof(badBufferReply), 1);

  const uint64_t destroyedMoveSend = send_port(destroyedMove.destination,
      destroyedMove.source, MACH_MSG_TYPE_MOVE_SEND);
  const kern_return_t destroyMoveReceiver = release_receive(task, destroyedMove.destination);
  const int destroyedMoveDroppedSend = !send_present(task, destroyedMove.source);

  const uint64_t destroyedCopySend = send_port(destroyedCopy.destination,
      destroyedCopy.source, MACH_MSG_TYPE_COPY_SEND);
  const kern_return_t destroyCopyReceiver = release_receive(task, destroyedCopy.destination);
  const int destroyedCopyKeptOriginalSend = send_present(task, destroyedCopy.source);
  invalidMoveSource = destroyedCopy.source;
  const kern_return_t invalidSourcePrepared = deallocate_send(task, invalidMoveSource);

  const uint64_t mixedSend = send_two_ports(moved.destination, moved.source,
      MACH_MSG_TYPE_MOVE_SEND, copied.source, MACH_MSG_TYPE_COPY_SEND);
  mach_port_type_t pendingMixedMoveType = MACH_PORT_TYPE_NONE;
  const kern_return_t pendingMixedMoveStatus =
      port_type(task, moved.source, &pendingMixedMoveType);
  const int pendingMixedCopySend = send_present(task, copied.source);
  struct MessageTwo mixedReply;
  const uint64_t mixedReceive = receive_two_ports(moved.destination, &mixedReply,
                                                   sizeof(mixedReply), 1);
  const int mixedDescriptors = mixedReceive == MACH_MSG_SUCCESS &&
      received_two_ports_valid(&mixedReply, moved.destination, moved.source, copied.source);

  const mach_port_t threeSources[] = {moved.source, copied.source, badBuffer.source};
  const mach_msg_type_name_t threeDispositions[] = {
      MACH_MSG_TYPE_MOVE_SEND, MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MOVE_SEND};
  const uint32_t threeMessageSize = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
      3 * sizeof(mach_msg_port_descriptor_t);
  const uint64_t threeSend = send_three_ports(moved.destination, threeSources,
      threeDispositions, threeMessageSize, threeMessageSize);
  struct MessageThree threeReply;
  const uint64_t threeReceive = receive_three_ports(moved.destination, &threeReply,
      sizeof(threeReply), 1);
  const int threeDescriptors = threeReceive == MACH_MSG_SUCCESS &&
      received_three_ports_valid(&threeReply, moved.destination, threeSources);

  const mach_port_t truncatedSources[] = {badBuffer.source, copied.source, moved.source};
  const mach_msg_type_name_t truncatedDispositions[] = {
      MACH_MSG_TYPE_MOVE_SEND, MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MOVE_SEND};
  const uint32_t truncatedSize = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
      2 * sizeof(mach_msg_port_descriptor_t);
  const uint64_t truncatedSend = send_three_ports(badBuffer.destination,
      truncatedSources, truncatedDispositions, truncatedSize, truncatedSize);
  const int truncatedKeptFirstSend = send_present(task, badBuffer.source);
  struct MessageThree truncatedReply;
  const uint64_t truncatedReceive = receive_three_ports(badBuffer.destination,
      &truncatedReply, sizeof(truncatedReply), 1);

  const uint64_t malformedSecondSend = send_two_ports_with_second_type(
      badBuffer.destination, badBuffer.source, MACH_MSG_TYPE_MOVE_SEND,
      invalidMoveSource, MACH_MSG_TYPE_MOVE_SEND, 1);
  const int malformedSecondKeptFirstSend = send_present(task, badBuffer.source);
  struct MessageTwo malformedSecondReply;
  const uint64_t malformedSecondReceive = receive_two_ports(badBuffer.destination,
      &malformedSecondReply, sizeof(malformedSecondReply), 1);

  const uint64_t partialMoveSend = send_two_ports(moved.destination,
      moved.source, MACH_MSG_TYPE_MOVE_SEND, invalidMoveSource,
      MACH_MSG_TYPE_MOVE_SEND);
  mach_port_type_t partialMoveType = MACH_PORT_TYPE_NONE;
  const kern_return_t partialMoveTypeStatus = port_type(task, moved.source,
                                                        &partialMoveType);
  struct MessageTwo partialMoveReply;
  const uint64_t partialMoveReceive = receive_two_ports(moved.destination,
      &partialMoveReply, sizeof(partialMoveReply), 1);

  const uint64_t partialCopySend = send_two_ports(copied.destination,
      copied.source, MACH_MSG_TYPE_COPY_SEND, invalidMoveSource,
      MACH_MSG_TYPE_MOVE_SEND);
  struct MessageTwo partialCopyReply;
  const uint64_t partialCopyReceive = receive_two_ports(copied.destination,
      &partialCopyReply, sizeof(partialCopyReply), 1);
  const int partialCopyRetainedSend = send_present(task, copied.source);

  printf("move=%llu pending=%d/%u receive=%llu descriptor=%d "
         "copy=%llu pending_send=%d receive=%llu descriptor=%d "
         "too_large=%llu/%llu dropped_send=%d retry=%llu "
         "bad_buffer=%llu restored_send=%d retry=%llu "
         "destroy_move=%llu/%d dropped_send=%d destroy_copy=%llu/%d kept_send=%d "
         "mixed=%llu pending=%d/%u+%d receive=%llu descriptors=%d "
         "three=%llu receive=%llu descriptors=%d truncated=%llu kept_first=%d receive=%llu "
         "malformed_second=%llu kept_first=%d receive=%llu "
         "partial_move=%llu pending=%d/%u receive=%llu "
         "partial_copy=%llu/%llu retained_send=%d\n",
      (unsigned long long)moveSend, pendingMoveStatus,
      (pendingMoveType & MACH_PORT_TYPE_SEND) != 0,
      (unsigned long long)moveReceive, moveDescriptor,
      (unsigned long long)copySend, pendingCopySend, (unsigned long long)copyReceive,
      copyDescriptor, (unsigned long long)tooLargeReceive,
      (unsigned long long)tooLargeSend, tooLargeDroppedSend,
      (unsigned long long)tooLargeRetry, (unsigned long long)badBufferReceive,
      badBufferRestoredSend, (unsigned long long)badBufferRetry,
      (unsigned long long)destroyMoveReceiver, destroyMoveReceiver,
      destroyedMoveDroppedSend, (unsigned long long)destroyCopyReceiver,
      destroyCopyReceiver, destroyedCopyKeptOriginalSend,
      (unsigned long long)mixedSend, pendingMixedMoveStatus,
      (pendingMixedMoveType & MACH_PORT_TYPE_SEND) != 0, pendingMixedCopySend,
      (unsigned long long)mixedReceive, mixedDescriptors,
      (unsigned long long)threeSend, (unsigned long long)threeReceive,
      threeDescriptors, (unsigned long long)truncatedSend, truncatedKeptFirstSend,
      (unsigned long long)truncatedReceive,
      (unsigned long long)malformedSecondSend, malformedSecondKeptFirstSend,
      (unsigned long long)malformedSecondReceive,
      (unsigned long long)partialMoveSend,
      partialMoveTypeStatus, (partialMoveType & MACH_PORT_TYPE_SEND) != 0,
      (unsigned long long)partialMoveReceive,
      (unsigned long long)partialCopySend, (unsigned long long)partialCopyReceive,
      partialCopyRetainedSend);

  const int cleanup =
      cleanup_port(task, moved.source,
                   mixedDescriptors && (partialMoveType & MACH_PORT_TYPE_SEND) == 0 ? 0 :
                       (moveDescriptor ? 1 : 0), 1) &&
      cleanup_port(task, moved.destination, 1, 1) &&
      cleanup_port(task, copied.source, copyDescriptor ? 4 : 1, 1) &&
      cleanup_port(task, copied.destination, 1, 1) &&
      cleanup_port(task, tooLarge.source, 0, 1) &&
      cleanup_port(task, tooLarge.destination, 1, 1) &&
      cleanup_port(task, badBuffer.source, badBufferRestoredSend ? 1 : 0, 1) &&
      cleanup_port(task, badBuffer.destination, 1, 1) &&
      cleanup_port(task, destroyedMove.source, 0, 1) &&
      cleanup_port(task, destroyedMove.destination, 1, 0) &&
      cleanup_port(task, destroyedCopy.source, 0, 1) &&
      cleanup_port(task, destroyedCopy.destination, 1, 0);
  printf("cleanup=%d\n", cleanup);
  const int expected = moveSend == MACH_MSG_SUCCESS && pendingMoveStatus == KERN_SUCCESS &&
      (pendingMoveType & MACH_PORT_TYPE_RECEIVE) != 0 &&
      (pendingMoveType & MACH_PORT_TYPE_SEND) == 0 && moveDescriptor &&
      copySend == MACH_MSG_SUCCESS && pendingCopySend && copyDescriptor &&
      tooLargeSend == MACH_MSG_SUCCESS && tooLargeReceive == MACH_RCV_TOO_LARGE &&
      tooLargeDroppedSend && tooLargeRetry == MACH_RCV_TIMED_OUT &&
      badBufferSend == MACH_MSG_SUCCESS && badBufferReceive == MACH_RCV_INVALID_DATA &&
      badBufferRestoredSend && badBufferRetry == MACH_RCV_TIMED_OUT &&
      destroyedMoveSend == MACH_MSG_SUCCESS && destroyMoveReceiver == KERN_SUCCESS &&
      destroyedMoveDroppedSend && destroyedCopySend == MACH_MSG_SUCCESS &&
      destroyCopyReceiver == KERN_SUCCESS && destroyedCopyKeptOriginalSend &&
      invalidSourcePrepared == KERN_SUCCESS &&
      mixedSend == MACH_MSG_SUCCESS && pendingMixedMoveStatus == KERN_SUCCESS &&
      (pendingMixedMoveType & MACH_PORT_TYPE_RECEIVE) != 0 &&
      (pendingMixedMoveType & MACH_PORT_TYPE_SEND) == 0 && pendingMixedCopySend &&
      mixedReceive == MACH_MSG_SUCCESS && mixedDescriptors &&
      threeSend == MACH_MSG_SUCCESS && threeReceive == MACH_MSG_SUCCESS && threeDescriptors &&
      truncatedSend == MACH_SEND_MSG_TOO_SMALL && truncatedKeptFirstSend &&
      truncatedReceive == MACH_RCV_TIMED_OUT &&
      malformedSecondSend == MACH_SEND_MSG_TOO_SMALL &&
      malformedSecondKeptFirstSend && malformedSecondReceive == MACH_RCV_TIMED_OUT &&
      partialMoveSend == MACH_SEND_INVALID_RIGHT &&
      partialMoveTypeStatus == KERN_SUCCESS &&
      (partialMoveType & MACH_PORT_TYPE_RECEIVE) != 0 &&
      (partialMoveType & MACH_PORT_TYPE_SEND) == 0 &&
      partialMoveReceive == MACH_RCV_TIMED_OUT &&
      partialCopySend == MACH_SEND_INVALID_RIGHT &&
      partialCopyReceive == MACH_RCV_TIMED_OUT && partialCopyRetainedSend && cleanup;
  return !expected;
}
