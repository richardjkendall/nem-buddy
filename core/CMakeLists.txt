cmake_minimum_required(VERSION 3.16)
project(nem_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Vendored deps (kept lint-clean by isolating them from our -Werror where needed)
add_library(cjson STATIC third_party/cJSON.c)
target_include_directories(cjson PUBLIC third_party)
target_compile_options(cjson PRIVATE -w)   # don't fail our build on vendored warnings

add_library(unity STATIC third_party/unity.c)
target_include_directories(unity PUBLIC third_party)
target_compile_options(unity PRIVATE -w)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# Our library — sources are added by each task as modules land.
add_library(nem_core STATIC
  src/regions.c
  src/config.c
  src/timeutil.c
)
target_include_directories(nem_core PUBLIC include)
target_link_libraries(nem_core PUBLIC cjson)

enable_testing()

# Helper: register a Unity test executable.
function(nem_add_test name)
  add_executable(${name} test/${name}.c)
  target_link_libraries(${name} PRIVATE nem_core unity)
  target_include_directories(${name} PRIVATE include third_party)
  # Fixtures are found relative to this dir at runtime.
  target_compile_definitions(${name} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

nem_add_test(test_regions)
nem_add_test(test_config)
nem_add_test(test_timeutil)
