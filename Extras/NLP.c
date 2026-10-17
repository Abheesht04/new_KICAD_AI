#include "NLP.h"

#include <stdio.h>
#include <string.h>

#include <onnxruntime_c_api.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * --------------------------------------------------------------------------
 * Internal implementation
 * --------------------------------------------------------------------------
 */

typedef struct {
  const OrtApi *api;

  OrtEnv *env;
  OrtSession *session;

  NlpExecutionProvider execution_provider;

  uint32_t device_id;

} NlpImpl;

/*
 * --------------------------------------------------------------------------
 * ORT error handling
 * --------------------------------------------------------------------------
 */

static NlpStatus nlp_ort_error(NlpImpl *impl, OrtStatus *status) {
  if (status == NULL) {
    return NLP_STATUS_OK;
  }

  if (impl != NULL && impl->api != NULL) {
    fprintf(stderr, "ONNX Runtime error: %s\n",
            impl->api->GetErrorMessage(status));

    impl->api->ReleaseStatus(status);
  }

  return NLP_STATUS_RUNTIME_FAILURE;
}

/*
 * --------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------
 */

void nlp_config_default(NlpConfig *config) {
  if (config == NULL) {
    return;
  }

  config->execution_provider = NLP_EXEC_AUTO;
  config->device_id = 0;
}

/*
 * --------------------------------------------------------------------------
 * Session options
 * --------------------------------------------------------------------------
 */

static OrtSessionOptions *nlp_create_cpu_options(NlpImpl *impl) {
  OrtSessionOptions *options = NULL;

  OrtStatus *status = impl->api->CreateSessionOptions(&options);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    return NULL;
  }

  return options;
}

static OrtSessionOptions *nlp_create_cuda_options(NlpImpl *impl) {
  OrtSessionOptions *options = NULL;
  OrtCUDAProviderOptionsV2 *cuda_options = NULL;

  OrtStatus *status = impl->api->CreateSessionOptions(&options);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    return NULL;
  }

  status = impl->api->CreateCUDAProviderOptions(&cuda_options);

  if (status != NULL) {
    nlp_ort_error(impl, status);

    impl->api->ReleaseSessionOptions(options);

    return NULL;
  }

  char device_id[32];

  snprintf(device_id, sizeof(device_id), "%u", impl->device_id);

  const char *keys[] = {"device_id"};

  const char *values[] = {device_id};

  status = impl->api->UpdateCUDAProviderOptions(cuda_options, keys, values, 1);

  if (status != NULL) {
    nlp_ort_error(impl, status);

    impl->api->ReleaseCUDAProviderOptions(cuda_options);

    impl->api->ReleaseSessionOptions(options);

    return NULL;
  }

  status = impl->api->SessionOptionsAppendExecutionProvider_CUDA_V2(
      options, cuda_options);

  impl->api->ReleaseCUDAProviderOptions(cuda_options);

  if (status != NULL) {
    nlp_ort_error(impl, status);

    impl->api->ReleaseSessionOptions(options);

    return NULL;
  }

  return options;
}

/*
 * --------------------------------------------------------------------------
 * Windows model-path conversion
 *
 * NLP API remains UTF-8 / char*.
 * ORT receives ORTCHAR_T.
 * --------------------------------------------------------------------------
 */

#ifdef _WIN32

static wchar_t *nlp_utf8_to_wide(const char *text) {
  if (text == NULL) {
    return NULL;
  }

  int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);

  if (required <= 0) {
    return NULL;
  }

  wchar_t *result =
      HeapAlloc(GetProcessHeap(), 0, (SIZE_T)required * sizeof(wchar_t));

  if (result == NULL) {
    return NULL;
  }

  int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, result, required);

  if (written <= 0) {
    HeapFree(GetProcessHeap(), 0, result);

    return NULL;
  }

  return result;
}

#endif

/*
 * --------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_init(NlpContext *context, Arena *arena, const NlpConfig *config) {
  if (context == NULL || arena == NULL) {
    return NLP_STATUS_INVALID_ARGUMENT;
  }

  context->impl = NULL;

  NlpConfig local_config;

  if (config == NULL) {
    nlp_config_default(&local_config);
    config = &local_config;
  }

  NlpImpl *impl = arena_alloc(arena, sizeof(*impl));

  if (impl == NULL) {
    return NLP_STATUS_ALLOCATION_FAILURE;
  }

  memset(impl, 0, sizeof(*impl));

  const OrtApiBase *base = OrtGetApiBase();

  if (base == NULL) {
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  impl->api = base->GetApi(ORT_API_VERSION);

  if (impl->api == NULL) {
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  OrtStatus *status = impl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                           "electronics_vendor", &impl->env);

  if (status != NULL) {
    nlp_ort_error(impl, status);

    return NLP_STATUS_RUNTIME_FAILURE;
  }

  impl->execution_provider = config->execution_provider;

  impl->device_id = config->device_id;

  context->impl = impl;

  return NLP_STATUS_OK;
}

/*
 * --------------------------------------------------------------------------
 * Load model
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_load_model(NlpContext *context, const char *model_path) {
  if (context == NULL || context->impl == NULL || model_path == NULL) {

    return NLP_STATUS_INVALID_ARGUMENT;
  }

  NlpImpl *impl = (NlpImpl *)context->impl;

  /*
   * Replace an existing model cleanly.
   */

  if (impl->session != NULL) {
    impl->api->ReleaseSession(impl->session);

    impl->session = NULL;
  }

  OrtSessionOptions *options = NULL;

  if (impl->execution_provider == NLP_EXEC_CPU) {

    options = nlp_create_cpu_options(impl);

  } else {

    options = nlp_create_cuda_options(impl);

    /*
     * AUTO:
     *
     * CUDA unavailable → CPU.
     */
    if (options == NULL && impl->execution_provider == NLP_EXEC_AUTO) {

      options = nlp_create_cpu_options(impl);
    }
  }

  if (options == NULL) {
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  OrtStatus *status = NULL;

#ifdef _WIN32

  wchar_t *wide_path = nlp_utf8_to_wide(model_path);

  if (wide_path == NULL) {
    impl->api->ReleaseSessionOptions(options);

    return NLP_STATUS_ALLOCATION_FAILURE;
  }

  status =
      impl->api->CreateSession(impl->env, wide_path, options, &impl->session);

  HeapFree(GetProcessHeap(), 0, wide_path);

#else

  status =
      impl->api->CreateSession(impl->env, model_path, options, &impl->session);

#endif

  impl->api->ReleaseSessionOptions(options);

  if (status == NULL) {
    return NLP_STATUS_OK;
  }

  /*
   * In AUTO mode, the provider itself may initialize successfully
   * but the model/session can still fail because of a missing
   * runtime dependency.
   *
   * Retry CPU.
   */

  if (impl->execution_provider != NLP_EXEC_AUTO) {
    return nlp_ort_error(impl, status);
  }

  nlp_ort_error(impl, status);

  options = nlp_create_cpu_options(impl);

  if (options == NULL) {
    return NLP_STATUS_RUNTIME_FAILURE;
  }

#ifdef _WIN32

  wide_path = nlp_utf8_to_wide(model_path);

  if (wide_path == NULL) {
    impl->api->ReleaseSessionOptions(options);

    return NLP_STATUS_ALLOCATION_FAILURE;
  }

  status =
      impl->api->CreateSession(impl->env, wide_path, options, &impl->session);

  HeapFree(GetProcessHeap(), 0, wide_path);

#else

  status =
      impl->api->CreateSession(impl->env, model_path, options, &impl->session);

#endif

  impl->api->ReleaseSessionOptions(options);

  if (status != NULL) {
    return nlp_ort_error(impl, status);
  }

  return NLP_STATUS_OK;
}

/*
 * --------------------------------------------------------------------------
 * Analysis
 * --------------------------------------------------------------------------
 */

/*
 * --------------------------------------------------------------------------
 * Model information
 * --------------------------------------------------------------------------
 */

/*
 * --------------------------------------------------------------------------
 * Raw Transformer inference
 * --------------------------------------------------------------------------
 */

NlpStatus nlp_infer_tokens(NlpContext *context, const NlpTokenInput *input,
                           NlpTokenOutput *output) {
  if (context == NULL || context->impl == NULL || input == NULL ||
      output == NULL || input->input_ids == NULL ||
      input->attention_mask == NULL || input->sequence_length == 0) {

    return NLP_STATUS_INVALID_ARGUMENT;
  }

  NlpImpl *impl = (NlpImpl *)context->impl;

  if (impl->session == NULL) {
    return NLP_STATUS_MODEL_NOT_LOADED;
  }

  output->logits_count = 0;
  output->sequence_length = 0;
  output->label_count = 0;

  /*
   * DistilBERT-style token-classification models use:
   *
   *     input_ids
   *     attention_mask
   *
   * with int64 tensors.
   */

  const int64_t shape[] = {1, (int64_t)input->sequence_length};

  const size_t input_bytes = input->sequence_length * sizeof(int64_t);

  OrtMemoryInfo *memory_info = NULL;
  OrtValue *input_ids_value = NULL;
  OrtValue *attention_mask_value = NULL;
  OrtValue *output_value = NULL;

  OrtStatus *status = impl->api->CreateCpuMemoryInfo(
      OrtArenaAllocator, OrtMemTypeDefault, &memory_info);

  if (status != NULL) {
    return nlp_ort_error(impl, status);
  }

  status = impl->api->CreateTensorWithDataAsOrtValue(
      memory_info, (void *)input->input_ids, input_bytes, shape, 2,
      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_ids_value);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseMemoryInfo(memory_info);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  status = impl->api->CreateTensorWithDataAsOrtValue(
      memory_info, (void *)input->attention_mask, input_bytes, shape, 2,
      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &attention_mask_value);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseValue(input_ids_value);
    impl->api->ReleaseMemoryInfo(memory_info);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  const char *input_names[] = {"input_ids", "attention_mask"};

  const OrtValue *inputs[] = {input_ids_value, attention_mask_value};

  const char *output_names[] = {"logits"};

  status = impl->api->Run(impl->session, NULL, input_names, inputs, 2,
                          output_names, 1, &output_value);

  impl->api->ReleaseValue(input_ids_value);
  impl->api->ReleaseValue(attention_mask_value);
  impl->api->ReleaseMemoryInfo(memory_info);

  if (status != NULL) {
    return nlp_ort_error(impl, status);
  }

  /*
   * Inspect the returned tensor.
   */

  OrtTensorTypeAndShapeInfo *shape_info = NULL;

  status = impl->api->GetTensorTypeAndShape(output_value, &shape_info);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseValue(output_value);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  ONNXTensorElementDataType element_type;

  status = impl->api->GetTensorElementType(shape_info, &element_type);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {

    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);

    return NLP_STATUS_RUNTIME_FAILURE;
  }

  size_t dimension_count = 0;

  status = impl->api->GetDimensionsCount(shape_info, &dimension_count);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  if (dimension_count != 3) {

    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);

    return NLP_STATUS_RUNTIME_FAILURE;
  }

  int64_t dimensions[3] = {0, 0, 0};

  status = impl->api->GetDimensions(shape_info, dimensions, 3);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  /*
   * Expected token-classification shape:
   *
   *     [batch, sequence, labels]
   *
   * We currently run batch = 1.
   */

  if (dimensions[0] != 1 || dimensions[1] != (int64_t)input->sequence_length ||
      dimensions[2] <= 0) {

    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);

    return NLP_STATUS_RUNTIME_FAILURE;
  }

  size_t tensor_element_count = 0;

  status =
      impl->api->GetTensorShapeElementCount(shape_info, &tensor_element_count);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);
    return NLP_STATUS_RUNTIME_FAILURE;
  }

  output->sequence_length = (size_t)dimensions[1];

  output->label_count = (size_t)dimensions[2];

  output->logits_count = tensor_element_count;

  if (output->logits == NULL ||
      output->logits_capacity < tensor_element_count) {

    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);

    return NLP_STATUS_OUTPUT_CAPACITY;
  }

  void *data = NULL;

  status = impl->api->GetTensorMutableData(output_value, &data);

  if (status != NULL) {
    nlp_ort_error(impl, status);
    impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
    impl->api->ReleaseValue(output_value);

    output->logits_count = 0;

    return NLP_STATUS_RUNTIME_FAILURE;
  }

  memcpy(output->logits, data, tensor_element_count * sizeof(float));

  impl->api->ReleaseTensorTypeAndShapeInfo(shape_info);
  impl->api->ReleaseValue(output_value);

  return NLP_STATUS_OK;
}

NlpStatus nlp_get_model_info(NlpContext *context, NlpModelInfo *info) {
  if (context == NULL || context->impl == NULL || info == NULL) {

    return NLP_STATUS_INVALID_ARGUMENT;
  }

  NlpImpl *impl = (NlpImpl *)context->impl;

  /*
   * Model information belongs to the loaded ORT session.
   */
  if (impl->session == NULL) {
    return NLP_STATUS_MODEL_NOT_LOADED;
  }

  memset(info, 0, sizeof(*info));

  OrtStatus *status =
      impl->api->SessionGetInputCount(impl->session, &info->input_count);

  if (status != NULL) {
    return nlp_ort_error(impl, status);
  }

  status = impl->api->SessionGetOutputCount(impl->session, &info->output_count);

  if (status != NULL) {
    return nlp_ort_error(impl, status);
  }

  return NLP_STATUS_OK;
}

NlpStatus nlp_analyze(NlpContext *context, const char *text,
                      NlpResult *result) {
  if (context == NULL || context->impl == NULL || text == NULL ||
      result == NULL) {

    return NLP_STATUS_INVALID_ARGUMENT;
  }

  NlpImpl *impl = (NlpImpl *)context->impl;

  if (impl->session == NULL) {
    return NLP_STATUS_MODEL_NOT_LOADED;
  }

  result->input = text;
  result->input_length = strlen(text);

  result->entity_count = 0;
  result->relation_count = 0;

  /*
   * TODO:
   *
   *     Pass 1
   *       tokenizer
   *       contextual Transformer
   *
   *     Pass 2
   *       entity extraction
   *       relation extraction
   *
   * We intentionally do not implement this until the model
   * input/output contract is defined.
   */

  return NLP_STATUS_NOT_IMPLEMENTED;
}

/*
 * --------------------------------------------------------------------------
 * Cleanup
 * --------------------------------------------------------------------------
 */

void nlp_free(NlpContext *context) {
  if (context == NULL || context->impl == NULL) {

    return;
  }

  NlpImpl *impl = (NlpImpl *)context->impl;

  if (impl->session != NULL) {
    impl->api->ReleaseSession(impl->session);

    impl->session = NULL;
  }

  if (impl->env != NULL) {
    impl->api->ReleaseEnv(impl->env);

    impl->env = NULL;
  }

  /*
   * NlpImpl belongs to the caller's Arena.
   * Arena owns the memory, so no free() here.
   */

  context->impl = NULL;
}

/*
 * --------------------------------------------------------------------------
 * Names
 * --------------------------------------------------------------------------
 */

const char *nlp_status_name(NlpStatus status) {
  switch (status) {

  case NLP_STATUS_OK:
    return "OK";

  case NLP_STATUS_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";

  case NLP_STATUS_ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";

  case NLP_STATUS_RUNTIME_FAILURE:
    return "RUNTIME_FAILURE";

  case NLP_STATUS_MODEL_NOT_LOADED:
    return "MODEL_NOT_LOADED";

  case NLP_STATUS_NOT_IMPLEMENTED:
    return "NOT_IMPLEMENTED";

  case NLP_STATUS_OUTPUT_CAPACITY:
    return "OUTPUT_CAPACITY";

  default:
    return "UNKNOWN";
  }
}

const char *nlp_entity_type_name(NlpEntityType type) {
  switch (type) {

  case NLP_ENTITY_UNKNOWN:
    return "unknown";

  case NLP_ENTITY_COMPONENT:
    return "component";

  case NLP_ENTITY_VALUE:
    return "value";

  case NLP_ENTITY_UNIT:
    return "unit";

  case NLP_ENTITY_QUANTITY:
    return "quantity";

  case NLP_ENTITY_TOPOLOGY:
    return "topology";

  case NLP_ENTITY_MEASUREMENT:
    return "measurement";

  case NLP_ENTITY_NODE_REFERENCE:
    return "node_reference";

  case NLP_ENTITY_ACTION:
    return "action";

  default:
    return "unknown";
  }
}

const char *nlp_relation_type_name(NlpRelationType type) {
  switch (type) {

  case NLP_RELATION_UNKNOWN:
    return "unknown";

  case NLP_RELATION_VALUE_OF:
    return "value_of";

  case NLP_RELATION_UNIT_OF:
    return "unit_of";

  case NLP_RELATION_QUANTITY_OF:
    return "quantity_of";

  case NLP_RELATION_CONNECTS:
    return "connects";

  case NLP_RELATION_MEASURES:
    return "measures";

  case NLP_RELATION_MODIFIES:
    return "modifies";

  case NLP_RELATION_ASSOCIATED_WITH:
    return "associated_with";

  default:
    return "unknown";
  }
}
