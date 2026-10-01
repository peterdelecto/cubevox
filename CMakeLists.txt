cmake_minimum_required(VERSION 3.20)
project(cubevox CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED glfw3)
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
add_custom_command(TARGET cubevox-proto POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          ${CMAKE_BINARY_DIR}/cubevox-proto.app ${CMAKE_SOURCE_DIR}/cubevox-proto.app
  VERBATIM)

# cubevox-render: loop in, WAV out, no window.
add_executable(cubevox-render tools/render.cpp tools/miniaudio_impl.cpp)
set_source_files_properties(tools/render.cpp PROPERTIES COMPILE_OPTIONS "${CUBEVOX_ENGINE_FLAGS}")
target_link_libraries(cubevox-render PRIVATE engine
  ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK}
  ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK})

# tests
enable_testing()
add_executable(unison_test test/unison_test.cpp)
target_compile_options(unison_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(unison_test PRIVATE engine)
add_test(NAME unison COMMAND unison_test)
add_executable(harmony_test test/harmony_test.cpp)
target_compile_options(harmony_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(harmony_test PRIVATE engine)
add_test(NAME harmony COMMAND harmony_test)
add_executable(octave_test test/octave_test.cpp)
target_compile_options(octave_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(octave_test PRIVATE engine)
add_test(NAME octave COMMAND octave_test)
add_executable(slapback_test test/slapback_test.cpp)
target_compile_options(slapback_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(slapback_test PRIVATE engine)
add_test(NAME slapback COMMAND slapback_test)
add_executable(distortion_test test/distortion_test.cpp)
target_compile_options(distortion_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(distortion_test PRIVATE engine)
add_test(NAME distortion COMMAND distortion_test)
add_executable(spring_test test/spring_test.cpp)
target_compile_options(spring_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(spring_test PRIVATE engine)
add_test(NAME spring COMMAND spring_test)
add_test(NAME proto_layout
  COMMAND $<TARGET_FILE:cubevox-proto> --layout)

add_executable(chasm_test test/chasm_test.cpp)
target_compile_options(chasm_test PRIVATE ${CUBEVOX_ENGINE_FLAGS})
target_link_libraries(chasm_test PRIVATE engine)
add_test(NAME chasm COMMAND chasm_test)
