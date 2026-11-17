#include "arena.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>

static int size_mul_overflow(size_t a, size_t b) {
  return b != 0 && a > SIZE_MAX / b;
}

static int size_add_overflow(size_t a, size_t b) { return a > SIZE_MAX - b; }

static int align_up(size_t value, size_t alignment, size_t *out) {
  if (!out || alignment == 0 || (alignment & (alignment - 1)) != 0) {
    return 0;
  }

  size_t mask = alignment - 1;

  if (value > SIZE_MAX - mask) {
    return 0;
  }

  *out = (value + mask) & ~mask;
  return 1;
}

static ArenaChunk *allocate_chunk(size_t capacity) {
  if (capacity == 0) {
    return NULL;
  }

  ArenaChunk *chunk = malloc(sizeof(*chunk));

  if (!chunk) {
    return NULL;
  }

  chunk->data = malloc(capacity);

  if (!chunk->data) {
    free(chunk);
    return NULL;
  }

  chunk->capacity = capacity;
  chunk->offset = 0;
  chunk->next = NULL;
  return chunk;
}

void arena_init(Arena *a, size_t chunk_size) {
  if (!a) {
    return;
  }

  a->head = NULL;
  a->current = NULL;
  a->default_chunk_size = chunk_size;

  if (chunk_size != 0) {
    ArenaChunk *chunk = allocate_chunk(chunk_size);

    if (chunk) {
      a->head = chunk;
      a->current = chunk;
    }
  }
}

void arena_reset(Arena *a) {
  if (!a) {
    return;
  }

  for (ArenaChunk *chunk = a->head; chunk; chunk = chunk->next) {
    chunk->offset = 0;
  }

  a->current = a->head;
}

void arena_destroy(Arena *a) {
  if (!a) {
    return;
  }

  ArenaChunk *chunk = a->head;

  while (chunk) {
    ArenaChunk *next = chunk->next;
    free(chunk->data);
    free(chunk);
    chunk = next;
  }

  a->head = NULL;
  a->current = NULL;
  a->default_chunk_size = 0;
}

void *arena_alloc_array(Arena *a, size_t elem_size, size_t count) {
  if (size_mul_overflow(elem_size, count)) {
    return NULL;
  }

  return arena_alloc(a, elem_size * count);
}

void *arena_alloc(Arena *a, size_t size) {
  if (!a || !a->current || size == 0) {
    return NULL;
  }

  const size_t alignment = alignof(long double);

  for (;;) {
    size_t aligned_offset;

    if (!align_up(a->current->offset, alignment, &aligned_offset)) {
      return NULL;
    }

    if (!size_add_overflow(aligned_offset, size) &&
        aligned_offset + size <= a->current->capacity) {

      void *ptr = a->current->data + aligned_offset;
      a->current->offset = aligned_offset + size;
      return ptr;
    }

    if (a->current->next) {
      a->current = a->current->next;
      continue;
    }

    size_t new_capacity = a->default_chunk_size;

    if (new_capacity < size) {
      new_capacity = size;
    }

    if (new_capacity == 0) {
      return NULL;
    }

    ArenaChunk *new_chunk = allocate_chunk(new_capacity);

    if (!new_chunk) {
      return NULL;
    }

    a->current->next = new_chunk;
    a->current = new_chunk;
  }
}
