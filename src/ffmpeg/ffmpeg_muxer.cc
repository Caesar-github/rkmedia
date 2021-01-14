// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "muxer.h"
#include <inttypes.h>
#include <assert.h>

#include "buffer.h"
#include "ffmpeg_utils.h"

namespace easymedia {

class FFMPEGMuxer : public Muxer {
public:
  FFMPEGMuxer(const char *param);
  virtual ~FFMPEGMuxer();
  static const char *GetMuxName() { return "ffmpeg"; }

  virtual bool Init() override;
  virtual bool
  NewMuxerStream(const MediaConfig &mc,
                 const std::shared_ptr<MediaBuffer> &enc_extra_data,
                 int &stream_no) override;
  virtual bool SetIoStream(std::shared_ptr<Stream> output _UNUSED) {
    // do not support
    return false;
  }
  virtual std::shared_ptr<MediaBuffer> WriteHeader(int stream_no);
  virtual std::shared_ptr<MediaBuffer>
  Write(std::shared_ptr<MediaBuffer> orig_data, int stream_no) override;

private:
  std::string path;
  std::string oformat;
  AVFormatContext *context;
  AVDictionary *opt;
  std::vector<AVStream *> streams;
  int nb_streams;
  std::vector<int64_t> first_timestamp;
  std::vector<int64_t> pre_pts;

  class FFMPEG_AV_INIT {
  public:
    FFMPEG_AV_INIT() { avformat_network_init(); }
    ~FFMPEG_AV_INIT() { avformat_network_deinit(); }
  };
  static const FFMPEG_AV_INIT gAVInit;
  static std::shared_ptr<MediaBuffer> empty;
};

std::shared_ptr<MediaBuffer> FFMPEGMuxer::empty =
    std::make_shared<MediaBuffer>();

static bool _convert_to_avdictionary(std::string avdictionary,
                                     AVDictionary **opt) {
  std::list<std::string> avdics;
  parse_media_param_list(avdictionary.c_str(), avdics, ',');

  for (auto &avdic : avdics) {
    std::list<std::string> values;
    parse_media_param_list(avdic.c_str(), values, '-');
    if (values.size() != 2) {
      RKMEDIA_LOGI("ffmpeg_muxer:: avdictionary error: %s.\n", avdic.c_str());
      continue;
    }
    std::string name, value;
    name = values.front();
    values.pop_front();
    value = values.front();
    values.pop_front();
    av_dict_set(opt, name.c_str(), value.c_str(), 0);
  }
  return true;
}

FFMPEGMuxer::FFMPEGMuxer(const char *param)
    : Muxer(param), context(NULL), opt(NULL), nb_streams(0) {
  std::map<std::string, std::string> params;
  std::string muxer_ffmpeg_avdictionary;
  std::list<std::pair<const std::string, std::string &>> req_list;
  req_list.push_back(
      std::pair<const std::string, std::string &>(KEY_PATH, path));
  req_list.push_back(
      std::pair<const std::string, std::string &>(KEY_OUTPUTDATATYPE, oformat));
  req_list.push_back(std::pair<const std::string, std::string &>(
      KEY_MUXER_FFMPEG_AVDICTIONARY, muxer_ffmpeg_avdictionary));

  parse_media_param_match(param, params, req_list);

  _convert_to_avdictionary(muxer_ffmpeg_avdictionary, &opt);
}

FFMPEGMuxer::~FFMPEGMuxer() {
  if (!context)
    return;
  if (m_handler != nullptr) {
    // customIO, may not free opaque, it comes from outside.
    if (context->pb && context->pb->buffer) {
      av_free(context->pb->buffer);
      av_free(context->pb);
    }
  } else if (context->pb)
    avio_closep(&context->pb);
  avformat_free_context(context);
}

bool FFMPEGMuxer::Init() {
  if (!empty)
    return false;
  if (path.empty() && oformat.empty()) {
    RKMEDIA_LOGI("you must set path or output format.\n");
    return false;
  }
  AVFormatContext *c = NULL;
  auto ret = avformat_alloc_output_context2(
      &c, NULL, oformat.empty() ? NULL : oformat.c_str(), path.c_str());
  if (!c) {
    fprintf(stderr,
            "avformat_alloc_output_context2 failed for url %s, ret: %d\n",
            path.c_str(), ret);
    return false;
  }
  context = c;
  return true;
}

static bool _convert_to_avcodecparam(AVFormatContext *c, const MediaConfig *mc,
                                     AVCodecParameters *par,
                                     AVRational *time_base) {
  Type type = mc->type;
  switch (type) {
  case Type::Image:
  case Type::Video: {
    const VideoConfig &vc = mc->vid_cfg;
    const ImageConfig &ic = (type == Type::Image) ? mc->img_cfg : vc.image_cfg;

    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = (type == Type::Image) ? AV_CODEC_ID_RAWVIDEO
                                          : CodecTypeToAVCodecID(ic.codec_type);
    if (par->codec_id == AV_CODEC_ID_NONE)
      return false;
    auto av_fmt = PixFmtToAVPixFmt(ic.image_info.pix_fmt);
    par->codec_tag =
        (type == Type::Image)
            ? avcodec_pix_fmt_to_codec_tag(av_fmt)
            : av_codec_get_tag(c->oformat->codec_tag, par->codec_id);
    par->format = av_fmt;
    par->sw_format = AV_PIX_FMT_NONE; // TODO
    par->width = ic.image_info.width;
    par->height = ic.image_info.height;
    par->video_delay = 0; // TODO

    if (type == Type::Video) {
      par->bit_rate = vc.bit_rate;
      par->profile = vc.profile;
      par->level = vc.level;
      *time_base = (AVRational){1, vc.frame_rate};
    }
  } break;
  case Type::Audio: {
    const AudioConfig &ac = mc->aud_cfg;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = CodecTypeToAVCodecID(ac.codec_type);
    if (par->codec_id == AV_CODEC_ID_NONE)
      return false;
    par->codec_tag = av_codec_get_tag(c->oformat->codec_tag, par->codec_id);
    par->format = SampleFmtToAVSamFmt(ac.sample_info.fmt);
    par->channels = ac.sample_info.channels;
    par->channel_layout = av_get_default_channel_layout(par->channels);
    par->sample_rate = ac.sample_info.sample_rate;
    // par->block_align
    if (par->codec_id == AV_CODEC_ID_MP2 || par->codec_id == AV_CODEC_ID_AAC)
      par->frame_size = ac.sample_info.nb_samples;
    *time_base = (AVRational){1, par->sample_rate};
  } break;
  case Type::Text: {
    const ImageConfig &ic = mc->img_cfg;
    par->codec_type = AVMEDIA_TYPE_SUBTITLE;
    par->width = ic.image_info.width;
    par->height = ic.image_info.height;
  } break;
  default:
    return false;
  }
  return true;
}

bool FFMPEGMuxer::NewMuxerStream(
    const MediaConfig &mc, const std::shared_ptr<MediaBuffer> &enc_extra_data,
    int &stream_no) {
  // if (oc->oformat->flags & AVFMT_GLOBALHEADER)
  //   c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  stream_no = -1;
  AVRational time_base = (AVRational){1, 1};
  AVCodecParameters *codecpar = avcodec_parameters_alloc();
  if (!codecpar) {
    LOG_NO_MEMORY();
    return false;
  }
  if (!_convert_to_avcodecparam(context, &mc, codecpar, &time_base))
    return false;
  AVStream *s = avformat_new_stream(context, NULL);
  if (!s) {
    RKMEDIA_LOGI("avformat_new_stream failed\n");
    return false;
  }
  assert(s->index < 64);
  stream_no = s->index;
  RKMEDIA_LOGD("new stream index %d\n", stream_no);
  s->id = context->nb_streams - 1;
  *(s->codecpar) = *codecpar;
  s->time_base = time_base;
#if 1 // make av_dump_format correct
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if (mc.type == Type::Video) {
    const VideoConfig &vc = mc.vid_cfg;
    const auto &info = vc.image_cfg.image_info;
    s->codec->qmin = vc.qp_min;
    s->codec->qmax = vc.qp_max;
    s->codec->coded_width = info.vir_width;
    s->codec->coded_height = info.vir_height;
  }
#pragma GCC diagnostic pop
#endif
  avcodec_parameters_free(&codecpar);
  if (enc_extra_data && enc_extra_data->GetValidSize() > 0) {
    auto size = enc_extra_data->GetValidSize();
    s->codecpar->extradata =
        (uint8_t *)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s->codecpar->extradata) {
      LOG_NO_MEMORY();
      return false;
    }
    memcpy(s->codecpar->extradata, enc_extra_data->GetPtr(), size);
    s->codecpar->extradata_size = size;
  }
  if ((int)streams.size() <= stream_no) {
    streams.resize(stream_no + 1);
    streams[stream_no] = NULL;
    first_timestamp.resize(stream_no + 1, -1);
    pre_pts.resize(stream_no + 1, 0);
  }
  assert(!streams[stream_no]);
  streams[stream_no] = s;
  nb_streams++;
  assert((int)context->nb_streams == nb_streams);

  return true;
}

std::shared_ptr<MediaBuffer> FFMPEGMuxer::WriteHeader(int stream_no) {
  if (stream_no < 0 || stream_no >= (int)streams.size()) {
    RKMEDIA_LOGI("Invalid stream no : %d\n", stream_no);
    return nullptr;
  }
  const char *url = path.c_str();
  if (context->pb)
    return empty;
#ifndef NDEBUG
  av_dump_format(context, stream_no, url, 1);
#endif
  int ret;
  // custom IO
  if (m_handler != nullptr) {
    AVIOContext *avio_ctx_;
    unsigned char *avio_ctx_buf_;
    int avio_ctx_buf_size_ = 102400;
    if (oformat == MUXER_MPEG_TS) {
      // TRANSPORT_PACKET_SIZE = 188
      avio_ctx_buf_size_ = 188 * 1000;
    }
    avio_ctx_buf_ = (unsigned char *)av_malloc(avio_ctx_buf_size_);
    int write_flag = 1;
    avio_ctx_ =
        avio_alloc_context(avio_ctx_buf_, avio_ctx_buf_size_, write_flag,
                           m_handler, NULL, m_write_callback_func, NULL);
    context->pb = avio_ctx_;
    context->oformat->flags = AVFMT_NOFILE;
  }

  if (!(context->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&context->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0) {
      PrintAVError(ret, "Could not open", path.c_str());
      return nullptr;
    }
  } else {
    if (url && !(context->url = av_strdup(url)))
      return nullptr;
  }
  ret = avformat_write_header(context, &opt);

  if (ret < 0) {
    PrintAVError(ret, "Fail to write header", path.c_str());
    return nullptr;
  }
#ifndef NDEBUG
  int i = 0;
  for (auto s : streams) {
    RKMEDIA_LOGD("stream index %d, after write header time_base: %d/%d\n", i++,
                 s->time_base.num, s->time_base.den);
  }
#endif
  return empty;
}

std::shared_ptr<MediaBuffer>
FFMPEGMuxer::Write(std::shared_ptr<MediaBuffer> data, int stream_no) {
  assert(stream_no >= 0 && stream_no < (int)streams.size());
  int ret;
  bool eof = data->IsEOF();
  size_t size = data->GetValidSize();
  if (size > 0) {
    auto s = streams[stream_no];
    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = (uint8_t *)data->GetPtr();
    avpkt.size = size;
    avpkt.stream_index = s->index;
    if (data->GetUserFlag() & MediaBuffer::kIntra)
      avpkt.flags |= AV_PKT_FLAG_KEY;
    // if (data->GetUserFlag() & MediaBuffer::kSingleNalUnit)
    //   avpkt.flags |= AV_PKT_FLAG_ONE_NAL;
    int64_t diff = 0;
    int64_t pts = 0;
    if (first_timestamp[stream_no] < 0) {
      first_timestamp[stream_no] = data->GetUSTimeStamp();
    } else {
      diff = data->GetUSTimeStamp() - first_timestamp[stream_no];
      // (enum AVRounding)(AV_ROUND_UP | AV_ROUND_PASS_MINMAX)
      pts = av_rescale_rnd(diff, s->time_base.den, s->time_base.num * 1000000LL,
                           AV_ROUND_UP);
      if (pts <= pre_pts[stream_no])
        pts = pre_pts[stream_no] + 1;
      pre_pts[stream_no] = pts;
    }
    avpkt.dts = avpkt.pts = pts;
    RKMEDIA_LOGD("[%d] pts = %" PRId64 ", num/den =%d/%d\n", stream_no, pts,
                 s->time_base.num, s->time_base.den);
    ret = av_write_frame(context, &avpkt);
    av_packet_unref(&avpkt);
    if (ret < 0) {
      PrintAVError(ret, "Fail to write frame", path.c_str());
      if (!eof)
        return nullptr;
    }
  }

  if (eof) {
    ret = av_write_trailer(context);
    if (ret < 0) {
      PrintAVError(ret, "Fail to write trailer", path.c_str());
      return nullptr;
    }
  }

  return empty;
}

DEFINE_COMMON_MUXER_FACTORY(FFMPEGMuxer)
const char *FACTORY(FFMPEGMuxer)::ExpectedInputDataType() {
  return TYPE_ANYTHING;
}
const char *FACTORY(FFMPEGMuxer)::OutPutDataType() { return TYPE_NOTHING; }

} // namespace easymedia
