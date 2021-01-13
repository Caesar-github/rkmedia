// Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "librtsp/rtsp_demo.h"
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

#include "common/sample_common.h"
#include "rkmedia_api.h"
#include "rkmedia_venc.h"

#define MAIN_ORDER 0
#define SUB_ORDER 1
#define THIRD_ORDER 2

rtsp_demo_handle g_rtsplive = NULL;
static rtsp_session_handle g_rtsp_session[3];
static FILE *g_save_file[3];
static bool quit = false;
static int g_buf_flag = 0;
static RK_S32 s32CamId = 0;
static RK_BOOL bMultictx = RK_FALSE;

static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  quit = true;
}

int StreamOn(int width, int height, IMAGE_TYPE_E img_type,
             const char *video_node, int sec) {
  static int stream_on_cnt = 0;
  int ret = 0;

  CODEC_TYPE_E codec_type;
  if (stream_on_cnt % 2)
    codec_type = RK_CODEC_TYPE_H264;
  else
    codec_type = RK_CODEC_TYPE_H265;

  printf("\n### %04d, wxh: %dx%d, CodeType: %s, ImgType: %s Start........\n\n",
         stream_on_cnt++, width, height,
         (codec_type == RK_CODEC_TYPE_H264) ? "H264" : "H265",
         (img_type == IMAGE_TYPE_FBC0) ? "FBC0" : "NV12");

  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = video_node;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = width;
  vi_chn_attr.u32Height = height;
  vi_chn_attr.enPixFmt = img_type;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(s32CamId, 0, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(s32CamId, 0);
  if (ret) {
    printf("Create Vi failed! ret=%d\n", ret);
    return -1;
  }

  if (img_type == IMAGE_TYPE_FBC0) {
    printf("TEST: INFO: FBC0 use rkispp_scale0 for luma caculation!\n");
    vi_chn_attr.pcVideoNode = "rkispp_scale0";
    vi_chn_attr.u32BufCnt = 3;
    vi_chn_attr.u32Width = 1280;
    vi_chn_attr.u32Height = 720;
    vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
    vi_chn_attr.enWorkMode = VI_WORK_MODE_LUMA_ONLY;
    ret = RK_MPI_VI_SetChnAttr(s32CamId, 3, &vi_chn_attr);
    ret |= RK_MPI_VI_EnableChn(s32CamId, 3);
    if (ret) {
      printf("Create Vi[3] for luma failed! ret=%d\n", ret);
      return -1;
    }
  }

  VENC_CHN_ATTR_S venc_chn_attr;
  memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
  venc_chn_attr.stVencAttr.enType = codec_type;
  venc_chn_attr.stVencAttr.imageType = img_type;
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
  ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);
  if (ret) {
    printf("Create avc failed! ret=%d\n", ret);
    return -1;
  }

  memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
  venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_JPEG;
  venc_chn_attr.stVencAttr.imageType = img_type;
  venc_chn_attr.stVencAttr.u32PicWidth = width;
  venc_chn_attr.stVencAttr.u32PicHeight = height;
  venc_chn_attr.stVencAttr.u32VirWidth = width;
  venc_chn_attr.stVencAttr.u32VirHeight = height;
  // venc_chn_attr.stVencAttr.enRotation = VENC_ROTATION_90;
  ret = RK_MPI_VENC_CreateChn(3, &venc_chn_attr);
  if (ret) {
    printf("Create jpeg failed! ret=%d\n", ret);
    return -1;
  }

  // The encoder defaults to continuously receiving frames from the previous
  // stage. Before performing the bind operation, set s32RecvPicNum to 0 to
  // make the encoding enter the pause state.
  VENC_RECV_PIC_PARAM_S stRecvParam;
  stRecvParam.s32RecvPicNum = 0;
  RK_MPI_VENC_StartRecvFrame(3, &stRecvParam);

  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = 0;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = 0;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Create RK_MPI_SYS_Bind0 failed! ret=%d\n", ret);
    return -1;
  }

  stDestChn.s32ChnId = 3;
  ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("Create RK_MPI_SYS_Bind1 failed! ret=%d\n", ret);
    return -1;
  }

  printf("%s initial finish\n", __func__);

  int loop_cnt = sec * 2;
  RECT_S stRects[2] = {{0, 0, 256, 256}, {256, 256, 256, 256}};

  VI_CHN luma_chn = 0;
  if (img_type == IMAGE_TYPE_FBC0) {
    luma_chn = 3;
    ret = RK_MPI_VI_StartStream(s32CamId, luma_chn);
    if (ret) {
      printf("ERROR: RK_MPI_VI_StartStream ret=%d\n", ret);
      return -1;
    }
  }

  VIDEO_REGION_INFO_S stVideoRgn;
  stVideoRgn.pstRegion = stRects;
  stVideoRgn.u32RegionNum = 2;
  RK_U64 u64LumaData[2];
  while (!quit) {
    usleep(500000);
    loop_cnt--;

    ret = RK_MPI_VI_GetChnRegionLuma(s32CamId, luma_chn, &stVideoRgn,
                                     u64LumaData, 100);
    if (ret) {
      printf("ERROR: VI[%d]:RK_MPI_VI_GetChnRegionLuma ret = %d\n", luma_chn,
             ret);
    } else {
      printf("VI[%d]:Rect[0] {0, 0, 256, 256} -> luma:%llu\n", luma_chn,
             u64LumaData[0]);
      printf("VI[%d]:Rect[1] {256, 256, 256, 256} -> luma:%llu\n", luma_chn,
             u64LumaData[1]);
    }
    if (loop_cnt < 0)
      break;
  }

  return 0;
}

int StreamOff(IMAGE_TYPE_E img_type) {
  printf("%s exit!\n", __func__);
  MPP_CHN_S stSrcChn;
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = 0;
  MPP_CHN_S stDestChn;
  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = 0;

  int ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("ERROR: unbind vi[0] -> venc[0] failed!\n");
    return -1;
  }
  stDestChn.s32ChnId = 3;
  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (ret) {
    printf("ERROR: unbind vi[0] -> venc[3] failed!\n");
    return -1;
  }

  RK_MPI_VENC_DestroyChn(0); // avc/hevc
  RK_MPI_VENC_DestroyChn(3); // jpeg
  RK_MPI_VI_DisableChn(s32CamId, 0);
  if (img_type == IMAGE_TYPE_FBC0) {
    printf("TEST: INFO: Disable luma caculation vi[3]!\n");
    RK_MPI_VI_DisableChn(s32CamId, 3);
  }
  printf("\n-------------------END----------------------------\n");
  return 0;
}

int SubStreamOn(int width, int height, const char *video_node, int vi_chn,
                int venc_chn) {
  static int sub_stream_cnt = 0;
  int ret = 0;

  printf("*** SubStreamOn[%d]: VideoNode:%s, wxh:%dx%d, vi:%d, venc:%d "
         "START....\n",
         sub_stream_cnt, video_node, width, height, vi_chn, venc_chn);

  CODEC_TYPE_E codec_type = RK_CODEC_TYPE_H264;
  VI_CHN_ATTR_S vi_chn_attr;
  vi_chn_attr.pcVideoNode = video_node;
  vi_chn_attr.u32BufCnt = 3;
  vi_chn_attr.u32Width = width;
  vi_chn_attr.u32Height = height;
  vi_chn_attr.enPixFmt = IMAGE_TYPE_NV12;
  vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
  ret = RK_MPI_VI_SetChnAttr(s32CamId, vi_chn, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(s32CamId, vi_chn);
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

  sub_stream_cnt++;
  printf("*** SubStreamOn: END....\n");

  return 0;
}

int SubStreamOff(int vi_chn, int venc_chn) {
  int ret = 0;

  printf("*** SubStreamOff: vi:%d, venc:%d START....\n", vi_chn, venc_chn);

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

  ret = RK_MPI_VENC_DestroyChn(venc_chn);
  if (ret) {
    printf("ERROR: disable venc[%d] failed, ret: %d\n", vi_chn, ret);
    return -1;
  }

  ret = RK_MPI_VI_DisableChn(s32CamId, vi_chn);
  if (ret) {
    printf("ERROR: disable vi[%d] failed, ret: %d\n", vi_chn, ret);
    return -1;
  }

  printf("*** SubStreamOff: END....\n");

  return 0;
}

static void *GetStreamThread() {
  while (g_buf_flag && !quit) {
    for (int i = 0; i < 3; i++) {
      MEDIA_BUFFER buffer;
      buffer = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, i, 0);
      if (buffer) {
        rtsp_tx_video(g_rtsp_session[i], RK_MPI_MB_GetPtr(buffer),
                      RK_MPI_MB_GetSize(buffer),
                      RK_MPI_MB_GetTimestamp(buffer));
        if (g_save_file[i])
          fwrite(RK_MPI_MB_GetPtr(buffer), 1, RK_MPI_MB_GetSize(buffer),
                 g_save_file[i]);
        // printf("cms buf:%p, size:%d, tm:%lld\n", RK_MPI_MB_GetPtr(buffer),
        // RK_MPI_MB_GetSize(buffer), RK_MPI_MB_GetTimestamp(buffer));
        RK_MPI_MB_ReleaseBuffer(buffer);
      }
    }
    rtsp_do_event(g_rtsplive);
  }
  return NULL;
}

static char optstr[] = "?::c:s:w:h:a::I:M:";

static const struct option long_options[] = {
    {"aiq", optional_argument, NULL, 'a'},
    {"count", required_argument, NULL, 'c'},
    {"seconds", required_argument, NULL, 's'},
    {"width", required_argument, NULL, 'w'},
    {"height", required_argument, NULL, 'h'},
    {"camid", required_argument, NULL, 'I'},
    {"multictx", required_argument, NULL, 'M'},
    {"help", optional_argument, NULL, '?'},
    {NULL, 0, NULL, 0},
};

static void print_usage(char *name) {
  printf("#Function description:\n");
  printf("There are three streams in total:MainStream/SubStream0/SubStream.\n"
         "The sub-stream remains unchanged, and the resolution of the main \n"
         "stream is constantly switched.\n");
  printf("  SubStream0: rkispp_scale1: 720x480 NV12 -> /userdata/sub0.h264(150 "
         "frames)\n");
  printf("  SubStream1: rkispp_scale2: 1280x720 NV12 -> "
         "/userdata/sub1.h264(150 frames)\n");
  printf("  MainStream: case1: rkispp_m_bypass: widthxheight FBC0\n");
  printf("                     rkispp_scale0: 1280x720 for luma caculation.\n");
  printf("  MainStream: case2: rkispp_scale0: 1280x720 NV12\n");
  printf("#Usage Example: \n");
  printf("  %s [-c 20] [-s 5] [-w 3840] [-h 2160] [-a the path of iqfiles]\n",
         name);
  printf("  @[-c] Main stream switching times. defalut:20\n");
  printf("  @[-s] The duration of the main stream. default:5\n");
  printf("  @[-w] img width for rkispp_m_bypass. default: 3840\n");
  printf("  @[-h] img height for rkispp_m_bypass. default: 2160\n");
  printf("  @[-a] the path of iqfiles. default: NULL\n");
  printf("  @[-I] camera ctx id, Default 0\n");
  printf("  @[-M] switch of multictx in isp, set 0 to disable, set 1 to "
         "enable. Default: 0\n");
}

int main(int argc, char *argv[]) {
  int loop_cnt = 20;
  int loop_seconds = 5; // 5s
  int width = 3840;
  int height = 2160;
  char *iq_file_dir = NULL;

  int c = 0;
  opterr = 1;
  while ((c = getopt_long(argc, argv, optstr, long_options, NULL)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'c':
      loop_cnt = atoi(optarg);
      printf("#IN ARGS: loop_cnt: %d\n", loop_cnt);
      break;
    case 's':
      loop_seconds = atoi(optarg);
      printf("#IN ARGS: loop_seconds: %d\n", loop_seconds);
      break;
    case 'w':
      width = atoi(optarg);
      printf("#IN ARGS: bypass width: %d\n", width);
      break;
    case 'h':
      height = atoi(optarg);
      printf("#IN ARGS: bypass height: %d\n", height);
      break;
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }
      if (tmp_optarg) {
        iq_file_dir = (char *)tmp_optarg;
      } else {
        iq_file_dir = "/oem/etc/iqfiles";
      }
      printf("#IN ARGS: the path of iqfiles: %s\n", iq_file_dir);
      break;
    case 'I':
      s32CamId = atoi(optarg);
      printf("#IN ARGS: s32CamId: %d\n", s32CamId);
      break;
    case 'M':
      if (atoi(optarg)) {
        bMultictx = RK_TRUE;
      }
      printf("#IN ARGS: bMultictx: %d\n", bMultictx);
      break;
    case '?':
    default:
      print_usage(argv[0]);
      exit(0);
    }
  }

  printf(">>>>>>>>>>>>>>> Test START <<<<<<<<<<<<<<<<<<<<<<\n");
  printf("-->LoopCnt:%d\n", loop_cnt);
  printf("-->LoopSeconds:%d\n", loop_seconds);
  printf("-->BypassWidth:%d\n", width);
  printf("-->BypassHeight:%d\n", height);
  printf("-->aiq xml dirpath:%s\n", iq_file_dir);

  g_rtsplive = create_rtsp_demo(554);
  g_rtsp_session[MAIN_ORDER] =
      rtsp_new_session(g_rtsplive, "/live/main_stream");
  g_rtsp_session[SUB_ORDER] = rtsp_new_session(g_rtsplive, "/live/sub_stream");
  g_rtsp_session[THIRD_ORDER] =
      rtsp_new_session(g_rtsplive, "/live/third_stream");
  rtsp_set_video(g_rtsp_session[MAIN_ORDER], RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
  rtsp_set_video(g_rtsp_session[SUB_ORDER], RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
  rtsp_set_video(g_rtsp_session[THIRD_ORDER], RTSP_CODEC_ID_VIDEO_H264, NULL,
                 0);
  rtsp_sync_video_ts(g_rtsp_session[MAIN_ORDER], rtsp_get_reltime(),
                     rtsp_get_ntptime());
  rtsp_sync_video_ts(g_rtsp_session[SUB_ORDER], rtsp_get_reltime(),
                     rtsp_get_ntptime());
  rtsp_sync_video_ts(g_rtsp_session[THIRD_ORDER], rtsp_get_reltime(),
                     rtsp_get_ntptime());

  RK_MPI_SYS_Init();
  signal(SIGINT, sigterm_handler);

  IMAGE_TYPE_E img_type_used;
  for (int i = 0; !quit && (i < loop_cnt); i++) {
    if (iq_file_dir) {
#ifdef RKAIQ
      rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
      int fps = 30;
      SAMPLE_COMM_ISP_Init(s32CamId, hdr_mode, bMultictx, iq_file_dir);
      SAMPLE_COMM_ISP_Run(s32CamId);
      SAMPLE_COMM_ISP_SetFrameRate(s32CamId, fps);
#endif
    }
    g_save_file[SUB_ORDER] = fopen("/tmp/sub0.h264", "w");
    if (SubStreamOn(720, 480, "rkispp_scale1", 1, 1)) {
      printf("ERROR: SubStreamOn failed!\n");
      quit = true;
    }

    g_save_file[THIRD_ORDER] = fopen("/tmp/sub1.h264", "w");
    if (SubStreamOn(1280, 720, "rkispp_scale2", 2, 2)) {
      printf("ERROR: SubStreamOn failed!\n");
      quit = true;
    }

    pthread_t stream_thread;
    g_buf_flag = 1;
    pthread_create(&stream_thread, NULL, GetStreamThread, NULL);

    // mainstream switch
    if (i % 2 == 0) {
      g_save_file[MAIN_ORDER] = fopen("/tmp/fbc0.h264", "w");
      img_type_used = IMAGE_TYPE_FBC0;
      if (StreamOn(width, height, IMAGE_TYPE_FBC0, "rkispp_m_bypass",
                   loop_seconds)) {
        printf("ERROR: StreamOn 2k failed!, w:%d, h:%d\n", width, height);
        quit = true;
      }
    } else {
      g_save_file[MAIN_ORDER] = fopen("/tmp/720p.h264", "w");
      img_type_used = IMAGE_TYPE_NV12;
      if (StreamOn(1280, 720, IMAGE_TYPE_NV12, "rkispp_scale0", loop_seconds)) {
        printf("ERROR: StreamOn 720p failed!\n");
        quit = true;
      }
    }

    g_buf_flag = 0;

    if (StreamOff(img_type_used)) {
      printf("ERROR: StreamOff failed!\n");
      quit = true;
    }

    if (SubStreamOff(1, 1)) {
      printf("ERROR: SubStreamOff 1 failed!\n");
      quit = true;
    }

    if (SubStreamOff(2, 2)) {
      printf("ERROR: SubStreamOff 2 failed!\n");
      quit = true;
    }

    if (iq_file_dir) {
#ifdef RKAIQ
      SAMPLE_COMM_ISP_Stop(s32CamId);
#endif
    }

    for (int j = 0; j < 3; j++) {
      fclose(g_save_file[j]);
      g_save_file[j] = NULL;
    }
  }
  quit = true;
  for (int i = 0; i < 3; i++) {
    if (g_save_file[i]) {
      fclose(g_save_file[i]);
    }
  }
  rtsp_del_demo(g_rtsplive);
  printf(">>>>>>>>>>>>>>> Test END <<<<<<<<<<<<<<<<<<<<<<\n");
  return 0;
}
