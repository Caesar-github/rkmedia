#
# Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#

aux_source_directory(. EASY_MEDIA_SOURCE_FILES)

# ----------------------------------------------------------------------------
# Start module definition
# ----------------------------------------------------------------------------

option(RKMPP "compile: rkmpp wrapper" OFF)
option(RKMPP_ENCODER_OSD "compile: rkmpp encoder osd wrapper" OFF)
if(RKMPP)
  pkg_check_modules(ROCKCHIP_MPP REQUIRED rockchip_mpp)
  include_directories(rkmpp)
  include_directories(${ROCKCHIP_MPP_INCLUDE_DIRS}/rockchip/)
  set(EASY_MEDIA_DEPENDENT_LIBS
    ${EASY_MEDIA_DEPENDENT_LIBS} ${ROCKCHIP_MPP_LIBRARIES})

  if (RKMPP_ENCODER)
    if (RKMPP_ENCODER_OSD)
      add_definitions(-DMPP_SUPPORT_HW_OSD)
    endif()
    if(RKRGA)
      add_definitions(-DJPEG_RGA_OSD_ENABLE)
    endif()
  endif()

  add_subdirectory(rkmpp)

  option(UVC "compile: uvc" OFF)
  if(UVC)
     add_subdirectory(uvc)
  endif()

endif()

option(OGG "compile: ogg" OFF)
if(OGG)
  add_subdirectory(ogg)
endif()

if(FFMPEG)
add_subdirectory(ffmpeg)
endif()

option(LIVE555 "compile: live555" OFF)
if(LIVE555)
  if(LIVE555_SERVER)
    if(LIVE555_SERVER_H264)
      add_definitions(-DLIVE555_SERVER_H264)
    endif()
    if(LIVE555_SERVER_H265)
      add_definitions(-DLIVE555_SERVER_H265)
    endif()
  endif()
  find_package(LiveMedia)
  include_directories(${LIVEMEDIA_INCLUDE_DIRS})
  add_subdirectory(live555)
endif()

option(FLOW "compile: flow" ON)
if(FLOW)
  add_subdirectory(flow)
  set(EASY_MEDIA_DEPENDENT_LIBS ${EASY_MEDIA_DEPENDENT_LIBS} pthread)
endif()

option(STREAM "compile: stream" ON)
if(STREAM)
  add_subdirectory(stream)
endif()

if(FILTER)
  add_subdirectory(rkrga)
  add_subdirectory(guard)
  add_subdirectory(filter)
  add_subdirectory(rknn)
endif()

  add_subdirectory(c_api)

if(MOVE_DETECTION)
add_definitions(-DRK_MOVE_DETECTION)
set(EASY_MEDIA_DEPENDENT_LIBS ${EASY_MEDIA_DEPENDENT_LIBS} md_share)
endif()

if(OCCLUSION_DETECTION)
set(EASY_MEDIA_DEPENDENT_LIBS ${EASY_MEDIA_DEPENDENT_LIBS} od_share)
endif()

if(ROCKFACE)
    add_definitions(-DUSE_ROCKFACE)
endif()

if(ROCKX)
  add_definitions(-DUSE_ROCKX)
  include_directories(${ROCKX_HEADER_DIR})
  set(EASY_MEDIA_DEPENDENT_LIBS ${EASY_MEDIA_DEPENDENT_LIBS} rockx)
endif()

set(LIBRARY_VERSION 1.0.1)
set(LIBRARY_NAME easymedia)

add_library(${LIBRARY_NAME} SHARED ${EASY_MEDIA_SOURCE_FILES})
file(GLOB EASY_MEDIA_RELEASE_HEADERS ${CMAKE_SOURCE_DIR}/include/${LIBRARY_NAME}/*.h)
set_target_properties(${LIBRARY_NAME}
                      PROPERTIES PUBLIC_HEADER "${EASY_MEDIA_RELEASE_HEADERS}")
set_target_properties(${LIBRARY_NAME} PROPERTIES VERSION ${LIBRARY_VERSION})
set_target_properties(${LIBRARY_NAME} PROPERTIES SOVERSION 1)

if(SANITIZER)
  add_definitions(-fsanitize=address -static-libasan -g)
  target_link_libraries(${LIBRARY_NAME} libasan.a)
endif()

if(USE_RKAIQ)
  set(EASY_MEDIA_DEPENDENT_LIBS ${EASY_MEDIA_DEPENDENT_LIBS} RkAiq::RkAiq)
endif()

if(EASY_MEDIA_COMPILE_DEFINITIONS)
  add_definitions(${EASY_MEDIA_COMPILE_DEFINITIONS})
endif()

if(EASY_MEDIA_DEPENDENT_LIBS)
  target_link_libraries(${LIBRARY_NAME} ${EASY_MEDIA_DEPENDENT_LIBS})
endif()

# cmake-format: off
# message(headers: "${EASY_MEDIA_RELEASE_HEADERS}")
# message(files: "${EASY_MEDIA_SOURCE_FILES}")
# message(definitions: "${EASY_MEDIA_COMPILE_DEFINITIONS}")
# message(libs: "${EASY_MEDIA_DEPENDENT_LIBS}")
# cmake-format: on

install(TARGETS ${LIBRARY_NAME}
        LIBRARY DESTINATION "lib"
        PUBLIC_HEADER DESTINATION "include/easymedia")

configure_file("libeasymedia.pc.cmake" "libeasymedia.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libeasymedia.pc"
        DESTINATION "lib/pkgconfig/")
