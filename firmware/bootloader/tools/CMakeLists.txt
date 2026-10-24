# Host builds of the bootloader's HAL-free code: the OLED screen proofs (proof), the image check
# (imagecheck) and the drive's text files (drivetext). build.sh --proofs and build.sh --selftest
# drive them.

cmake_minimum_required(VERSION 3.16)
project(cubevox_bootloader_tools C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../boards/cubevox)
set(REPO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
include_directories(${BOARD_DIR} ${REPO_DIR}/firmware/cubevox/src/board ${REPO_DIR}/tools/font-editor/fonts)
add_compile_options(-Wall -Wextra -Werror)

add_executable(proof proof.c ${BOARD_DIR}/screens.c ${BOARD_DIR}/fonts.cpp ${BOARD_DIR}/image_check.c)
add_executable(imagecheck imagecheck.c ${BOARD_DIR}/image_check.c)

# board.h is the cubevox board's; the version a real build takes from git is a placeholder here.
add_executable(drivetext drivetext.c ${BOARD_DIR}/diag.c ${BOARD_DIR}/image_check.c)
target_compile_definitions(drivetext PRIVATE CV_BOARD_WEACT=0 CV_TINYUF2_VERSION="host-sample")
