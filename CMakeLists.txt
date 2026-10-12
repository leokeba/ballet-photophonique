# Ballet Photophonique's code: an ESP-IDF component when built by idf.py (the
# firmware in firmware/ adds this directory), host library + tests otherwise.
set(PHOTOPHONIQUE_SRCS
    src/photophonique/timing.cpp
    src/photophonique/output.cpp
    src/photophonique/outputs_tree.cpp
    src/photophonique/midi.cpp
    src/photophonique/smf.cpp
    src/photophonique/recorder.cpp
    src/photophonique/recorder_tree.cpp)

if(ESP_PLATFORM)
    # TesserAPI, under whatever component name it was added with (a directory
    # in EXTRA_COMPONENT_DIRS, or the component manager's
    # "<namespace>__<name>"), as TesserKIT finds its own.
    idf_build_get_property(PHOTOPHONIQUE_BUILD_COMPONENTS BUILD_COMPONENTS)
    set(PHOTOPHONIQUE_TESSERAPI "")
    foreach(c ${PHOTOPHONIQUE_BUILD_COMPONENTS})
        string(TOLOWER "${c}" c_lower)
        if(c_lower MATCHES "(^|__)tesserapi$")
            set(PHOTOPHONIQUE_TESSERAPI ${c})
        endif()
    endforeach()
    if(NOT PHOTOPHONIQUE_TESSERAPI AND NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(FATAL_ERROR "ballet-photophonique needs TesserAPI as a component")
    endif()
    idf_component_register(SRCS ${PHOTOPHONIQUE_SRCS} src/photophonique/platform/esp/pwm_esp.cpp
                                src/photophonique/platform/esp/player_esp.cpp
                           INCLUDE_DIRS "src"
                           PRIV_REQUIRES esp_driver_ledc esp_timer)
    if(PHOTOPHONIQUE_TESSERAPI)
        idf_component_get_property(lib ${PHOTOPHONIQUE_TESSERAPI} COMPONENT_LIB)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${lib})
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(ballet_photophonique CXX)

option(PHOTOPHONIQUE_SANITIZE "Build with ASan/UBSan" ON)
set(TESSERKIT_DIR "" CACHE PATH "TesserKIT source tree (default: ../TesserKIT)")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TesserKIT, which brings TesserAPI (../TesserAPI next to it, or
# TESSERAPI_DIR), and its fake platform for the simulator (later).
if(NOT TESSERKIT_DIR)
    get_filename_component(TESSERKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TesserKIT" ABSOLUTE)
endif()
if(NOT EXISTS "${TESSERKIT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "TesserKIT not found at ${TESSERKIT_DIR}: clone it there, or set TESSERKIT_DIR")
endif()
set(TESSERKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${TESSERKIT_DIR}" "${CMAKE_BINARY_DIR}/tesserkit")

set(PHOTOPHONIQUE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -fno-exceptions -fno-rtti)

add_library(photophonique_core ${PHOTOPHONIQUE_SRCS})
target_include_directories(photophonique_core PUBLIC src)
target_link_libraries(photophonique_core PUBLIC tesser_core)
set_target_properties(photophonique_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(photophonique_core PRIVATE ${PHOTOPHONIQUE_WARNINGS})

enable_testing()
add_executable(photophonique_tests test/test_main.cpp)
target_link_libraries(photophonique_tests PRIVATE photophonique_core)
set_target_properties(photophonique_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(photophonique_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
add_test(NAME photophonique_tests COMMAND photophonique_tests)

if(PHOTOPHONIQUE_SANITIZE)
    # Every library in the binary, so it is instrumented alike.
    foreach(t tesser_core photophonique_core photophonique_tests)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
