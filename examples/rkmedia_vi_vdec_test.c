// Copyright 2021 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rkmedia_api.h"

static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

#if 0
static void *GetMediaBufferDec(void *arg) {
  (void)arg;
  MEDIA_BUFFER mb = NULL;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VDEC, 0, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    MB_IMAGE_INFO_S stImageInfo = {0};
    int ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo);
    if (ret)
      printf("Warn: Get image info failed! ret = %d\n", ret);

    printf("Dec Get Frame:ptr:%p, fd:%d, size:%zu, mode:%d, channel:%d, "
           "timestamp:%lld, ImgInfo:<wxh %dx%d, fmt 0x%x>\n",
           RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb), RK_MPI_MB_GetSize(mb),
           RK_MPI_MB_GetModeID(mb), RK_MPI_MB_GetChannelID(mb),
           RK_MPI_MB_GetTimestamp(mb), stImageInfo.u32Width,
           stImageInfo.u32Height, stImageInfo.enImgType);

    RK_MPI_MB_ReleaseBuffer(mb);
  }

  return NULL;
}
#endif

static RK_CHAR optstr[] = "?::w:h:d:";
static const struct option long_options[] = {
    {"device_name", required_argument, NULL, 'd'},
    {"width", required_argument, NULL, 'w'},
    {"height", required_argument, NULL, 'h'},
    {"help", optional_argument, NULL, '?'},
    {NULL, 0, NULL, 0},
};

static void print_usage(const RK_CHAR *name) {
  printf("usage example:\n");
  printf("\t%s [-w 1920] [-h 1080] [-d /dev/video0]\n", name);
  printf("\t-w | --width: VI width, Default:1280\n");
  printf("\t-h | --heght: VI height, Default:720\n");
  printf("\t-d | --device_name: set device node(v4l2), Default:/dev/video0\n");
}

int main(int argc, char *argv[]) {
  RK_U32 u32DispWidth = 720;
  RK_U32 u32DispHeight = 1280;
  RK_U32 u32Width = 1280;
  RK_U32 u32Height = 720;
  RK_CHAR *pDeviceName = "/dev/video0";
  int c;
  int ret = 0;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    switch (c) {
    case 'w':
      u32Width = atoi(optarg);
      break;
    case 'h':
      u32Height = atoi(optarg);
      break;
    case 'd':
      pDeviceName = optarg;
      break;
    case '?':
    default:
      print_usage(argv[0]);
      return 0;
    }
  }

  printf("#####Device: %s\n", pDeviceName);
  printf("#####Resolution: %dx%d\n", u32Width, u32Height);
  signal(SIGINT, sigterm_handler);

  RK_MPI_SYS_Init();
  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = pDeviceName;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = u32Width;
  vi_chn_attr.u32Height = u32Height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_JPEG;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP;
  ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 0);
  if (ret) {
    printf("Create VI[0] failed! ret=%d\n", ret);
    return -1;
  }

  // VDEC
  VDEC_CHN_ATTR_S stVdecAttr;
  memset(&stVdecAttr, 0, sizeof(stVdecAttr));
  stVdecAttr.enCodecType = RK_CODEC_TYPE_MJPEG;
  stVdecAttr.enMode = VIDEO_MODE_FRAME;
  stVdecAttr.enDecodecMode = VIDEO_DECODEC_HADRWARE;

  ret = RK_MPI_VDEC_CreateChn(0, &stVdecAttr);
  if (ret) {
    printf("Create Vdec[0] failed! ret=%d\n", ret);
    return -1;
  }

  // rga0 for primary plane
  RGA_ATTR_S stRgaAttr;
  memset(&stRgaAttr, 0, sizeof(stRgaAttr));
  stRgaAttr.bEnBufPool = RK_TRUE;
  stRgaAttr.u16BufPoolCnt = 3;
  stRgaAttr.u16Rotaion = 90;
  stRgaAttr.stImgIn.u32X = 0;
  stRgaAttr.stImgIn.u32Y = 0;
  stRgaAttr.stImgIn.imgType = IMAGE_TYPE_NV16;
  stRgaAttr.stImgIn.u32Width = u32Width;
  stRgaAttr.stImgIn.u32Height = u32Height;
  stRgaAttr.stImgIn.u32HorStride = u32Width;
  stRgaAttr.stImgIn.u32VirStride = u32Height;
  stRgaAttr.stImgOut.u32X = 0;
  stRgaAttr.stImgOut.u32Y = 0;
  stRgaAttr.stImgOut.imgType = IMAGE_TYPE_RGB888;
  stRgaAttr.stImgOut.u32Width = u32DispWidth;
  stRgaAttr.stImgOut.u32Height = u32DispHeight;
  stRgaAttr.stImgOut.u32HorStride = u32DispWidth;
  stRgaAttr.stImgOut.u32VirStride = u32DispHeight;
  ret = RK_MPI_RGA_CreateChn(0, &stRgaAttr);
  if (ret) {
    printf("Create rga[0] falied! ret=%d\n", ret);
    return -1;
  }

  VO_CHN_ATTR_S stVoAttr = {0};
  // VO[0] for primary plane
  stVoAttr.pcDevNode = "/dev/dri/card0";
  stVoAttr.emPlaneType = VO_PLANE_PRIMARY;
  stVoAttr.enImgType = IMAGE_TYPE_RGB888;
  stVoAttr.u16Zpos = 0;
  stVoAttr.stDispRect.s32X = 0;
  stVoAttr.stDispRect.s32Y = 0;
  stVoAttr.stDispRect.u32Width = u32DispWidth;
  stVoAttr.stDispRect.u32Height = u32DispHeight;
  ret = RK_MPI_VO_CreateChn(0, &stVoAttr);
  if (ret) {
    printf("Create vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S VdecChn, ViChn;
  VdecChn.enModId = RK_ID_VDEC;
  VdecChn.s32DevId = 0;
  VdecChn.s32ChnId = 0;
  ViChn.enModId = RK_ID_VI;
  ViChn.s32DevId = 0;
  ViChn.s32ChnId = 0;
  // VI->VDEC
  ret = RK_MPI_SYS_Bind(&ViChn, &VdecChn);
  if (ret) {
    printf("Bind VI[0] to VDEC[0] failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S RgaChn;
  RgaChn.enModId = RK_ID_RGA;
  RgaChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&VdecChn, &RgaChn);
  if (ret) {
    printf("Bind VDEC[0] to RGA[0] failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S VoChn;
  VoChn.enModId = RK_ID_VO;
  VoChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&RgaChn, &VoChn);
  if (ret) {
    printf("Bind RGA[0] to VO[0] failed! ret=%d\n", ret);
    return -1;
  }

#if 0
  pthread_t dec_thread;
  pthread_create(&dec_thread, NULL, GetMediaBufferDec, NULL);
#endif

  printf("%s initial finish\n", __func__);

  while (!quit) {
    usleep(500000);
  }

#if 0
  pthread_join(dec_thread, NULL);
#endif

  ret = RK_MPI_SYS_UnBind(&RgaChn, &VoChn);
  if (ret)
    printf("UnBind RGA[0] to VO[0] failed! ret=%d\n", ret);

  ret = RK_MPI_SYS_UnBind(&VdecChn, &RgaChn);
  if (ret)
    printf("UnBind VDEC[0] to RGA[0] failed! ret=%d\n", ret);

  ret = RK_MPI_SYS_UnBind(&ViChn, &VdecChn);
  if (ret)
    printf("UnBind VI[0] to VDEC[0] failed! ret=%d\n", ret);

  RK_MPI_VO_DestroyChn(0);
  RK_MPI_RGA_DestroyChn(0);
  RK_MPI_VDEC_DestroyChn(0);
  RK_MPI_VI_DisableChn(0, 0);

  printf("%s exit!\n", __func__);
  return 0;
}
