#ifndef VEC_H
#define VEC_H

#include <stddef.h>

/*
 * Hidden header immediately preceding the user array.
 * The union alignment member aligns the payload for the platform's
 * fundamental scalar types without depending on MSVC's max_align_t spelling.
 */

typedef union {
  struct {
    size_t length;
    size_t capacity;
  };
  long double alignment;
} VecHeader;

#define vec_header(v) ((VecHeader *)((unsigned char *)(v) - sizeof(VecHeader)))

#define vec_len(v) ((v) ? vec_header(v)->length : 0)

#define vec_cap(v) ((v) ? vec_header(v)->capacity : 0)

#define vec_push(v, value)                                                     \
  vec_push_impl((void **)&(v), &(value), sizeof((v)[0]))

#define vec_reserve(v, count)                                                  \
  vec_reserve_impl((void **)&(v), (count), sizeof((v)[0]))

#define vec_clear(v)                                                           \
  do {                                                                         \
    if (v) {                                                                   \
      vec_header(v)->length = 0;                                               \
    }                                                                          \
  } while (0)

#define vec_free(v) vec_free_impl((void **)&(v))

int vec_push_impl(void **vector, const void *element, size_t element_size);
int vec_reserve_impl(void **vector, size_t count, size_t element_size);
void vec_free_impl(void **vector);

#endif
