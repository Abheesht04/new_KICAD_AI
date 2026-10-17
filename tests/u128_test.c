#include "u128.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    U128 r = u128_mul(UINT64_MAX, UINT64_MAX); /* (2^64-1)^2 = 2^128 - 2^65 + 1 */
    assert(r.hi == 0xFFFFFFFFFFFFFFFEull && r.lo == 1ull);
    r = u128_mul(0, 12345);          assert(r.hi == 0 && r.lo == 0);
    r = u128_mul(1ull << 32, 1ull << 32); assert(r.hi == 1 && r.lo == 0);
    r = u128_mul(3, 5);              assert(r.hi == 0 && r.lo == 15);
    assert(u128_cmp(u128_mul(7, 9), u128_mul(9, 7)) == 0);
    assert(u128_cmp(u128_mul(1ull << 40, 1ull << 40), u128_mul(UINT64_MAX, 1)) == 1);
    puts("u128 TEST SUITE: PASS");
    return 0;
}
