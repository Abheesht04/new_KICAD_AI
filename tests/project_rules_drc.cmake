# ctest project_rules_drc (Step 3b): KiCad DRC really applies the rules copper-core project-rules writes.
# Board: tests/fixtures/project_rules/fine_pitch.kicad_pcb, Copper-written; its SOT-563 pads (0.15 mm apart) are
# copied from KiCad's Package_TO_SOT_SMD.pretty/SOT-563.kicad_mod.
#   JLCPCB (smd pad to pad 0.15 mm): the fine-pitch rule allows the 0.15 mm gap: no clearance violation.
#   OSH Park (no pad limit; default copper clearance 0.1524 mm): the rule is written at 0.1524 mm, so KiCad must
#   report U3's 4 pad pairs under that rule's name. This shows the rule is evaluated (kicad-cli silently ignores
#   a rule file it cannot parse) and never lowers a limit below the fab's.
# Without KICAD_CLI it prints SKIPPED (UNVERIFIED). KiCad 10 results are a development check, not KiCad 9 evidence.
# usage: cmake -DCORE=<copper-core> -DROOT=<source dir> -DWORK=<scratch dir> -P project_rules_drc.cmake
if(NOT DEFINED ENV{KICAD_CLI} OR NOT EXISTS "$ENV{KICAD_CLI}")
    message("SKIPPED: KICAD_CLI is not set (UNVERIFIED)")
    return()
endif()
set(KICAD "$ENV{KICAD_CLI}")

function(run_case name profile rules expect_exit expect_count)
    set(dir "${WORK}/${name}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    file(COPY "${ROOT}/tests/fixtures/project_rules/fine_pitch.kicad_pcb" DESTINATION "${dir}")
    execute_process(COMMAND "${CORE}" project-rules "${dir}/fine_pitch.kicad_pcb" "${ROOT}/fab_profiles/${profile}.json"
                            "${rules}"
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL ${expect_exit})
        message(FATAL_ERROR "${name}: project-rules exit ${code}, expected ${expect_exit}\n${out}${err}")
    endif()
    execute_process(COMMAND "${KICAD}" pcb drc --format json --severity-all --output "${dir}/drc.json" "${dir}/fine_pitch.kicad_pcb"
                    RESULT_VARIABLE kcode OUTPUT_VARIABLE kout ERROR_VARIABLE kerr)
    if(NOT EXISTS "${dir}/drc.json")
        message(FATAL_ERROR "${name}: kicad-cli wrote no DRC report (exit ${kcode})\n${kout}${kerr}")
    endif()
    file(READ "${dir}/drc.json" drc)
    string(REGEX MATCHALL "Clearance violation" all "${drc}")
    string(REGEX MATCHALL "rule 'copper_fine_pitch_U3'" ours "${drc}")
    list(LENGTH all n_all)
    list(LENGTH ours n_ours)
    if(NOT n_all EQUAL ${expect_count} OR NOT n_ours EQUAL ${expect_count})
        message(FATAL_ERROR "${name}: ${n_all} clearance violation(s), ${n_ours} under copper_fine_pitch_U3; expected ${expect_count}")
    endif()
    message(STATUS "${name}: ${n_all} clearance violation(s) as expected")
endfunction()

#   Step 3c: the default routing rules carry fine_pitch_clearance_mm 0.15 mm, below OSH Park's 0.1524 mm default
#   clearance, so they are refused for OSH Park (exit 2); the OSH Park case uses a copy without the fine-pitch keys.
set(rules "${ROOT}/routing_profiles/copper_2layer_default.json")
file(READ "${rules}" text)
string(REGEX REPLACE ",\r?\n *\"fine_pitch_clearance_mm\"[^\n]*\n *\"fine_pitch_neck_mm\"[^\n]*" "" nofine "${text}")
if(nofine STREQUAL text)
    message(FATAL_ERROR "the routing rules have no fine-pitch keys to remove")
endif()
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/rules_nofine.json" "${nofine}")
run_case(jlcpcb jlcpcb_2layer "${rules}" 0 0)
execute_process(COMMAND "${CORE}" project-rules "${WORK}/jlcpcb/fine_pitch.kicad_pcb" "${ROOT}/fab_profiles/oshpark_2layer.json" "${rules}"
                RESULT_VARIABLE refused OUTPUT_VARIABLE rout ERROR_VARIABLE rerr)
if(NOT refused EQUAL 2 OR NOT rout MATCHES "fine_pitch_clearance_mm")
    message(FATAL_ERROR "OSH Park with the default rules: exit ${refused}, expected 2 naming fine_pitch_clearance_mm\n${rout}${rerr}")
endif()
message(STATUS "oshpark + default rules: refused as expected")
run_case(oshpark oshpark_2layer "${WORK}/rules_nofine.json" 3 4)
