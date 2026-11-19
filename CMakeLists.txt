cmake_minimum_required(VERSION 3.20)
project(CopperStudio VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(COPPER_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(COPPER_TRACE "Enable trace diagnostics" OFF)
option(COPPER_NO_LIBM_CHECK "Lint geometry modules for floating point and libm" ON)

set(COPPER_CORE_SOURCES
  v2_src/IR.c v2_src/arena.c v2_src/board.c v2_src/board_kicad.c v2_src/circuit.c v2_src/copper_linalg.c
  v2_src/schematic.c v2_src/project_kicad.c v2_src/netlist_kicad.c v2_src/design_parity.c
  v2_src/copper_design.c v2_src/copper_design_json.c v2_src/topology.c v2_src/topology_json.c 
  v2_src/engineering_loop.c v2_src/engineering_loop_json.c v2_src/optimization_loop.c 
  v2_src/optimization_loop_json.c v2_src/design_resolver.c v2_src/kicad_asset_resolver.c 
  v2_src/design_resolution_json.c v2_src/copper_project.c v2_src/copper_spice.c 
  v2_src/copper_spice_expand.c v2_src/copper_spice_fs.c v2_src/cplx.c v2_src/dfm_check.c 
  v2_src/excellon.c v2_src/fab_rules.c v2_src/filesystem.c v2_src/g2_types.c
  v2_src/jlc_catalogue.c v2_src/kicad_assets.c v2_src/kicad_library_table.c v2_src/mna.c 
  v2_src/nmath.c v2_src/part_catalogue.c v2_src/placer.c v2_src/report.c v2_src/router.c 
  v2_src/sexpr.c v2_src/sha256.c v2_src/u128.c)

add_library(copper_core STATIC ${COPPER_CORE_SOURCES})
target_include_directories(copper_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/v2_src")

function(copper_apply_compile_policy target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /fp:strict)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
  endif()
  if(COPPER_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    get_target_property(copper_target_type ${target} TYPE)
    if(copper_target_type STREQUAL "STATIC_LIBRARY")
      target_link_options(${target} INTERFACE -fsanitize=address,undefined)
    else()
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

copper_apply_compile_policy(copper_core)
if(COPPER_TRACE)
  target_compile_definitions(copper_core PUBLIC COPPER_TRACE=1)
endif()
if(NOT MSVC)
  target_link_libraries(copper_core PUBLIC m)
endif()

add_executable(copper v2_src/copper_cli.c)
target_link_libraries(copper PRIVATE copper_core)
copper_apply_compile_policy(copper)

if(COPPER_NO_LIBM_CHECK)
  foreach(module board.c board_kicad.c dfm_check.c placer.c router.c fab_rules.c jlc_catalogue.c)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/v2_src/${module}" module_source)
    if(module_source MATCHES "#[ \t]*include[ \t]*<math\\.h>" OR
       module_source MATCHES "(^|[^A-Za-z0-9_])(double|float)([^A-Za-z0-9_]|$)" OR
       module_source MATCHES "\\b(sin|cos|sqrt|hypot|fabs|floor|ceil|atan2|llround)\\s*\\(")
      message(FATAL_ERROR "Floating point/libm use is forbidden in ${module}")
    endif()
  endforeach()
endif()

enable_testing()
add_executable(copper_core_tests v2_src/tests/test_core.c)
target_link_libraries(copper_core_tests PRIVATE copper_core)
target_compile_definitions(copper_core_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_core_tests)
add_test(NAME copper_core_tests COMMAND copper_core_tests)

add_executable(copper_design_tests v2_src/tests/test_design.c)
target_link_libraries(copper_design_tests PRIVATE copper_core)
target_compile_definitions(copper_design_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_design_tests)
add_test(NAME copper_design_tests COMMAND copper_design_tests)

add_executable(copper_catalogue_asset_tests v2_src/tests/test_catalogue_assets.c)
target_link_libraries(copper_catalogue_asset_tests PRIVATE copper_core)
target_compile_definitions(copper_catalogue_asset_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_catalogue_asset_tests)
add_test(NAME copper_catalogue_asset_tests COMMAND copper_catalogue_asset_tests)

add_executable(copper_topology_tests v2_src/tests/test_topology.c)
target_link_libraries(copper_topology_tests PRIVATE copper_core)
target_compile_definitions(copper_topology_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_topology_tests)
add_test(NAME copper_topology_tests COMMAND copper_topology_tests)

add_executable(copper_engineering_tests v2_src/tests/test_engineering_loop.c)
target_link_libraries(copper_engineering_tests PRIVATE copper_core)
target_compile_definitions(copper_engineering_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_engineering_tests)
add_test(NAME copper_engineering_tests COMMAND copper_engineering_tests)

add_executable(copper_optimization_tests v2_src/tests/test_optimization_loop.c)
target_link_libraries(copper_optimization_tests PRIVATE copper_core)
target_compile_definitions(copper_optimization_tests PRIVATE COPPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/v2_src")
copper_apply_compile_policy(copper_optimization_tests)
add_test(NAME copper_optimization_tests COMMAND copper_optimization_tests)