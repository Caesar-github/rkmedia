// Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "common/sample_double_cam_isp.h"
#include "rkmedia_api.h"
#include <rga/RgaApi.h>
#include <rga/rga.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static MEDIA_BUFFER ir_mb;
static MEDIA_BUFFER rgb_mb;
static int video_width = 1920;
static int video_height = 1080;
static int disp_width = 720;
static int disp_height = 1280;
static int scale_width = 720;
static int scale_height = 640;

static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static void scale_buffer(void *src_p, int src_w, int src_h, int src_fmt,
                         void *dst_p, int dst_w, int dst_h, int dst_fmt,
                         int rotation) {
  rga_info_t src, dst;
  memset(&src, 0, sizeof(rga_info_t));
  src.fd = -1;
  src.virAddr = src_p;
  src.mmuFlag = 1;
  src.rotation = rotation;
  rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
  memset(&dst, 0, sizeof(rga_info_t));
  dst.fd = -1;
  dst.virAddr = dst_p;
  dst.mmuFlag = 1;
  rga_set_rect(&dst.rect, 0, 0, dst_w, dst_h, dst_w, dst_h, dst_fmt);
  if (c_RkRgaBlit(&src, &dst, NULL))
    printf("%s: rga fail\n", __func__);
}

static void compose_buffer(void *src_p, int src_w, int src_h, int src_fmt,
                           void *dst_p, int dst_w, int dst_h, int dst_fmt,
                           int x, int y, int w, int h) {
  rga_info_t src, dst;
  memset(&src, 0, sizeof(rga_info_t));
  src.fd = -1;
  src.virAddr = src_p;
  src.mmuFlag = 1;
  rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
  memset(&dst, 0, sizeof(rga_info_t));
  dst.fd = -1;
  dst.virAddr = dst_p;
  dst.mmuFlag = 1;
  rga_set_rect(&dst.rect, x, y, w, h, dst_w, dst_h, dst_fmt);
  if (c_RkRgaBlit(&src, &dst, NULL))
    printf("%s: rga fail\n", __func__);
}

static void *GetRgbBuffer(void *arg) {
  MEDIA_BUFFER mb = NULL;

  (void)arg;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_RGA, 0, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    scale_buffer(RK_MPI_MB_GetPtr(mb), disp_width, disp_height,
                 RK_FORMAT_YCbCr_420_SP, RK_MPI_MB_GetPtr(rgb_mb), scale_width,
                 scale_height, RK_FORMAT_YCbCr_420_SP, 0);
    compose_buffer(RK_MPI_MB_GetPtr(rgb_mb), scale_width, scale_height,
                   RK_FORMAT_YCbCr_420_SP, RK_MPI_MB_GetPtr(mb), disp_width,
                   disp_height, RK_FORMAT_YCbCr_420_SP, 0, 0, scale_width,
                   scale_height);
    pthread_mutex_lock(&mutex);
    compose_buffer(RK_MPI_MB_GetPtr(ir_mb), scale_width, scale_height,
                   RK_FORMAT_YCbCr_420_SP, RK_MPI_MB_GetPtr(mb), disp_width,
                   disp_height, RK_FORMAT_YCbCr_420_SP, 0, scale_height,
                   scale_width, scale_height);
    pthread_mutex_unlock(&mutex);

    RK_MPI_SYS_SendMediaBuffer(RK_ID_VO, 0, mb);

    RK_MPI_MB_ReleaseBuffer(mb);
  }

  return NULL;
}

static void *GetIrBuffer(void *arg) {
  MEDIA_BUFFER mb = NULL;

  (void)arg;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 1, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    pthread_mutex_lock(&mutex);
    scale_buffer(RK_MPI_MB_GetPtr(mb), video_width, video_height,
                 RK_FORMAT_YCbCr_420_SP, RK_MPI_MB_GetPtr(ir_mb), scale_width,
                 scale_height, RK_FORMAT_YCbCr_420_SP, HAL_TRANSFORM_ROT_270);
    pthread_mutex_unlock(&mutex);

    RK_MPI_MB_ReleaseBuffer(mb);
  }

  return NULL;
}

void usage(char *name) {
  printf("%s [-a id_dir] [-W video_width] [-H video_height]\n"
         "\t[-w disp_width] [-h disp_height] [-u ui(0/1)]\n",
         name);
  exit(-1);
}

int main(int argc, char *argv[]) {
  int ret = 0;
  int ui = 1;
  pthread_t trgb, tir;
  MB_IMAGE_INFO_S disp_info = {scale_width, scale_height, scale_width,
                               scale_height, IMAGE_TYPE_NV12};
  char *iq_dir = NULL;
  int ch;

  while ((ch = getopt(argc, argv, "a:W:H:w:h:u:")) != -1) {
    switch (ch) {
    case 'a':
      iq_dir = optarg;
      if (access(iq_dir, R_OK))
        usage(argv[0]);
      break;
    case 'W':
      video_width = atoi(optarg);
      break;
    case 'H':
      video_height = atoi(optarg);
      break;
    case 'w':
      disp_width = atoi(optarg);
      break;
    case 'h':
      disp_height = atoi(optarg);
      break;
    case 'u':
      ui = atoi(optarg);
      if (ui != 0 && ui != 1)
        usage(argv[0]);
      break;
    default:
      usage(argv[0]);
      break;
    }
  }

#ifdef RKAIQ
  rk_aiq_sys_ctx_t *ctx0 =
      aiq_double_cam_init(0, RK_AIQ_WORKING_MODE_NORMAL, iq_dir);
  if (!ctx0)
    return -1;
  rk_aiq_sys_ctx_t *ctx1 =
      aiq_double_cam_init(1, RK_AIQ_WORKING_MODE_NORMAL, iq_dir);
  if (!ctx1)
    return -1;
#endif // RKAIQ

  rgb_mb = RK_MPI_MB_CreateImageBuffer(&disp_info, RK_TRUE, 0);
  if (!rgb_mb) {
    printf("ERROR: no space left!\n");
    return -1;
  }
  ir_mb = RK_MPI_MB_CreateImageBuffer(&disp_info, RK_TRUE, 0);
  if (!ir_mb) {
    printf("ERROR: no space left!\n");
    return -1;
  }

  RK_MPI_SYS_Init();
  VI_CHN_ATTR_S vi_chn_attr;
  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = "rkispp_scale1";
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = video_width;
  vi_chn_attr.u32Height = video_height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 0);
  if (ret) {
    printf("Create vi[0] failed! ret=%d\n", ret);
    return -1;
  }

  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = "rkispp_scale1";
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = video_width;
  vi_chn_attr.u32Height = video_height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(1, 1, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(1, 1);
  if (ret) {
    printf("Create vi[1] failed! ret=%d\n", ret);
    return -1;
  }

  RGA_ATTR_S stRgaAttr;
  memset(&stRgaAttr, 0, sizeof(stRgaAttr));
  stRgaAttr.bEnBufPool = RK_TRUE;
  stRgaAttr.u16BufPoolCnt = 2;
  stRgaAttr.u16Rotaion = 90;
  stRgaAttr.stImgIn.u32X = 0;
  stRgaAttr.stImgIn.u32Y = 0;
  stRgaAttr.stImgIn.imgType = IMAGE_TYPE_NV12;
  stRgaAttr.stImgIn.u32Width = video_width;
  stRgaAttr.stImgIn.u32Height = video_height;
  stRgaAttr.stImgIn.u32HorStride = video_width;
  stRgaAttr.stImgIn.u32VirStride = video_height;
  stRgaAttr.stImgOut.u32X = 0;
  stRgaAttr.stImgOut.u32Y = 0;
  stRgaAttr.stImgOut.imgType = IMAGE_TYPE_NV12;
  stRgaAttr.stImgOut.u32Width = disp_width;
  stRgaAttr.stImgOut.u32Height = disp_height;
  stRgaAttr.stImgOut.u32HorStride = disp_width;
  stRgaAttr.stImgOut.u32VirStride = disp_height;
  ret = RK_MPI_RGA_CreateChn(0, &stRgaAttr);
  if (ret) {
    printf("Create rga[0] falied! ret=%d\n", ret);
    return -1;
  }

  VO_CHN_ATTR_S stVoAttr = {0};
  stVoAttr.pcDevNode = "/dev/dri/card0";
  stVoAttr.emPlaneType = VO_PLANE_OVERLAY;
  stVoAttr.enImgType = IMAGE_TYPE_NV12;
  stVoAttr.u16Zpos = 0;
  stVoAttr.stImgRect.s32X = 0;
  stVoAttr.stImgRect.s32Y = 0;
  stVoAttr.stImgRect.u32Width = disp_width;
  stVoAttr.stImgRect.u32Height = disp_height;
  stVoAttr.stDispRect.s32X = 0;
  stVoAttr.stDispRect.s32Y = 0;
  stVoAttr.stDispRect.u32Width = disp_width;
  stVoAttr.stDispRect.u32Height = disp_height;
  ret = RK_MPI_VO_CreateChn(0, &stVoAttr);
  if (ret) {
    printf("Create vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  // VO[1] for primary plane
  memset(&stVoAttr, 0, sizeof(stVoAttr));
  stVoAttr.pcDevNode = "/dev/dri/card0";
  stVoAttr.emPlaneType = VO_PLANE_PRIMARY;
  stVoAttr.enImgType = IMAGE_TYPE_RGB888;
  stVoAttr.u16Zpos = ui;
  stVoAttr.stImgRect.s32X = 0;
  stVoAttr.stImgRect.s32Y = 0;
  stVoAttr.stImgRect.u32Width = disp_width;
  stVoAttr.stImgRect.u32Height = disp_height;
  stVoAttr.stDispRect.s32X = 0;
  stVoAttr.stDispRect.s32Y = 0;
  stVoAttr.stDispRect.u32Width = disp_width;
  stVoAttr.stDispRect.u32Height = disp_height;
  ret = RK_MPI_VO_CreateChn(1, &stVoAttr);
  if (ret) {
    printf("Create vo[1] failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S stSrcChn = {0};
  MPP_CHN_S stDestChn = {0};

  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32ChnId = 0;
  stDestChn.enModId = RK_ID_RGA;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Bind vi[0] to rga[0] failed! ret=%d\n", ret);
    return -1;
  }

  if (pthread_create(&trgb, NULL, GetRgbBuffer, NULL)) {
    printf("create GetRgbBuffer thread failed!\n");
    return -1;
  }
  if (pthread_create(&tir, NULL, GetIrBuffer, NULL)) {
    printf("create GetIrBuffer thread failed!\n");
    return -1;
  }
  ret = RK_MPI_VI_StartStream(1, 1);
  if (ret) {
    printf("Start Vi[1] failed! ret=%d\n", ret);
    return -1;
  }

  printf("%s initial finish\n", __func__);
  signal(SIGINT, sigterm_handler);
  while (!quit) {
    usleep(500000);
  }

  printf("%s exit!\n", __func__);
  pthread_join(trgb, NULL);
  pthread_join(tir, NULL);
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32ChnId = 0;
  stDestChn.enModId = RK_ID_RGA;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("UnBind vi[0] to rga[0] failed! ret=%d\n", ret);
    return -1;
  }

  RK_MPI_MB_ReleaseBuffer(rgb_mb);
  RK_MPI_MB_ReleaseBuffer(ir_mb);
  RK_MPI_VO_DestroyChn(0);
  RK_MPI_VO_DestroyChn(1);
  RK_MPI_RGA_DestroyChn(0);
  RK_MPI_RGA_DestroyChn(1);
  RK_MPI_VI_DisableChn(0, 0);
  RK_MPI_VI_DisableChn(1, 1);

#ifdef RKAIQ
  aiq_double_cam_exit(ctx0);
  aiq_double_cam_exit(ctx1);
#endif

  return 0;
}
