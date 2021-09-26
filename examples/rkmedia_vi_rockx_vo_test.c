// Copyright 2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <rga/im2d.h>
#include <rga/rga.h>
#ifdef RKAIQ
#include "common/sample_common.h"
#endif

#include "rkmedia_api.h"
#include "rockx.h"

#define MAX_SESSION_NUM 2
#define DRAW_INDEX 0
#define RKNN_INDEX 1
#define MAX_RKNN_LIST_NUM 10

static int draw_w = 640;
static int draw_h = 480;
static int rknn_w = 640;
static int rknn_h = 480;
static int display_w = 720;
static int display_h = 1280;
static char runtime_path[128];

typedef struct node {
  long timeval;
  rockx_object_array_t person_array;
  struct node *next;
} Node;

typedef struct my_stack {
  int size;
  Node *top;
} rknn_list;

void create_rknn_list(rknn_list **s) {
  if (*s != NULL)
    return;
  *s = (rknn_list *)malloc(sizeof(rknn_list));
  (*s)->top = NULL;
  (*s)->size = 0;
  printf("create rknn_list success\n");
}

void destory_rknn_list(rknn_list **s) {
  Node *t = NULL;
  if (*s == NULL)
    return;
  while ((*s)->top) {
    t = (*s)->top;
    (*s)->top = t->next;
    free(t);
  }
  free(*s);
  *s = NULL;
}

void rknn_list_push(rknn_list *s, long timeval,
                    rockx_object_array_t person_array) {
  Node *t = NULL;
  t = (Node *)malloc(sizeof(Node));
  t->timeval = timeval;
  t->person_array = person_array;
  if (s->top == NULL) {
    s->top = t;
    t->next = NULL;
  } else {
    t->next = s->top;
    s->top = t;
  }
  s->size++;
}

void rknn_list_pop(rknn_list *s, long *timeval,
                   rockx_object_array_t *person_array) {
  Node *t = NULL;
  if (s == NULL || s->top == NULL)
    return;
  t = s->top;
  *timeval = t->timeval;
  *person_array = t->person_array;
  s->top = t->next;
  free(t);
  s->size--;
}

void rknn_list_drop(rknn_list *s) {
  Node *t = NULL;
  if (s == NULL || s->top == NULL)
    return;
  t = s->top;
  s->top = t->next;
  free(t);
  s->size--;
}

int rknn_list_size(rknn_list *s) {
  if (s == NULL)
    return -1;
  return s->size;
}

rknn_list *rknn_list_;

static int g_flag_run = 1;

static void sig_proc(int signo) {
  fprintf(stderr, "signal %d\n", signo);
  g_flag_run = 0;
}

static long getCurrentTimeMsec() {
  long msec = 0;
  char str[20] = {0};
  struct timeval stuCurrentTime;

  gettimeofday(&stuCurrentTime, NULL);
  sprintf(str, "%ld%03ld", stuCurrentTime.tv_sec,
          (stuCurrentTime.tv_usec) / 1000);
  for (size_t i = 0; i < strlen(str); i++) {
    msec = msec * 10 + (str[i] - '0');
  }

  return msec;
}

int nv12_border(char *pic, int pic_w, int pic_h, int rect_x, int rect_y,
                int rect_w, int rect_h, int R, int G, int B) {
  /* Set up the rectangle border size */
  const int border = 5;
  /* RGB convert YUV */
  int Y, U, V;
  Y = 0.299 * R + 0.587 * G + 0.114 * B;
  U = -0.1687 * R + 0.3313 * G + 0.5 * B + 128;
  V = 0.5 * R - 0.4187 * G - 0.0813 * B + 128;
  /* Locking the scope of rectangle border range */
  int j, k;
  for (j = rect_y; j < rect_y + rect_h; j++) {
    for (k = rect_x; k < rect_x + rect_w; k++) {
      if (k < (rect_x + border) || k > (rect_x + rect_w - border) ||
          j < (rect_y + border) || j > (rect_y + rect_h - border)) {
        /* Components of YUV's storage address index */
        int y_index = j * pic_w + k;
        int u_index =
            (y_index / 2 - pic_w / 2 * ((j + 1) / 2)) * 2 + pic_w * pic_h;
        int v_index = u_index + 1;
        /* set up YUV's conponents value of rectangle border */
        pic[y_index] = Y;
        pic[u_index] = U;
        pic[v_index] = V;
      }
    }
  }

  return 0;
}

static void *RknnThread(void *arg) {
  printf("#Start %s thread, arg:%p\n", __func__, arg);
  // rockx
  rockx_ret_t rockx_ret;
  rockx_handle_t object_det_handle;
  rockx_image_t input_image;
  input_image.height = rknn_h;
  input_image.width = rknn_w;
  input_image.pixel_format = ROCKX_PIXEL_FORMAT_YUV420SP_NV12;
  input_image.is_prealloc_buf = 1;
  // create a object detection handle
  rockx_config_t *config = rockx_create_config();
  rockx_add_config(config, ROCKX_CONFIG_RKNN_RUNTIME_PATH, runtime_path);
  rockx_ret =
      rockx_create(&object_det_handle, ROCKX_MODULE_FACE_DETECTION_V3_LARGE,
                   config, sizeof(rockx_config_t));
  if (rockx_ret != ROCKX_RET_SUCCESS) {
    printf("init error %d\n", rockx_ret);
    g_flag_run = 0;
    return NULL;
  }
  printf("rockx_create success\n");
  // create rockx_object_array_t for store result
  rockx_object_array_t person_array;
  memset(&person_array, 0, sizeof(person_array));
  // create a object track handle
  rockx_handle_t object_track_handle;
  rockx_ret =
      rockx_create(&object_track_handle, ROCKX_MODULE_OBJECT_TRACK, NULL, 0);
  if (rockx_ret != ROCKX_RET_SUCCESS) {
    printf("init rockx module ROCKX_MODULE_OBJECT_DETECTION error %d\n",
           rockx_ret);
    g_flag_run = 0;
    return NULL;
  }

  MEDIA_BUFFER buffer = NULL;
  while (g_flag_run) {
    buffer = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, RKNN_INDEX, -1);
    if (!buffer) {
      continue;
    }
    input_image.size = RK_MPI_MB_GetSize(buffer);
    input_image.data = RK_MPI_MB_GetPtr(buffer);
    // detect object
    // long nn_before_time = getCurrentTimeMsec();
    // different method for person_detect and face_detect
    rockx_ret =
        rockx_face_detect(object_det_handle, &input_image, &person_array, NULL);
    if (rockx_ret != ROCKX_RET_SUCCESS) {
      printf("rockx_person_detect error %d\n", rockx_ret);
      g_flag_run = 0;
    }
    // long nn_after_time = getCurrentTimeMsec();
    // printf("Algorithm time-consuming is %ld\n", (nn_after_time -
    // nn_before_time));
    // if (person_array.count)
    //   printf("person_array.count is %d\n", person_array.count);

    // process result
    if (person_array.count > 0) {
      rknn_list_push(rknn_list_, getCurrentTimeMsec(), person_array);
      int size = rknn_list_size(rknn_list_);
      if (size >= MAX_RKNN_LIST_NUM)
        rknn_list_drop(rknn_list_);
      // printf("size is %d\n", size);
    }
    RK_MPI_MB_ReleaseBuffer(buffer);
  }
  // release
  rockx_image_release(&input_image);
  rockx_destroy(object_track_handle);
  rockx_destroy(object_det_handle);
  rockx_release_config(config);
  return NULL;
}

static void *MainStream(void *arg) {
  printf("#Start %s thread, arg:%p\n", __func__, arg);
  MEDIA_BUFFER mb;
  float x_rate = draw_w / rknn_w;
  float y_rate = draw_h / rknn_h;
  printf("x_rate is %f, y_rate is %f\n", x_rate, y_rate);

  while (g_flag_run) {
    mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, DRAW_INDEX, -1);
    if (!mb)
      continue;
    // draw
    if (rknn_list_size(rknn_list_)) {
      long time_before;
      rockx_object_array_t person_array;
      memset(&person_array, 0, sizeof(person_array));
      rknn_list_pop(rknn_list_, &time_before, &person_array);
      // printf("time interval is %ld\n", getCurrentTimeMsec() - time_before);

      for (int j = 0; j < person_array.count; j++) {
        int x = person_array.object[j].box.left * x_rate;
        int y = person_array.object[j].box.top * y_rate;
        int w = (person_array.object[j].box.right -
                 person_array.object[j].box.left) *
                x_rate;
        int h = (person_array.object[j].box.bottom -
                 person_array.object[j].box.top) *
                y_rate;
        if (x < 0)
          x = 0;
        if (y < 0)
          y = 0;
        while ((int)(x + w) >= draw_w) {
          w -= 16;
        }
        while ((int)(y + h) >= draw_h) {
          h -= 16;
        }
        // printf("border=(%d %d %d %d)\n", x, y, w, h);
        nv12_border((char *)RK_MPI_MB_GetPtr(mb), draw_w, draw_h, x, y, w, h, 0,
                    0, 255);
      }
    }
    // send from VI to VO
    usleep(30 * 1000);
    RK_MPI_SYS_SendMediaBuffer(RK_ID_RGA, 0, mb);
    RK_MPI_MB_ReleaseBuffer(mb);
  }

  return NULL;
}

static RK_CHAR optstr[] = "?a::c:v:l:sr:p:t:f:m:I:M:";
static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"data_version", required_argument, NULL, 'v'},
    {"runtime_path", required_argument, NULL, 'r'},
    {"fps", required_argument, NULL, 'f'},
    {"hdr_mode", required_argument, NULL, 'm'},
    {"camid", required_argument, NULL, 'I'},
    {"multictx", required_argument, NULL, 'M'},
    {"help", no_argument, NULL, '?'},
    {NULL, 0, NULL, 0},
};

static void print_usage(const RK_CHAR *name) {
  printf("usage example:\n");
  printf("\t%s [-a [iqfiles_dir]] "
         "[-I 0] "
         "[-M 0] "
         "[-v 0] "
         "[-r librknn_runtime.so] "
         "[-f 30] "
         "[-m 0] "
         "[-s] \n",
         name);
  printf("\t-a | --aiq: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, "
         "set dirpath empty to using path by default, without this option aiq "
         "should run in other application\n");
  printf("\t-M | --multictx: switch of multictx in isp, set 0 to disable, set "
         "1 to enable. Default: 0\n");
  printf("\t-I | --camid: camera ctx id, Default 0\n");
  printf("\t-r | --runtime_path, rknn runtime lib, Default: "
         "\"/usr/lib/librknn_runtime.so\"\n");
  printf("\t-f | --fps Default:30\n");
  printf("\t-m | --hdr_mode Default:0, set 1 to open hdr, set 0 to close\n");
}

int main(int argc, char **argv) {
  RK_S32 ret = 0;
  RK_CHAR *pIqfilesPath = NULL;
  sprintf(runtime_path, "/usr/lib/librknn_runtime.so");
  RK_S32 s32CamId = 0;
#ifdef RKAIQ
  RK_BOOL bMultictx = RK_FALSE;
  rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
  int fps = 30;
#endif
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
        pIqfilesPath = "/oem/etc/iqfiles";
      }
      break;
    case 'r':
      memset(&runtime_path, 0, sizeof(runtime_path));
      sprintf(runtime_path, optarg);
      break;
    case 'f':
#ifdef RKAIQ
      fps = atoi(optarg);
#endif
      break;
    case 'm':
#ifdef RKAIQ
      if (atoi(optarg)) {
        hdr_mode = RK_AIQ_WORKING_MODE_ISP_HDR2;
      }
#endif
      break;
    case 'I':
      s32CamId = atoi(optarg);
      break;
#ifdef RKAIQ
    case 'M':
      if (atoi(optarg)) {
        bMultictx = RK_TRUE;
      }
      break;
#endif
    case '?':
    default:
      print_usage(argv[0]);
      return 0;
    }
  }

#ifdef RKAIQ
  printf("####Aiq xml dirpath: %s\n\n", pIqfilesPath);
  printf("####hdr mode: %d\n\n", hdr_mode);
  printf("####fps: %d\n\n", fps);
  printf("#bMultictx: %d\n\n", bMultictx);
#endif
  printf("#CameraIdx: %d\n\n", s32CamId);
  printf("####runtime path: %s\n\n", runtime_path);

  signal(SIGINT, sig_proc);

  if (pIqfilesPath) {
#ifdef RKAIQ
    SAMPLE_COMM_ISP_Init(s32CamId, hdr_mode, bMultictx, pIqfilesPath);
    SAMPLE_COMM_ISP_Run(s32CamId);
    SAMPLE_COMM_ISP_SetFrameRate(s32CamId, fps);
#endif
  }

  // init mpi
  RK_MPI_SYS_Init();

  create_rknn_list(&rknn_list_);

  // VI
  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = draw_w;
  vi_chn_attr.u32Height = draw_h;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.pcVideoNode = "rkispp_scale0";
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  ret |= RK_MPI_VI_SetChnAttr(s32CamId, DRAW_INDEX, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(s32CamId, DRAW_INDEX);
  RK_MPI_VI_StartStream(s32CamId, DRAW_INDEX);

  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = rknn_w;
  vi_chn_attr.u32Height = rknn_h;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  vi_chn_attr.pcVideoNode = "rkispp_scale1";
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  ret |= RK_MPI_VI_SetChnAttr(s32CamId, RKNN_INDEX, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(s32CamId, RKNN_INDEX);
  RK_MPI_VI_StartStream(s32CamId, RKNN_INDEX);

  // RGA
  RGA_ATTR_S stRgaAttr;
  memset(&stRgaAttr, 0, sizeof(stRgaAttr));
  stRgaAttr.bEnBufPool = RK_TRUE;
  stRgaAttr.u16BufPoolCnt = 3;
  stRgaAttr.u16Rotaion = 90;
  stRgaAttr.stImgIn.u32X = 0;
  stRgaAttr.stImgIn.u32Y = 0;
  stRgaAttr.stImgIn.imgType = IMAGE_TYPE_NV12;
  stRgaAttr.stImgIn.u32Width = draw_w;
  stRgaAttr.stImgIn.u32Height = draw_h;
  stRgaAttr.stImgIn.u32HorStride = draw_w;
  stRgaAttr.stImgIn.u32VirStride = draw_h;
  stRgaAttr.stImgOut.u32X = 0;
  stRgaAttr.stImgOut.u32Y = 0;
  stRgaAttr.stImgOut.imgType = IMAGE_TYPE_NV12;
  stRgaAttr.stImgOut.u32Width = display_w;
  stRgaAttr.stImgOut.u32Height = display_h;
  stRgaAttr.stImgOut.u32HorStride = display_w;
  stRgaAttr.stImgOut.u32VirStride = display_h;
  ret = RK_MPI_RGA_CreateChn(0, &stRgaAttr);
  if (ret) {
    printf("Create rga[0] falied! ret=%d\n", ret);
    return -1;
  }

  // VO
  VO_CHN_ATTR_S stVoAttr = {0};
  // VO[0] for primary plane
  stVoAttr.pcDevNode = "/dev/dri/card0";
  stVoAttr.emPlaneType = VO_PLANE_OVERLAY;
  stVoAttr.enImgType = IMAGE_TYPE_NV12;
  stVoAttr.u16Zpos = 0;
  stVoAttr.stDispRect.s32X = 0;
  stVoAttr.stDispRect.s32Y = 0;
  stVoAttr.stDispRect.u32Width = display_w;
  stVoAttr.stDispRect.u32Height = display_h;
  ret = RK_MPI_VO_CreateChn(0, &stVoAttr);
  if (ret) {
    printf("Create vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  // Bind RGA VO
  MPP_CHN_S stSrcChn = {0};
  MPP_CHN_S stDestChn = {0};
  printf("# Bind RGA[0] to VO[0]....\n");
  stSrcChn.enModId = RK_ID_RGA;
  stSrcChn.s32ChnId = 0;
  stDestChn.enModId = RK_ID_VO;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Bind rga[0] to vo[0] failed! ret=%d\n", ret);
    return -1;
  }

  // Get the sub-stream buffer for humanoid recognition
  pthread_t read_thread;
  pthread_create(&read_thread, NULL, RknnThread, NULL);
  // The mainstream draws a box asynchronously based on the recognition result
  pthread_t main_stream_thread;
  pthread_create(&main_stream_thread, NULL, MainStream, NULL);

  while (g_flag_run) {
    usleep(1000 * 1000);
  }

  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("UnBind vi[0] to rga[0] failed! ret=%d\n", ret);
    return -1;
  }
  RK_MPI_VO_DestroyChn(0);
  RK_MPI_RGA_DestroyChn(0);
  RK_MPI_VI_DisableChn(s32CamId, DRAW_INDEX);
  RK_MPI_VI_DisableChn(s32CamId, RKNN_INDEX);
  if (pIqfilesPath) {
#ifdef RKAIQ
    SAMPLE_COMM_ISP_Stop(s32CamId);
#endif
  }
  destory_rknn_list(&rknn_list_);

  return 0;
}
