// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mpp_encoder.h"

#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <memory>

#ifdef JPEG_RGA_OSD_ENABLE
#include <rga/im2d.h>
#include <rga/rga.h>
#endif // JPEG_RGA_OSD_ENABLE

#include "buffer.h"
#include "utils.h"

#ifdef MOD_TAG
#undef MOD_TAG
#endif
#define MOD_TAG 4

namespace easymedia {

MPPEncoder::MPPEncoder()
    : coding_type(MPP_VIDEO_CodingAutoDetect), output_mb_flags(0),
      encoder_sta_en(false), stream_size_1s(0), frame_cnt_1s(0), last_ts(0),
      cur_ts(0), userdata_len(0), userdata_frame_id(0),
      userdata_all_frame_en(0) {
#ifdef MPP_SUPPORT_HW_OSD
  // reset osd data.
  memset(&osd_data, 0, sizeof(osd_data));
  osd_buf_grp = NULL;
#endif

#ifdef JPEG_RGA_OSD_ENABLE
  // reset jpeg osd data.
  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    rga_osd_data[i].data.reset();
    rga_osd_data[i].pos_x = 0;
    rga_osd_data[i].pos_y = 0;
    rga_osd_data[i].width = 0;
    rga_osd_data[i].height = 0;
    rga_osd_data[i].enable = 0;
    rga_osd_data[i].inverse = 0;
  }
  rga_osd_cnt = 0;
#endif

  memset(&roi_cfg, 0, sizeof(roi_cfg));
  rc_api_brief_name = "default";
}

MPPEncoder::~MPPEncoder() {
#ifdef MPP_SUPPORT_HW_OSD
  if (osd_data.buf) {
    RKMEDIA_LOGD("MPP Encoder: free osd buff\n");
    mpp_buffer_put(osd_data.buf);
    osd_data.buf = NULL;
  }
  if (osd_buf_grp) {
    RKMEDIA_LOGD("MPP Encoder: free osd buff group\n");
    mpp_buffer_group_put(osd_buf_grp);
    osd_buf_grp = NULL;
  }
#endif

#ifdef JPEG_RGA_OSD_ENABLE
  // reset jpeg osd data.
  for (int i = 0; i < OSD_REGIONS_CNT; i++)
    rga_osd_data[i].data.reset();
  rga_osd_cnt = 0;
#endif

  if (roi_cfg.regions) {
    RKMEDIA_LOGD("MPP Encoder: free enc roi region buff\n");
    free(roi_cfg.regions);
    roi_cfg.regions = NULL;
  }
}

void MPPEncoder::SetMppCodeingType(MppCodingType type) {
  coding_type = type;
  if (type == MPP_VIDEO_CodingMJPEG)
    codec_type = CODEC_TYPE_JPEG;
  else if (type == MPP_VIDEO_CodingAVC)
    codec_type = CODEC_TYPE_H264;
  else if (type == MPP_VIDEO_CodingHEVC)
    codec_type = CODEC_TYPE_H265;
  // mpp always return a single nal
  if (type == MPP_VIDEO_CodingAVC || type == MPP_VIDEO_CodingHEVC)
    output_mb_flags |= MediaBuffer::kSingleNalUnit;
}

bool MPPEncoder::Init() {
  int ret = 0;
  if (coding_type == MPP_VIDEO_CodingUnused)
    return false;
#ifdef MPP_SUPPORT_HW_OSD
  ret = mpp_buffer_group_get_internal(&osd_buf_grp, MPP_BUFFER_TYPE_DRM);
  if (ret) {
    RKMEDIA_LOGE("MPP Encoder: failed to get mpp buffer group! ret=%d\n", ret);
    return false;
  }
  ret = mpp_buffer_group_limit_config(osd_buf_grp, 0, 2);
  if (ret) {
    RKMEDIA_LOGE("MPP Encoder: failed to limit buffer group! ret=%d\n", ret);
    return false;
  }
#endif
  mpp_ctx = std::make_shared<MPPContext>();
  if (!mpp_ctx)
    return false;
  MppCtx ctx = NULL;
  MppApi *mpi = NULL;
  ret = mpp_create(&ctx, &mpi);
  if (ret) {
    RKMEDIA_LOGI("mpp_create failed\n");
    return false;
  }
  mpp_ctx->ctx = ctx;
  mpp_ctx->mpi = mpi;
  ret = mpp_init(ctx, MPP_CTX_ENC, coding_type);
  if (ret != MPP_OK) {
    RKMEDIA_LOGI("mpp_init failed with type %d\n", coding_type);
    mpp_destroy(ctx);
    ctx = NULL;
    mpi = NULL;
    return false;
  }

  return true;
}

int MPPEncoder::PrepareMppFrame(const std::shared_ptr<MediaBuffer> &input,
                                std::shared_ptr<MediaBuffer> &mdinfo,
                                MppFrame &frame) {
  MppBuffer pic_buf = nullptr;
  if (input->GetType() != Type::Image) {
    RKMEDIA_LOGI("mpp encoder input source only support image buffer\n");
    return -EINVAL;
  }
  PixelFormat fmt = input->GetPixelFormat();
  if (fmt == PIX_FMT_NONE) {
    RKMEDIA_LOGI("mpp encoder input source invalid pixel format\n");
    return -EINVAL;
  }
  ImageBuffer *hw_buffer = static_cast<ImageBuffer *>(input.get());

  assert(input->GetValidSize() > 0);
  mpp_frame_set_pts(frame, hw_buffer->GetUSTimeStamp());
  mpp_frame_set_dts(frame, hw_buffer->GetUSTimeStamp());
  mpp_frame_set_width(frame, hw_buffer->GetWidth());
  mpp_frame_set_height(frame, hw_buffer->GetHeight());
  mpp_frame_set_fmt(frame, ConvertToMppPixFmt(fmt));

  if (fmt == PIX_FMT_YUYV422 || fmt == PIX_FMT_UYVY422)
    mpp_frame_set_hor_stride(frame, hw_buffer->GetVirWidth() * 2);
  else
    mpp_frame_set_hor_stride(frame, hw_buffer->GetVirWidth());
  mpp_frame_set_ver_stride(frame, hw_buffer->GetVirHeight());

  MppMeta meta = mpp_frame_get_meta(frame);
  auto &related_vec = input->GetRelatedSPtrs();
  if (!related_vec.empty()) {
    mdinfo = std::static_pointer_cast<MediaBuffer>(related_vec[0]);
    RKMEDIA_LOGD("MPP Encoder: set mdinfo(%p, %zuBytes) to frame\n",
                 mdinfo->GetPtr(), mdinfo->GetValidSize());
    mpp_meta_set_ptr(meta, KEY_MV_LIST, mdinfo->GetPtr());
  }

  if (roi_cfg.number && roi_cfg.regions) {
    RKMEDIA_LOGD("MPP Encoder: set roi cfg(cnt:%d,%p) to frame\n",
                 roi_cfg.number, roi_cfg.regions);
    mpp_meta_set_ptr(meta, KEY_ROI_DATA, &roi_cfg);
  }

#ifdef MPP_SUPPORT_HW_OSD
  if (osd_data.num_region && osd_data.buf) {
    RKMEDIA_LOGD("MPP Encoder: set osd data(%d regions) to frame\n",
                 osd_data.num_region);
    mpp_meta_set_ptr(meta, KEY_OSD_DATA, (void *)&osd_data);
  }
#endif // MPP_SUPPORT_HW_OSD

#ifdef JPEG_RGA_OSD_ENABLE
  if (rga_osd_cnt > 0)
    RgaOsdRegionProcess(hw_buffer);
#endif

  int ud_idx = 0;
  if (userdata_len) {
    RKMEDIA_LOGD("MPP Encoder: set userdata(%dBytes) to frame\n", userdata_len);
    bool skip_frame = false;
    if (!userdata_all_frame_en) {
      MediaConfig &cfg = GetConfig();
      // userdata_frame_id = 0 : first gop frame.
      if (userdata_frame_id)
        skip_frame = true;

      userdata_frame_id++;
      if (userdata_frame_id == cfg.vid_cfg.gop_size)
        userdata_frame_id = 0;
    }

    if (!skip_frame) {
      ud_datas[ud_idx].pdata = userdata;
      ud_datas[ud_idx].len = userdata_len;
      ud_idx++;
    }
  }

  if (input->GetDbgInfo() && input->GetDbgInfoSize()) {
    ud_datas[ud_idx].pdata = input->GetDbgInfo();
    ud_datas[ud_idx].len = input->GetDbgInfoSize();
    ud_idx++;
  }

  if (ud_idx) {
    ud_set.count = ud_idx;
    ud_set.datas = ud_datas;
    RKMEDIA_LOGD("MPP Encoder: userdata: ud_set.count:%d, ud_set.datas:%p...\n",
         ud_set.count, ud_set.datas);
    mpp_meta_set_ptr(meta, KEY_USER_DATAS, &ud_set);
  }

  MPP_RET ret = init_mpp_buffer_with_content(pic_buf, input);
  if (ret) {
    RKMEDIA_LOGI("prepare picture buffer failed\n");
    return ret;
  }

  mpp_frame_set_buffer(frame, pic_buf);
  if (input->IsEOF())
    mpp_frame_set_eos(frame, 1);

  mpp_buffer_put(pic_buf);

  return 0;
}

int MPPEncoder::PrepareMppPacket(std::shared_ptr<MediaBuffer> &output,
                                 MppPacket &packet) {
  MppBuffer mpp_buf = nullptr;

  if (!output->IsHwBuffer())
    return 0;

  MPP_RET ret = init_mpp_buffer(mpp_buf, output, 0);
  if (ret) {
    RKMEDIA_LOGI("import output stream buffer failed\n");
    return ret;
  }

  if (mpp_buf) {
    mpp_packet_init_with_buffer(&packet, mpp_buf);
    mpp_buffer_put(mpp_buf);
  }

  return 0;
}

int MPPEncoder::PrepareMppExtraBuffer(std::shared_ptr<MediaBuffer> extra_output,
                                      MppBuffer &buffer) {
  MppBuffer mpp_buf = nullptr;
  if (!extra_output || !extra_output->IsValid())
    return 0;
  MPP_RET ret =
      init_mpp_buffer(mpp_buf, extra_output, extra_output->GetValidSize());
  if (ret) {
    RKMEDIA_LOGI("import extra stream buffer failed\n");
    return ret;
  }
  buffer = mpp_buf;
  return 0;
}

class MPPPacketContext {
public:
  MPPPacketContext(std::shared_ptr<MPPContext> ctx, MppPacket p)
      : mctx(ctx), packet(p) {}
  ~MPPPacketContext() {
    if (packet)
      mpp_packet_deinit(&packet);
  }

private:
  std::shared_ptr<MPPContext> mctx;
  MppPacket packet;
};

static int __free_mpppacketcontext(void *p) {
  assert(p);
  delete (MPPPacketContext *)p;
  return 0;
}

int MPPEncoder::Process(const std::shared_ptr<MediaBuffer> &input,
                        std::shared_ptr<MediaBuffer> &output,
                        std::shared_ptr<MediaBuffer> extra_output) {
  MppFrame frame = nullptr;
  MppPacket packet = nullptr;
  MppPacket import_packet = nullptr;
  MppBuffer mv_buf = nullptr;
  size_t packet_len = 0;
  RK_U32 packet_flag = 0;
  RK_U32 out_eof = 0;
  RK_S64 pts = 0;
  std::shared_ptr<MediaBuffer> mdinfo;
  RK_S32 temporal_id = -1;
  Type out_type;

  if (!input)
    return 0;
  if (!output)
    return -EINVAL;

  // all changes must set before encode and among the same thread
  while (HasChangeReq()) {
    auto change = PeekChange();
    if (change.first && !CheckConfigChange(change))
      return -1;
  }

  // check input buffer.
  MediaConfig &cfg_check = GetConfig();
  ImageBuffer *hw_buffer = static_cast<ImageBuffer *>(input.get());
  if (cfg_check.img_cfg.image_info.width != hw_buffer->GetWidth() ||
      cfg_check.img_cfg.image_info.height != hw_buffer->GetHeight() ||
      cfg_check.img_cfg.image_info.vir_width != hw_buffer->GetVirWidth() ||
      cfg_check.img_cfg.image_info.vir_height != hw_buffer->GetVirHeight()) {
    RKMEDIA_LOGE("The resolution of the input buffer is wrong.\n");
    return 0;
  }

  int ret = mpp_frame_init(&frame);
  if (MPP_OK != ret) {
    RKMEDIA_LOGI("mpp_frame_init failed\n");
    goto ENCODE_OUT;
  }

  ret = PrepareMppFrame(input, mdinfo, frame);
  if (ret) {
    RKMEDIA_LOGI("PrepareMppFrame failed\n");
    goto ENCODE_OUT;
  }

  if (output->IsValid()) {
    ret = PrepareMppPacket(output, packet);
    if (ret) {
      RKMEDIA_LOGI("PrepareMppPacket failed\n");
      goto ENCODE_OUT;
    }
    import_packet = packet;
  }

  ret = PrepareMppExtraBuffer(extra_output, mv_buf);
  if (ret) {
    RKMEDIA_LOGI("PrepareMppExtraBuffer failed\n");
    goto ENCODE_OUT;
  }

  ret = Process(frame, packet, mv_buf);
  if (ret)
    goto ENCODE_OUT;

  if (!packet) {
    RKMEDIA_LOGE("MPP Encoder: input frame:%p, %zuBytes; output null packet!\n",
                 frame, mpp_buffer_get_size(mpp_frame_get_buffer(frame)));
    goto ENCODE_OUT;
  }

  packet_len = mpp_packet_get_length(packet);
  {
    MppMeta packet_meta = mpp_packet_get_meta(packet);
    RK_S32 is_intra = 0;
    mpp_meta_get_s32(packet_meta, KEY_OUTPUT_INTRA, &is_intra);
    packet_flag = (is_intra) ? MediaBuffer::kIntra : MediaBuffer::kPredicted;
    mpp_meta_get_s32(packet_meta, KEY_TEMPORAL_ID, &temporal_id);
  }
  out_eof = mpp_packet_get_eos(packet);
  pts = mpp_packet_get_pts(packet);
  if (pts <= 0)
    pts = mpp_packet_get_dts(packet);

  // out fps < in fps ?
  if (packet_len == 0) {
    output->SetValidSize(0);
    if (extra_output)
      extra_output->SetValidSize(0);
    goto ENCODE_OUT;
  }

  // Calculate bit rate statistics.
  if (encoder_sta_en) {
    MediaConfig &cfg = GetConfig();
    int target_fps = cfg.vid_cfg.frame_rate;
    int target_bpsmax = cfg.vid_cfg.bit_rate_max;
    int enable_bps = 1;
    frame_cnt_1s += 1;
    stream_size_1s += packet_len;
    if (target_fps <= 0) {
      target_fps = 30;
      enable_bps = 0;
    }
    // Refresh every second
    if ((frame_cnt_1s % target_fps) == 0) {
      // Calculate the frame rate based on the system time.
      cur_ts = gettimeofday();
      if (last_ts)
        encoded_fps = ((float)target_fps / (cur_ts - last_ts)) * 1000000;
      else
        encoded_fps = 0;

      last_ts = cur_ts;
      if (enable_bps) {
        // convert bytes to bits
        encoded_bps = stream_size_1s * 8;
        RKMEDIA_LOGI(
            "MPP ENCODER: bps:%d, actual_bps:%d, fps:%d, actual_fps:%f\n",
            target_bpsmax, encoded_bps, target_fps, encoded_fps);
      } else {
        RKMEDIA_LOGI("MPP ENCODER: fps statistical period:%d, actual_fps:%f\n",
                     target_fps, encoded_fps);
      }

      // reset 1s variable
      stream_size_1s = 0;
      frame_cnt_1s = 0;
    }
  } else if (cur_ts) {
    // clear tmp statistics variable.
    stream_size_1s = 0;
    frame_cnt_1s = 0;
    cur_ts = 0;
    last_ts = 0;
  }

  if (output->IsValid()) {
    if (!import_packet) {
      // !!time-consuming operation
      void *ptr = output->GetPtr();
      assert(ptr);
      RKMEDIA_LOGD("extra time-consuming memcpy to cpu!\n");
      memcpy(ptr, mpp_packet_get_data(packet), packet_len);
      // sync to cpu?
    }
  } else {
    MPPPacketContext *ctx = new MPPPacketContext(mpp_ctx, packet);
    if (!ctx) {
      LOG_NO_MEMORY();
      ret = -ENOMEM;
      goto ENCODE_OUT;
    }
    output->SetFD(mpp_buffer_get_fd(mpp_packet_get_buffer(packet)));
    output->SetPtr(mpp_packet_get_data(packet));
    output->SetSize(mpp_packet_get_size(packet));
    output->SetUserData(ctx, __free_mpppacketcontext);
    packet = nullptr;
  }
  output->SetValidSize(packet_len);
  output->SetUserFlag(packet_flag | output_mb_flags);
  output->SetTsvcLevel(temporal_id);
  output->SetUSTimeStamp(pts);
  output->SetEOF(out_eof ? true : false);
  out_type = output->GetType();
  if (out_type == Type::Image) {
    auto out_img = std::static_pointer_cast<ImageBuffer>(output);
    auto &info = out_img->GetImageInfo();
    const auto &in_cfg = GetConfig();
    info = (coding_type == MPP_VIDEO_CodingMJPEG)
               ? in_cfg.img_cfg.image_info
               : in_cfg.vid_cfg.image_cfg.image_info;
    // info.pix_fmt = codec_type;
  } else {
    output->SetType(Type::Video);
  }

  if (mv_buf) {
    if (extra_output->GetFD() < 0) {
      void *ptr = extra_output->GetPtr();
      assert(ptr);
      memcpy(ptr, mpp_buffer_get_ptr(mv_buf), mpp_buffer_get_size(mv_buf));
    }
    extra_output->SetValidSize(mpp_buffer_get_size(mv_buf));
    extra_output->SetUserFlag(packet_flag);
    extra_output->SetUSTimeStamp(pts);
  }

ENCODE_OUT:
  if (frame)
    mpp_frame_deinit(&frame);
  if (packet)
    mpp_packet_deinit(&packet);
  if (mv_buf)
    mpp_buffer_put(mv_buf);

  return ret;
}

int MPPEncoder::Process(MppFrame frame, MppPacket &packet, MppBuffer &mv_buf) {
  MppCtx ctx = mpp_ctx->ctx;
  MppApi *mpi = mpp_ctx->mpi;

  if (mv_buf)
    RKMEDIA_LOGI("TODO move detection frome mpp encoder...\n");

  int ret = mpi->encode_put_frame(ctx, frame);
  if (ret) {
    RKMEDIA_LOGI("mpp encode put frame failed\n");
    return -1;
  }

  ret = mpi->encode_get_packet(ctx, &packet);
  if (ret) {
    RKMEDIA_LOGI("mpp encode get packet failed\n");
    return -1;
  }

  return 0;
}

int MPPEncoder::SendInput(const std::shared_ptr<MediaBuffer> &) {
  errno = ENOSYS;
  return -1;
}
std::shared_ptr<MediaBuffer> MPPEncoder::FetchOutput() {
  errno = ENOSYS;
  return nullptr;
}

int MPPEncoder::EncodeControl(int cmd, void *param) {
  MpiCmd mpi_cmd = (MpiCmd)cmd;
  int ret = mpp_ctx->mpi->control(mpp_ctx->ctx, mpi_cmd, (MppParam)param);

  if (ret) {
    RKMEDIA_LOGI("mpp control cmd 0x%08x param %p failed\n", cmd, param);
    return ret;
  }

  return 0;
}

void MPPEncoder::QueryChange(uint32_t change, void *value, int32_t size) {
  if (!value || !size) {
    RKMEDIA_LOGE("MPP ENCODER: %s invalid argument!\n", __func__);
    return;
  }
  switch (change) {
  case VideoEncoder::kMoveDetectionFlow:
    if (size < (int)sizeof(int32_t)) {
      RKMEDIA_LOGE("MPP ENCODER: %s change:[%d], size invalid!\n", __func__,
                   VideoEncoder::kMoveDetectionFlow);
      return;
    }
    if (rc_api_brief_name == "smart")
      *((int32_t *)value) = 1;
    else
      *((int32_t *)value) = 0;
    break;
  default:
    RKMEDIA_LOGW("MPP ENCODER: %s change:[%d] not support!\n", __func__,
                 change);
  }
}

void MPPEncoder::set_statistics_switch(bool value) {
  RKMEDIA_LOGI("[INFO] MPP ENCODER %s statistics\n",
               value ? "enable" : "disable");
  encoder_sta_en = value;
}

int MPPEncoder::get_statistics_bps() {
  if (!encoder_sta_en)
    RKMEDIA_LOGI("[WARN] MPP ENCODER statistics should enable first!\n");
  return encoded_bps;
}

int MPPEncoder::get_statistics_fps() {
  if (!encoder_sta_en)
    RKMEDIA_LOGI("[WARN] MPP ENCODER statistics should enable first!\n");
  return encoded_fps;
}

#ifdef MPP_SUPPORT_HW_OSD

#define OSD_PTL_SIZE 1024 // Bytes.

#ifndef NDEBUG
static void OsdDummpRegions(OsdRegionData *rdata) {
  if (!rdata)
    return;

  RKMEDIA_LOGD("#RegionData:%p:\n", rdata->buffer);
  RKMEDIA_LOGI("\t enable:%u\n", rdata->enable);
  RKMEDIA_LOGI("\t region_id:%u\n", rdata->region_id);
  RKMEDIA_LOGI("\t inverse:%u\n", rdata->inverse);
  RKMEDIA_LOGI("\t pos_x:%u\n", rdata->pos_x);
  RKMEDIA_LOGI("\t pos_y:%u\n", rdata->pos_y);
  RKMEDIA_LOGI("\t width:%u\n", rdata->width);
  RKMEDIA_LOGI("\t height:%u\n", rdata->height);
}

static void OsdDummpMppOsd(MppEncOSDData *osd) {
  RKMEDIA_LOGD("#MPP OsdData: cnt:%d buff:%p, bufSize:%zu\n", osd->num_region,
               osd->buf, mpp_buffer_get_size(osd->buf));
  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    RKMEDIA_LOGD("#MPP OsdData[%d]:\n", i);
    RKMEDIA_LOGI("\t enable:%u\n", osd->region[i].enable);
    RKMEDIA_LOGI("\t inverse:%u\n", osd->region[i].inverse);
    RKMEDIA_LOGI("\t pos_x:%u\n", osd->region[i].start_mb_x * 16);
    RKMEDIA_LOGI("\t pos_y:%u\n", osd->region[i].start_mb_y * 16);
    RKMEDIA_LOGI("\t width:%u\n", osd->region[i].num_mb_x * 16);
    RKMEDIA_LOGI("\t height:%u\n", osd->region[i].num_mb_y * 16);
    RKMEDIA_LOGI("\t buf_offset:%u\n", osd->region[i].buf_offset);
  }
}

static void SaveOsdImg(MppEncOSDData *_data, int index) {
  if (!_data->buf)
    return;

  char _path[64] = {0};
  sprintf(_path, "/tmp/osd_img%d", index);
  RKMEDIA_LOGD("MPP Encoder: save osd img to %s\n", _path);
  int fd = open(_path, O_WRONLY | O_CREAT);
  if (fd <= 0)
    return;

  int size = _data->region[index].num_mb_x * 16;
  size *= _data->region[index].num_mb_y * 16;
  uint8_t *ptr = (uint8_t *)mpp_buffer_get_ptr(_data->buf);
  ptr += _data->region[index].buf_offset;
  if (ptr && size)
    write(fd, ptr, size);
  close(fd);
}
#endif // NDEBUG

int MPPEncoder::OsdPaletteSet(uint32_t *ptl_data) {
  if (!ptl_data)
    return -1;

  RKMEDIA_LOGD("MPP Encoder: setting yuva palette...\n");
  MppCtx ctx = mpp_ctx->ctx;
  MppApi *mpi = mpp_ctx->mpi;
  MppEncOSDPltCfg osd_plt_cfg;
  MppEncOSDPlt osd_plt;

  // TODO rgba plt to yuva plt.
  for (int k = 0; k < 256; k++)
    osd_plt.data[k].val = *(ptl_data + k);

  osd_plt_cfg.change = MPP_ENC_OSD_PLT_CFG_CHANGE_ALL;
  osd_plt_cfg.type = MPP_ENC_OSD_PLT_TYPE_USERDEF;
  osd_plt_cfg.plt = &osd_plt;

  int ret = mpi->control(ctx, MPP_ENC_SET_OSD_PLT_CFG, &osd_plt_cfg);
  if (ret)
    RKMEDIA_LOGE("MPP Encoder: set osd plt failed ret %d\n", ret);

  return ret;
}

static int OsdUpdateRegionInfo(MppEncOSDData *osd, OsdRegionData *region_data,
                               MppBufferGroup buf_grp) {
  uint32_t new_size = 0;
  uint32_t old_size = 0;
  uint8_t rid = region_data->region_id;
  uint8_t *region_src = NULL;
  uint8_t *region_dst = NULL;

  if (!region_data->enable) {
    osd->region[rid].enable = 0;
    osd->num_region = 0;
    for (int i = 0; i < OSD_REGIONS_CNT; i++) {
      if (osd->region[i].enable)
        osd->num_region = i + 1;
    }
    assert(osd->num_region <= 8);
    return 0;
  }

  // get buffer size to compare.
  new_size = region_data->width * region_data->height;
  // If there is enough space, reuse the previous buffer.
  // However, the current area must be active, so as to
  // avoid opening up too large a buffer at the beginning,
  // and it will not be reduced later.
  if (osd->region[rid].enable)
    old_size = osd->region[rid].num_mb_x * osd->region[rid].num_mb_y * 256;

  // update region info.
  osd->region[rid].enable = 1;
  osd->region[rid].inverse = region_data->inverse;
  osd->region[rid].start_mb_x = region_data->pos_x / 16;
  osd->region[rid].start_mb_y = region_data->pos_y / 16;
  osd->region[rid].num_mb_x = region_data->width / 16;
  osd->region[rid].num_mb_y = region_data->height / 16;

  // 256 * 16 => 4096 is enough for osd.
  assert(osd->region[rid].start_mb_x <= 256);
  assert(osd->region[rid].start_mb_y <= 256);
  assert(osd->region[rid].num_mb_x <= 256);
  assert(osd->region[rid].num_mb_y <= 256);

  // region[rid] buffer size is enough, copy data directly.
  if (old_size >= new_size) {
    RKMEDIA_LOGD("MPP Encoder: Region[%d] reuse old buff:%u, new_size:%u\n",
                 rid, old_size, new_size);
    region_src = region_data->buffer;
    region_dst = (uint8_t *)mpp_buffer_get_ptr(osd->buf);
    region_dst += osd->region[rid].buf_offset;
    memcpy(region_dst, region_src, new_size);
#ifndef NDEBUG
    SaveOsdImg(osd, rid);
#endif
    return 0;
  }

  // region[rid] buffer size too small, resize buffer.
  MppBuffer new_buff = NULL;
  MppBuffer old_buff = NULL;
  uint32_t old_offset[OSD_REGIONS_CNT] = {0};
  uint32_t total_size = 0;
  uint32_t current_size = 0;

  osd->num_region = 0;
  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    if (osd->region[i].enable) {
      old_offset[i] = osd->region[i].buf_offset;
      osd->region[i].buf_offset = total_size;
      total_size += osd->region[i].num_mb_x * osd->region[i].num_mb_y * 256;
      osd->num_region = i + 1;
    } else {
      osd->region[i].start_mb_x = 0;
      osd->region[i].start_mb_y = 0;
      osd->region[i].buf_offset = 0;
      osd->region[i].num_mb_x = 0;
      osd->region[i].num_mb_y = 0;
    }
  }

  old_buff = osd->buf;
  int ret = mpp_buffer_get(buf_grp, &new_buff, total_size);
  if (ret) {
    RKMEDIA_LOGE("MPP Encoder: get osd %dBytes buffer failed(%d)\n", total_size,
                 ret);
    // reset target region.
    osd->region[rid].enable = 0;
    osd->region[rid].start_mb_x = 0;
    osd->region[rid].start_mb_y = 0;
    osd->region[rid].buf_offset = 0;
    return -1;
  }

  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    if (!osd->region[i].enable)
      continue;

    if (i != rid) {
      // copy other region data to new buffer.
      region_src = (uint8_t *)mpp_buffer_get_ptr(old_buff);
      region_src += old_offset[i];
      region_dst = (uint8_t *)mpp_buffer_get_ptr(new_buff);
      region_dst += osd->region[i].buf_offset;
      current_size = osd->region[i].num_mb_x * osd->region[i].num_mb_y * 256;
    } else {
      // copy current region data to new buffer.
      region_src = region_data->buffer;
      region_dst = (uint8_t *)mpp_buffer_get_ptr(new_buff);
      region_dst += osd->region[i].buf_offset;
      current_size = new_size;
    }

    assert(region_src);
    assert(region_dst);
    memcpy(region_dst, region_src, current_size);
#ifndef NDEBUG
    SaveOsdImg(osd, i);
#endif
  }

  // replace old buff with new buff.
  osd->buf = new_buff;
  if (old_buff)
    mpp_buffer_put(old_buff);

  return 0;
}

int MPPEncoder::OsdRegionSet(OsdRegionData *rdata) {
  if (!rdata)
    return -EINVAL;

  RKMEDIA_LOGD("MPP Encoder: setting osd regions...\n");
  if ((rdata->region_id >= OSD_REGIONS_CNT)) {
    RKMEDIA_LOGE("MPP Encoder: invalid region id(%d), should be [0, %d).\n",
                 rdata->region_id, OSD_REGIONS_CNT);
    return -EINVAL;
  }

  if (rdata->enable && !rdata->buffer) {
    RKMEDIA_LOGE("MPP Encoder: invalid region data");
    return -EINVAL;
  }

  if ((rdata->width % 16) || (rdata->height % 16) || (rdata->pos_x % 16) ||
      (rdata->pos_y % 16)) {
    RKMEDIA_LOGW("MPP Encoder: osd size must be 16 aligned\n");
    rdata->width = UPALIGNTO16(rdata->width);
    rdata->height = UPALIGNTO16(rdata->height);
    rdata->pos_x = UPALIGNTO16(rdata->pos_x);
    rdata->pos_y = UPALIGNTO16(rdata->pos_y);
  }

#ifndef NDEBUG
  OsdDummpRegions(rdata);
#endif
  int ret = OsdUpdateRegionInfo(&osd_data, rdata, osd_buf_grp);
#ifndef NDEBUG
  OsdDummpMppOsd(&osd_data);
#endif

  return ret;
}

int MPPEncoder::OsdRegionGet(OsdRegionData *rdata) {
  RKMEDIA_LOGI("ToDo...%p\n", rdata);
  return 0;
}
#endif // MPP_SUPPORT_HW_OSD

#ifdef JPEG_RGA_OSD_ENABLE
#if 0
static void OsdDummpRgaRegions(OsdRegionData *rdata) {
  if (!rdata)
    return;

  RKMEDIA_LOGD("#RegionData:%p:\n", rdata->buffer);
  RKMEDIA_LOGI("\t enable:%u\n", rdata->enable);
  RKMEDIA_LOGI("\t region_id:%u\n", rdata->region_id);
  RKMEDIA_LOGI("\t inverse:%u\n", rdata->inverse);
  RKMEDIA_LOGI("\t pos_x:%u\n", rdata->pos_x);
  RKMEDIA_LOGI("\t pos_y:%u\n", rdata->pos_y);
  RKMEDIA_LOGI("\t width:%u\n", rdata->width);
  RKMEDIA_LOGI("\t height:%u\n", rdata->height);
}

static void OsdDummpRgaOsd(RgaOsdData *osd, int valid_cnt) {
  RKMEDIA_LOGD("#JPEG OsdData: cnt:%d\n", valid_cnt);
  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    printf("#JPEG OsdData[%d]:\n", i);
    printf("\t enable:%u\n", osd[i].enable);
    printf("\t inverse:%u\n", osd[i].inverse);
    printf("\t pos_x:%u\n", osd[i].pos_x);
    printf("\t pos_y:%u\n", osd[i].pos_y);
    printf("\t width:%u\n", osd[i].width);
    printf("\t height:%u\n", osd[i].height);
    if (osd[i].data) {
      printf("\t buf ptr:%p\n", osd[i].data->GetPtr());
      printf("\t buf size:%zuBytes\n", osd[i].data->GetValidSize());
    }
  }
}

static void JpegSaveOsdImg(RgaOsdData *osdRgn, int index) {
  if (!osdRgn || !osdRgn->data)
    return;

  char _path[64] = {0};
  sprintf(_path, "/tmp/osd_img%d", index);
  RKMEDIA_LOGD("MPP Encoder: save osd img to %s\n", _path);
  int fd = open(_path, O_WRONLY | O_CREAT);
  if (fd <= 0)
    return;

  int size = osdRgn->data->GetValidSize();
  uint8_t *ptr = (uint8_t *)osdRgn->data->GetPtr();
  if (ptr && size)
    write(fd, ptr, size);
  close(fd);
}
#endif // #ifndef NDEBUG

int MPPEncoder::RgaOsdRegionSet(OsdRegionData *rdata) {
  if (!rdata)
    return -EINVAL;

  RKMEDIA_LOGD("MPP Encoder[JPEG]: setting osd regions...\n");
  if ((rdata->region_id >= OSD_REGIONS_CNT)) {
    RKMEDIA_LOGE(
        "MPP Encoder[JPEG]: invalid region id(%d), should be [0, %d).\n",
        rdata->region_id, OSD_REGIONS_CNT);
    return -EINVAL;
  }

  if (rdata->enable && !rdata->buffer) {
    RKMEDIA_LOGE("MPP Encoder[JPEG]: invalid region data");
    return -EINVAL;
  }

  int rid = rdata->region_id;
  if (!rdata->enable) {
    if (rga_osd_data[rid].enable)
      rga_osd_cnt--;
    if (rga_osd_cnt < 0) {
      RKMEDIA_LOGW("[JPEG]: jpeg osd cnt incorrect!\n");
      rga_osd_cnt = 0;
    }
    rga_osd_data[rid].enable = 0;
    rga_osd_data[rid].data.reset();
    return 0;
  }

  int total_size = rdata->width * rdata->height * 4; // rgab8888
  rga_osd_data[rid].data =
      MediaBuffer::Alloc(total_size, MediaBuffer::MemType::MEM_HARD_WARE);
  if (!rga_osd_data[rid].data) {
    RKMEDIA_LOGW("[JPEG]: jpeg osd: no space left!\n");
    return -ENOMEM;
    ;
  }
  rga_osd_data[rid].data->BeginCPUAccess(true);
  memcpy(rga_osd_data[rid].data->GetPtr(), rdata->buffer, total_size);
  rga_osd_data[rid].data->EndCPUAccess(true);
  rga_osd_data[rid].data->SetValidSize(total_size);
  if (!rga_osd_data[rid].enable)
    rga_osd_cnt++;
  if (rga_osd_cnt > OSD_REGIONS_CNT) {
    RKMEDIA_LOGW("[JPEG]: Osd: rga osd cnt > OSD_REGIONS_CNT\n");
    rga_osd_cnt = OSD_REGIONS_CNT;
  }
  rga_osd_data[rid].pos_x = rdata->pos_x;
  rga_osd_data[rid].pos_y = rdata->pos_y;
  rga_osd_data[rid].width = rdata->width;
  rga_osd_data[rid].height = rdata->height;
  rga_osd_data[rid].enable = rdata->enable;
  rga_osd_data[rid].inverse = rdata->inverse;

  return 0;
}

int MPPEncoder::RgaOsdRegionProcess(ImageBuffer *hw_buffer) {
  rga_buffer_t pat;
  rga_buffer_t src;
  IM_STATUS STATUS;

  if (!hw_buffer)
    return 0;

  ImageInfo imgInfo = hw_buffer->GetImageInfo();
  im_rect pat_rect;
  // ToDo: fmt check and convert

  src = wrapbuffer_fd(hw_buffer->GetFD(), imgInfo.width, imgInfo.height,
                      RK_FORMAT_YCbCr_420_SP);
  for (int i = 0; i < OSD_REGIONS_CNT; i++) {
    if (!rga_osd_data[i].enable)
      continue;
    pat = wrapbuffer_fd(rga_osd_data[i].data->GetFD(), rga_osd_data[i].width,
                        rga_osd_data[i].height, RK_FORMAT_BGRA_8888);
    pat_rect.x = rga_osd_data[i].pos_x;
    pat_rect.y = rga_osd_data[i].pos_y;
    pat_rect.width = rga_osd_data[i].width;
    pat_rect.height = rga_osd_data[i].height;
    STATUS = improcess(src, src, pat, pat_rect, pat_rect, pat_rect,
                       IM_ALPHA_BLEND_DST_OVER);
    if (STATUS != IM_STATUS_SUCCESS) {
      RKMEDIA_LOGE("[JPEG]: osd: blend failed!\n");
      break;
    }
  }

  return 0;
}
#endif // JPEG_RGA_OSD_ENABLE

int MPPEncoder::RoiUpdateRegions(EncROIRegion *regions, int region_cnt) {
  if (!regions || region_cnt == 0) {
    roi_cfg.number = 0;
    if (roi_cfg.regions) {
      free(roi_cfg.regions);
      roi_cfg.regions = NULL;
    }
    RKMEDIA_LOGI("MPP Encoder: disable roi function.");
    return 0;
  }

  int msize = region_cnt * sizeof(MppEncROIRegion);
  MppEncROIRegion *region = (MppEncROIRegion *)malloc(msize);
  if (!region) {
    LOG_NO_MEMORY();
    return -ENOMEM;
  }

  for (int i = 0; i < region_cnt; i++) {
    if ((regions[i].x % 16) || (regions[i].y % 16) || (regions[i].w % 16) ||
        (regions[i].h % 16)) {
      RKMEDIA_LOGW(
          "MPP Encoder: region parameter should be an integer multiple "
          "of 16\n");
      RKMEDIA_LOGW("MPP Encoder: reset region[%d] frome <%d,%d,%d,%d> to "
                   "<%d,%d,%d,%d>\n",
                   i, regions[i].x, regions[i].y, regions[i].w, regions[i].h,
                   UPALIGNTO16(regions[i].x), UPALIGNTO16(regions[i].y),
                   UPALIGNTO16(regions[i].w), UPALIGNTO16(regions[i].h));
      regions[i].x = UPALIGNTO16(regions[i].x);
      regions[i].y = UPALIGNTO16(regions[i].y);
      regions[i].w = UPALIGNTO16(regions[i].w);
      regions[i].h = UPALIGNTO16(regions[i].h);
    }
    RKMEDIA_LOGD("MPP Encoder: roi region[%d]:<%d,%d,%d,%d>\n", i, regions[i].x,
                 regions[i].y, regions[i].w, regions[i].h);
    RKMEDIA_LOGD("MPP Encoder: roi region[%d].intra=%d,\n", i,
                 regions[i].intra);
    RKMEDIA_LOGD("MPP Encoder: roi region[%d].quality=%d,\n", i,
                 regions[i].quality);
    RKMEDIA_LOGD("MPP Encoder: roi region[%d].abs_qp_en=%d,\n", i,
                 regions[i].abs_qp_en);
    RKMEDIA_LOGD("MPP Encoder: roi region[%d].qp_area_idx=%d,\n", i,
                 regions[i].qp_area_idx);
    RKMEDIA_LOGD("MPP Encoder: roi region[%d].area_map_en=%d,\n", i,
                 regions[i].area_map_en);
    assert(regions[i].x < 8192);
    assert(regions[i].y < 8192);
    assert(regions[i].w < 8192);
    assert(regions[i].h < 8192);
    assert(regions[i].x < 8192);
    assert(regions[i].intra <= 1);
    assert(regions[i].abs_qp_en <= 1);
    assert(regions[i].qp_area_idx <= 7);
    assert(regions[i].area_map_en <= 1);
    VALUE_SCOPE_CHECK(regions[i].quality, -48, 51);

    region[i].x = regions[i].x;
    region[i].y = regions[i].y;
    region[i].w = regions[i].w;
    region[i].h = regions[i].h;
    region[i].intra = regions[i].intra;
    region[i].quality = regions[i].quality;
    region[i].abs_qp_en = regions[i].abs_qp_en;
    region[i].qp_area_idx = regions[i].qp_area_idx;
    region[i].area_map_en = regions[i].area_map_en;
  }
  roi_cfg.number = region_cnt;
  if (roi_cfg.regions)
    free(roi_cfg.regions);
  roi_cfg.regions = region;
  return 0;
}

int MPPEncoder::SetUserData(const char *data, uint16_t len) {
  uint16_t valid_size = len;

  if (!data && len) {
    RKMEDIA_LOGE("Mpp Encoder: invalid userdata!\n");
    return -1;
  }

  if (valid_size > MPP_ENCODER_USERDATA_MAX_SIZE) {
    valid_size = MPP_ENCODER_USERDATA_MAX_SIZE;
    RKMEDIA_LOGW("Mpp Encoder: UserData exceeds maximum length(%d),"
                 "Reset to %d\n",
                 valid_size, valid_size);
  }

  if (valid_size)
    memcpy(userdata, data, valid_size);

  userdata_len = valid_size;
  return 0;
}

void MPPEncoder::ClearUserData() { userdata_len = 0; }

void MPPEncoder::RestartUserData() { userdata_frame_id = 0; }

void MPPEncoder::EnableUserDataAllFrame(bool value) {
  userdata_all_frame_en = value ? 1 : 0;
}

} // namespace easymedia
