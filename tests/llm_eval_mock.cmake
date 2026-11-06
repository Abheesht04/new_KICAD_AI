# The LLM eval pipeline end to end with the mock backend: copper-core agent-eval eval/prompts.jsonl must PASS every
# prompt and write results.jsonl (10 lines), summary.txt and per-prompt logs. The output folder is this test's own
# scratch (agent-eval never writes into an existing folder).
file(REMOVE_RECURSE "${WORK}")
execute_process(COMMAND "${CORE}" agent-eval "${ROOT}/eval/prompts.jsonl" "${WORK}/run" --root "${ROOT}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
    file(READ "${WORK}/run/summary.txt" summary)
    message(FATAL_ERROR "agent-eval exited with ${rc}: ${err}\n${summary}")
endif()
file(STRINGS "${WORK}/run/results.jsonl" results)
list(LENGTH results n)
if(NOT n EQUAL 10 OR NOT EXISTS "${WORK}/run/summary.txt" OR NOT EXISTS "${WORK}/run/logs/load-divider.agent.jsonl")
    message(FATAL_ERROR "expected 10 results, summary.txt and per-prompt logs")
endif()
