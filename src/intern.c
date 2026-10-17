#include "intern.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INTERN_INITIAL_CAPACITY 16
#define INTERN_MAX_LOAD_NUM 7
#define INTERN_MAX_LOAD_DEN 10
#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t hash_string(const char *text, size_t length)
{
    uint64_t hash = FNV_OFFSET_BASIS;

    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

static size_t table_index(uint64_t hash, size_t capacity)
{
    return (size_t)(hash & (capacity - 1));
}

static int needs_resize(const Interner *interner)
{
    return
        (interner->count + 1) * INTERN_MAX_LOAD_DEN >=
        interner->capacity * INTERN_MAX_LOAD_NUM;
}

static size_t find_slot(
    const Interner *interner,
    const char *text,
    size_t length,
    uint64_t hash,
    int *found
)
{
    if (!interner || !interner->entries ||
        interner->capacity == 0 || !found) {
        return SIZE_MAX;
    }

    size_t index = table_index(hash, interner->capacity);

    for (size_t probes = 0; probes < interner->capacity; ++probes) {
        const InternEntry *entry = &interner->entries[index];

        if (!entry->text) {
            *found = 0;
            return index;
        }

        if (entry->hash == hash &&
            entry->length == length &&
            memcmp(entry->text, text, length) == 0) {
            *found = 1;
            return index;
        }

        index = (index + 1) & (interner->capacity - 1);
    }

    return SIZE_MAX;
}

static int interner_resize(Interner *interner, size_t new_capacity)
{
    if (!interner || new_capacity == 0 ||
        (new_capacity & (new_capacity - 1)) != 0) {
        return -1;
    }

    InternEntry *new_entries = calloc(
        new_capacity,
        sizeof(InternEntry)
    );

    if (!new_entries) {
        return -1;
    }

    for (size_t i = 0; i < interner->capacity; ++i) {
        InternEntry *old = &interner->entries[i];

        if (!old->text) {
            continue;
        }

        size_t index = table_index(old->hash, new_capacity);

        while (new_entries[index].text) {
            index = (index + 1) & (new_capacity - 1);
        }

        new_entries[index] = *old;
    }

    free(interner->entries);
    interner->entries = new_entries;
    interner->capacity = new_capacity;

    return 0;
}

int interner_init(Interner *interner, size_t initial_capacity)
{
    if (!interner) {
        return -1;
    }

    if (initial_capacity == 0) {
        initial_capacity = INTERN_INITIAL_CAPACITY;
    }

    if ((initial_capacity & (initial_capacity - 1)) != 0) {
        return -1;
    }

    interner->entries = calloc(
        initial_capacity,
        sizeof(InternEntry)
    );

    if (!interner->entries) {
        return -1;
    }

    interner->capacity = initial_capacity;
    interner->count = 0;

    return 0;
}

void interner_destroy(Interner *interner)
{
    if (!interner) {
        return;
    }

    if (interner->entries) {
        for (size_t i = 0; i < interner->capacity; ++i) {
            free((void *)interner->entries[i].text);
        }
    }

    free(interner->entries);

    interner->entries = NULL;
    interner->capacity = 0;
    interner->count = 0;
}

const char *intern_string_n(
    Interner *interner,
    const char *text,
    size_t length
)
{
    if (!interner || !text || !interner->entries ||
        interner->capacity == 0 || length == SIZE_MAX || length > SIZE_MAX - 1) {
        return NULL;
    }

    uint64_t hash = hash_string(text, length);

    if (needs_resize(interner)) {
        if (interner->capacity > SIZE_MAX / 2) {
            return NULL;
        }

        if (interner_resize(interner, interner->capacity * 2) != 0) {
            return NULL;
        }
    }

    int found = 0;
    size_t index = find_slot(
        interner,
        text,
        length,
        hash,
        &found
    );

    if (index == SIZE_MAX) {
        return NULL;
    }

    if (found) {
        return interner->entries[index].text;
    }

    char *copy = malloc(length + 1);

    if (!copy) {
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';

    interner->entries[index].text = copy;
    interner->entries[index].length = length;
    interner->entries[index].hash = hash;

    interner->count++;

    return copy;
}

const char *intern_string(Interner *interner, const char *text)
{
    if (!text) {
        return NULL;
    }

    return intern_string_n(interner, text, strlen(text));
}

size_t interner_count(const Interner *interner)
{
    return interner ? interner->count : 0;
}
