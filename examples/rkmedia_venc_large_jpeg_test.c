// Copyright 2021 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// RV1126 JPEG VENC width is limited to 4096.
// When the width of the encoded picture is greater than 4096,
// it needs to be rotated by RGA by 90 degrees, then sent to the encoder,
// and then rotated by 270 degrees after encode.
// example: 2688*1520(NV12) →RGA rotate 90°→ 1520*2688(NV12) →RGA interpolation→
// 3040*5376(NV12)
//          →JPEG VENC→ 3040*5376(JPEG) →RGA rotate 270°→ 5376*3040(JPEG)

#include <assert.h>
#include <fcntl.h>
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

#include "rkmedia_api.h"
#include <rga/im2d.h>
#include <rga/rga.h>

int complete_flag = 0;
const char *input_file_path = NULL;
const char *output_file_path = NULL;

int src_width = 2688;
int src_height = 1520;

int mid_width = 1520;
int mid_height = 2688;

// 16M 5376 * 3040
int dst_width = 5376;
int dst_height = 3040;

long time_before;
long time_after;

MEDIA_BUFFER mb_src, mb_mid, mb_dst;

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

void video_packet_cb(MEDIA_BUFFER mb) {
  time_after = getCurrentTimeMsec();
  printf("jpeg encode success, %ld ms\n", time_after - time_before);
  printf("Get JPEG packet:ptr:%p, fd:%d, size:%zu, mode:%d, channel:%d, "
         "timestamp:%lld\n",
         RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetFD(mb), RK_MPI_MB_GetSize(mb),
         RK_MPI_MB_GetModeID(mb), RK_MPI_MB_GetChannelID(mb),
         RK_MPI_MB_GetTimestamp(mb));

  FILE *file = fopen(output_file_path, "wb");
  if (file) {
    fwrite(RK_MPI_MB_GetPtr(mb), 1, RK_MPI_MB_GetSize(mb), file);
    fclose(file);
  }

  RK_MPI_MB_ReleaseBuffer(mb);
  complete_flag = 1;
}

static int large_photo_encode() {
  MB_IMAGE_INFO_S mid_info = {mid_height, mid_width, mid_height, mid_width,
                              IMAGE_TYPE_RGB888};
  // RGA buffer has been rotated 90 degrees, width and height are reversed
  int rga_dst_width = dst_height;
  int rga_dst_height = dst_width;
  MB_IMAGE_INFO_S dst_info = {rga_dst_width, rga_dst_height, rga_dst_width,
                              rga_dst_height, IMAGE_TYPE_ARGB8888};
  mb_mid = RK_MPI_MB_CreateImageBuffer(&mid_info, RK_TRUE, 0);
  mb_dst = RK_MPI_MB_CreateImageBuffer(&dst_info, RK_TRUE, 0);
  if (!mb_mid || !mb_dst) {
    printf("ERROR: no space left!\n");
    return -1;
  }
  printf("mb_mid mb_dst RK_MPI_MB_CreateImageBuffer success\n");
  rga_buffer_t src, dst;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  src = wrapbuffer_fd(RK_MPI_MB_GetFD(mb_src), src_width, src_height,
                      RK_FORMAT_YCbCr_420_SP);
  dst = wrapbuffer_fd(RK_MPI_MB_GetFD(mb_mid), mid_width, mid_height,
                      RK_FORMAT_BGR_888);
  time_before = getCurrentTimeMsec();
  if (imrotate(src, dst, IM_HAL_TRANSFORM_ROT_90) != IM_STATUS_SUCCESS) {
    printf("%s: %d: imrotate failed!\n", __func__, __LINE__);
    return -1;
  }
  time_after = getCurrentTimeMsec();
  printf("imrotate success, %ld ms\n", time_after - time_before);
  RK_MPI_MB_ReleaseBuffer(mb_src);

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  src = wrapbuffer_fd(RK_MPI_MB_GetFD(mb_mid), mid_width, mid_height / 2,
                      RK_FORMAT_BGR_888);
  dst = wrapbuffer_fd(RK_MPI_MB_GetFD(mb_dst), rga_dst_width,
                      rga_dst_height / 2, RK_FORMAT_RGBA_8888);
  time_before = getCurrentTimeMsec();
  if (imresize(src, dst) != IM_STATUS_SUCCESS) {
    printf("%s: %d: imresize failed!\n", __func__, __LINE__);
    return -1;
  }
  time_after = getCurrentTimeMsec();
  printf("first imresize success, %ld ms\n", time_after - time_before);

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  src = wrapbuffer_virtualaddr((char *)RK_MPI_MB_GetPtr(mb_mid) +
                                   mid_width * mid_height / 2 * 3,
                               mid_width, mid_height / 2, RK_FORMAT_BGR_888);
  dst = wrapbuffer_virtualaddr(
      (char *)RK_MPI_MB_GetPtr(mb_dst) + rga_dst_width * rga_dst_height / 2 * 4,
      rga_dst_width, rga_dst_height / 2, RK_FORMAT_RGBA_8888);
  time_before = getCurrentTimeMsec();
  if (imresize(src, dst) != IM_STATUS_SUCCESS) {
    printf("%s: %d: imresize failed!\n", __func__, __LINE__);
    return -1;
  }
  time_after = getCurrentTimeMsec();
  printf("second imresize success, %ld ms\n", time_after - time_before);

  // FILE *fp;
  // fp = fopen("/userdata/mb_src.nv12", "wb");
  // fwrite((char *)RK_MPI_MB_GetPtr(mb_src), 1, src_width * src_height * 1.5,
  // fp);
  // fflush(fp);
  // fclose(fp);

  // fp = fopen("/userdata/mb_mid.bgr", "wb");
  // fwrite((char *)RK_MPI_MB_GetPtr(mb_mid), 1, mid_width * mid_height * 3,
  // fp);
  // fflush(fp);
  // fclose(fp);

  // fp = fopen("/userdata/mb_dst.rgba", "wb");
  // fwrite((char *)RK_MPI_MB_GetPtr(mb_dst), 1, rga_dst_width * rga_dst_height
  // * 4, fp); fflush(fp); fclose(fp);

  int ret;
  VENC_CHN_ATTR_S venc_chn_attr;
  memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
  venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_JPEG;
  venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_RGBA8888;
  venc_chn_attr.stVencAttr.u32PicWidth = rga_dst_width;
  venc_chn_attr.stVencAttr.u32PicHeight = rga_dst_height;
  venc_chn_attr.stVencAttr.u32VirWidth = rga_dst_width;
  venc_chn_attr.stVencAttr.u32VirHeight = rga_dst_height;
  venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomWidth = rga_dst_width;
  venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomHeight = rga_dst_height;
  venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomVirWidth = rga_dst_width;
  venc_chn_attr.stVencAttr.stAttrJpege.u32ZoomVirHeight = rga_dst_height;
  venc_chn_attr.stVencAttr.enRotation = VENC_ROTATION_270;
  ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);
  if (ret) {
    printf("Create Venc failed! ret=%d\n", ret);
    return -1;
  }
  printf("Create Venc success\n");
  VENC_JPEG_PARAM_S stJpegParam;
  stJpegParam.u32Qfactor = 70;
  RK_MPI_VENC_SetJpegParam(0, &stJpegParam);

  MPP_CHN_S stEncChn;
  stEncChn.enModId = RK_ID_VENC;
  stEncChn.s32ChnId = 0;
  ret = RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
  time_before = getCurrentTimeMsec();
  RK_MPI_SYS_SendMediaBuffer(RK_ID_VENC, 0, mb_dst);

  return 0;
}

static const char short_options[] = "i:o:w:h:W:H:?";
static const struct option long_options[] = {
    {"input_file", required_argument, NULL, 'i'},
    {"output_file", required_argument, NULL, 'o'},
    {"src_width", required_argument, NULL, 'w'},
    {"src_height", no_argument, NULL, 'h'},
    {"dst_width", no_argument, NULL, 'W'},
    {"dst_height", no_argument, NULL, 'H'},
    {0, 0, 0, 0}};

static void usage_tip(FILE *fp, char **argv) {
  fprintf(fp,
          "Usage: %s [options]\n"
          "Version %s\n"
          "Options:\n"
          "-i | --input_file, default is /userdata/test.nv12 \n"
          "-o | --output_file, default is /userdata/test.jpeg \n"
          "-w | --src_width \n"
          "-h | --src_height \n"
          "-W | --dst_width \n"
          "-H | --dst_height \n"
          "-? | --help for help \n\n"
          "\n",
          argv[0], "V1.0");
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  for (;;) {
    int idx;
    int c;
    c = getopt_long(argc, argv, short_options, long_options, &idx);
    if (-1 == c)
      break;
    switch (c) {
    case 0: /* getopt_long() flag */
      break;
    case 'i':
      input_file_path = optarg;
      break;
    case 'o':
      output_file_path = optarg;
      break;
    case 'w':
      src_width = atoi(optarg);
      break;
    case 'h':
      src_height = atoi(optarg);
      break;
    case 'W':
      dst_width = atoi(optarg);
      break;
    case 'H':
      dst_height = atoi(optarg);
      break;
    case '?':
      usage_tip(stdout, argv);
      exit(EXIT_SUCCESS);
    default:
      usage_tip(stderr, argv);
      exit(EXIT_FAILURE);
    }
  }
  if (input_file_path == NULL)
    input_file_path = "/userdata/test.nv12";
  if (output_file_path == NULL)
    output_file_path = "/userdata/test.jpeg";

  printf("input_file_path is %s, output_file_path is %s\n", input_file_path,
         output_file_path);
  printf(
      "src_width is %d, src_height is %d, dst_width is %d, dst_height is %d\n",
      src_width, src_height, dst_width, dst_height);
  // total mem = src_width * src_height * 3 + dst_width * dst_height * (4 +
  // 1[mpp mem])
  printf("the theoretical peak memory is %d Byte\n",
         src_width * src_height * 3 + dst_width * dst_height * (4 + 1));

  // Rotate 90 degrees
  mid_width = src_height;
  mid_height = src_width;

  RK_MPI_SYS_Init();

  MB_IMAGE_INFO_S src_info = {src_width, src_height, src_width, src_height,
                              IMAGE_TYPE_NV12};
  mb_src = RK_MPI_MB_CreateImageBuffer(&src_info, RK_TRUE, 0);
  if (!mb_src) {
    printf("ERROR: no space left!\n");
    return -1;
  }
  printf("RK_MPI_MB_CreateImageBuffer success\n");

  FILE *fp = fopen(input_file_path, "rb");
  fread((char *)RK_MPI_MB_GetPtr(mb_src), 1, src_width * src_height * 1.5, fp);
  fflush(fp);
  fclose(fp);

  large_photo_encode();

  while (!complete_flag) {
    usleep(300 * 1000);
  }

  RK_MPI_MB_ReleaseBuffer(mb_mid);
  RK_MPI_MB_ReleaseBuffer(mb_dst);

  RK_MPI_VENC_DestroyChn(0);

  return 0;
}
