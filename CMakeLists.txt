cmake_minimum_required(VERSION 3.20)
project(hardware_compiler C)

set(CMAKE_C_STANDARD 17 CACHE STRING "Portable C standard (17 or newer)")
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(HWCOMPILER_BUILD_TESTS "Build test executables" ON)

add_library(hwcore STATIC
    src/v2/project.c
    src/v2/transformer.c
    src/arena.c
    src/component.c
    src/component_model.c
    src/copper.c
    src/cplx.c
    src/mna_ac.c
    src/transient.c
    src/pcb_copper.c
    src/u128.c
    src/constraint.c
    src/design.c
    src/defect.c
    src/geom.c
    src/dfm.c
    src/dfm_pcb.c
    src/fab_profile_io.c
    src/diagnostic.c
    src/e_series.c
    src/intern.c
    src/IR.c
    src/nlp_rules.c
    src/design_validate.c
    src/device_models.c
    src/kicad_schematic.c
    src/kicad_generic_provider.c
    src/kicad_pcb.c
    src/matrix.c
    src/mna.c
    src/mna_from_design.c
    src/model_registry.c
    src/net.c
    src/node_map.c
    src/nmath.c
    src/part_provider.c
    src/part_registry.c
    src/pcb.c
    src/range.c
    src/sexpr.c
    src/symlib.c
    Extras/tokenizer.c
    src/vec.c
    src/xForm.c
)

target_include_directories(hwcore PUBLIC src)
target_compile_definitions(hwcore PRIVATE HWC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(hwcore PRIVATE /W4)
else()
    target_compile_options(hwcore PRIVATE -Wall -Wextra -Wpedantic)
    if(UNIX)
        target_link_libraries(hwcore PUBLIC m)
    endif()
endif()

if(HWCOMPILER_BUILD_TESTS)
    enable_testing()

    set(TESTS
        v2_geometry
        vec
        component_model
        u128
        geom
        copper
        defect
        design
        dfm
        intern
        ir
        ir_nm
        matrix
        mna
        model
        node_map
        part_registry
        provider
        pcb_dfm
        kicad_pcb
        e_series
        fab_profile_io
        nlp_rules
        design_validate
        schematic_export
        mna_from_design
        device_model
        cplx
        mna_ac
        nonlinear_models
        transient
    )

    foreach(test_name IN LISTS TESTS)
        add_executable(${test_name}_test tests/${test_name}_test.c)
        target_link_libraries(${test_name}_test PRIVATE hwcore)
        if(test_name STREQUAL "ir")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/kicad9/symbols" IR_FIXTURE_PATH)
            target_compile_definitions(${test_name}_test PRIVATE IR_TEST_FIXTURE_DIR="${IR_FIXTURE_PATH}")
        endif()
        # Legacy tests use assert(expr) for both checking and setup. Keep
        # assertions enabled in Release configurations until those tests are
        # refactored to avoid side effects inside assert().
        if(MSVC)
            target_compile_options(${test_name}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_name}_test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
endif()

add_executable(fabcheck_demo apps/fabcheck_demo.c)
target_link_libraries(fabcheck_demo PRIVATE hwcore)

# Shell-free process launching and UTF-8 paths for the command-line front ends.
add_library(cli_support STATIC apps/pcba/cli_os.c)
target_include_directories(cli_support PUBLIC apps/pcba)
target_compile_definitions(cli_support PRIVATE PCBA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(cli_support PUBLIC shell32)
endif()

# pcba: the single user-facing command. Engineering work stays in copper-core / tools/copper.py.
set(PCBA_VERSION "2.0.0-dev")
add_executable(pcba
    apps/pcba/main.c
    apps/pcba/cli_args.c
    apps/pcba/cli_backend.c
    apps/pcba/cli_commands.c
    apps/pcba/cli_doctor.c
    apps/pcba/cli_help.c
    apps/pcba/cli_term.c
)
target_link_libraries(pcba PRIVATE cli_support)
target_compile_definitions(pcba PRIVATE
    PCBA_VERSION="${PCBA_VERSION}"
    PCBA_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
    PCBA_C_STANDARD="${CMAKE_C_STANDARD}"
    PCBA_BUILD_TYPE="$<CONFIG>"
    PCBA_CTEST="${CMAKE_CTEST_COMMAND}"
)
add_dependencies(pcba copper-core)
if(MSVC)
    target_compile_options(pcba PRIVATE /W4)
    target_compile_options(cli_support PRIVATE /W4)
else()
    target_compile_options(pcba PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(cli_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(hwc apps/hwc.c)
target_link_libraries(hwc PRIVATE hwcore cli_support)
if(MSVC)
    target_compile_options(hwc PRIVATE /W4)
else()
    target_compile_options(hwc PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(HWCOMPILER_BUILD_TESTS)
    add_executable(cli_quote_test tests/cli_quote_test.c)
    target_link_libraries(cli_quote_test PRIVATE cli_support)
    if(MSVC)
        target_compile_options(cli_quote_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(cli_quote_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME cli_quote COMMAND cli_quote_test)
    add_test(NAME hwc_help COMMAND hwc --help)
    add_test(
        NAME hwc_cli_integration
        COMMAND ${CMAKE_COMMAND}
            "-DHWC_EXE=$<TARGET_FILE:hwc>"
            "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/hwc_cli_integration.cmake"
    )
endif()

add_executable(copper-core apps/copper_core.c)
target_link_libraries(copper-core PRIVATE hwcore)
# Measurement tool for docs/engineering/NUMERICAL_ROADMAP.md: kernels against analytical oracles.
add_executable(numeric_bench apps/numeric_bench.c)
target_include_directories(numeric_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(numeric_bench PRIVATE hwcore)
option(COPPER_SANITIZE "Enable address/undefined sanitizers" OFF)
if(COPPER_SANITIZE AND NOT MSVC)
  target_compile_options(hwcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hwcore PUBLIC -fsanitize=address,undefined)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(HWCOMPILER_BUILD_TESTS AND Python3_Interpreter_FOUND)
  add_test(NAME v2_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_v2.py $<TARGET_FILE:copper-core> V2 ModelContract AcAnalysis Transistors TransientEngine)
  # Installed KiCad libraries / KiCad 9.x CLI. Exit 77 = skipped (UNVERIFIED), never reported as Passed.
  add_test(NAME v2_native COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_v2.py $<TARGET_FILE:copper-core> V2Native)
  set_tests_properties(v2_integration v2_native PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME engineering_spec COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_spec.py $<TARGET_FILE:copper-core>)
  add_test(NAME parts_registry COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_parts.py $<TARGET_FILE:copper-core> Registry McuBoundary Geometry Import)
  # ESP32 module and MOSFET variant through PCB generation and KiCad 9.x gates. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME parts_native COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_parts.py $<TARGET_FILE:copper-core> Native)
  set_tests_properties(parts_native PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME layout_optimizer COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_layout.py $<TARGET_FILE:copper-core> Router Selection Fixtures)
  # Fixtures needing footprints outside the committed snapshot. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME layout_native COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_layout.py $<TARGET_FILE:copper-core> Libraries)
  set_tests_properties(layout_native PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME design_compiler COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_design.py $<TARGET_FILE:copper-core> Intent Questions Contradictions Compiler)
  # The ESP32 LED acceptance run with KiCad 9.x libraries and CLI. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME design_acceptance COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_design.py $<TARGET_FILE:copper-core> Native)
  set_tests_properties(design_acceptance PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME dfm_rules COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_dfm_rules.py $<TARGET_FILE:copper-core>)
  add_test(NAME native_gates COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_native_gates.py $<TARGET_FILE:copper-core>)
  add_test(NAME prompt_acceptance_harness COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_prompt_acceptance.py $<TARGET_FILE:pcba> Classification UnicodeRun)
  # Slice 1: English -> divider board passing KiCad 9.x ERC/DRC; the project stays in slice1/ to open in KiCad. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME slice1_divider COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_prompt_acceptance.py $<TARGET_FILE:pcba> Slice1)
  set_tests_properties(slice1_divider PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "COPPER_SLICE1_OUT=${CMAKE_BINARY_DIR}/slice1")
  add_test(NAME numeric_oracles COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_numeric_oracles.py $<TARGET_FILE:copper-core> $<TARGET_FILE:numeric_bench>)
  add_test(NAME pcba_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_pcba_cli.py
           $<TARGET_FILE:pcba> $<TARGET_FILE:hwc> $<TARGET_FILE:copper-core>)
  set_tests_properties(pcba_cli PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME bridge_regression COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_hwc_kicad.py)
endif()
option(COPPER_BUILD_DESKTOP "Build GLFW + Vulkan Copper Studio" OFF)
if(COPPER_BUILD_DESKTOP)
  enable_language(CXX)
  add_subdirectory(desktop)
endif()
