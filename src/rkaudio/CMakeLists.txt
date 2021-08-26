#
# Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#

# vi: set noexpandtab syntax=cmake:

option(RKAUDIO "compile: rkaudio wrapper" OFF)

install(FILES ${CMAKE_CURRENT_LIST_DIR}/libthird_media.so DESTINATION "lib")