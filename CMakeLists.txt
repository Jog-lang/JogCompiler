cmake_minimum_required(VERSION 3.20)

project(jog C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(CT_WARNINGS -Wall -Wextra -Wpedantic)
endif()

add_library(ct_util STATIC
    tools/arena.c
    tools/map.c
    tools/diag.c
    tools/file.c
    tools/da.c
    tools/coro.c
    tools/coro_switch.c
    tools/str.c
    tools/intern.c
    tools/fs.c
    tools/proc.c
)
# The context switch is GNU C. The rest of the library stays C17.
set_source_files_properties(tools/coro_switch.c PROPERTIES
    COMPILE_OPTIONS "-std=gnu17"
)
target_include_directories(ct_util PUBLIC tools)
target_compile_options(ct_util PRIVATE ${CT_WARNINGS})

# Every compiler source except the driver's main goes into the library, so
# parallel streams add files without editing this list. Re-run cmake after
# adding a file.
file(GLOB JOGC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/compiler/src/*.c)
list(REMOVE_ITEM JOGC_SOURCES ${CMAKE_SOURCE_DIR}/compiler/src/main.c)
add_library(jogc STATIC ${JOGC_SOURCES})
target_include_directories(jogc PUBLIC compiler/src)
target_link_libraries(jogc PUBLIC ct_util)
target_compile_options(jogc PRIVATE ${CT_WARNINGS})

add_executable(jog compiler/src/main.c)
target_link_libraries(jog PRIVATE jogc)
if(WIN32)
    # CommandLineToArgvW, for the UTF-16 command line.
    target_link_libraries(jog PRIVATE shell32)
endif()
target_compile_options(jog PRIVATE ${CT_WARNINGS})

# The runtime, as a library for runtime tests. Programs built by jog compile
# runtime/*.c themselves.
file(GLOB JOGRT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.c)
list(REMOVE_ITEM JOGRT_SOURCES ${CMAKE_SOURCE_DIR}/runtime/jogrt_main.c)
if(JOGRT_SOURCES)
    add_library(jogrt STATIC ${JOGRT_SOURCES})
    target_include_directories(jogrt PUBLIC runtime)
    target_compile_options(jogrt PRIVATE ${CT_WARNINGS})
    set_source_files_properties(${CMAKE_SOURCE_DIR}/runtime/jogrt_switch.c PROPERTIES
        COMPILE_OPTIONS "-std=gnu17"
    )
endif()

enable_testing()

# Each tests/<name>_test.c is its own executable and ctest entry. Tests for
# compiler code link jogc; runtime_*_test.c link jogrt instead.
file(GLOB CT_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*_test.c)
foreach(test_source ${CT_TESTS})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    if(test_name MATCHES "^runtime_")
        target_link_libraries(${test_name} PRIVATE jogrt)
    else()
        target_link_libraries(${test_name} PRIVATE jogc)
    endif()
    target_compile_options(${test_name} PRIVATE ${CT_WARNINGS})
    target_compile_definitions(${test_name} PRIVATE CT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
