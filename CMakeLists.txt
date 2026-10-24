cmake_minimum_required(VERSION 3.20)
project(cubevox C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release builds compile GLFW in statically so the app runs on a Mac without
# Homebrew (tools/package.sh). Dev builds use the installed GLFW.
option(CUBEVOX_BUNDLE_GLFW "Build GLFW from source and link it statically" OFF)
if(CUBEVOX_BUNDLE_GLFW)
  include(FetchContent)
  set(GLFW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
  FetchContent_MakeAvailable(glfw)
  set(glfw3_FOUND TRUE)
else()
  find_package(glfw3 QUIET)
  if(NOT glfw3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED glfw3)
  endif()
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# engine: header-only, firmware-portable. The contract flags below are applied
# to the targets that compile only engine code (test, render); the GUI target
# carries vendored ImGui and ObjC that are not held to them.
add_library(engine INTERFACE)
target_include_directories(engine INTERFACE ${CMAKE_SOURCE_DIR})
set(CUBEVOX_ENGINE_FLAGS -Wall -Wextra -Werror -fno-exceptions -fno-rtti)

function(cubevox_gui_target name)
  add_executable(${name}
    ${ARGN}
    emulator/open_panel.mm
    tools/miniaudio_impl.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(${name} PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
  target_compile_definitions(${name} PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(${name} PRIVATE engine
    ${COCOA_FRAMEWORK} ${OPENGL_FRAMEWORK} ${IOKIT_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK})
  if(glfw3_FOUND)
    target_link_libraries(${name} PRIVATE glfw)
  else()
    target_include_directories(${name} PRIVATE ${GLFW_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE ${GLFW_LIBRARY_DIRS})
    target_link_libraries(${name} PRIVATE ${GLFW_LIBRARIES})
  endif()
  set_target_properties(${name} PROPERTIES
    OUTPUT_NAME "${name}"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "${name}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.peterdelecto.${name}"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/emulator/Info.plist.in")
endfunction()

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)

# cubevox-proto: the prototype face. The bundle builds under build/ and a
# symlink at the project root keeps it Finder-launchable across rebuilds.
cubevox_gui_target(cubevox-proto emulator/main.cpp)
# The face shows VERSION.txt bottom left; editing the file re-runs configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION.txt)
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION.txt CUBEVOX_VERSION LIMIT_COUNT 1)
target_compile_definitions(cubevox-proto PRIVATE CUBEVOX_VERSION="v${CUBEVOX_VERSION}")
if(NOT CUBEVOX_BUNDLE_GLFW)
  add_custom_command(TARGET cubevox-proto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/cubevox-proto.app ${CMAKE_SOURCE_DIR}/cubevox-proto.app
    VERBATIM)
endif()

# cubevox-render: loop in, WAV out, no window.
add_executable(cubevox-render tools/render.cpp tools/miniaudio_impl.cpp)
set_source_files_properties(tools/render.cpp PROPERTIES COMPILE_OPTIONS "${CUBEVOX_ENGINE_FLAGS}")
target_link_libraries(cubevox-render PRIVATE engine
  ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK}
  ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK})

# tests
enable_testing()

# Engine tests compile like the firmware: optimised, with the firmware's float contraction.
set(CUBEVOX_ENGINE_TEST_FLAGS ${CUBEVOX_ENGINE_FLAGS} -O2 -ffp-contract=fast)
set(CUBEVOX_CORE_DIR ${CMAKE_SOURCE_DIR}/firmware/cubevox/src/core)
# The registry's knob mapping, for tests that set an engine the way the box does.
set(CUBEVOX_REGISTRY_SOURCES ${CUBEVOX_CORE_DIR}/registry.cpp ${CUBEVOX_CORE_DIR}/chain_params.cpp
                             ${CUBEVOX_CORE_DIR}/unison_rate.cpp)

# test/<name>_test.cpp as ctest <name>, plus any extra sources.
function(cubevox_engine_test name)
  add_executable(${name}_test test/${name}_test.cpp ${ARGN})
  target_include_directories(${name}_test PRIVATE ${CMAKE_SOURCE_DIR}/firmware/cubevox/src)
  target_compile_options(${name}_test PRIVATE ${CUBEVOX_ENGINE_TEST_FLAGS})
  target_link_libraries(${name}_test PRIVATE engine)
  add_test(NAME ${name} COMMAND ${name}_test)
endfunction()

cubevox_engine_test(unison ${CUBEVOX_CORE_DIR}/unison_rate.cpp)
cubevox_engine_test(reverb_clean)
cubevox_engine_test(octave)
cubevox_engine_test(autotune)
cubevox_engine_test(pitch)
cubevox_engine_test(fastmath)
cubevox_engine_test(smooth)
cubevox_engine_test(slapback)
cubevox_engine_test(distortion)
add_test(NAME proto_layout
  COMMAND $<TARGET_FILE:cubevox-proto> --layout)
cubevox_engine_test(chasm)
cubevox_engine_test(polish)
cubevox_engine_test(polish_comp)
cubevox_engine_test(soft_ceiling)
cubevox_engine_test(spring)
cubevox_engine_test(gate)
cubevox_engine_test(level ${CUBEVOX_REGISTRY_SOURCES})
cubevox_engine_test(knob_step ${CUBEVOX_REGISTRY_SOURCES})
cubevox_engine_test(stage_feedback)

# firmware core and ui tiers: every src/core/*.cpp and src/ui/*.cpp must compile on the host
# (no Arduino, no HAL, no board/ include).
file(GLOB CUBEVOX_FW_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/firmware/cubevox/src/core/*.cpp
                                  ${CMAKE_SOURCE_DIR}/firmware/cubevox/src/ui/*.cpp)
# boot_decide.c is the bootloader's pure policy; the host build compiles it as C99.
set(CUBEVOX_BOOT_DECIDE_DIR ${CMAKE_SOURCE_DIR}/firmware/bootloader/boards/cubevox)
# Test sources are one file per subject in test/firmware/. main.cpp runs them in a fixed order.
add_executable(firmware_test
               test/firmware/main.cpp
               test/firmware/support.cpp
               test/firmware/boot_decide.cpp
               test/firmware/core_boot_health.cpp
               test/firmware/core_canvas.cpp
               test/firmware/core_crc32.cpp
               test/firmware/core_entry_log.cpp
               test/firmware/core_input.cpp
               test/firmware/core_layout.cpp
               test/firmware/core_layout_fade.cpp
               test/firmware/core_level.cpp
               test/firmware/core_panel_test.cpp
               test/firmware/core_protocol.cpp
               test/firmware/core_protocol_fuzz.cpp
               test/firmware/core_record.cpp
               test/firmware/core_registry.cpp
               test/firmware/core_tx_queue.cpp
               test/firmware/ui_face.cpp
               test/firmware/ui_gui.cpp
               test/firmware/ui_menu.cpp
               test/firmware/ui_screens.cpp
               test/firmware/ui_splash.cpp
               ${CUBEVOX_FW_CORE_SOURCES}
               ${CUBEVOX_BOOT_DECIDE_DIR}/boot_decide.c)
target_include_directories(firmware_test PRIVATE ${CMAKE_SOURCE_DIR}/firmware/cubevox/src
                                                 ${CUBEVOX_BOOT_DECIDE_DIR}
                                                 ${CMAKE_SOURCE_DIR}/tools/font-editor/fonts)
set_target_properties(firmware_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(firmware_test PRIVATE CUBEVOX_OUT_DIR="${CMAKE_BINARY_DIR}"
  CUBEVOX_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/test/firmware/golden"
  CUBEVOX_FW_VERSION="v${CUBEVOX_VERSION}")
target_compile_options(firmware_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
# Optimised like the firmware, so optimisation-only warnings reach the sanitizers, with the same
# float contraction as the target. The extra warnings matter on a single-precision FPU.
target_compile_options(firmware_test PRIVATE -O2 -g -ffp-contract=fast
                       -Wshadow -Wdouble-promotion -Wundef -Wformat=2)
target_link_libraries(firmware_test PRIVATE engine)
# ASan + UBSan on this target only; any report aborts the process, so ctest fails.
set(CUBEVOX_SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
target_compile_options(firmware_test PRIVATE ${CUBEVOX_SANITIZE})
target_link_options(firmware_test PRIVATE ${CUBEVOX_SANITIZE})
add_test(NAME firmware COMMAND firmware_test)
add_test(NAME bench_ops_map COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/bench_ops_check.py)
add_test(NAME stamp_image_selftest COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/stamp_image.py selftest)
add_test(NAME tooling_scripts COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/scripts_check.py)

# hello_dump prints the firmware's real `hello` reply. app/fixtures/hello.json must equal it.
add_executable(hello_dump tools/hello_dump.cpp ${CUBEVOX_FW_CORE_SOURCES})
target_include_directories(hello_dump PRIVATE ${CMAKE_SOURCE_DIR}/firmware/cubevox/src
                                              ${CMAKE_SOURCE_DIR}/tools/font-editor/fonts)
target_compile_options(hello_dump PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(hello_dump PRIVATE engine)
add_test(NAME hello_fixture
         COMMAND ${CMAKE_COMMAND} -DDUMP=$<TARGET_FILE:hello_dump>
                 -DFIXTURE=${CMAKE_SOURCE_DIR}/app/fixtures/hello.json
                 -P ${CMAKE_SOURCE_DIR}/tools/hello_fixture_check.cmake)
add_custom_target(hello_fixture_refresh
                  COMMAND ${CMAKE_COMMAND} -DREFRESH=1 -DDUMP=$<TARGET_FILE:hello_dump>
                          -DFIXTURE=${CMAKE_SOURCE_DIR}/app/fixtures/hello.json
                          -P ${CMAKE_SOURCE_DIR}/tools/hello_fixture_check.cmake
                  DEPENDS hello_dump VERBATIM)

# Emulator-to-firmware parity: knob position -> engine params, both sides (test/parity_test.cpp).
add_executable(parity test/parity_test.cpp ${CUBEVOX_FW_CORE_SOURCES})
target_include_directories(parity PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/firmware/cubevox/src
                                          ${CMAKE_SOURCE_DIR}/tools/font-editor/fonts)
target_compile_definitions(parity PRIVATE CUBEVOX_OUT_DIR="${CMAKE_BINARY_DIR}")
target_compile_options(parity PRIVATE ${CUBEVOX_ENGINE_FLAGS} ${CUBEVOX_SANITIZE})
target_link_options(parity PRIVATE ${CUBEVOX_SANITIZE})
target_link_libraries(parity PRIVATE engine)
add_test(NAME parity COMMAND parity)
