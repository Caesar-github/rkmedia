// Copyright 2021 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>

#include "common/sample_common.h"
#include "rkmedia_api.h"
#include <rga/RgaApi.h>
#include <rga/rga.h>

#define THREE_SENSOR

#define XCAM_STATIC_FPS_CALCULATION(objname, count, mb, image_info)   \
  do {                                                                \
    static uint32_t num_frame = 0;                                    \
    static struct timeval last_sys_time;                              \
    static struct timeval first_sys_time;                             \
    static bool b_last_sys_time_init = false;                         \
    if (!b_last_sys_time_init) {                                      \
      gettimeofday (&last_sys_time, NULL);                            \
      gettimeofday (&first_sys_time, NULL);                           \
      b_last_sys_time_init = true;                                    \
    } else {                                                          \
      if ((num_frame%count)==0) {                                     \
        double total, current;                                        \
        struct timeval cur_sys_time;                                  \
        gettimeofday (&cur_sys_time, NULL);                           \
        total = (cur_sys_time.tv_sec -                                \
                        first_sys_time.tv_sec)*1.0f +                 \
                (cur_sys_time.tv_usec -                               \
                        first_sys_time.tv_usec)/1000000.0f;           \
        current = (cur_sys_time.tv_sec -                              \
                        last_sys_time.tv_sec)*1.0f +                  \
                  (cur_sys_time.tv_usec -                             \
                        last_sys_time.tv_usec)/1000000.0f;            \
        printf("%s Current fps: %.2f, Total avg fps: %.2f, ",         \
               #objname, ((float)(count))/current,                    \
               (float)num_frame/total);                               \
        printf("channel: %d, ImgInfo:<wxh %dx%d, fmt 0x%x>\r\n",      \
               RK_MPI_MB_GetChannelID(mb), image_info.u32Width,       \
               image_info.u32Height, image_info.enImgType);           \
        last_sys_time = cur_sys_time;                                 \
      }                                                               \
    }                                                                 \
    ++num_frame;                                                      \
  } while(0)

typedef struct {
  char *file_path0;
  int frame_cnt0;
  char *file_path1;
  int frame_cnt1;
} OutputArgs;

static int video_width = 640;
static int video_height = 480;

static int video_width1 = 640;
static int video_height1 = 480;

static int video_width2 = 1920;
static int video_height2 = 1080;

static int disp_width = 480;
static int disp_height = 854;

static bool quit = false;

static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

void usage() {
  printf("    -a,  id_dir\n"
         "  --w0,  default 640,   optional, width of ispp0 image,        -e\n"
         "  --h0,  default 480,   optional, height of ispp0 image,       -f\n"
         "  --w1,  default 640,   optional, width of ispp1 image,        -j\n"
         "  --h1,  default 480,   optional, height of ispp1 image,       -k\n"
         "  --w2,  default 1920,  optional, width of ispp2(dvp) image,   -l\n"
         "  --h2,  default 1080,  optional, height of ispp2(dvp) image,  -m\n"
         "  --vop, default no,    optional, drm display,                 -v\n"
         "    -w,  default 480,   optional, width of display,            -l\n"
         "    -h,  default 854,   optional, height of display,           -m\n");
  exit(-1);
}

static void *CaptureMipi0(void *arg) {
  OutputArgs *outArgs = (OutputArgs *)arg;
  char *save_path = outArgs->file_path0;
  int save_cnt = outArgs->frame_cnt0;
  int frame_id = 0;
  FILE *save_file = NULL;
  if (save_path) {
    save_file = fopen(save_path, "w");
    if (!save_file)
      printf("ERROR: Open %s failed!\n", save_path);
  }

  MEDIA_BUFFER mb = NULL;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 0, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    MB_IMAGE_INFO_S stImageInfo = {0};
    int ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo);
    if (ret)
      printf("Warn: Get image info failed! ret = %d\n", ret);

    XCAM_STATIC_FPS_CALCULATION(rkmedia_vi_three_camera_test, 30,
                                mb, stImageInfo);

    if (save_file) {
      fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), save_file);
      printf("#===0===Save frame-%d to %s\n", frame_id++, save_path);
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

static void *CaptureMipi1(void *arg) {
  OutputArgs *outArgs = (OutputArgs *)arg;
  char *save_path = outArgs->file_path1;
  int save_cnt = outArgs->frame_cnt1;
  int frame_id = 0;
  FILE *save_file = NULL;
  if (save_path) {
    save_file = fopen(save_path, "w");
    if (!save_file)
      printf("ERROR: Open %s failed!\n", save_path);
  }

  MEDIA_BUFFER mb = NULL;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 1, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    MB_IMAGE_INFO_S stImageInfo = {0};
    int ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo);
    if (ret)
        printf("Warn: Get image info failed! ret = %d\n", ret);

    XCAM_STATIC_FPS_CALCULATION(rkmedia_vi_three_camera_test, 30,
                                mb, stImageInfo);

    if (save_file) {
      fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), save_file);
      printf("#===1===Save frame-%d to %s\n", frame_id++, save_path);
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

static void *GetMediaBuffer(void *arg) {
  int chn = (int)arg;

  MEDIA_BUFFER mb = NULL;
  while (!quit) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, chn, -1);
    if (!mb) {
      printf("RK_MPI_SYS_GetMediaBuffer get null buffer!\n");
      break;
    }

    MB_IMAGE_INFO_S stImageInfo = {0};
    int ret = RK_MPI_MB_GetImageInfo(mb, &stImageInfo);
    if (ret)
      printf("Warn: Get image info failed! ret = %d\n", ret);

    XCAM_STATIC_FPS_CALCULATION(rkmedia_vi_three_camera_test, 30,
                                mb, stImageInfo);

    RK_MPI_MB_ReleaseBuffer(mb);
  }

  pthread_detach(pthread_self());

  return NULL;
}

static struct option long_options[] = {
  {"w0",  required_argument, 0, 'e'},
  {"h0",  required_argument, 0, 'f'},
  {"w1",  required_argument, 0, 'j'},
  {"h1",  required_argument, 0, 'k'},
  {"w2",  required_argument, 0, 'l'},
  {"h2",  required_argument, 0, 'm'},
  {"vop", no_argument,       0, 'v'},
  {0, 0, 0, 0}
};

int main(int argc, char *argv[]) {
  int ret = 0;

  int frameCnt0 = -1;
  int frameCnt1 = -1;
  char *iq_dir = NULL;
  int ch;
  int vop = 0;
  RK_CHAR *pOutPath0 = NULL;
  RK_CHAR *pOutPath1 = NULL;

  int option_index = 0;
  while ((ch = getopt_long(argc, argv, "a:c:d:e:f:j:k:l:m:o:p:w:h:",
         long_options, &option_index)) != -1) {
    switch (ch) {
      case 'a':
        iq_dir = optarg;
        if (access(iq_dir, R_OK))
          usage(argv[0]);
        break;
      case 'c':
        frameCnt0 = atoi(optarg);
        break;
      case 'o':
        pOutPath0 = optarg;
        break;
      case 'd':
        frameCnt1 = atoi(optarg);
        break;
      case 'p':
        pOutPath1 = optarg;
        break;
      case 'e':
        video_width = atoi(optarg);
        break;
      case 'f':
        video_height = atoi(optarg);
        break;
      case 'j':
        video_width1 = atoi(optarg);
        break;
      case 'k':
        video_height1 = atoi(optarg);
        break;
      case 'l':
        video_width2 = atoi(optarg);
        break;
      case 'm':
        video_height2 = atoi(optarg);
        break;
      case 'w':
        disp_width = atoi(optarg);
        break;
      case 'h':
        disp_height = atoi(optarg);
        break;
      case 'v':
        vop = 1;
        break;

      default:
        usage(argv[0]);
        break;
    }
  }

  #ifdef RKAIQ
  printf("#####Aiq xml dirpath: %s\n\n", iq_dir);

  ret = SAMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_TRUE,
                             iq_dir);
  if (ret)
    return -1;
  SAMPLE_COMM_ISP_Run(0);

  ret = SAMPLE_COMM_ISP_Init(1, RK_AIQ_WORKING_MODE_NORMAL, RK_TRUE,
                             iq_dir);
  if (ret)
    return -1;
  SAMPLE_COMM_ISP_Run(1);

  ret = SAMPLE_COMM_ISP_Init(2, RK_AIQ_WORKING_MODE_NORMAL, RK_TRUE,
                             iq_dir);
  if (ret)
    return -1;
  SAMPLE_COMM_ISP_Run(2);

  #endif // RKAIQ

  //VI init
  RK_MPI_SYS_Init();
  VI_CHN_ATTR_S vi_chn_attr;

  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.u32BufCnt = 4;
  vi_chn_attr.u32Width = video_width;
  vi_chn_attr.u32Height = video_height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP;

  ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, 0);
  if (ret) {
    printf("Create VI[0] failed! ret=%d\n", ret);
    return -1;
  }
  printf("Create VI[0] successful! ret=%d\n", ret);

  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.u32BufCnt = 4;
  vi_chn_attr.u32Width = video_width1;
  vi_chn_attr.u32Height = video_height1;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP;

  ret = RK_MPI_VI_SetChnAttr(1, 1, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(1, 1);
  if (ret) {
    printf("Create VI[1] failed! ret=%d\n", ret);
    return -1;
  }
  printf("Create VI[1] successful! ret=%d\n", ret);

  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.u32BufCnt = 4;
  vi_chn_attr.u32Width = video_width2;
  vi_chn_attr.u32Height = video_height2;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP;

  ret = RK_MPI_VI_SetChnAttr(2, 2, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(2, 2);
  if (ret) {
    printf("Create VI[2] failed! ret=%d\n", ret);
    return -1;
  }
  printf("Create VI[2] successful! ret=%d\n", ret);

  if (vop) {
    VO_CHN_ATTR_S stVoAttr = {0};
    stVoAttr.pcDevNode = "/dev/dri/card0";
    stVoAttr.emPlaneType = VO_PLANE_OVERLAY;
    stVoAttr.enImgType = IMAGE_TYPE_NV12;
    stVoAttr.u16Zpos = 0;
    stVoAttr.stImgRect.s32X = 0;
    stVoAttr.stImgRect.s32Y = 0;
    stVoAttr.stImgRect.u32Width = video_width2;
    stVoAttr.stImgRect.u32Height = video_height2;
    stVoAttr.stDispRect.s32X = 0;
    stVoAttr.stDispRect.s32Y = 0;
    stVoAttr.stDispRect.u32Width = disp_width;
    stVoAttr.stDispRect.u32Height = disp_height;
    ret = RK_MPI_VO_CreateChn(0, &stVoAttr);
    if (ret) {
      printf("Create vo[0] failed! ret=%d\n", ret);
      return -1;
    }

    MPP_CHN_S stSrcChn = {0};
    MPP_CHN_S stDestChn = {0};

    printf("#Bind VI[2] to VO[0]....\n");
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32ChnId = 2;
    stDestChn.enModId = RK_ID_VO;
    stDestChn.s32ChnId = 0;
    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (ret) {
      printf("Bind vi[2] to vo[0] failed! ret=%d\n", ret);
      return -1;
    }
  }

  pthread_t read_thread0;
  OutputArgs outArgs = {pOutPath0, frameCnt0, pOutPath1, frameCnt1};
  pthread_create(&read_thread0, NULL, CaptureMipi0, &outArgs);
  ret = RK_MPI_VI_StartStream(0, 0);
  if (ret) {
    printf("Start VI[0] failed! ret=%d\n", ret);
    return -1;
  }
  pthread_t read_thread1;
  pthread_create(&read_thread1, NULL, CaptureMipi1, &outArgs);
  ret = RK_MPI_VI_StartStream(1, 1);
  if (ret) {
    printf("Start VI[1] failed! ret=%d\n", ret);
    return -1;
  }
  pthread_t th5;
  int chn = 0;

  chn = 2;
  pthread_create(&th5, NULL, GetMediaBuffer, (void *)chn);
  ret = RK_MPI_VI_StartStream(2, 2);
  if (ret) {
    printf("Start VI[2] failed! ret=%d\n", ret);
    return -1;
  }

  printf("%s: initial finish\n", __func__);

  signal(2, sigterm_handler);
  while (!quit) {
    usleep(500000);
  }

  RK_MPI_VI_DisableChn(0, 0);
  RK_MPI_VI_DisableChn(1, 1);
  RK_MPI_VI_DisableChn(2, 2);
#ifdef RKAIQ
  SAMPLE_COMM_ISP_Stop(0);
  SAMPLE_COMM_ISP_Stop(1);
  SAMPLE_COMM_ISP_Stop(2);
#endif

  return 0;
}
