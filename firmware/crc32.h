#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

// CRC-32 as in zlib/PNG/Ethernet (reflected, polynomial 0xEDB88320, initial
// and final XOR 0xFFFFFFFF): crc32_update(0, p, n) == zlib.crc32(p).  Pass
// the previous result back in to continue over more data.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif
