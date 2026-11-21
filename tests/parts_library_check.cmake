# ctest parts_library_parse: every parts_library/*.parts file must parse and give no FAIL finding.
# Run without KiCad roots, so the KiCad check is NOT_RUN and the verdict is UNVERIFIED (exit 3) or PASS (exit 0).
# A FAIL (exit 1), INVALID_INPUT (exit 2) or any other exit fails the test.
# usage: cmake -DCOPPER=<copper.exe> -DROOT=<source dir> -P parts_library_check.cmake
file(GLOB libraries "${ROOT}/parts_library/*.parts")
list(LENGTH libraries count)
if(count EQUAL 0)
    message(FATAL_ERROR "no parts_library/*.parts files found under ${ROOT}")
endif()
foreach(library ${libraries})
    execute_process(COMMAND "${COPPER}" catalogue check "${library}"
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT (code EQUAL 0 OR code EQUAL 3))
        message(FATAL_ERROR "${library}: exit ${code}\n${out}${err}")
    endif()
    if(out MATCHES "\n  FAIL ")
        message(FATAL_ERROR "${library}: FAIL finding\n${out}")
    endif()
    message(STATUS "${library}: exit ${code}")
endforeach()
