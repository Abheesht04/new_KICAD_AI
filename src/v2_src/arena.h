// arena.h
#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArenaChunk {
  size_t capacity;
  size_t offset;
  uint8_t *data;
  struct ArenaChunk *next;
} ArenaChunk;

typedef struct {
  ArenaChunk *head;
  ArenaChunk *current;
  size_t default_chunk_size;
} Arena;

void arena_init(Arena *a, size_t chunk_size);
void arena_reset(Arena *a);
void arena_destroy(Arena *a);
void *arena_alloc(Arena *a, size_t size);
void *arena_alloc_array(Arena *a, size_t elem_size, size_t count);

#endif
