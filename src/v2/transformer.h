#ifndef COPPER_TRANSFORMER_H
#define COPPER_TRANSFORMER_H
typedef struct { int label, accepted; double confidence; } CtResult;
CtResult ct_classify(const char *english);
const char *ct_label(int label);
#endif
