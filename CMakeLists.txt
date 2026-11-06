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

# Copper V2 strict-C engine (src/v2_src, CLAUDE.md section 2): int64 nm geometry, Arena memory, status enums,
# no printf/exit in library code. The shared base modules (arena, IR, sexpr, nmath, u128, cplx) exist in both
# src/ and src/v2_src/ byte for byte; they are compiled once, from src/ in hwcore, and the v2_src_shared_in_sync
# test fails if the two copies ever differ.
add_library(copper_v2 STATIC
    src/v2_src/g2_types.c
    src/v2_src/excellon.c
    src/v2_src/kicad_library_table.c
    src/v2_src/filesystem.c
    src/v2_src/copper_project.c
    src/v2_src/board.c
    src/v2_src/json.c
    src/v2_src/dfm.c
    src/v2_src/kicad_cli.c
    src/v2_src/gerber.c
    src/v2_src/finding.c
    src/v2_src/tool_server.c
    src/v2_src/llm.c
    src/v2_src/llm_mock.c
    src/v2_src/llm_ollama.c
    src/v2_src/schema_gbnf.c
    src/v2_src/agent_loop.c
    src/v2_src/dfm_cpu_ref.c
    src/v2_src/rest_server.c
)
target_include_directories(copper_v2 PUBLIC src/v2_src)
# Track H: the CUDA clearance kernel is opt-in (-DCOPPER_CUDA=ON, needs nvcc and CUDA 11.5+). Without it a stub
# reports NOT_BUILT; nothing needs a GPU to build or to pass tests.
option(COPPER_CUDA "Build the CUDA DFM kernel (cuda/dfm_gpu.cu)" OFF)
if(COPPER_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(copper_v2 PRIVATE cuda/dfm_gpu.cu)
    set_source_files_properties(cuda/dfm_gpu.cu PROPERTIES LANGUAGE CUDA)
    target_link_libraries(copper_v2 PUBLIC CUDA::cudart)
    target_compile_definitions(copper_v2 PUBLIC COPPER_HAVE_CUDA=1)
    message(STATUS "copper_v2: CUDA DFM kernel enabled")
else()
    target_sources(copper_v2 PRIVATE src/v2_src/dfm_gpu_stub.c)
endif()
target_link_libraries(copper_v2 PUBLIC hwcore)
if(MSVC)
    target_compile_options(copper_v2 PRIVATE /W4)
else()
    target_compile_options(copper_v2 PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(copper_v2 PUBLIC ws2_32)  # llm_ollama.c: HTTP to an Ollama server
    target_link_libraries(copper_v2 PUBLIC ole32)  # CoCreateGuid for native KiCad project UUIDs (copper_project.c)
endif()
# Sanitizers for the V2 engine and its tests. MinGW has no libasan: UBSan runs in trap mode there and ASan is
# reported as not run, never as passed.
option(COPPER_V2_SANITIZE "Build copper_v2 and its tests with sanitizers" OFF)
if(COPPER_V2_SANITIZE AND NOT MSVC)
    if(MINGW)
        set(COPPER_V2_SAN_FLAGS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
        message(STATUS "copper_v2: UBSan (trap mode); ASan NOT RUN (unavailable on MinGW)")
    else()
        set(COPPER_V2_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    target_compile_options(copper_v2 PUBLIC ${COPPER_V2_SAN_FLAGS})
    target_link_options(copper_v2 PUBLIC ${COPPER_V2_SAN_FLAGS})
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
        g2_types
        kicad_native
        board
        json
        dfm_v2
        kicad_cli
        gerber
        finding
        tool_server
        llm
        dfm_gpu
        rest_server
    )

    # A stand-in for kicad-cli for tests/kicad_cli_test.c (records its argv; modes for each failure path).
    add_executable(fake_kicad_cli tests/fake_kicad_cli.c)
    if(WIN32)
        target_link_libraries(fake_kicad_cli PRIVATE shell32)
    endif()
    foreach(test_name IN LISTS TESTS)
        add_executable(${test_name}_test tests/${test_name}_test.c)
        if(test_name STREQUAL "g2_types" OR test_name STREQUAL "kicad_native" OR test_name STREQUAL "board" OR test_name STREQUAL "json" OR test_name STREQUAL "dfm_v2" OR test_name STREQUAL "kicad_cli" OR test_name STREQUAL "gerber" OR test_name STREQUAL "finding" OR test_name STREQUAL "tool_server" OR test_name STREQUAL "llm" OR test_name STREQUAL "dfm_gpu" OR test_name STREQUAL "rest_server")
            target_link_libraries(${test_name}_test PRIVATE copper_v2)
        else()
            target_link_libraries(${test_name}_test PRIVATE hwcore)
        endif()
        if(test_name STREQUAL "kicad_native")
            # Real KiCad 9.0.6 CAM output of the divider board (manufacturing_audit/{gerber_test,drill_test}).
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/manufacturing_audit" KICAD9_CAM_PATH)
            target_compile_definitions(${test_name}_test PRIVATE KICAD9_CAM_DIR="${KICAD9_CAM_PATH}")
        endif()
        if(test_name STREQUAL "board")
            # r0603x5: Copper-generated, plus its KiCad 10.0.5 re-save (development preview); the divider board is
            # checked against the real KiCad 9.0.6 drill export in manufacturing_audit/drill_test.
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" BOARD_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE
                BOARD_GOLDEN_DIR="${BOARD_ROOT}/tests/golden/board"
                DIVIDER_BOARD="${BOARD_ROOT}/examples/generated/divider/design.kicad_pcb"
                KICAD9_DRILL="${BOARD_ROOT}/manufacturing_audit/drill_test/design.drl")
        endif()
        if(test_name STREQUAL "dfm_v2")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" DFM_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE DFM_ROOT="${DFM_ROOT}")
        endif()
        if(test_name STREQUAL "kicad_cli")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/board" KCLI_GOLDEN)
            target_compile_definitions(${test_name}_test PRIVATE
                FAKE_KICAD_CLI="$<TARGET_FILE:fake_kicad_cli>"
                KCLI_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/kicad_cli_test"
                BOARD_GOLDEN_DIR="${KCLI_GOLDEN}")
            add_dependencies(${test_name}_test fake_kicad_cli)
        endif()
        if(test_name STREQUAL "gerber")
            # Real KiCad 9.0.6 Gerbers and drill file (manufacturing_audit) and the divider board model.
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" GERBER_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE GERBER_ROOT="${GERBER_ROOT}"
                GERBER_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/gerber_real")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gerber_real")
        endif()
        if(test_name STREQUAL "finding")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" FINDING_ROOT_PATH)
            target_compile_definitions(${test_name}_test PRIVATE FINDING_ROOT="${FINDING_ROOT_PATH}")
        endif()
        if(test_name STREQUAL "tool_server")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" TS_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE TS_ROOT="${TS_ROOT}"
                TS_WORK="${CMAKE_CURRENT_BINARY_DIR}/tool_server_work" FAKE_KICAD_CLI="$<TARGET_FILE:fake_kicad_cli>")
            add_dependencies(${test_name}_test fake_kicad_cli)
        endif()
        if(test_name STREQUAL "llm")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" LLM_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE LLM_ROOT="${LLM_ROOT}")
        endif()
        if(test_name STREQUAL "rest_server")
            file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" REST_ROOT)
            target_compile_definitions(${test_name}_test PRIVATE REST_ROOT="${REST_ROOT}")
        endif()
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
    # The same runner against a real kicad-cli (KICAD_CLI). Exit 77 = skipped (UNVERIFIED) when none is configured.
    add_test(NAME kicad_cli_real COMMAND kicad_cli_test --real)
    set_tests_properties(kicad_cli_real PROPERTIES SKIP_RETURN_CODE 77)
    # KiCad's own Gerbers of KiCad 9.0 demo boards (KICAD_CLI, KICAD_DEMOS_DIR). Exit 77 = skipped (UNVERIFIED).
    add_test(NAME gerber_kicad_demos COMMAND gerber_test --real)
    set_tests_properties(gerber_kicad_demos PROPERTIES SKIP_RETURN_CODE 77)
    # The tool server over real stdio: copper-core serve with a scripted transcript (tests/golden/tool_server).
    add_test(NAME tool_server_stdio COMMAND ${CMAKE_COMMAND} "-DCORE=$<TARGET_FILE:copper-core>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tool_server_stdio" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_server_stdio.cmake")
    # The same LLM test against a live Ollama server (OLLAMA_URL, OLLAMA_MODEL). Exit 77 = skipped (UNVERIFIED).
    add_test(NAME llm_ollama_live COMMAND llm_test --live)
    set_tests_properties(llm_ollama_live PROPERTIES SKIP_RETURN_CODE 77)
    # The eval pipeline end to end with the mock backend (eval/prompts.jsonl): every prompt must PASS.
    add_test(NAME llm_eval_mock COMMAND ${CMAKE_COMMAND} "-DCORE=$<TARGET_FILE:copper-core>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/llm_eval_mock" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/llm_eval_mock.cmake")
    # Track H: the GPU against the CPU reference on 1000 random boards. Exit 77 = NOT RUN (no CUDA build or device).
    add_test(NAME dfm_gpu_compare COMMAND dfm_gpu_test --gpu)
    set_tests_properties(dfm_gpu_compare PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME v2_src_shared_in_sync COMMAND ${CMAKE_COMMAND} "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2_src_sync.cmake")
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

# Track H benchmark: CPU reference vs GPU clearance at N = 64, 1024, 16384.
add_executable(dfm_bench apps/dfm_bench.c)
target_link_libraries(dfm_bench PRIVATE copper_v2)
add_executable(copper-core apps/copper_core.c)
target_link_libraries(copper-core PRIVATE copper_v2)
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
  add_test(NAME layout_optimizer COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_layout.py $<TARGET_FILE:copper-core> Router Dfm Parallel Selection Fixtures)
  # Fixtures needing footprints outside the committed snapshot. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME layout_native COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_layout.py $<TARGET_FILE:copper-core> Libraries)
  set_tests_properties(layout_native PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME design_compiler COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_design.py $<TARGET_FILE:copper-core> Intent Questions Contradictions Compiler)
  # The ESP32 LED acceptance run with KiCad 9.x libraries and CLI. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME design_acceptance COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_design.py $<TARGET_FILE:copper-core> Native)
  set_tests_properties(design_acceptance PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME dfm_rules COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_dfm_rules.py $<TARGET_FILE:copper-core>)
  add_test(NAME circuit_blocks COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_blocks.py $<TARGET_FILE:copper-core>)
  add_test(NAME kicad_rules COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_kicad_rules.py $<TARGET_FILE:pcba>)
  add_test(NAME kicad_library COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_kicad_library.py $<TARGET_FILE:pcba>)
  add_test(NAME native_gates COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_native_gates.py $<TARGET_FILE:copper-core>)
  add_test(NAME prompt_acceptance_harness COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_prompt_acceptance.py $<TARGET_FILE:pcba> Classification UnicodeRun)
  # Slice 1: English -> divider board passing KiCad 9.x ERC/DRC; the project stays in slice1/ to open in KiCad. Exit 77 = skipped (UNVERIFIED).
  add_test(NAME slice1_divider COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_prompt_acceptance.py $<TARGET_FILE:pcba> Slice1)
  set_tests_properties(slice1_divider PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "COPPER_SLICE1_OUT=${CMAKE_BINARY_DIR}/slice1")
  add_test(NAME numeric_oracles COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_numeric_oracles.py $<TARGET_FILE:copper-core> $<TARGET_FILE:numeric_bench>)
  add_test(NAME pcba_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_pcba_cli.py
           $<TARGET_FILE:pcba> $<TARGET_FILE:hwc> $<TARGET_FILE:copper-core>)
  set_tests_properties(pcba_cli PROPERTIES SKIP_RETURN_CODE 77)
  # Track B on KiCad 9.0-saved boards from a KiCad install (KICAD_DEMOS_DIR; KICAD_CLI for the drill cross-check).
  # Exit 77 = skipped (UNVERIFIED) when no install is configured.
  add_test(NAME board_kicad_demos COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_board_demos.py $<TARGET_FILE:copper-core>)
  set_tests_properties(board_kicad_demos PROPERTIES SKIP_RETURN_CODE 77)
  # Track C clearance against a brute-force oracle on KiCad 9.0-saved boards (KICAD_DEMOS_DIR). Exit 77 = UNVERIFIED.
  add_test(NAME dfm_kicad_demos COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_dfm_demos.py $<TARGET_FILE:copper-core>)
  set_tests_properties(dfm_kicad_demos PROPERTIES SKIP_RETURN_CODE 77)
  # The REST front end over real HTTP: copper-core serve-http on a free port, driven with urllib.
  add_test(NAME rest_server_http COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_rest_server.py $<TARGET_FILE:copper-core>)
  add_test(NAME bridge_regression COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_hwc_kicad.py)
endif()
option(COPPER_BUILD_DESKTOP "Build GLFW + Vulkan Copper Studio" OFF)
if(COPPER_BUILD_DESKTOP)
  enable_language(CXX)
  add_subdirectory(desktop)
endif()
