# LLM bridge (V2 track G)

The model runs elsewhere: Ollama on this machine or another one. Copper's side is a C client, an agent loop and an eval, in line with the language split (CHARTER §3.1). Nothing needs a model to build or to pass its tests; the mock backend is the default.

| File | Role |
|---|---|
| `src/v2_src/llm.h`, `llm.c` | the backend interface `LlmStatus (*chat)(LlmBackend*, const LlmMessage* msgs, size_t n, const char* json_schema, Arena*, LlmReply* out)`, and status and role names |
| `llm_mock.c` | **the default.** It runs scripted replies, or a deterministic rule planner that reads the paths and keywords of a request, calls `board.load`, `dfm.check`, `excellon.check` or `export.fab`, and answers from the tool results. |
| `llm_ollama.c` | `POST {base}/api/chat` with `"stream": false`, `"think": false`, the reply schema as `"format"` (Ollama's structured output) and `"options": {"temperature": 0}`. Plain HTTP/1.1 sockets: Winsock on Windows, POSIX elsewhere; no TLS, no libraries. Content-Length and chunked replies are read; an unreachable server is `UNAVAILABLE`, a slow one `TIMEOUT`. |
| `schema_gbnf.c` | a JSON Schema subset → GBNF (llama.cpp syntax) for servers that take a grammar: objects with every property required, any object, string (with enum), integer, number, boolean, null, array. Anything else is refused with a reason. |
| `agent_loop.h`, `agent_loop.c` | the conversation against the tool server (track F) |
| `eval/prompts.jsonl`, `scripts/run_llm_eval.sh`, `.ps1` | the eval: 10 prompts with expectations |

## Agent loop

**System prompt.** It tells the model the tools, taken from `tools/list` with their schemas. It also tells the model to base its answer only on tool results, never invent part numbers, ratings or capabilities, and say when something is FAIL, UNVERIFIED or not run.

**The reply.** Every turn the model must answer with one JSON object, following `agent_reply_schema()`:

```json
{"action": "tool",  "tool": "dfm.check", "arguments": {"path": "...", "profile": "..."}, "answer": ""}
{"action": "final", "tool": "",          "arguments": {},                               "answer": "..."}
```

**Tool calls.**
- A tool call goes through `tool_server_handle` as a real JSON-RPC `tools/call`.
- The result, or the error, returns as a tool message: `{"tool": name, "result": …}` or `{"tool": name, "error": …}`.
- Results are cut at 16 KiB, and the cut is marked.

**Bad replies.**
- A reply that is not the format is answered with a correction.
- Three bad replies in a row end the run with `INVALID_REPLIES`.
- A ```` ```json ```` fence is accepted.

**Limits and logging.**
- At most **12 turns** (`AGENT_MAX_TURNS`), whatever the config asks; then the run ends with `TURN_LIMIT`.
- Every turn is one JSONL log line: the raw reply, the action, and the tool with its response.

**Scoring** (`agent_score`):
- **PASS:** the run ended with a final answer, the expected tools were called in order, and the answer contains every expected phrase (case-insensitive).
- **FAIL:** the run is done but a check is not met.
- **UNVERIFIED:** the backend could not answer (no model, no server). This is never counted as FAIL.

## Eval

```
scripts/run_llm_eval.sh                                   # mock: deterministic, no model
BACKEND=ollama MODEL=qwen3:8b scripts/run_llm_eval.sh     # a local Ollama
scripts\run_llm_eval.ps1 -Backend ollama -Model llama3.2:latest -Url http://gpu-box:11434
```

**What the scripts do.** Both call `copper-core agent-eval eval/prompts.jsonl eval_out/<YYYY-MM-DD>[-N]`. They pick a new folder (never reused), and `eval_out/` is gitignored.

**What it writes.**
- `results.jsonl`: one line per prompt, with the verdict, status, turns, tools, answer, reasons, model time and tokens.
- `summary.txt`.
- Per prompt, `logs/<id>.agent.jsonl` (the turns) and `logs/<id>.tools.jsonl` (the tool-server session).

**The prompts.** The 10 prompts cover:
- loading a board;
- DFM that FAILs, DFM that is UNVERIFIED, and listing what fails;
- a drill file that matches and one that does not;
- outline closure;
- a missing file;
- "what tools do you have";
- a two-step load-then-check.

## Evidence

**`llm` test** (no model):
- **URL and HTTP parsing:** Content-Length, chunked, cut-short and garbage replies.
- **The exact Ollama request body,** including `stream`, `think` and `format`.
- **Reply parsing:** normal replies, `{"error": …}`, and non-JSON.
- **An unreachable server:** it reports `UNAVAILABLE` or `TIMEOUT` and does not hang.
- **GBNF:** the agent schema's grammar equals `tests/golden/llm/agent_reply.gbnf`; arrays and nested objects work; optional properties and unsupported types are refused.
- **Agent loop:**
  - a tool call then a fenced final answer;
  - the schema is passed on every call;
  - the 12-turn limit holds against a config of 50;
  - three bad replies stop the run, while one is corrected;
  - a tool error goes back to the model;
  - a backend failure scores UNVERIFIED.
- **All 10 eval prompts** run through the rule planner and PASS.

**Other tests:**
- **`llm_eval_mock`:** the real `copper-core agent-eval` run gives 10 PASS, with 10 result lines, a summary and logs.
- **`llm_ollama_live`** (`llm_test --live`, needs `OLLAMA_URL` and `OLLAMA_MODEL`): one structured-output call to a real model. **It was not run on this machine, which has no Ollama;** it skips as UNVERIFIED.

**Sanitizers:** clean under UBSan. ASan was not run; it is unavailable on MinGW.

**What the mock eval measures:** the pipeline (prompts, tools, scoring), not a model. The summary says so. Model quality is measured only by running the eval with `--backend ollama`.
