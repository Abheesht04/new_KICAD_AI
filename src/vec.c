#include "vec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VEC_INITIAL_CAPACITY 8

static int size_mul_overflow(size_t a, size_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }

    return a > SIZE_MAX / b;
}

static int size_add_overflow(size_t a, size_t b)
{
    return a > SIZE_MAX - b;
}

static int vec_resize(
    void **vector,
    size_t capacity,
    size_t element_size
)
{
    if (!vector || element_size == 0) {
        return -1;
    }

    if (size_mul_overflow(capacity, element_size)) {
        return -1;
    }

    size_t element_bytes = capacity * element_size;

    if (size_add_overflow(sizeof(VecHeader), element_bytes)) {
        return -1;
    }

    size_t total_bytes = sizeof(VecHeader) + element_bytes;

    void *allocation;

    if (*vector) {
        allocation = realloc(
            vec_header(*vector),
            total_bytes
        );
    } else {
        allocation = malloc(total_bytes);
    }

    if (!allocation) {
        return -1;
    }

    VecHeader *header = (VecHeader *)allocation;

    if (*vector == NULL) {
        header->length = 0;
    }

    header->capacity = capacity;

    *vector = (unsigned char *)allocation + sizeof(VecHeader);

    return 0;
}

int vec_reserve_impl(
    void **vector,
    size_t count,
    size_t element_size
)
{
    if (!vector || element_size == 0) {
        return -1;
    }

    if (*vector) {
        VecHeader *header = vec_header(*vector);

        if (header->capacity >= count) {
            return 0;
        }
    }

    size_t capacity = *vector ? vec_header(*vector)->capacity : 0;

    if (capacity == 0) {
        capacity = VEC_INITIAL_CAPACITY;
    }

    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }

        capacity *= 2;
    }

    return vec_resize(vector, capacity, element_size);
}

int vec_push_impl(
    void **vector,
    const void *element,
    size_t element_size
)
{
    if (!vector || !element || element_size == 0) {
        return -1;
    }

    size_t length = 0;
    size_t capacity = 0;

    if (*vector) {
        VecHeader *header = vec_header(*vector);
        length = header->length;
        capacity = header->capacity;
    }

    if (length == capacity) {
        size_t new_capacity;

        if (capacity == 0) {
            new_capacity = VEC_INITIAL_CAPACITY;
        } else {
            if (capacity > SIZE_MAX / 2) {
                return -1;
            }

            new_capacity = capacity * 2;
        }

        if (vec_resize(vector, new_capacity, element_size) != 0) {
            return -1;
        }
    }

    VecHeader *header = vec_header(*vector);

    unsigned char *destination =
        (unsigned char *)(*vector) +
        header->length * element_size;

    memcpy(destination, element, element_size);

    header->length++;

    return 0;
}

void vec_free_impl(void **vector)
{
    if (!vector || !*vector) {
        return;
    }

    free(vec_header(*vector));
    *vector = NULL;
}
