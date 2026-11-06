# copper-core serve over real stdin/stdout: tests/golden/tool_server/stdio_requests.jsonl in, the responses must equal
# stdio_responses.jsonl line for line (line endings aside). The request after "exit" must not be answered. The
# session log must hold one line per message in and out.
# Run with -DWRITE_GOLDEN=1 to rewrite stdio_responses.jsonl for review.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(GOLDEN "${ROOT}/tests/golden/tool_server")
execute_process(
    COMMAND "${CORE}" serve --root "${ROOT}" --log "${WORK}/session.jsonl"
    INPUT_FILE "${GOLDEN}/stdio_requests.jsonl"
    OUTPUT_FILE "${WORK}/responses.jsonl"
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
    TIMEOUT 120)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "copper-core serve exited with ${rc}: ${err}")
endif()
file(READ "${WORK}/responses.jsonl" got)
string(REPLACE "\r\n" "\n" got "${got}")
if(WRITE_GOLDEN)
    file(WRITE "${GOLDEN}/stdio_responses.jsonl" "${got}")
    message(STATUS "stdio_responses.jsonl written; review it")
    return()
endif()
file(READ "${GOLDEN}/stdio_responses.jsonl" want)
string(REPLACE "\r\n" "\n" want "${want}")
if(NOT got STREQUAL want)
    message(FATAL_ERROR "copper-core serve responses differ from ${GOLDEN}/stdio_responses.jsonl:\n--- got ---\n${got}")
endif()
file(STRINGS "${WORK}/responses.jsonl" responses)
list(LENGTH responses n_out)
file(STRINGS "${WORK}/session.jsonl" log_lines)
list(LENGTH log_lines n_log)
# 8 requests before "exit" are read (7 answered: exit is a notification); the 9th, after exit, is never read.
if(NOT n_out EQUAL 7 OR NOT n_log EQUAL 15)
    message(FATAL_ERROR "expected 7 responses and 15 log lines (8 in, 7 out), got ${n_out} and ${n_log}")
endif()
message(STATUS "copper-core serve: ${n_out} responses as expected, ${n_log} session log lines")
