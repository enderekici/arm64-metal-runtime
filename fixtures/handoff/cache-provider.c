#include <zlib.h>
unsigned int guest_zlib_crc(void) {
  const unsigned char input[] = {'M', 'e', 't', 'a', 'l'};
  return (unsigned int)crc32(0, input, sizeof(input));
}
