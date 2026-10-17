#ifndef INTERN_H
#define INTERN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *text;
    size_t length;
    uint64_t hash;
} InternEntry;

typedef struct {
    InternEntry *entries;
    size_t capacity;
    size_t count;
} Interner;

int interner_init(Interner *interner, size_t initial_capacity);
void interner_destroy(Interner *interner);

const char *intern_string(
    Interner *interner,
    const char *text
);

const char *intern_string_n(
    Interner *interner,
    const char *text,
    size_t length
);

size_t interner_count(const Interner *interner);

#endif
