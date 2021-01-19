// Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/sample_common.h"
#include "rkmedia_api.h"

#define ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define WIDTH 720
#define HEIGHT 1280

typedef struct {
  char *input_video;
  int input_width;
  int input_height;
  IMAGE_TYPE_E input_type;
} stDvr;

RK_U32 u32DispWidth = WIDTH;
RK_U32 u32DispHeight = HEIGHT;

RK_U16 input[8];

stDvr dvr8[8] = {
    {"/dev/video30", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video31", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video32", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video33", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video37", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video38", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video39", 1920, 1080, IMAGE_TYPE_NV12},
    {"/dev/video40", 1920, 1080, IMAGE_TYPE_NV12},
};

RECT_S area_2x4[8] = {
    {0, 0, WIDTH / 2, HEIGHT / 4},
    {WIDTH / 2, 0, WIDTH / 2, HEIGHT / 4},
    {0, HEIGHT / 4, WIDTH / 2, HEIGHT / 4},
    {WIDTH / 2, HEIGHT / 4, WIDTH / 2, HEIGHT / 4},
    {0, HEIGHT / 2, WIDTH / 2, HEIGHT / 4},
    {WIDTH / 2, HEIGHT / 2, WIDTH / 2, HEIGHT / 4},
    {0, HEIGHT * 3 / 4, WIDTH / 2, HEIGHT / 4},
    {WIDTH / 2, HEIGHT * 3 / 4, WIDTH / 2, HEIGHT / 4},
};

RECT_S line_2x4[4] = {
    {0, HEIGHT / 4, WIDTH, 2},
    {0, HEIGHT / 2, WIDTH, 2},
    {0, HEIGHT * 3 / 4, WIDTH, 2},
    {WIDTH / 2, 0, 2, HEIGHT},
};

RECT_S area_2x2[4] = {
    {0, 0, WIDTH / 2, HEIGHT / 2},
    {WIDTH / 2, 0, WIDTH / 2, HEIGHT / 2},
    {0, HEIGHT / 2, WIDTH / 2, HEIGHT / 2},
    {WIDTH / 2, HEIGHT / 2, WIDTH / 2, HEIGHT / 2},
};

RECT_S line_2x2[2] = {
    {0, HEIGHT / 2, WIDTH, 2}, {WIDTH / 2, 0, 2, HEIGHT},
};

static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static int dvr_enable(stDvr *dvr, RK_U16 dvr_num) {
  int ret = 0;

  for (RK_U16 i = 0; i < dvr_num; i++) {
    if (!dvr[i].input_video)
      continue;
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.pcVideoNode = dvr[i].input_video;
    vi_chn_attr.u32BufCnt = 4;
    vi_chn_attr.u32Width = dvr[i].input_width;
    vi_chn_attr.u32Height = dvr[i].input_height;
    vi_chn_attr.enPixFmt = dvr[i].input_type;
    vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
    vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP;
    ret = RK_MPI_VI_SetChnAttr(0, i, &vi_chn_attr);
    ret |= RK_MPI_VI_EnableChn(0, i);
    if (ret) {
      printf("Create vi[%d] failed! ret=%d\n", i, ret);
      return -1;
    }
  }
  return 0;
}

static int dvr_bind(RK_U16 *in, stDvr *dvr, RK_U16 dvr_num, RECT_S *area,
                    RK_U16 area_num, RECT_S *line, RK_U16 line_num) {
  int ret = 0;
  VMIX_DEV_INFO_S stDevInfo;
  memset(&stDevInfo, 0, sizeof(stDevInfo));
  stDevInfo.enImgType = IMAGE_TYPE_NV12;
  stDevInfo.u16Fps = 30;
  stDevInfo.u32ImgWidth = u32DispWidth;
  stDevInfo.u32ImgHeight = u32DispHeight;
  for (RK_U16 i = 0; i < dvr_num && i < area_num; i++) {
    stDevInfo.stChnInfo[i].stInRect.s32X = 0;
    stDevInfo.stChnInfo[i].stInRect.s32Y = 0;
    stDevInfo.stChnInfo[i].stInRect.u32Width = dvr[i].input_width;
    stDevInfo.stChnInfo[i].stInRect.u32Height = dvr[i].input_height;
    stDevInfo.stChnInfo[i].stOutRect.s32X = area[i].s32X;
    stDevInfo.stChnInfo[i].stOutRect.s32Y = area[i].s32Y;
    stDevInfo.stChnInfo[i].stOutRect.u32Width = area[i].u32Width;
    stDevInfo.stChnInfo[i].stOutRect.u32Height = area[i].u32Height;
    printf("#CHN[%d]:IN<0,0,%d,%d> --> Out<%d,%d,%d,%d>\n", i,
           dvr[i].input_width, dvr[i].input_height, area[i].s32X, area[i].s32Y,
           area[i].u32Width, area[i].u32Height);
    stDevInfo.u16ChnCnt++;
  }

  ret = RK_MPI_VMIX_CreateDev(0, &stDevInfo);
  if (ret) {
    printf("Init VMIX device failed! ret=%d\n", ret);
    return -1;
  }

  for (RK_U16 i = 0; i < dvr_num && i < area_num; i++) {
    ret = RK_MPI_VMIX_EnableChn(0, i);
    if (ret) {
      printf("Enable VM[0]:Chn[%d] failed! ret=%d\n", i, ret);
      return -1;
    }
  }

  VMIX_LINE_INFO_S stVmLine;
  memset(&stVmLine, 0, sizeof(stVmLine));
  for (RK_U16 i = 0; i < line_num; i++) {
    stVmLine.stLines[stVmLine.u32LineCnt].s32X = line[i].s32X;
    stVmLine.stLines[stVmLine.u32LineCnt].s32Y = line[i].s32Y;
    stVmLine.stLines[stVmLine.u32LineCnt].u32Width = line[i].u32Width;
    stVmLine.stLines[stVmLine.u32LineCnt].u32Height = line[i].u32Height;
    stVmLine.u32LineCnt++;
  }
  /* only last channel need line info */
  ret = RK_MPI_VMIX_SetLineInfo(0, stDevInfo.u16ChnCnt - 1, stVmLine);
  if (ret) {
    printf("Set VM[0] line info failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S stSrcChn = {0};
  MPP_CHN_S stDestChn = {0};

  printf("#Bind VMX[0] to VO[0]....\n");
  stSrcChn.enModId = RK_ID_VMIX;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = 0; // invalid
  stDestChn.enModId = RK_ID_VO;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Bind VMX[0] to vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  for (RK_U16 i = 0; i < dvr_num && i < area_num; i++) {
    printf("#Bind VI[%u] to VM[0]:Chn[%u]....\n", i, i);
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = 0;
    stSrcChn.s32ChnId = in[i];
    stDestChn.enModId = RK_ID_VMIX;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = i;
    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (ret) {
      printf("Bind vi[%d] to vmix[0]:Chn[%u] failed! ret=%d\n", i, i, ret);
      return -1;
    }
    //    RK_MPI_SYS_DumpChn(RK_ID_VMIX);
    //    getchar();
  }

  return 0;
}

static void dvr_unbind(RK_U16 *in, RK_U16 dvr_num, RK_U16 area_num) {
  int ret = 0;
  MPP_CHN_S stSrcChn = {0};
  MPP_CHN_S stDestChn = {0};
  printf("#UnBind VMX[0] to VO[0]....\n");
  stSrcChn.enModId = RK_ID_VMIX;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = 0; // invalid
  stDestChn.enModId = RK_ID_VO;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("UnBind VMX[0] to vo[0] failed! ret=%d\n", ret);
  }

  for (RK_U16 i = 0; i < dvr_num && i < area_num; i++) {
    printf("#UnBind VI[%d] to VM[0]:Chn[%u]....\n", i, i);
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = 0;
    stSrcChn.s32ChnId = in[i];
    stDestChn.enModId = RK_ID_VMIX;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = i;
    ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
    if (ret) {
      printf("UnBind vi[%d] to vmix[0]:Chn[%u] failed! ret=%d\n", i, i, ret);
    }
  }

  for (RK_U16 i = 0; i < dvr_num && i < area_num; i++) {
    ret = RK_MPI_VMIX_DisableChn(0, i);
    if (ret)
      printf("Disable VIMX[0]:Chn[%u] failed! ret=%d\n", i, ret);
  }

  ret = RK_MPI_VMIX_DestroyDev(0);
  if (ret) {
    printf("DeInit VIMX[0] failed! ret=%d\n", ret);
  }
}

static void dvr_disable(RK_U16 dvr_num) {
  int ret;
  for (RK_U16 i = 0; i < dvr_num; i++) {
    ret = RK_MPI_VI_DisableChn(0, i);
    if (ret)
      printf("Disable VI[%d] failed! ret=%d\n", i, ret);
  }
}

int main(int argc, char *argv[]) {
  int ret;
  (void)argc;
  (void)argv;

  RK_MPI_SYS_Init();

  // VO[1] for overlay plane
  VO_CHN_ATTR_S stVoAttr;
  memset(&stVoAttr, 0, sizeof(stVoAttr));
  stVoAttr.pcDevNode = "/dev/dri/card0";
  stVoAttr.emPlaneType = VO_PLANE_OVERLAY;
  stVoAttr.enImgType = IMAGE_TYPE_NV12;
  stVoAttr.u16Zpos = 1;
  stVoAttr.stImgRect.s32X = 0;
  stVoAttr.stImgRect.s32Y = 0;
  stVoAttr.stImgRect.u32Width = u32DispWidth;
  stVoAttr.stImgRect.u32Height = u32DispHeight;
  stVoAttr.stDispRect.s32X = 0;
  stVoAttr.stDispRect.s32Y = 0;
  stVoAttr.stDispRect.u32Width = u32DispWidth;
  stVoAttr.stDispRect.u32Height = u32DispHeight;
  ret = RK_MPI_VO_CreateChn(0, &stVoAttr);
  if (ret) {
    printf("Create vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  dvr_enable(dvr8, ARRAY_ELEMS(dvr8));

  printf("%s initial finish\n", __func__);
  signal(SIGINT, sigterm_handler);
  while (!quit) {
    /* display 8 area */
    memset(input, 0, sizeof(input));
    for (RK_U16 i = 0; i < 8; i++)
      input[i] = i;
    dvr_bind(input, dvr8, ARRAY_ELEMS(dvr8), area_2x4, ARRAY_ELEMS(area_2x4),
             line_2x4, ARRAY_ELEMS(line_2x4));
    usleep(5000000);
    dvr_unbind(input, ARRAY_ELEMS(dvr8), ARRAY_ELEMS(area_2x4));

    /* display 4 area */
    memset(input, 0, sizeof(input));
    for (RK_U16 i = 0; i < 4; i++)
      input[i] = i;
    dvr_bind(input, dvr8, ARRAY_ELEMS(dvr8), area_2x2, ARRAY_ELEMS(area_2x2),
             line_2x2, ARRAY_ELEMS(line_2x2));
    usleep(5000000);
    dvr_unbind(input, ARRAY_ELEMS(dvr8), ARRAY_ELEMS(area_2x2));

    /* display another 4 area */
    memset(input, 0, sizeof(input));
    for (RK_U16 i = 0; i < 4; i++)
      input[i] = 4 + i;
    dvr_bind(input, dvr8, ARRAY_ELEMS(dvr8), area_2x2, ARRAY_ELEMS(area_2x2),
             line_2x2, ARRAY_ELEMS(line_2x2));
    usleep(5000000);
    dvr_unbind(input, ARRAY_ELEMS(dvr8), ARRAY_ELEMS(area_2x2));
  }

  dvr_disable(ARRAY_ELEMS(dvr8));

  ret = RK_MPI_VO_DestroyChn(0);
  if (ret)
    printf("Destroy VO[0] failed! ret=%d\n", ret);

  return 0;
}
