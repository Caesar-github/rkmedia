// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>

#include "buffer.h"
#include "filter.h"
#include "flow.h"
#include "image.h"
#include "key_string.h"

namespace easymedia {

static bool do_filters(Flow *f, MediaBufferVector &input_vector);

class FilterFlow : public Flow {
public:
  FilterFlow(const char *param);
  virtual ~FilterFlow() {
    AutoPrintLine apl(__func__);
    StopAllThread();
  }
  static const char *GetFlowName() { return "filter"; }
  virtual int Control(unsigned long int request, ...) final {
    int ret = 0;
    if (!filters.size())
      return -1;
    for (auto &filter : filters) {
      va_list vl;
      va_start(vl, request);
      void *arg = va_arg(vl, void *);
      va_end(vl);
      ret |= filter->IoCtrl(request, arg);
    }
    return ret;
  }

private:
  std::vector<std::shared_ptr<Filter>> filters;
  bool support_async;
  Model thread_model;
  PixelFormat input_pix_fmt; // a hack for rga copy yuyv, by set fake rgb565
  ImageInfo out_img_info;
  std::shared_ptr<BufferPool> buffer_pool;

  friend bool do_filters(Flow *f, MediaBufferVector &input_vector);
};

FilterFlow::FilterFlow(const char *param)
    : support_async(true), thread_model(Model::NONE),
      input_pix_fmt(PIX_FMT_NONE) {
  memset(&out_img_info, 0, sizeof(out_img_info));
  out_img_info.pix_fmt = PIX_FMT_NONE;
  std::list<std::string> separate_list;
  std::map<std::string, std::string> params;
  if (!ParseWrapFlowParams(param, params, separate_list)) {
    SetError(-EINVAL);
    return;
  }
  std::string &name = params[KEY_NAME];
  const char *filter_name = name.c_str();
  // check input/output type
  std::string &&rule = gen_datatype_rule(params);
  if (!rule.empty()) {
    if (!REFLECTOR(Filter)::IsMatch(filter_name, rule.c_str())) {
      RKMEDIA_LOGI("Unsupport for filter %s : [%s]\n", filter_name,
                   rule.c_str());
      SetError(-EINVAL);
      return;
    }
  }
  input_pix_fmt = StringToPixFmt(params[KEY_INPUTDATATYPE].c_str());
  SlotMap sm;
  int input_maxcachenum = 2;
  ParseParamToSlotMap(params, sm, input_maxcachenum);
  if (sm.thread_model == Model::NONE)
    sm.thread_model =
        !params[KEY_FPS].empty() ? Model::ASYNCATOMIC : Model::SYNC;
  thread_model = sm.thread_model;
  if (sm.mode_when_full == InputMode::NONE)
    sm.mode_when_full = InputMode::DROPCURRENT;
  int input_idx = 0;
  for (auto &param_str : separate_list) {
    auto filter =
        REFLECTOR(Filter)::Create<Filter>(filter_name, param_str.c_str());
    if (!filter) {
      RKMEDIA_LOGI("Fail to create filter %s<%s>\n", filter_name,
                   param_str.c_str());
      SetError(-EINVAL);
      return;
    }
    filters.push_back(filter);
    sm.input_slots.push_back(input_idx++);
    sm.input_maxcachenum.push_back(input_maxcachenum);
  }
  sm.output_slots.push_back(0);
  auto &hold = params[KEY_OUTPUT_HOLD_INPUT];
  if (!hold.empty())
    sm.hold_input.push_back((HoldInputMode)std::stoi(hold));

  sm.process = do_filters;
  std::string tag = "FilterFlow:";
  tag.append(filter_name);
  if (!InstallSlotMap(sm, tag, -1)) {
    RKMEDIA_LOGI("Fail to InstallSlotMap for %s\n", tag.c_str());
    SetError(-EINVAL);
    return;
  }
  SetFlowTag(tag);
  if (filters[0]->SendInput(nullptr) == -1 && errno == ENOSYS) {
    support_async = false;
    if (input_pix_fmt != PIX_FMT_NONE &&
        !ParseImageInfoFromMap(params, out_img_info, false)) {
      if (filters.size() > 1) {
        RKMEDIA_LOGI("missing out image info for multi filters\n");
        SetError(-EINVAL);
        return;
      }
    }
    // Create buffer pool with vir_height and vir_width.
    std::string &mem_type = params[KEY_MEM_TYPE];
    std::string &mem_cnt = params[KEY_MEM_CNT];
    if ((input_pix_fmt != PIX_FMT_NONE) && (out_img_info.vir_height > 0) &&
        (out_img_info.vir_width > 0) && (!mem_type.empty()) &&
        (!mem_cnt.empty())) {
      RKMEDIA_LOGI("%s: Enable BufferPool! memtype:%s, memcnt:%s\n",
                   tag.c_str(), mem_type.c_str(), mem_cnt.c_str());

      int m_cnt = std::stoi(mem_cnt);
      if (m_cnt <= 0) {
        RKMEDIA_LOGE("%s: mem_cnt %s invalid!\n", tag.c_str(), mem_cnt.c_str());
        SetError(-EINVAL);
        return;
      }
      size_t m_size = CalPixFmtSize(out_img_info);
      MediaBuffer::MemType m_type = StringToMemType(mem_type.c_str());

      buffer_pool = std::make_shared<BufferPool>(m_cnt, m_size, m_type);
    }
  } else {
    // support async mode (one input, multi output)
    support_async = true;
    return;
  }
}

// comparing timestamp as modification?
bool do_filters(Flow *f, MediaBufferVector &input_vector) {
  FilterFlow *flow = static_cast<FilterFlow *>(f);
  int i = 0;
  bool has_valid_input = false;
  std::shared_ptr<Filter> last_filter;
  assert(flow->filters.size() == input_vector.size());
  for (auto &in : input_vector) {
    if (in) {
      has_valid_input = true;
      break;
    }
  }
  if (!has_valid_input)
    return false;
  std::shared_ptr<MediaBuffer> out_buffer;
  if (!flow->support_async) {
    const auto &info = flow->out_img_info;
    if (info.pix_fmt == PIX_FMT_NONE) {
      out_buffer = std::make_shared<MediaBuffer>();
    } else {
      if (info.vir_width > 0 && info.vir_height > 0) {
        if (flow->buffer_pool) {
          auto mb = flow->buffer_pool->GetBuffer(false);
          if (!mb) {
            RKMEDIA_LOGE("%s: buffer_pool get null buffer!\n",
                         flow->GetFlowTag());
            return false;
          }
          out_buffer = std::make_shared<ImageBuffer>(*(mb.get()), info);
        } else {
          size_t size = CalPixFmtSize(info);
          auto &&mb =
              MediaBuffer::Alloc2(size, MediaBuffer::MemType::MEM_HARD_WARE);
          out_buffer = std::make_shared<ImageBuffer>(mb, info);
        }
      } else {
        auto ib = std::make_shared<ImageBuffer>();
        if (ib) {
          ib->GetImageInfo().pix_fmt = info.pix_fmt;
          out_buffer = ib;
        }
      }
    }
  }
  for (auto &filter : flow->filters) {
    auto &in = input_vector[i];
    if (!in) {
      i++;
      continue;
    }
    last_filter = filter;
    if (flow->input_pix_fmt != PIX_FMT_NONE && in->GetType() == Type::Image) {
      auto in_img = std::static_pointer_cast<ImageBuffer>(in);
      // hack for n4 cif
      if (flow->input_pix_fmt != in_img->GetPixelFormat()) {
        ImageInfo &info = in_img->GetImageInfo();
        int flow_num = 0, flow_den = 0;
        GetPixFmtNumDen(flow->input_pix_fmt, flow_num, flow_den);
        int in_num = 0, in_den = 0;
        GetPixFmtNumDen(info.pix_fmt, in_num, in_den);
        int num = in_num * flow_den;
        int den = in_den * flow_num;
        info.width = info.width * num / den;
        info.vir_width = info.vir_width * num / den;
        info.pix_fmt = flow->input_pix_fmt;
      }
    }
    if (flow->support_async) {
      int ret = filter->SendInput(in);
      if (ret == -EAGAIN)
        flow->SendInput(in, i);
    } else {
      if (filter->Process(in, out_buffer))
        return false;
    }
    i++;
  }
  bool ret = false;
  if (!flow->support_async) {
    ret = flow->SetOutput(out_buffer, 0);
  } else {
    // flow->thread_model == Model::SYNC;
    do {
      auto out = last_filter->FetchOutput();
      if (!out)
        break;
      if (flow->SetOutput(out, 0))
        ret = true;
    } while (true);
  }
  return ret;
}

DEFINE_FLOW_FACTORY(FilterFlow, Flow)
// TODO!
const char *FACTORY(FilterFlow)::ExpectedInputDataType() { return ""; }
// TODO!
const char *FACTORY(FilterFlow)::OutPutDataType() { return ""; }

} // namespace easymedia
