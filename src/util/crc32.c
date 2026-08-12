#include <pthread.h>

#include "crc32.h"

static unsigned long table[256];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void
build_table(void) {
  unsigned long c;
  int n, k;

  for (n = 0; n < 256; n++) {
    c = (unsigned long)n;
    for (k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
    }
    table[n] = c;
  }
}

unsigned long
crc32_init(void) {
  pthread_once(&table_once, build_table);
  return 0xFFFFFFFFUL;
}

unsigned long
crc32_update(unsigned long crc, const unsigned char *data, size_t len) {
  size_t i;

  pthread_once(&table_once, build_table);

  for (i = 0; i < len; i++) {
    crc = table[(crc ^ data[i]) & 0xFFUL] ^ (crc >> 8);
  }

  return crc;
}

unsigned long
crc32_final(unsigned long crc) {
  return crc ^ 0xFFFFFFFFUL;
}
