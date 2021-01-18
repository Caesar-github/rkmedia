#
# Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#

# vi: set noexpandtab syntax=cmake:

project(easymedia_ffmpeg_test)

set(FFMPEG_TEST_DEPENDENT_LIBS easymedia)

set(CMAKE_CXX_STANDARD 11)

add_definitions(-DDEBUG)

add_executable(ffmpeg_enc_mux_test ffmpeg_enc_mux_test.cc)
target_compile_features(ffmpeg_enc_mux_test PRIVATE cxx_std_17)
add_dependencies(ffmpeg_enc_mux_test easymedia)
target_link_libraries(ffmpeg_enc_mux_test ${FFMPEG_TEST_DEPENDENT_LIBS})
install(TARGETS ffmpeg_enc_mux_test RUNTIME DESTINATION "bin")
