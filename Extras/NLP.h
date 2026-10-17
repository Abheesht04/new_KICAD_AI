#ifndef NLP_H
#define NLP_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

/*
 * --------------------------------------------------------------------------
 * Execution provider
 * --------------------------------------------------------------------------
 */

typedef enum {
  NLP_EXEC_AUTO = 0,
  NLP_EXEC_CPU,
  NLP_EXEC_CUDA
} NlpExecutionProvider;

/*
 * --------------------------------------------------------------------------
 * NLP entities
 *
 * These describe language.
 *
 * They are deliberately NOT ComponentKind.
 * NLP must not know what a resistor electrically means.
 * --------------------------------------------------------------------------
 */

typedef enum {
  NLP_ENTITY_UNKNOWN = 0,

  NLP_ENTITY_COMPONENT,
  NLP_ENTITY_VALUE,
  NLP_ENTITY_UNIT,
  NLP_ENTITY_QUANTITY,

  NLP_ENTITY_TOPOLOGY,
  NLP_ENTITY_MEASUREMENT,
  NLP_ENTITY_NODE_REFERENCE,

  NLP_ENTITY_ACTION
} NlpEntityType;

/*
 * --------------------------------------------------------------------------
 * NLP relations
 * --------------------------------------------------------------------------
 */

typedef enum {
  NLP_RELATION_UNKNOWN = 0,

  NLP_RELATION_VALUE_OF,
  NLP_RELATION_UNIT_OF,
  NLP_RELATION_QUANTITY_OF,

  NLP_RELATION_CONNECTS,
  NLP_RELATION_MEASURES,
  NLP_RELATION_MODIFIES,
  NLP_RELATION_ASSOCIATED_WITH
} NlpRelationType;

/*
 * --------------------------------------------------------------------------
 * Entity span
 *
 * The entity points back into the original prompt.
 * No string copy is required.
 * --------------------------------------------------------------------------
 */

typedef struct {
  size_t start;
  size_t length;

  NlpEntityType type;

  double confidence;
} NlpEntity;

/*
 * --------------------------------------------------------------------------
 * Relation
 *
 * source_entity and target_entity index the NlpResult entity array.
 * --------------------------------------------------------------------------
 */

typedef struct {
  size_t source_entity;
  size_t target_entity;

  NlpRelationType type;

  double confidence;
} NlpRelation;

/*
 * --------------------------------------------------------------------------
 * Analysis result
 *
 * Storage is caller-owned.
 * NLP only fills the supplied arrays.
 * --------------------------------------------------------------------------
 */

typedef struct {
  const char *input;
  size_t input_length;

  NlpEntity *entities;
  size_t entity_capacity;
  size_t entity_count;

  NlpRelation *relations;
  size_t relation_capacity;
  size_t relation_count;
} NlpResult;

/*
 * --------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------
 */

typedef struct {
  NlpExecutionProvider execution_provider;

  uint32_t device_id;
} NlpConfig;

/*
 * --------------------------------------------------------------------------
 * Status
 * --------------------------------------------------------------------------
 */

typedef enum {
  NLP_STATUS_OK = 0,

  NLP_STATUS_INVALID_ARGUMENT,
  NLP_STATUS_ALLOCATION_FAILURE,
  NLP_STATUS_RUNTIME_FAILURE,

  NLP_STATUS_MODEL_NOT_LOADED,
  NLP_STATUS_NOT_IMPLEMENTED,

  NLP_STATUS_OUTPUT_CAPACITY
} NlpStatus;

/*
 * --------------------------------------------------------------------------
 * Context
 *
 * Implementation is private to NLP.c.
 * --------------------------------------------------------------------------
 */

typedef struct {
  void *impl;
} NlpContext;

/*
 * --------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------
 */

typedef struct {
  size_t input_count;
  size_t output_count;
} NlpModelInfo;

/*
 * --------------------------------------------------------------------------
 * Transformer input/output
 * --------------------------------------------------------------------------
 *
 * This is intentionally below the text-level NLP API.
 *
 * nlp_analyze():
 *     English text -> tokenizer -> transformer -> NLP result
 *
 * nlp_infer_tokens():
 *     token IDs -> transformer -> logits
 *
 * The latter lets us validate the Transformer independently before
 * introducing tokenization.
 * --------------------------------------------------------------------------
 */

typedef struct {
  const int64_t *input_ids;
  const int64_t *attention_mask;

  size_t sequence_length;
} NlpTokenInput;

typedef struct {
  float *logits;
  size_t logits_capacity;
  size_t logits_count;

  size_t sequence_length;
  size_t label_count;
} NlpTokenOutput;

NlpStatus nlp_infer_tokens(NlpContext *context, const NlpTokenInput *input,
                           NlpTokenOutput *output);

NlpStatus nlp_get_model_info(NlpContext *context, NlpModelInfo *info);

void nlp_config_default(NlpConfig *config);

NlpStatus nlp_init(NlpContext *context, Arena *arena, const NlpConfig *config);

void nlp_free(NlpContext *context);

/*
 * --------------------------------------------------------------------------
 * Model
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_load_model(NlpContext *context, const char *model_path);

/*
 * --------------------------------------------------------------------------
 * Analysis
 *
 * This will eventually perform:
 *
 *     Pass 1: contextual language representation
 *     Pass 2: entity/relation extraction
 *
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_analyze(NlpContext *context, const char *text, NlpResult *result);

/*
 * --------------------------------------------------------------------------
 * Names
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_get_model_info(NlpContext *context, NlpModelInfo *info);

const char *nlp_status_name(NlpStatus status);

const char *nlp_entity_type_name(NlpEntityType type);

const char *nlp_relation_type_name(NlpRelationType type);

#endif
