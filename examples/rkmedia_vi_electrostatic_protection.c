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
#include <time.h>
#include <unistd.h>

#include "common/sample_common.h"
#include "rkmedia_api.h"

typedef struct {
  char *file_path;
  int frame_cnt;
} OutputArgs;

static bool quit = false;
static bool restart_flag = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static void *GetMediaBuffer(void *arg) {
  OutputArgs *outArgs = (OutputArgs *)arg;
  char *save_path = outArgs->file_path;
  int save_cnt = outArgs->frame_cnt;
  int frame_id = 0;
  FILE *save_file = NULL;
  if (save_path) {
    save_file = fopen(save_path, "w");
    if (!save_file)
      printf("ERROR: Open %s failed!\n", save_path);
  }
  int timeout_cnt = 0;
  MEDIA_BUFFER mb = NULL;
  while (!quit && !restart_flag) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 0, 100);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      if (timeout_cnt++ > 30) {
        restart_flag = true;
      }
      continue;
    } else {
      timeout_cnt = 0;
    }
    MB_IMAGE_INFO_S stImageInfo = {0};
    int ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo);
    if (ret)
      printf("Warn: Get image info failed! ret = %d\n", ret);

    printf("Get Frame:ptr:%p, fd:%d, size:%zu, mode:%d, channel:%d, "
           "timestamp:%lld, ImgInfo:<wxh %dx%d, fmt 0x%x>\n",
           RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb), RK_MPI_MB_GetSize(mb),
           RK_MPI_MB_GetModeID(mb), RK_MPI_MB_GetChannelID(mb),
           RK_MPI_MB_GetTimestamp(mb), stImageInfo.u32Width,
           stImageInfo.u32Height, stImageInfo.enImgType);

    if (save_file) {
      fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), save_file);
      printf("#Save frame-%d to %s\n", frame_id++, save_path);
    }

    RK_MPI_MB_ReleaseBuffer(mb);

    if (save_cnt > 0)
      save_cnt--;

    // exit when complete
    if (!save_cnt) {
      quit = true;
      break;
    }
  }

  if (save_file)
    fclose(save_file);

  return NULL;
}

static RK_CHAR optstr[] = "?::a::w:h:c:o:d:";
static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"device_name", required_argument, NULL, 'd'},
    {"width", required_argument, NULL, 'w'},
    {"height", required_argument, NULL, 'h'},
    {"frame_cnt", required_argument, NULL, 'c'},
    {"output", required_argument, NULL, 'o'},
    {"help", optional_argument, NULL, '?'},
    {NULL, 0, NULL, 0},
};

static void print_usage(const RK_CHAR *name) {
  printf("usage example:\n");
#ifdef RKAIQ
  printf("\t%s [-a [iqfiles_dir]] [-w 1920] "
         "[-h 1080]"
         "[-c 10] "
         "[-d rkispp_scale0] "
         "[-o out.nv12] \n",
         name);
  printf("\t-a | --aiq: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, "
         "set dirpath emtpty to using path by default, without this option aiq "
         "should run in other application\n");
#else
  printf("\t%s [-w 1920] "
         "[-h 1080]"
         "[-c 10] "
         "[-d rkispp_scale0] "
         "[-o out.nv12] \n",
         name);
#endif
  printf("\t-w | --width: VI width, Default:1920\n");
  printf("\t-h | --heght: VI height, Default:1080\n");
  printf(
      "\t-d | --device_name: set device node(v4l2), Default:rkispp_scale0\n");
  printf("\t-c | --frame_cnt: record frame, Default:-1(unlimit)\n");
  printf("\t-o | --output: output path, Default:NULL\n");
  printf("Notice: fmt always NV12\n");
}

int main(int argc, char *argv[]) {
  RK_U32 u32Width = 1920;
  RK_U32 u32Height = 1080;
  int frameCnt = -1;
  RK_CHAR *pDeviceName = "rkispp_scale0";
  RK_CHAR *pOutPath = NULL;
  RK_CHAR *pIqfilesPath = NULL;
  int c;
  int ret = 0;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }
      if (tmp_optarg) {
        pIqfilesPath = (char *)tmp_optarg;
      } else {
        pIqfilesPath = "/oem/etc/iqfiles/";
      }
      break;
    case 'w':
      u32Width = atoi(optarg);
      break;
    case 'h':
      u32Height = atoi(optarg);
      break;
    case 'c':
      frameCnt = atoi(optarg);
      break;
    case 'o':
      pOutPath = optarg;
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
  printf("#####Frame Count to save: %d\n", frameCnt);
  printf("#####Output Path: %s\n", pOutPath);
  printf("#####Aiq xml dirpath: %s\n\n", pIqfilesPath);

  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = pDeviceName;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = u32Width;
  vi_chn_attr.u32Height = u32Height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  pthread_t read_thread;
  OutputArgs outArgs = {pOutPath, frameCnt};
  signal(SIGINT, sigterm_handler);

Restart:
  if (pIqfilesPath) {
#ifdef RKAIQ
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    int fps = 30;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, RK_FALSE, pIqfilesPath);
    SAMPLE_COMM_ISP_Run(0);
    SAMPLE_COMM_ISP_SetFrameRate(0, fps);
#endif
  }

  RK_MPI_SYS_Init();
  ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 0);
  if (ret) {
    printf("Create VI[0] failed! ret=%d\n", ret);
    return -1;
  }

  printf("%s initial finish\n", __func__);

  pthread_create(&read_thread, NULL, GetMediaBuffer, &outArgs);
  ret = RK_MPI_VI_StartStream(0, 0);
  if (ret) {
    printf("Start VI[0] failed! ret=%d\n", ret);
    return -1;
  }

  while (!quit && !restart_flag) {
    usleep(500000);
  }

  pthread_join(read_thread, NULL);

  RK_MPI_VI_DisableChn(0, 0);

#ifdef RKAIQ
  if (pIqfilesPath)
    SAMPLE_COMM_ISP_Stop(0);
#endif

  if (restart_flag) {
    restart_flag = false;
    goto Restart;
  }

  printf("%s exit!\n", __func__);
  return 0;
}
