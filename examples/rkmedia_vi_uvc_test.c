// Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
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

#include <uvc/uvc_control.h>
#include <uvc/uvc_video.h>

#include "common/sample_common.h"
#include "rkmedia_api.h"

static pthread_t th;
static int start;
static RK_CHAR *pDeviceName = "rkispp_scale0";
RK_CHAR *pIqfilesPath = NULL;

static bool quit = false;
static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

static void *GetMediaBuffer(void *arg) {
  MEDIA_BUFFER mb = NULL;
  (void)arg;
  while (start) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 0, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    MPP_ENC_INFO_DEF info = {0};
    info.fd = RK_MPI_MB_GetFD(mb);
    info.size = RK_MPI_MB_GetSize(mb);
    uvc_read_camera_buffer(RK_MPI_MB_GetPtr(mb), &info, NULL, 0);

    RK_MPI_MB_ReleaseBuffer(mb);
  }

  return NULL;
}

static RK_CHAR optstr[] = "?::a::d:r";
static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"device_name", required_argument, NULL, 'd'},
    {"rndis", 0, NULL, 'r'},
    {"help", optional_argument, NULL, '?'},
    {NULL, 0, NULL, 0},
};

static void print_usage(const RK_CHAR *name) {
  printf("usage example:\n");
#ifdef RKAIQ
  printf("\t%s [-a [iqfiles_dir]] [-d rkispp_scale0] [-r]\n", name);
  printf("\t-a | --aiq: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, "
         "set dirpath emtpty to using path by default, without this option aiq "
         "should run in other application\n");
#else
  printf("\t%s [-d rkispp_scale0] [-r]\n", name);
#endif
  printf("\t-d | --device_name set pcDeviceName, Default:rkispp_scale0\n");
  printf("\t-r | --rndis use uvc+rndis\n");
  printf("Notice: fmt always NV12\n");
}

int camera_start(int id, int width, int height, int fps, int format, int eptz) {
  int ret = 0;
  (void)id;
  (void)fps;
  (void)format;
  (void)eptz;

  if (pIqfilesPath) {
#ifdef RKAIQ
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    RK_BOOL fec_enable = RK_FALSE;
    SAMPLE_COMM_ISP_Init(hdr_mode, fec_enable, pIqfilesPath);
    SAMPLE_COMM_ISP_Run();
    SAMPLE_COMM_ISP_SetFrameRate(fps);
#endif
  }

  VI_CHN_ATTR_S vi_chn_attr;
  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = pDeviceName;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = width;
  vi_chn_attr.u32Height = height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 0);
  if (ret) {
    printf("Create VI[0] failed! ret=%d\n", ret);
    return -1;
  }

  ret = RK_MPI_VI_StartStream(0, 0);
  if (ret) {
    printf("Start VI[0] failed! ret=%d\n", ret);
    return -1;
  }

  start = 1;
  if (pthread_create(&th, NULL, GetMediaBuffer, NULL)) {
    printf("create GetMediaBuffer thread failed!\n");
    start = 0;
    return -1;
  }

  return 0;
}

void camera_stop(void) {
  if (start) {
    start = 0;
    pthread_join(th, NULL);
    RK_MPI_VI_DisableChn(0, 0);
#ifdef RKAIQ
    if (pIqfilesPath)
      SAMPLE_COMM_ISP_Stop();
#endif
  }
}

int main(int argc, char *argv[]) {
  char *config = "uvc_config.sh";
  int c;
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
    case 'd':
      pDeviceName = optarg;
      break;
    case 'r':
      config = "uvc_config.sh rndis";
      break;
    case '?':
    default:
      print_usage(argv[0]);
      return 0;
    }
  }

  printf("#####Device: %s\n", pDeviceName);
  printf("#####Aiq xml dirpath: %s\n\n", pIqfilesPath);

  RK_MPI_SYS_Init();

  system(config);

  printf("%s initial finish\n", __func__);
  signal(SIGINT, sigterm_handler);

  uvc_control_start_setcallback(camera_start);
  uvc_control_stop_setcallback(camera_stop);
  uvc_control_run(UVC_CONTROL_CAMERA);

  while (!quit) {
    if (0 == uvc_control_loop())
      break;
    usleep(500000);
  }

  camera_stop();
  uvc_control_join(UVC_CONTROL_CAMERA);

  printf("%s exit!\n", __func__);
  return 0;
}
