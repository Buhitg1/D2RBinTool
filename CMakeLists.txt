cmake_minimum_required(VERSION 3.16)

project(d2datables
        VERSION 1.0.0
        DESCRIPTION "D2R excel datatable compiler / decompiler (byte-exact port of D2R.exe)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Two binaries from one source set:
#   d2datables         - 3.1.1 engine (game build 91923, D2R.exe)
#   d2datatables_93854 - 3.3 engine (game build 93854, D2R_93854_RAC_China.exe),
#                        same sources plus -DD2DT_BUILD_93854, which selects
#                        the 93854 descriptor variants in tables.cpp.
# Both are needed: data/ is verified with the first, data_93847/ (and the
# blizzless / mod data sets) with the second.
# ---------------------------------------------------------------------------
set(D2DT_SOURCES
    src/main.cpp
    src/excel_engine.cpp
    src/tables.cpp
    src/idx_defaults.cpp
    src/calc_compiler.cpp
    src/decompiler.cpp)

# Both executables land in the directories the verify scripts (tools/verify.py,
# tools/verify_93854.py) and the VERIFY*.md reports reference, no matter which
# build directory you configure:
#     d2datatables/build/d2datatables.exe         (3.1.1)
#     d2datatables/build_93854/d2datatables.exe   (3.3)
# Only the executables are placed there; CMake's intermediates stay under your
# -B directory.  This mirrors what build.bat / build_93854.bat always did.  The
# per-config variants matter for multi-config generators (Visual Studio), which
# otherwise append a Release/ or Debug/ subdirectory.
function(d2dt_set_output_dirs target dir)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                 "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE         "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG           "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO  "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
endfunction()

add_executable(d2datatables ${D2DT_SOURCES})
target_include_directories(d2datatables PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
d2dt_set_output_dirs(d2datatables build)

add_executable(d2datatables_93854 ${D2DT_SOURCES})
target_include_directories(d2datatables_93854 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(d2datatables_93854 PRIVATE D2DT_BUILD_93854)
# Same file name as the 3.1.1 binary - the two live in different directories.
set_target_properties(d2datatables_93854 PROPERTIES OUTPUT_NAME d2datatables)
d2dt_set_output_dirs(d2datatables_93854 build_93854)

# The txt/json parse paths use std library facilities that throw (matches the
# /EHsc in build.bat).
if (MSVC)
  target_compile_options(d2datatables PRIVATE /EHsc)
  target_compile_options(d2datatables_93854 PRIVATE /EHsc)
else()
  target_compile_options(d2datatables PRIVATE -Wall -Wextra)
  target_compile_options(d2datatables_93854 PRIVATE -Wall -Wextra)
  # std::filesystem lives in a separate library before GCC 9 / Clang 10.
  if ((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0) OR
      (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10.0))
    target_link_libraries(d2datatables PRIVATE stdc++fs)
    target_link_libraries(d2datatables_93854 PRIVATE stdc++fs)
  endif()
endif()
