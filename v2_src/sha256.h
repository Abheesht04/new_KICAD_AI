#ifndef COPPER_SHA256_H
#define COPPER_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FsSha256 {
  uint32_t state[8];
  uint64_t bit_count;
  unsigned char block[64];
  size_t block_used;
} FsSha256;

void fs_sha256_init(FsSha256 *context);
void fs_sha256_update(FsSha256 *context, const void *data, size_t size);
void fs_sha256_final(FsSha256 *context, unsigned char digest[32]);
void fs_sha256_digest(const void *data, size_t size, unsigned char digest[32]);

#ifdef __cplusplus
}
#endif
#endif
