#include "NLP.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int expect_status(NlpStatus actual, NlpStatus expected,
                         const char *operation) {
  if (actual == expected) {
    return 1;
  }

  fprintf(stderr, "FAIL: %s: expected %s, got %s\n", operation,
          nlp_status_name(expected), nlp_status_name(actual));

  return 0;
}

int main(void) {
  Arena arena;

  arena_init(&arena, 4096);

  NlpContext context = {0};

  NlpConfig config;

  nlp_config_default(&config);

  /*
   * For this development test we explicitly request CUDA.
   *
   * Later the release path can use NLP_EXEC_AUTO.
   */
  config.execution_provider = NLP_EXEC_CUDA;

  NlpStatus status = nlp_init(&context, &arena, &config);

  if (!expect_status(status, NLP_STATUS_OK, "nlp_init")) {

    arena_destroy(&arena);
    return 1;
  }

  status = nlp_load_model(&context, "models/distilbert_ner/model.onnx");

  if (!expect_status(status, NLP_STATUS_OK, "nlp_load_model")) {

    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  NlpModelInfo model_info = {0};

  status = nlp_get_model_info(&context, &model_info);

  if (!expect_status(status, NLP_STATUS_OK, "nlp_get_model_info")) {

    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  printf("Transformer model loaded.\n"
         "  inputs  = %zu\n"
         "  outputs = %zu\n",
         model_info.input_count, model_info.output_count);

  /*
   * DistilBERT/BERT vocabulary:
   *
   * [CLS]  = 101
   * hello   = 7592
   * world   = 2088
   * [SEP]  = 102
   */
  const int64_t input_ids[] = {101, 7592, 2088, 102};

  const int64_t attention_mask[] = {1, 1, 1, 1};

  NlpTokenInput input = {.input_ids = input_ids,
                         .attention_mask = attention_mask,
                         .sequence_length =
                             sizeof(input_ids) / sizeof(input_ids[0])};

  /*
   * The model has 9 token labels.
   *
   * 4 tokens × 9 labels = 36 logits.
   *
   * Give ourselves some extra capacity so the test isn't
   * coupled too tightly to that exact count.
   */

  float logits[1024] = {0};

  NlpTokenOutput output = {
      .logits = logits, .logits_capacity = sizeof(logits) / sizeof(logits[0])};

  status = nlp_infer_tokens(&context, &input, &output);

  if (!expect_status(status, NLP_STATUS_OK, "nlp_infer_tokens")) {

    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  printf("Inference succeeded.\n"
         "  sequence length = %zu\n"
         "  label count     = %zu\n"
         "  logits          = %zu\n",
         output.sequence_length, output.label_count, output.logits_count);

  if (output.sequence_length != 4) {
    fprintf(stderr, "FAIL: wrong sequence length\n");
    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  if (output.label_count == 0) {
    fprintf(stderr, "FAIL: zero labels\n");
    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  if (output.logits_count != output.sequence_length * output.label_count) {

    fprintf(stderr, "FAIL: unexpected logits count\n");
    nlp_free(&context);
    arena_destroy(&arena);
    return 1;
  }

  /*
   * Verify that we actually received finite floating-point output.
   */

  for (size_t i = 0; i < output.logits_count; ++i) {

    if (!isfinite(output.logits[i])) {
      fprintf(stderr, "FAIL: non-finite logit at %zu\n", i);

      nlp_free(&context);
      arena_destroy(&arena);
      return 1;
    }
  }

  /*
   * Print argmax label for every token.
   */

  for (size_t token = 0; token < output.sequence_length; ++token) {

    size_t best_label = 0;
    float best_value = output.logits[token * output.label_count];

    for (size_t label = 1; label < output.label_count; ++label) {

      float value = output.logits[token * output.label_count + label];

      if (value > best_value) {
        best_value = value;
        best_label = label;
      }
    }

    printf("  token[%zu] id=%lld -> label=%zu logit=%f\n", token,
           (long long)input_ids[token], best_label, best_value);
  }

  printf("\nNLP TRANSFORMER TEST: PASS\n");

  nlp_free(&context);
  arena_destroy(&arena);

  return 0;
}
