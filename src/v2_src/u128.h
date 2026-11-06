#pragma once
#include <stdint.h>

typedef struct {
  uint64_t hi, lo;
} U128;

U128 u128_mul(uint64_t a, uint64_t b);
int u128_cmp(U128 a, U128 b);
