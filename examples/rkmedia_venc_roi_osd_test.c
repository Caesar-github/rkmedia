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
#include "rkmedia_venc.h"

#define TEST_ARGB32_YELLOW 0xFFFFFF00
#define TEST_ARGB32_RED 0xFFFF0033
#define TEST_ARGB32_BLUE 0xFF003399
#define TEST_ARGB32_TRANS 0x00000000

static FILE *g_save_file;
static FILE *g_save_file_sub0;
static FILE *g_save_file_sub1;
static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static void set_argb8888_buffer(RK_U32 *buf, RK_U32 size, RK_U32 color) {
  for (RK_U32 i = 0; buf && (i < size); i++)
    *(buf + i) = color;
}

void video_packet_cb(MEDIA_BUFFER mb) {
  if (g_save_file)
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), g_save_file);

  RK_MPI_MB_ReleaseBuffer(mb);
}

void video_packet_cb_sub0(MEDIA_BUFFER mb) {
  if (g_save_file_sub0)
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), g_save_file_sub0);
  RK_MPI_MB_ReleaseBuffer(mb);
}

void video_packet_cb_sub1(MEDIA_BUFFER mb) {
  if (g_save_file_sub0)
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), g_save_file_sub1);
  RK_MPI_MB_ReleaseBuffer(mb);
}

static int EnableOsd(RK_U16 u16VencChn) {
  // Generate a test bitmap, bitmap width and height 64 x 256,
  // color distribution is as follows:
  // ***********
  // *         *
  // * YELLOW  *
  // *         *
  // ***********
  // *         *
  // *  TRANS  *
  // *         *
  // ***********
  // *         *
  // *   RED   *
  // *         *
  // ***********
  // *         *
  // *  BLUE   *
  // *         *
  // ***********
  BITMAP_S BitMap;
  BitMap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
  BitMap.u32Width = 64;
  BitMap.u32Height = 256;
  BitMap.pData = malloc(BitMap.u32Width * 4 * BitMap.u32Height);
  RK_U8 *ColorData = (RK_U8 *)BitMap.pData;
  RK_U16 ColorBlockSize = BitMap.u32Height * BitMap.u32Width;
  set_argb8888_buffer((RK_U32 *)ColorData, ColorBlockSize / 4,
                      TEST_ARGB32_YELLOW);
  set_argb8888_buffer((RK_U32 *)(ColorData + ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_TRANS);
  set_argb8888_buffer((RK_U32 *)(ColorData + 2 * ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_RED);
  set_argb8888_buffer((RK_U32 *)(ColorData + 3 * ColorBlockSize),
                      ColorBlockSize / 4, TEST_ARGB32_BLUE);

  // Case 1: Canvas and bitmap are equal in size
  OSD_REGION_INFO_S RngInfo;
  RngInfo.enRegionId = REGION_ID_0;
  RngInfo.u32PosX = 0;
  RngInfo.u32PosY = 0;
  RngInfo.u32Width = 64;
  RngInfo.u32Height = 256;
  RngInfo.u8Enable = 1;
  RngInfo.u8Inverse = 0;
  return RK_MPI_VENC_RGN_SetBitMap(u16VencChn, &RngInfo, &BitMap);
}

static int EnableRoi(RK_U16 u16VencChn, RECT_S stRect) {
  VENC_ROI_ATTR_S stRoiAttr = {0};
  stRoiAttr.bAbsQp = RK_FALSE;
  stRoiAttr.bEnable = RK_FALSE;
  stRoiAttr.stRect.s32X = stRect.s32X;
  stRoiAttr.stRect.s32Y = stRect.s32Y;
  stRoiAttr.stRect.u32Width = stRect.u32Width;
  stRoiAttr.stRect.u32Height = stRect.u32Height;
  stRoiAttr.u32Index = 0;
  stRoiAttr.s32Qp = 6;
  stRoiAttr.bIntra = RK_FALSE;
  int ret = RK_MPI_VENC_SetRoiAttr(u16VencChn, &stRoiAttr, 1);
  printf("set roi : %d\n", ret);
  return ret;
}

int StreamOn(int width, int height, const char *video_node, int vi_chn,
             int venc_chn) {
  static int stream_cnt = 0;
  int ret = 0;

  printf(
      "*** StreamOn[%d]: VideoNode:%s, wxh:%dx%d, vi:%d, venc:%d START....\n",
      stream_cnt, video_node, width, height, vi_chn, venc_chn);

  CODEC_TYPE_E codec_type = RK_CODEC_TYPE_H264;
  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = video_node;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = width;
  vi_chn_attr.u32Height = height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(0, vi_chn, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, vi_chn);
  if (ret) {
    printf("Create Vi failed! ret=%d\n", ret);
    return -1;
  }

  VENC_CHN_ATTR_S venc_chn_attr;
  memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
  venc_chn_attr.stVencAttr.enType = codec_type;
  venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
  venc_chn_attr.stVencAttr.u32PicWidth = width;
  venc_chn_attr.stVencAttr.u32PicHeight = height;
  venc_chn_attr.stVencAttr.u32VirWidth = width;
  venc_chn_attr.stVencAttr.u32VirHeight = height;
  venc_chn_attr.stVencAttr.u32Profile = 77;
  venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
  venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = 75;
  venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = width * height * 30 / 14;
  venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 0;
  venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 25;
  venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 0;
  venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;
  ret = RK_MPI_VENC_CreateChn(venc_chn, &venc_chn_attr);
  if (ret) {
    printf("Create avc failed! ret=%d\n", ret);
    return -1;
  }

  MPP_CHN_S stEncChn;
  stEncChn.enModId = RK_ID_VENC;
  stEncChn.s32DevId = 0;
  stEncChn.s32ChnId = venc_chn;
  if (stream_cnt == 0)
    RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
  else if (stream_cnt == 1)
    RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb_sub0);
  else
    RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb_sub1);

  RK_MPI_VENC_RGN_Init(venc_chn, NULL);

  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = vi_chn;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = venc_chn;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Create RK_MPI_SYS_Bind0 failed! ret=%d\n", ret);
    return -1;
  }

  stream_cnt++;
  printf("*** StreamOn: END....\n");

  return 0;
}

int StreamOff(int vi_chn, int venc_chn) {
  int ret = 0;

  printf("*** StreamOff: vi:%d, venc:%d START....\n", vi_chn, venc_chn);

  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = vi_chn;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = venc_chn;
  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("ERROR: unbind vi[%d] -> venc[%d] failed!\n", vi_chn, venc_chn);
    return -1;
  }

  RK_MPI_VI_DisableChn(0, vi_chn);
  RK_MPI_VENC_DestroyChn(venc_chn);
  printf("*** StreamOff: END....\n");

  return 0;
}

static RK_CHAR optstr[] = "?::a::o:";
static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"help", optional_argument, NULL, '?'},
    {"output", required_argument, NULL, 'o'},
    {NULL, 0, NULL, 0},
};

static void print_usage(char *name) {
  printf("#Function description:\n");
  printf("In the case of multiple streams, verify the effect of roi and osd at "
         "the same time.\n");
  printf(
      "  MainStream: rkispp_scale0: 1920x1080 NV12 -> /<output>/main.h264\n");
  printf("  SubStream0: rkispp_scale1: 720x480 NV12 -> /<output>/sub0.h264\n");
  printf("  SubStream1: rkispp_scale2: 1280x720 NV12 -> /<output>/sub1.h264\n");
  printf("#Usage Example: \n");
#ifdef RKAIQ
  printf("\t%s [-a [iqfiles_dir]] [-o output_dir]\n", name);
  printf("\t-a | --aiq: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, "
         "set dirpath empty to using path by default, without this option aiq "
         "should run in other application\n");
#else
  printf("\t%s [-o output_dir]\n", name);
#endif
  printf("\t-o | --output: output dirpath, Default:/tmp/\n");
}

int main(int argc, char *argv[]) {
  char *iq_file_dir = NULL;
  int c = 0;
  RK_CHAR *pOutPath = "/tmp/";
  opterr = 1;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }
      if (tmp_optarg) {
        iq_file_dir = (char *)tmp_optarg;
      } else {
        iq_file_dir = "/oem/etc/iqfiles/";
      }
      break;
    case 'o':
      pOutPath = optarg;
      break;
    case '?':
    default:
      print_usage(argv[0]);
      return 0;
    }
  }

  char file_name[128] = {0};
  char file_name0[128] = {0};
  char file_name1[128] = {0};
  sprintf(file_name, "%s/main.h264", pOutPath);
  sprintf(file_name0, "%s/sub0.h264", pOutPath);
  sprintf(file_name1, "%s/sub1.h264", pOutPath);
  printf("#output main:%s\n", file_name);
  printf("#output sub0:%s\n", file_name0);
  printf("#output sub1:%s\n", file_name1);
  g_save_file = fopen(file_name, "w");
  if (!g_save_file) {
    printf("******open %s fail\n", file_name);
  }
  g_save_file_sub0 = fopen(file_name0, "w");
  if (!g_save_file_sub0) {
    printf("******open %s fail\n", file_name0);
  }
  g_save_file_sub1 = fopen(file_name1, "w");
  if (!g_save_file_sub1) {
    printf("******open %s fail\n", file_name1);
  }
  printf(">>>>>>>>>>>>>>> Test START <<<<<<<<<<<<<<<<<<<<<<\n");
  if (iq_file_dir) {
#ifdef RKAIQ
    printf("#Aiq xml dirpath: %s\n\n", iq_file_dir);
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    RK_BOOL fec_enable = RK_FALSE;
    int fps = 30;
    SAMPLE_COMM_ISP_Init(hdr_mode, fec_enable, iq_file_dir);
    SAMPLE_COMM_ISP_Run();
    SAMPLE_COMM_ISP_SetFrameRate(fps);
#endif
  }

  RK_MPI_SYS_Init();
  if (StreamOn(1920, 1080, "rkispp_scale0", 0, 0)) {
    printf("ERROR: StreamOn failed!\n");
    return -1;
  }

  if (StreamOn(720, 480, "rkispp_scale1", 1, 1)) {
    printf("ERROR: StreamOn failed!\n");
    return -1;
  }

  if (StreamOn(1280, 720, "rkispp_scale2", 2, 2)) {
    printf("ERROR: StreamOn failed!\n");
    return -1;
  }

  printf("Encodering for 5s...\n");
  sleep(5);

  printf("Enable osd for three streams...\n");
  for (int i = 0; i < 3; i++) {
    if (EnableOsd(i)) {
      printf("TEST: ERROR: Enable venc[%d] osd failed!\n", i);
      quit = true;
      break;
    }
  }

  if (!quit) {
    printf("Encodering for 3s after enable osd...\n");
    sleep(3);

    printf("Enable MainStream roi[128, 128, 1280, 768]...\n");
    RECT_S stRect = {0};
    stRect.s32X = 128;
    stRect.s32Y = 128;
    stRect.u32Width = 1280;
    stRect.u32Height = 768;
    if (EnableRoi(0, stRect)) {
      printf("TEST: ERROR: Enable venc[0] roi failed!\n");
      quit = true;
    }

    printf("Enable SubStream0 roi[128, 128, 512, 320]...\n");
    stRect.s32X = 128;
    stRect.s32Y = 128;
    stRect.u32Width = 512;
    stRect.u32Height = 320;
    if (EnableRoi(1, stRect)) {
      printf("TEST: ERROR: Enable venc[1] roi failed!\n");
      quit = true;
    }

    printf("Enable SubStream1 roi[128, 128, 960, 512]...\n");
    stRect.s32X = 128;
    stRect.s32Y = 128;
    stRect.u32Width = 960;
    stRect.u32Height = 512;
    if (EnableRoi(2, stRect)) {
      printf("TEST: ERROR: Enable venc[2] roi failed!\n");
      quit = true;
    }
  }

  signal(SIGINT, sigterm_handler);
  while (!quit) {
    usleep(500000);
  }

  StreamOff(0, 0);
  StreamOff(1, 1);
  StreamOff(2, 2);

  if (iq_file_dir) {
#ifdef RKAIQ
    SAMPLE_COMM_ISP_Stop();
#endif
  }

  fclose(g_save_file);
  fclose(g_save_file_sub0);
  fclose(g_save_file_sub1);
  printf(">>>>>>>>>>>>>>> Test END <<<<<<<<<<<<<<<<<<<<<<\n");
  return 0;
}
