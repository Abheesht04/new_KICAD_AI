if(NOT DEFINED HWC_EXE OR NOT DEFINED TEST_WORK_DIR)
  message(FATAL_ERROR "HWC_EXE and TEST_WORK_DIR are required")
endif()

function(run_success label)
  execute_process(
    COMMAND "${HWC_EXE}" ${ARGN}
    WORKING_DIRECTORY "${TEST_WORK_DIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT "${rc}" STREQUAL "0")
    message(FATAL_ERROR "${label} failed (exit=${rc})\nSTDOUT:\n${stdout}\nSTDERR:\n${stderr}")
  endif()
  set(LAST_STDOUT "${stdout}" PARENT_SCOPE)
endfunction()

run_success("divider prompt" make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm)
if(NOT LAST_STDOUT MATCHES "DESIGN CREATED")
  message(FATAL_ERROR "divider prompt did not confirm Design creation:\n${LAST_STDOUT}")
endif()
if(NOT EXISTS "${TEST_WORK_DIR}/out/current_design.kicad_sch")
  message(FATAL_ERROR "C schematic exporter did not create current_design.kicad_sch")
endif()

run_success("structural validation" validate)
if(NOT LAST_STDOUT MATCHES "STRUCTURAL VALIDATION: PASS")
  message(FATAL_ERROR "divider structural validation failed:\n${LAST_STDOUT}")
endif()

run_success("DFM apply" DFM -apply)
if(NOT LAST_STDOUT MATCHES "logical DFM errors: 0")
  message(FATAL_ERROR "divider DFM unexpectedly reported errors:\n${LAST_STDOUT}")
endif()

run_success("physics apply" physics-apply)
if(NOT LAST_STDOUT MATCHES "independent MNA validation = PASS" OR
   NOT LAST_STDOUT MATCHES "VOUT = 3.333333")
  message(FATAL_ERROR "MNA divider solve did not match expected output:\n${LAST_STDOUT}")
endif()

# Negative test: the CLI must reject a disconnected resistor as electrically incomplete.
run_success("standalone resistor template" make resistor)
execute_process(
  COMMAND "${HWC_EXE}" validate
  WORKING_DIRECTORY "${TEST_WORK_DIR}"
  RESULT_VARIABLE validation_rc
  OUTPUT_VARIABLE validation_out
  ERROR_VARIABLE validation_err
)
if("${validation_rc}" STREQUAL "0")
  message(FATAL_ERROR "validate incorrectly accepted an unconnected resistor:\n${validation_out}")
endif()
if(NOT validation_out MATCHES "unconnected_pin")
  message(FATAL_ERROR "unconnected resistor did not produce an explicit pin diagnostic:\n${validation_out}\n${validation_err}")
endif()

message(STATUS "CLI integration PASS: prompt -> Design -> KiCad schematic -> validation -> DFM -> MNA, plus negative unconnected-pin check")
