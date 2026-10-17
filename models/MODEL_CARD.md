# C intent transformer

The adapter maps a short English component description to R, C, L, D, LED, J,
V, Q, U, or UNKNOWN. It cannot generate a circuit, select ratings, determine
connectivity, size copper, or change an engineering result.

Architecture: learned word embeddings (24 dimensions), one 24-dimensional
self-attention head (Q/K/V/output matrices), residual connection, 48-unit ReLU
feed-forward layer, residual connection, masked mean pooling, and a 10-class
linear head. No positional embeddings: this role classifier intentionally uses
an order-invariant representation. `src/v2/transformer.c` implements the actual
forward graph in C; `intent_weights.h` contains trained floating-point weights.
No Python or model runtime is needed for inference.

Training data: 2,580 synthetic phrases generated entirely by `tools/train_intent.py`;
random seed 17; Adam, 65 epochs. All examples are inspectable in the generator.
`training.json` records training accuracy, not an independent benchmark. We tested
additional phrase compositions and rejection behavior in `test_v2.py`. This does
not establish broad natural-language accuracy or calibrated probabilities.

Inference limits: 24 word tokens, 2,048 input bytes, bounded arrays, softmax maximum
subtraction, confidence threshold 0.90. Unknown vocabulary, empty/oversized input,
and mixed explicit component roles are rejected. Numbers/units and multi-component
requests should be handled by a deterministic grammar or explicit project editor.
A confidence of 1 is a classifier score, not certainty or engineering assurance.

Retraining (optional): install PyTorch CPU, then run `python tools/train_intent.py`.
The shipped model was trained with torch 2.14.1+cpu. Floating-point exactness across
training library versions/platforms is not promised. Rebuild and rerun tests after
regeneration. Never use a retrained model's output without downstream validation.
