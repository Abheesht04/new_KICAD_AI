#include "u128.h"

/*
 * u128_mul - long multiplication, but in "digits" of 32 bits.
 *
 * Remember school multiplication: 47 * 38 is split into tens and ones:
 *     (4*10 + 7) * (3*10 + 8)
 * and four small products are added with the right place values.
 *
 * Here the "digit" is 32 bits, so each number has two digits:
 *     a = a_hi * 2^32 + a_lo
 *     b = b_hi * 2^32 + b_lo
 *
 *     a*b = a_hi*b_hi * 2^64
 *         + (a_hi*b_lo + a_lo*b_hi) * 2^32
 *         + a_lo*b_lo
 *
 * Each of the four small products is (32 bits) x (32 bits), which is at most
 * 2^64 - 2^33 + 1, so it fits in a uint64_t. No overflow in the small steps.
 */
U128 u128_mul(uint64_t a, uint64_t b) {
  const uint64_t MASK32 = 0xFFFFFFFFu;

  const uint64_t a_lo = a & MASK32;
  const uint64_t a_hi = a >> 32;
  const uint64_t b_lo = b & MASK32;
  const uint64_t b_hi = b >> 32;

  const uint64_t ll = a_lo * b_lo; /* weight 2^0  */
  const uint64_t lh = a_lo * b_hi; /* weight 2^32 */
  const uint64_t hl = a_hi * b_lo; /* weight 2^32 */
  const uint64_t hh = a_hi * b_hi; /* weight 2^64 */

  /*
   * Add up everything that lands in bits 32..95. That is:
   *   - the top half of ll
   *   - the bottom half of lh
   *   - the bottom half of hl
   * Each term is below 2^32, so the sum is below 3 * 2^32 and cannot
   * overflow a uint64_t. Its top part (mid >> 32) is the carry into `hi`.
   */
  const uint64_t mid = (ll >> 32) + (lh & MASK32) + (hl & MASK32);

  U128 result;
  result.lo = (mid << 32) | (ll & MASK32);
  result.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  return result;
}

int u128_cmp(U128 a, U128 b) {
  /* The high half decides first; only if equal does the low half matter. */
  if (a.hi != b.hi)
    return a.hi < b.hi ? -1 : 1;
  if (a.lo != b.lo)
    return a.lo < b.lo ? -1 : 1;
  return 0;
}
