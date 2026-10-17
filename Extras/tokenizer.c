#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKENIZER_INITIAL_CAPACITY 32768
#define TOKENIZER_MAX_WORD_BYTES 128

static uint64_t tokenizer_hash(const char *text) {
  uint64_t hash = 1469598103934665603ULL;

  while (*text) {
    hash ^= (unsigned char)*text;
    hash *= 1099511628211ULL;
    ++text;
  }

  return hash;
}

static size_t next_power_of_two(size_t value) {
  size_t result = 1;

  while (result < value) {
    result *= 2;
  }

  return result;
}

static int tokenizer_lookup(const NlpTokenizer *tokenizer, const char *text,
                            int64_t *out_id) {
  if (!tokenizer || !text || !out_id || tokenizer->slot_count == 0) {
    return 0;
  }

  size_t mask = tokenizer->slot_count - 1;
  size_t slot = (size_t)tokenizer_hash(text) & mask;

  for (;;) {
    uint32_t entry = tokenizer->slots[slot];

    if (entry == 0) {
      return 0;
    }

    size_t id = (size_t)entry - 1;

    if (strcmp(tokenizer->vocab[id], text) == 0) {
      *out_id = (int64_t)id;
      return 1;
    }

    slot = (slot + 1) & mask;
  }
}

int nlp_tokenizer_load(NlpTokenizer *tokenizer, const char *vocab_path) {
  if (!tokenizer || !vocab_path) {
    return 0;
  }

  memset(tokenizer, 0, sizeof(*tokenizer));

  FILE *file = fopen(vocab_path, "rb");

  if (!file) {
    return 0;
  }

  size_t capacity = TOKENIZER_INITIAL_CAPACITY;

  tokenizer->vocab = calloc(capacity, sizeof(*tokenizer->vocab));

  if (!tokenizer->vocab) {
    fclose(file);
    return 0;
  }

  char line[1024];

  while (fgets(line, sizeof(line), file)) {
    size_t length = strlen(line);

    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }

    if (tokenizer->vocab_count == capacity) {
      size_t new_capacity = capacity * 2;

      char **new_vocab =
          realloc(tokenizer->vocab, new_capacity * sizeof(*new_vocab));

      if (!new_vocab) {
        fclose(file);
        nlp_tokenizer_free(tokenizer);
        return 0;
      }

      tokenizer->vocab = new_vocab;
      capacity = new_capacity;
    }

    char *token = malloc(length + 1);

    if (!token) {
      fclose(file);
      nlp_tokenizer_free(tokenizer);
      return 0;
    }

    memcpy(token, line, length + 1);

    tokenizer->vocab[tokenizer->vocab_count++] = token;
  }

  fclose(file);

  tokenizer->slot_count = next_power_of_two(tokenizer->vocab_count * 2);

  tokenizer->slots = calloc(tokenizer->slot_count, sizeof(*tokenizer->slots));

  if (!tokenizer->slots) {
    nlp_tokenizer_free(tokenizer);
    return 0;
  }

  size_t mask = tokenizer->slot_count - 1;

  for (size_t i = 0; i < tokenizer->vocab_count; ++i) {
    size_t slot = (size_t)tokenizer_hash(tokenizer->vocab[i]) & mask;

    while (tokenizer->slots[slot] != 0) {
      slot = (slot + 1) & mask;
    }

    tokenizer->slots[slot] = (uint32_t)i + 1;
  }

  if (!tokenizer_lookup(tokenizer, "[UNK]", &tokenizer->unk_id) ||
      !tokenizer_lookup(tokenizer, "[CLS]", &tokenizer->cls_id) ||
      !tokenizer_lookup(tokenizer, "[SEP]", &tokenizer->sep_id)) {

    nlp_tokenizer_free(tokenizer);
    return 0;
  }

  return 1;
}

void nlp_tokenizer_free(NlpTokenizer *tokenizer) {
  if (!tokenizer) {
    return;
  }

  if (tokenizer->vocab) {
    for (size_t i = 0; i < tokenizer->vocab_count; ++i) {
      free(tokenizer->vocab[i]);
    }

    free(tokenizer->vocab);
  }

  free(tokenizer->slots);

  memset(tokenizer, 0, sizeof(*tokenizer));
}

static int is_whitespace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_punctuation(unsigned char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
         (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static unsigned char ascii_lower(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return (unsigned char)(c + ('a' - 'A'));
  }

  return c;
}

static int token_push(NlpTokenizedText *output, int64_t id, size_t start,
                      size_t length) {
  if (output->count >= output->capacity) {
    return 0;
  }

  output->input_ids[output->count] = id;
  output->attention_mask[output->count] = 1;

  output->tokens[output->count].id = id;
  output->tokens[output->count].start = start;
  output->tokens[output->count].length = length;

  output->count++;

  return 1;
}

static int wordpiece_encode(const NlpTokenizer *tokenizer, const char *text,
                            size_t start, size_t length,
                            NlpTokenizedText *output) {
  if (length == 0 || length >= TOKENIZER_MAX_WORD_BYTES) {
    return token_push(output, tokenizer->unk_id, start, length);
  }

  char word[TOKENIZER_MAX_WORD_BYTES];

  for (size_t i = 0; i < length; ++i) {
    word[i] = (char)ascii_lower((unsigned char)text[start + i]);
  }

  word[length] = '\0';

  size_t position = 0;

  while (position < length) {
    size_t remaining = length - position;
    size_t best_length = 0;
    int64_t best_id = -1;

    for (size_t candidate_length = remaining; candidate_length > 0;
         --candidate_length) {

      char candidate[TOKENIZER_MAX_WORD_BYTES];

      size_t prefix = position == 0 ? 0 : 2;

      if (prefix + candidate_length + 1 >= sizeof(candidate)) {
        continue;
      }

      size_t out = 0;

      if (position != 0) {
        candidate[out++] = '#';
        candidate[out++] = '#';
      }

      memcpy(candidate + out, word + position, candidate_length);

      out += candidate_length;
      candidate[out] = '\0';

      int64_t id;

      if (tokenizer_lookup(tokenizer, candidate, &id)) {
        best_length = candidate_length;
        best_id = id;
        break;
      }
    }

    if (best_length == 0) {
      return token_push(output, tokenizer->unk_id, start, length);
    }

    if (!token_push(output, best_id, start + position, best_length)) {
      return 0;
    }

    position += best_length;
  }

  return 1;
}

int nlp_tokenizer_encode(const NlpTokenizer *tokenizer, const char *text,
                         NlpTokenizedText *output) {
  if (!tokenizer || !text || !output || !output->input_ids ||
      !output->attention_mask || !output->tokens || output->capacity < 2) {
    return 0;
  }

  output->count = 0;

  if (!token_push(output, tokenizer->cls_id, 0, 0)) {
    return 0;
  }

  size_t position = 0;
  size_t text_length = strlen(text);

  while (position < text_length) {
    while (position < text_length &&
           is_whitespace((unsigned char)text[position])) {
      ++position;
    }

    if (position >= text_length) {
      break;
    }

    if (is_punctuation((unsigned char)text[position])) {
      char punctuation[2];

      punctuation[0] = (char)ascii_lower((unsigned char)text[position]);
      punctuation[1] = '\0';

      int64_t id;

      if (tokenizer_lookup(tokenizer, punctuation, &id)) {

        if (!token_push(output, id, position, 1)) {
          return 0;
        }
      } else {
        if (!token_push(output, tokenizer->unk_id, position, 1)) {
          return 0;
        }
      }

      ++position;
      continue;
    }

    size_t word_start = position;

    while (position < text_length &&
           !is_whitespace((unsigned char)text[position]) &&
           !is_punctuation((unsigned char)text[position])) {
      ++position;
    }

    if (!wordpiece_encode(tokenizer, text, word_start, position - word_start,
                          output)) {
      return 0;
    }
  }

  if (!token_push(output, tokenizer->sep_id, text_length, 0)) {
    return 0;
  }

  return 1;
}
