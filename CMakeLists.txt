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

add_executable(hwc apps/hwc.c)
target_link_libraries(hwc PRIVATE hwcore)
if(MSVC)
    target_compile_options(hwc PRIVATE /W4)
else()
    target_compile_options(hwc PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(HWCOMPILER_BUILD_TESTS)
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
option(COPPER_SANITIZE "Enable address/undefined sanitizers" OFF)
if(COPPER_SANITIZE AND NOT MSVC)
  target_compile_options(hwcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hwcore PUBLIC -fsanitize=address,undefined)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(HWCOMPILER_BUILD_TESTS AND Python3_Interpreter_FOUND)
  add_test(NAME v2_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_v2.py $<TARGET_FILE:copper-core>)
  add_test(NAME bridge_regression COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_hwc_kicad.py)
endif()
option(COPPER_BUILD_DESKTOP "Build GLFW + Vulkan Copper Studio" OFF)
if(COPPER_BUILD_DESKTOP)
  enable_language(CXX)
  add_subdirectory(desktop)
endif()
