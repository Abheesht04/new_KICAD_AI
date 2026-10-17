#ifndef NLP_TOKENIZER_H
#define NLP_TOKENIZER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  char **vocab;
  size_t vocab_count;

  uint32_t *slots;
  size_t slot_count;

  int64_t unk_id;
  int64_t cls_id;
  int64_t sep_id;
} NlpTokenizer;

typedef struct {
  int64_t id;

  size_t start;
  size_t length;
} NlpToken;

typedef struct {
  int64_t *input_ids;
  int64_t *attention_mask;

  NlpToken *tokens;

  size_t count;
  size_t capacity;
} NlpTokenizedText;

int nlp_tokenizer_load(NlpTokenizer *tokenizer, const char *vocab_path);

void nlp_tokenizer_free(NlpTokenizer *tokenizer);

int nlp_tokenizer_encode(const NlpTokenizer *tokenizer, const char *text,
                         NlpTokenizedText *output);

#endif
