// Copyright 2019 Fuzhou Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <string.h>

#include "alsa/alsa_utils.h"
#include "rk_audio.h"
#include "sound.h"

#include "utils.h"

#ifdef AUDIO_ALGORITHM_ENABLE
extern "C" {

#if USE_RKAPPLUS
#include <skv.h>
#else
#include <RKAP_3A.h>
#include <RKAP_ANR.h>
#endif
}

#define RK_AUDIO_BUFFER_MAX_SIZE 12288
#define ALGO_FRAME_TIMS_MS 20 // 20ms
#define USE_DEEP_DUMP 0       // For your deep debugging and enabling it

#if USE_RKAPPLUS
#define rt_malloc(x) calloc(1, x)
#define rt_safe_free free
#define RT_NULL NULL

typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32;
typedef int64_t INT64;

typedef uint8_t UINT8;
typedef uint8_t UCHAR;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;

typedef void *RT_PTR;
typedef void RT_VOID;
typedef float RT_FLOAT;
typedef short RT_SHORT;

#define RT_LOGD(format, ...)                                                   \
  do {                                                                         \
    fprintf(stderr, "[RKSKV]:" format, ##__VA_ARGS__);                         \
    fprintf(stderr, "\n");                                                     \
  } while (0)

#define RT_ASSERT assert

typedef enum _RTSKVMODE {
  RT_SKV_AEC = 1 << 0,
  RT_SKV_BF = 1 << 1,
  RT_SKV_RX = 1 << 2,
  RT_SKV_CMD = 1 << 3,
} RTSKVMODE;

typedef struct _RTSkvParam {
  INT32 model;
  SkvAecParam *aec;
  SkvBeamFormParam *bf;
  SkvRxParam *rx;
} RTSkvParam;
#endif

typedef struct rkAUDIO_QUEUE_S {
  int buffer_size;
  char *buffer;
  int roffset;
} AUDIO_QUEUE_S;
struct rkAUDIO_VQE_S {
  AUDIO_QUEUE_S *in_queue;  /* for before process */
  AUDIO_QUEUE_S *out_queue; /* for after process */
  VQE_CONFIG_S stVqeConfig;
  SampleInfo sample_info;
#if USE_RKAPPLUS
  void *mSkvHandle;
  RTSkvParam *mParam;
#else
  RKAP_Handle ap_handle;
#endif

  AI_LAYOUT_E layout;
};

static int16_t get_bit_width(SampleFormat fmt) {
  int16_t bit_width = -1;

  switch (fmt) {
  case SAMPLE_FMT_U8:
  case SAMPLE_FMT_U8P:
    bit_width = 8;
    break;
  case SAMPLE_FMT_S16:
  case SAMPLE_FMT_S16P:
    bit_width = 16;
    break;
  case SAMPLE_FMT_S32:
  case SAMPLE_FMT_S32P:
    bit_width = 32;
    break;
  case SAMPLE_FMT_FLT:
  case SAMPLE_FMT_FLTP:
    bit_width = sizeof(float);
    break;
  case SAMPLE_FMT_G711A:
  case SAMPLE_FMT_G711U:
    bit_width = 8;
    break;
  default:
    break;
  }

  return bit_width;
}

static AUDIO_QUEUE_S *queue_create(int buf_size) {
  AUDIO_QUEUE_S *queue = (AUDIO_QUEUE_S *)calloc(sizeof(AUDIO_QUEUE_S), 1);
  if (!queue)
    return NULL;
  queue->buffer = (char *)malloc(buf_size);
  return queue;
}
static void queue_free(AUDIO_QUEUE_S *queue) {
  if (queue) {
    if (queue->buffer)
      free(queue->buffer);
    free(queue);
  }
}

static int queue_size(AUDIO_QUEUE_S *queue) { return queue->buffer_size; }

static int queue_write(AUDIO_QUEUE_S *queue, const unsigned char *buffer,
                       int bytes) {
  if ((buffer == NULL) || (bytes <= 0)) {
    RKMEDIA_LOGI("queue_capture_buffer buffer error! (buffer:%p, bytes:%d)",
                 buffer, bytes);
    return -1;
  }
  if (queue->buffer_size + bytes > RK_AUDIO_BUFFER_MAX_SIZE) {
    RKMEDIA_LOGI(
        "unexpected cap buffer size too big!! buffer_size:%d, bytes:%d",
        queue->buffer_size, bytes);
    return -1;
  }

  memcpy((char *)queue->buffer + queue->buffer_size, (char *)buffer, bytes);
  queue->buffer_size += bytes;
  return bytes;
}

static int queue_read(AUDIO_QUEUE_S *queue, unsigned char *buffer, int bytes) {
  if ((buffer == NULL) || (bytes <= 0)) {
    RKMEDIA_LOGI("queue_capture_buffer buffer error!");
    return -1;
  }
  if ((queue->roffset + bytes) > queue->buffer_size) {
    RKMEDIA_LOGI("queue_read  error!");
    return -1;
  }
  memcpy(buffer, queue->buffer + queue->roffset, bytes);
  queue->roffset += bytes;
  return bytes;
}

static void queue_tune(AUDIO_QUEUE_S *queue) {
  /* Copy the rest of the sample to the beginning of the Buffer */
  memcpy(queue->buffer, queue->buffer + queue->roffset,
         queue->buffer_size - queue->roffset);
  queue->buffer_size = queue->buffer_size - queue->roffset;
  queue->roffset = 0;
}

#if USE_RKAPPLUS
static SkvAnrParam *createSkvAnrConfig() {
  SkvAnrParam *param = (SkvAnrParam *)rt_malloc(sizeof(SkvAnrParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->noiseFactor = 0.88;
  param->swU = 10;
  param->psiMin = 0.2;
  param->psiMax = 0.316;
  param->fGmin = 0.2;
  return param;
}

static SkvAgcParam *createSkvAgcConfig() {
  SkvAgcParam *param = (SkvAgcParam *)rt_malloc(sizeof(SkvAgcParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->attack_time = 200.0;
  param->release_time = 300.0;
  param->max_gain = 12.0;
  param->max_peak = -1.0;
  param->fRk0 = 2;
  param->fRth2 = -15;
  param->fRth1 = -35;
  param->fRth0 = -45;

  param->fs = 0;
  param->frmlen = 0;
  param->attenuate_time = 1000;

  param->fRk1 = 0.8;
  param->fRk2 = 0.4;
  param->fLineGainDb = 10.0f;
  param->swSmL0 = 40;
  param->swSmL1 = 80;
  param->swSmL2 = 80;

  return param;
}

static SkvCngParam *createSkvCngConfig() {
  SkvCngParam *param = (SkvCngParam *)rt_malloc(sizeof(SkvCngParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->fSmoothAlpha = 0.92f;
  param->fSpeechGain = 0.3f;
  param->fGain = 3;
  param->fMpy = 6;

  return param;
}

static SkvDtdParam *createSkvDtdConfig() {
  SkvDtdParam *param = (SkvDtdParam *)rt_malloc(sizeof(SkvDtdParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->ksiThd_high = 0.70f;
  param->ksiThd_low = 0.50f;
  return param;
}

static SkvAecParam *createSkvAecConfig() {
  SkvAecParam *param = (SkvAecParam *)rt_malloc(sizeof(SkvAecParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->pos = 1;
  param->drop_ref_channel = 0;
  param->aec_mode = 0;
  param->delay_len = 0;
  param->look_ahead = 0;
  param->mic_chns_map = RT_NULL;

  return param;
}

static SkvDereverbParam *createSkvDereverbConfig() {
  SkvDereverbParam *param =
      (SkvDereverbParam *)rt_malloc(sizeof(SkvDereverbParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->rlsLg = 4;
  param->curveLg = 30;
  param->delay = 2;
  param->forgetting = 0.98;
  param->t60 = 1.2f;
  param->coCoeff = 2.0f;

  return param;
}

static SkvNlpParam *createSkvNlpconfig() {
  SkvNlpParam *param = (SkvNlpParam *)rt_malloc(sizeof(SkvNlpParam));
  if (param == RT_NULL)
    return RT_NULL;

  INT32 ashwAecBandNlpPara_16k[8][2] = {
      /* BandPassThd      SuperEstFactor*/
      {10, 6},  /* Hz: 0    - 300  */
      {10, 10}, /* Hz: 300  - 575  */
      {10, 10}, /* Hz: 575  - 950  */
      {5, 10},  /* Hz: 950  - 1425 */
      {5, 10},  /* Hz: 1425 - 2150 */
      {5, 10},  /* Hz: 2150 - 3350 */
      {0, 6},   /* Hz: 3350 - 5450 */
      {0, 6},   /* Hz: 5450 - 8000 */
  };

  INT32 i, j;
  for (i = 0; i < 8; i++) {
    for (j = 0; j < 2; j++) {
      param->nlp16k[i][j] = ashwAecBandNlpPara_16k[i][j];
    }
  }
  return param;
}

static SkvAesParam *createSkvAesconfig() {
  SkvAesParam *param = (SkvAesParam *)rt_malloc(sizeof(SkvAesParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->beta_up = 0.002f;
  param->beta_down = 0.001f;
  return param;
}

static SkvEqParam *createSkvEqconfig() {
  SkvEqParam *param = (SkvEqParam *)rt_malloc(sizeof(SkvEqParam));
  if (param == RT_NULL)
    return RT_NULL;

  INT16 EqPara[5][13] = {
      // filter_bank 1
      {-1, -1, -1, -1, -2, -1, -1, -1, -1, -1, -1, -2, -3},
      // filter_bank 2
      {-1, -1, -1, -1, -2, -2, -3, -5, -3, -2, -1, -1, -2},
      // filter_bank 3
      {-2, -5, -9, -4, -2, -2, -1, -5, -5, -11, -20, -11, -5},
      // filter_bank 4
      {-5, -1, -7, -7, -19, -40, -20, -9, -10, -1, -20, -24, -60},
      // filter_bank 5
      {-128, -76, -40, -44, -1, -82, -111, -383, -1161, -1040, -989, -3811,
       32764},
  };

  param->shwParaLen = 65;
  int i, j;
  for (i = 0; i < 5; i++) {
    for (j = 0; j < 13; j++) {
      param->pfCoeff[i][j] = EqPara[i][j];
    }
  }

  return param;
}

static SkvBeamFormParam *createSkvBeamFormConfig() {
  SkvBeamFormParam *param =
      (SkvBeamFormParam *)rt_malloc(sizeof(SkvBeamFormParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->model_en = BF_EN_AES | BF_EN_ANR | BF_EN_AGC | BF_EN_STDT;
  param->targ = 2;
  param->ref_pos = 1;
  param->num_ref_channel = 1;
  param->drop_ref_channel = 0;

  param->agc = createSkvAgcConfig();
  param->anr = createSkvAnrConfig();
  param->nlp = createSkvNlpconfig();
  param->dereverb = createSkvDereverbConfig();
  param->cng = createSkvCngConfig();
  param->dtd = createSkvDtdConfig();
  param->aes = createSkvAesconfig();
  param->eq = createSkvEqconfig();

  return param;
}

static void destroySkvAnrConfig(SkvAnrParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvAgcConfig(SkvAgcParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvAecConfig(SkvAecParam **config) {
  SkvAecParam *aec = *config;
  if (aec != RT_NULL)
    rt_safe_free(aec->mic_chns_map);

  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvNlpConfig(SkvNlpParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvDereverbConfig(SkvDereverbParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvCngConfig(SkvCngParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvDtdConfig(SkvDtdParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvAesConfig(SkvAesParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvEqConfig(SkvEqParam **config) {
  rt_safe_free(*config);
  *config = RT_NULL;
}

static void destroySkvBeamFormConfig(SkvBeamFormParam **config) {
  SkvBeamFormParam *param = *config;
  if (param != RT_NULL) {
    destroySkvDereverbConfig(&param->dereverb);
    destroySkvNlpConfig(&param->nlp);
    destroySkvAnrConfig(&param->anr);
    destroySkvAgcConfig(&param->agc);
    destroySkvCngConfig(&param->cng);
    destroySkvDtdConfig(&param->dtd);
    destroySkvAesConfig(&param->aes);
    destroySkvEqConfig(&param->eq);
    rt_safe_free(param);
    *config = RT_NULL;
  }
}

static void dumpSkvAecConfig(SkvAecParam *param) {
  if (param == RT_NULL)
    return;

  RT_LOGD("********************* AEC Configs dump ******************");
  RT_LOGD("pos = %d, drop_ref_channel = %d", param->pos,
          param->drop_ref_channel);
  RT_LOGD("aec_mode = 0x%x, delay_len = %d, look_ahead = %d", param->aec_mode,
          param->delay_len, param->look_ahead);
  RT_LOGD("********************** End ******************************");
}

static void dumpSkvBeamFormConfig(SkvBeamFormParam *param) {
  INT32 i;

  if (param == RT_NULL)
    return;

  RT_LOGD("****************** BeamForm Configs dump ******************");
  RT_LOGD("model_en = 0x%x, ref_pos = %d", param->model_en, param->ref_pos);
  RT_LOGD("targ = %d", param->targ);
  RT_LOGD("num_ref_channel = %d, drop_ref_channel = %d", param->num_ref_channel,
          param->drop_ref_channel);

  if ((param->model_en & BF_EN_DEREV) && param->dereverb) {
    RT_LOGD("dereverb:");
    SkvDereverbParam *dere = param->dereverb;
    RT_LOGD("   rlsLg = %d, curveLg = %d, delay = %d", dere->rlsLg,
            dere->curveLg, dere->delay);
    RT_LOGD("   forgetting = %f, t60 = %f, coCoeff = %f", dere->forgetting,
            dere->t60, dere->coCoeff);
  }

  if ((param->model_en & BF_EN_NLP) && param->nlp) {
    RT_LOGD("nlp:");
    RT_LOGD("   BandPassThd   SuperEstFactor");
    SkvNlpParam *nlp = param->nlp;
    for (i = 0; i < 8; i++) {
      RT_LOGD("        %2d             %2d", nlp->nlp16k[i][0],
              nlp->nlp16k[i][1]);
    }
  }

  if ((param->model_en & BF_EN_AES) && param->aes) {
    SkvAesParam *aes = param->aes;
    RT_LOGD("aes:");
    RT_LOGD("   beta_up = %f, beta_down = %f", aes->beta_up, aes->beta_down);
  }

  if ((param->model_en & BF_EN_ANR) && param->anr) {
    SkvAnrParam *anr = param->anr;
    RT_LOGD("anr:");
    RT_LOGD("   noiseFactor = %f, swU = %d", anr->noiseFactor, anr->swU);
    RT_LOGD("   psiMin = %f, psiMax = %f, fGmin = %f", anr->psiMin, anr->psiMax,
            anr->fGmin);
  }

  if ((param->model_en & BF_EN_AGC) && param->agc) {
    SkvAgcParam *agc = param->agc;
    RT_LOGD("agc:");
    RT_LOGD("   attack_time = %f, release_time = %f, attenuate_time = %f",
            agc->attack_time, agc->release_time, agc->attenuate_time);
    RT_LOGD("   max_gain = %f, max_peak = %f", agc->max_gain, agc->max_peak);
    RT_LOGD("   fs = %d, frmlen = %d", agc->fs, agc->frmlen);

    RT_LOGD("   fLineGainDb = %f", agc->fLineGainDb);
    RT_LOGD("   fRth0 = %f, fRth1 = %f, fRth2 = %f", agc->fRth0, agc->fRth1,
            agc->fRth2);
    RT_LOGD("   fRk0 = %f, fRk1 = %f, fRk1 = %f", agc->fRk0, agc->fRk1,
            agc->fRk2);
    RT_LOGD("   swSmL0 = %d, swSmL1 = %d, swSmL2 = %d", agc->swSmL0,
            agc->swSmL1, agc->swSmL2);
  }

  if ((param->model_en & BF_EN_CNG) && param->cng) {
    SkvCngParam *cng = param->cng;
    RT_LOGD("cng:");
    RT_LOGD("   fGain = %f, fMpy = %f", cng->fGain, cng->fMpy);
    RT_LOGD("   fSmoothAlpha = %f, fSpeechGain = %f", cng->fSmoothAlpha,
            cng->fSpeechGain);
  }

  if ((param->model_en & BF_EN_STDT) && param->dtd) {
    SkvDtdParam *dtd = param->dtd;
    RT_LOGD("stdt:");
    RT_LOGD("   ksiThd_high = %f, ksiThd_low = %f", dtd->ksiThd_high,
            dtd->ksiThd_low);
  }
  RT_LOGD("********************** End ***************************");
}

static RTSkvParam *createSkvConfigs() {
  RTSkvParam *param = (RTSkvParam *)rt_malloc(sizeof(RTSkvParam));
  if (param == RT_NULL)
    return RT_NULL;

  param->model = RT_SKV_BF | RT_SKV_AEC;
  param->aec = createSkvAecConfig();
  RT_ASSERT(param->aec != RT_NULL);

  param->bf = createSkvBeamFormConfig();
  RT_ASSERT(param->bf != RT_NULL);

  param->rx = RT_NULL;

  return param;
}

static void destroySkvConfig(RTSkvParam **config) {
  RTSkvParam *param = *config;
  if (param == RT_NULL)
    return;

  destroySkvAecConfig(&param->aec);
  destroySkvBeamFormConfig(&param->bf);
  rt_safe_free(param);
  *config = RT_NULL;
}
#endif

static bool RK_AUDIO_VQE_Dumping(void) {
  char *value = NULL;

  /* env: "RKAP_DUMP_PCM=TRUE" or "RKAP_DUMP_PCM=true" ? */
  value = getenv("RKAP_DUMP_PCM");
  if (value && (!strcmp("TRUE", value) || !strcmp("true", value)))
    return true;

  return false;
}

int AI_TALKVQE_Init(AUDIO_VQE_S *handle, VQE_CONFIG_S *config) {
  SampleInfo sample_info = handle->sample_info;

#if USE_RKAPPLUS
  int mic_num = 1, ref_num = 1;

  // check params
  if (sample_info.channels != 2 && sample_info.channels != 4) {
    RKMEDIA_LOGE("check failed: aec channels(%d) should be equal 2 or 4",
                 sample_info.channels);
    return -1;
  }

  handle->mParam = createSkvConfigs();
  RT_ASSERT(handle->mParam != RT_NULL);

  switch (handle->layout) {
  case AI_LAYOUT_2MIC_REF_NONE:
  case AI_LAYOUT_2MIC_NONE_REF:
    mic_num = 2;
    ref_num = 1;
    break;
  case AI_LAYOUT_2MIC_2REF:
    mic_num = 2;
    ref_num = 2;
    break;
  default:
    break;
  }

  /* FIXME: sync the AGC param fs and frmlen with runtime */
  handle->mParam->bf->agc->fs = sample_info.sample_rate;
  handle->mParam->bf->agc->frmlen =
      handle->stVqeConfig.stAiTalkConfig.s32FrameSample;

  RKMEDIA_LOGI("%s - %d, layout=%d, mic_num=%d, ref_num=%d, frmlen=%d\n",
               __func__, __LINE__, handle->layout, mic_num, ref_num,
               handle->mParam->bf->agc->frmlen);

  handle->mSkvHandle = rkaudio_preprocess_init(
      sample_info.sample_rate, get_bit_width(sample_info.fmt), mic_num, ref_num,
      handle->mParam);

  if (handle->mParam->model & RT_SKV_AEC) {
    dumpSkvAecConfig(handle->mParam->aec);
  }

  if (handle->mParam->model & RT_SKV_BF) {
    dumpSkvBeamFormConfig(handle->mParam->bf);
  }

  // rkaudio_param_printf(mic_num, ref_num, handle->mParam);

  UNUSED(config);

#else

  // check params
  if (sample_info.channels != 2) {
    RKMEDIA_LOGE("check failed: aec channels(%d) must equal 2",
                 sample_info.channels);
    return -1;
  }

  if (!(sample_info.fmt == SAMPLE_FMT_S16 &&
        (sample_info.sample_rate == 8000 ||
         sample_info.sample_rate == 16000))) {
    RKMEDIA_LOGE("check failed: sample_info.fmt == SAMPLE_FMT_S16 && \
      (sample_info.sample_rate == 8000 | 16000))");
    return -1;
  }

  if (handle->layout != AI_LAYOUT_MIC_REF &&
      handle->layout != AI_LAYOUT_REF_MIC) {
    RKMEDIA_LOGE("check failed: enAiLayout must be AI_LAYOUT_MIC_REF or "
                 "AI_LAYOUT_REF_MIC\n");
    return -1;
  }

  RKAP_AEC_State state;
  state.swSampleRate = sample_info.sample_rate; // 8k|16k
  state.swFrameLen =
      ALGO_FRAME_TIMS_MS * sample_info.sample_rate / 1000; // hard code 20ms
  state.pathPara = config->stAiTalkConfig.aParamFilePath;
  RKMEDIA_LOGI("AEC: param file = %s\n", state.pathPara);
  RKAP_3A_DumpVersion();
  handle->ap_handle = RKAP_3A_Init(&state, AEC_TX_TYPE);
  if (!handle->ap_handle) {
    RKMEDIA_LOGI("AEC: init failed\n");
    return -1;
  }

#endif

  RKMEDIA_LOGI("sample_info - fmt:%d ch:%d rate:%d nb:%d frm:%d ALGO: %dms\n",
               sample_info.fmt, sample_info.channels, sample_info.sample_rate,
               sample_info.nb_samples,
               handle->stVqeConfig.stAiTalkConfig.s32FrameSample,
               handle->stVqeConfig.stAiTalkConfig.s32FrameSample * 1000 /
                   sample_info.sample_rate);

  return 0;
}

int AI_TALKVQE_Deinit(AUDIO_VQE_S *handle) {
#if USE_RKAPPLUS
  rkaudio_preprocess_destory(handle->mSkvHandle);
  destroySkvConfig(&handle->mParam);
#else
  RKAP_3A_Destroy(handle->ap_handle);
#endif
  return 0;
}

static int AI_TALKVQE_Process(AUDIO_VQE_S *handle, unsigned char *in,
                              unsigned char *out) {
  // for hardware refs signal
  FILE *fp;
  char filename[64] = {0};
#if USE_RKAPPLUS
  int nb_samples = handle->stVqeConfig.stAiTalkConfig.s32FrameSample;
  int channels = handle->sample_info.channels;
  int sorted_channels = channels;
  int16_t *sigin_skv;
  int16_t *sigout_skv;
  unsigned char skvbuf_in[nb_samples * channels * sizeof(int16_t)] = {
      0}; // FIXME: 1mic or 2mic
  unsigned char skvbuf_out[nb_samples * 1 * sizeof(int16_t)] = {0};
  bool only_anr = false;
  int is_wakeup = 0;
  int in_short = nb_samples * channels; /* Based 1mic channel bytes */

  sigout_skv = (int16_t *)skvbuf_out;
  sigin_skv = (int16_t *)skvbuf_in;

  /* FIXME: needs refence layout */
  if (handle->layout == AI_LAYOUT_MIC_REF) {
    RT_ASSERT(channels == 2);

    for (int i = 0; i < nb_samples; i++) {
      sigin_skv[i * channels] = *((int16_t *)in + i * channels);

      if (only_anr)
        sigin_skv[i * channels + 1] = 0;
      else
        sigin_skv[i * channels + 1] = *((int16_t *)in + i * channels + 1);
    }
  } else if (handle->layout == AI_LAYOUT_REF_MIC) {
    RT_ASSERT(channels == 2);

    for (int i = 0; i < nb_samples; i++) {
      if (only_anr)
        sigin_skv[i * channels] = 0;
      else
        sigin_skv[i * channels] = *((int16_t *)in + i * channels + 1);

      sigin_skv[i * channels + 1] = *((int16_t *)in + i * channels);
    }
  } else if (handle->layout == AI_LAYOUT_2MIC_REF_NONE) {
    RT_ASSERT(channels == 4);

    sorted_channels = channels - 1;
    in_short = nb_samples * (channels - 1);

    for (int i = 0; i < nb_samples; i++) {
      sigin_skv[i * sorted_channels] = *((int16_t *)in + i * channels);
      sigin_skv[i * sorted_channels + 1] = *((int16_t *)in + i * channels + 1);

      if (only_anr)
        sigin_skv[i * sorted_channels + 2] = 0;
      else
        sigin_skv[i * sorted_channels + 2] =
            *((int16_t *)in + i * channels + 2);
    }
  } else if (handle->layout == AI_LAYOUT_2MIC_NONE_REF) {
    RT_ASSERT(channels == 4);

    sorted_channels = channels - 1;
    in_short = nb_samples * (channels - 1);

    for (int i = 0; i < nb_samples; i++) {
      sigin_skv[i * sorted_channels] = *((int16_t *)in + i * channels);
      sigin_skv[i * sorted_channels + 1] = *((int16_t *)in + i * channels + 1);

      if (only_anr)
        sigin_skv[i * sorted_channels + 2] = 0;
      else
        sigin_skv[i * sorted_channels + 2] =
            *((int16_t *)in + i * channels + 3);
    }
  } else if (handle->layout == AI_LAYOUT_2MIC_2REF) {
    RT_ASSERT(channels == 4);

    for (int i = 0; i < nb_samples; i++) {
      sigin_skv[i * channels] = *((int16_t *)in + i * channels);
      sigin_skv[i * channels + 1] = *((int16_t *)in + i * channels + 1);

      if (only_anr) {
        sigin_skv[i * channels + 2] = 0;
        sigin_skv[i * channels + 3] = 0;
      } else {
        sigin_skv[i * channels + 2] = *((int16_t *)in + i * channels + 2);
        sigin_skv[i * channels + 3] = *((int16_t *)in + i * channels + 3);
      }
    }
  }

  if (RK_AUDIO_VQE_Dumping() == true) {
    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-plus-in-%dch-%d.pcm", sorted_channels,
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(sigin_skv, 1, nb_samples * sizeof(int16_t) * sorted_channels, fp);
    fclose(fp);

    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-plus-in-%dch-%d-raw.pcm", channels,
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(in, 1, nb_samples * sizeof(int16_t) * channels, fp);
    fclose(fp);
  }

  rkaudio_preprocess_short(handle->mSkvHandle, sigin_skv, sigout_skv, in_short,
                           &is_wakeup);

  if (RK_AUDIO_VQE_Dumping() == true) {
    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-plus-out-1ch-%d.pcm",
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(sigout_skv, 1, nb_samples * sizeof(int16_t) * 1, fp);
    fclose(fp);
  }

  int16_t *tmp2 = (int16_t *)sigout_skv;
  int16_t *tmp1 = (int16_t *)out;
  // need to export 2ch to output buffer
  for (int i = 0; i < nb_samples; i++) {
    tmp1[i * channels + 0] = tmp2[i * 1 + 0];
    tmp1[i * channels + 1] = tmp2[i * 1 + 0];
  }

#else

  int16_t bytes =
      handle->stVqeConfig.stAiTalkConfig.s32FrameSample * 4; // S16 && 2 channel
  int16_t *sigin;
  int16_t *sigref;
  unsigned char prebuf[bytes * 2] = {0};
  unsigned char sigout[bytes / 2] = {0};
  int nb_samples = bytes / 4;

  sigin = (int16_t *)prebuf;
  sigref = (int16_t *)prebuf + nb_samples;
  if (handle->layout == AI_LAYOUT_MIC_REF) {
    for (int i = 0; i < nb_samples; i++) {
      sigin[i] = *((int16_t *)in + i * 2);
      sigref[i] = *((int16_t *)in + i * 2 + 1);
    }
  } else if (handle->layout == AI_LAYOUT_REF_MIC) {
    for (int i = 0; i < nb_samples; i++) {
      sigref[i] = *((int16_t *)in + i * 2);
      sigin[i] = *((int16_t *)in + i * 2 + 1);
    }
  }

  if (RK_AUDIO_VQE_Dumping() == true) {
    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-in-%d.pcm",
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(sigin, 1, nb_samples * sizeof(int16_t) * 1, fp);
    fclose(fp);

    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-ref-%d.pcm",
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(sigref, 1, nb_samples * sizeof(int16_t) * 1, fp);
    fclose(fp);
  }

  RKAP_3A_Process(handle->ap_handle, sigin, sigref, (int16_t *)sigout);

  if (RK_AUDIO_VQE_Dumping() == true) {
    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-out-%d.pcm",
            handle->sample_info.sample_rate);
    fp = fopen(filename, "ab+");
    fwrite(sigout, 1, nb_samples * sizeof(int16_t) * 1, fp);
    fclose(fp);
  }

  int16_t *tmp2 = (int16_t *)sigout;
  int16_t *tmp1 = (int16_t *)out;
  for (int j = 0; j < nb_samples; j++) {
    *tmp1++ = *tmp2;
    *tmp1++ = *tmp2++;
  }
#endif

  return 0;
}

int AI_RECORDVQE_Init(AUDIO_VQE_S *handle, VQE_CONFIG_S *config) {
#if USE_RKAPPLUS
  UNUSED(handle);
  UNUSED(config);
#else
  SampleInfo sample_info = handle->sample_info;
  if (!(sample_info.fmt == SAMPLE_FMT_S16 && sample_info.sample_rate >= 8000 &&
        sample_info.sample_rate <= 48000)) {
    RKMEDIA_LOGI("check failed: fmt == SAMPLE_FMT_S16 && sample_rate >= 8000 "
                 "&& sample_rate <= 48000");
    return -1;
  }
  if (handle->layout == AI_LAYOUT_NORMAL) {
    if (sample_info.channels != 1) {
      RKMEDIA_LOGI(
          "ANR check failed: layout == AI_LAYOUT_NORMAL, channels must be 1\n");
      return -1;
    }
  } else {
    if (sample_info.channels != 2) {
      RKMEDIA_LOGI(
          "ANR check failed: layout != AI_LAYOUT_NORMAL, channels must be 2\n");
      return -1;
    }
  }

  RKAP_ANR_State state;
  state.swSampleRate = sample_info.sample_rate; // 8k|16k; // 8-48k
  state.swFrameLen =
      ALGO_FRAME_TIMS_MS * sample_info.sample_rate / 1000; // hard code 20ms
  state.fGmin = config->stAiRecordConfig.stAnrConfig.fGmin;
  state.fPostAddGain = config->stAiRecordConfig.stAnrConfig.fPostAddGain;
  state.fNoiseFactor = config->stAiRecordConfig.stAnrConfig.fNoiseFactor;
  state.enHpfSwitch = config->stAiRecordConfig.stAnrConfig.enHpfSwitch;
  state.fHpfFc = config->stAiRecordConfig.stAnrConfig.fHpfFc;
  state.enLpfSwitch = config->stAiRecordConfig.stAnrConfig.enLpfSwitch;
  state.fLpfFc = config->stAiRecordConfig.stAnrConfig.fLpfFc;

  RKAP_ANR_DumpVersion();
  handle->ap_handle = RKAP_ANR_Init(&state);
  if (!handle->ap_handle) {
    RKMEDIA_LOGI("ANR: init failed\n");
    return -1;
  }
#endif
  return 0;
}

int AI_RECORDVQE_Deinit(AUDIO_VQE_S *handle) {
#if USE_RKAPPLUS
  UNUSED(handle);
#else
  RKAP_ANR_Destroy(handle->ap_handle);
#endif
  return 0;
}

static int AI_RECORDVQE_Process(AUDIO_VQE_S *handle, unsigned char *in,
                                unsigned char *out) {
  // for hardware refs signal
  int16_t byte_width;
  SampleInfo sample_info = handle->sample_info;

  byte_width = get_bit_width(sample_info.fmt) / 8;
  if (byte_width < 0) {
    RKMEDIA_LOGE("get byte width(%d) failed, SampleFormat: %d\n", byte_width,
                 sample_info.fmt);
    return -1;
  }

  int nb_samples = handle->stVqeConfig.stAiTalkConfig.s32FrameSample;
  int16_t bytes = nb_samples * byte_width * sample_info.channels;

#if USE_RKAPPLUS
  memcpy(out, in, bytes);
#else
  int16_t *sigin;
  unsigned char prebuf[bytes] = {0};
  unsigned char sigout[bytes / sample_info.channels] = {0};

  sigin = (int16_t *)prebuf;
  if (handle->layout == AI_LAYOUT_MIC_REF) {
    for (int i = 0; i < nb_samples; i++)
      sigin[i] = *((int16_t *)in + i * 2);
  } else if (handle->layout == AI_LAYOUT_REF_MIC) {
    for (int i = 0; i < nb_samples; i++)
      sigin[i] = *((int16_t *)in + i * 2 + 1);
  } else if (handle->layout == AI_LAYOUT_NORMAL) {
    return RKAP_ANR_Process(handle->ap_handle, (int16_t *)in, (int16_t *)out);
  }

  RKAP_ANR_Process(handle->ap_handle, sigin, (int16_t *)sigout);

  int16_t *tmp2 = (int16_t *)sigout;
  int16_t *tmp1 = (int16_t *)out;
  for (int j = 0; j < nb_samples; j++) {
    *tmp1++ = *tmp2;
    *tmp1++ = *tmp2++;
  }
#endif

  return 0;
}

int AO_VQE_Init(AUDIO_VQE_S *handle, VQE_CONFIG_S *config) {
  SampleInfo sample_info = handle->sample_info;

#if USE_RKAPPLUS
  UNUSED(config);

  if (!(sample_info.fmt == SAMPLE_FMT_S16 &&
        (sample_info.sample_rate == 8000 || sample_info.sample_rate == 16000 ||
         sample_info.sample_rate == 48000))) {
    RKMEDIA_LOGI("check failed: sample_info.fmt == SAMPLE_FMT_S16 && \
     (sample_info.sample_rate == 8000 || 16000 || 48000))");
    return -1;
  }
#else
  if (!(sample_info.fmt == SAMPLE_FMT_S16 &&
        (sample_info.sample_rate == 8000 ||
         sample_info.sample_rate == 16000))) {
    RKMEDIA_LOGI("check failed: sample_info.fmt == SAMPLE_FMT_S16 && \
		 (sample_info.sample_rate == 8000 || 16000))");
    return -1;
  }

  RKAP_AEC_State state;
  state.swSampleRate = sample_info.sample_rate; // 8k|16k
  state.swFrameLen =
      ALGO_FRAME_TIMS_MS * sample_info.sample_rate / 1000; // hard code 20ms
  state.pathPara = config->stAoConfig.aParamFilePath;
  RKMEDIA_LOGI("AEC: param file = %s\n", state.pathPara);
  handle->ap_handle = RKAP_3A_Init(&state, AEC_RX_TYPE);
  if (!handle->ap_handle) {
    RKMEDIA_LOGI("AEC: init failed\n");
    return -1;
  }
#endif
  return 0;
}

int AO_VQE_Deinit(AUDIO_VQE_S *handle) {
#if USE_RKAPPLUS
  UNUSED(handle);
#else
  RKAP_3A_Destroy(handle->ap_handle);
#endif
  return 0;
}

static int AO_VQE_Process(AUDIO_VQE_S *handle, unsigned char *in,
                          unsigned char *out) {
  int16_t bytes =
      handle->stVqeConfig.stAiTalkConfig.s32FrameSample * 4; // S16 && 2 channel

#if USE_RKAPPLUS
  memcpy(out, in, bytes);
#else
  SampleInfo sample_info = handle->sample_info;

  if (sample_info.channels == 1) {
    RKAP_3A_Process(handle->ap_handle, (int16_t *)in, NULL, (int16_t *)out);
    return 0;
  }

  int16_t *left;
  unsigned char prebuf[bytes] = {0};
  unsigned char sigout[bytes / 2] = {0};
  int nb_samples = bytes / 4;

  left = (int16_t *)prebuf;
  for (int i = 0; i < nb_samples; i++) {
    left[i] = *((int16_t *)in + i * 2);
  }
  RKAP_3A_Process(handle->ap_handle, left, NULL, (int16_t *)sigout);

  int16_t *tmp2 = (int16_t *)sigout;
  int16_t *tmp1 = (int16_t *)out;
  for (int j = 0; j < nb_samples; j++) {
    *tmp1++ = *tmp2;
    *tmp1++ = *tmp2++;
  }
#endif

  return 0;
}

static int VQE_Process(AUDIO_VQE_S *handle, unsigned char *in,
                       unsigned char *out) {
  int ret;

  switch (handle->stVqeConfig.u32VQEMode) {
  case VQE_MODE_AI_TALK:
    ret = AI_TALKVQE_Process(handle, in, out);
    break;
  case VQE_MODE_AO:
    ret = AO_VQE_Process(handle, in, out);
    break;
  case VQE_MODE_AI_RECORD:
    ret = AI_RECORDVQE_Process(handle, in, out);
    break;
  default:
    ret = -1;
    break;
  }
  return ret;
}

bool RK_AUDIO_VQE_Support() { return true; }

AUDIO_VQE_S *RK_AUDIO_VQE_Init(const SampleInfo &sample_info,
                               AI_LAYOUT_E layout, VQE_CONFIG_S *config) {
  int ret = -1;
  AUDIO_VQE_S *handle = (AUDIO_VQE_S *)calloc(sizeof(AUDIO_VQE_S), 1);
  if (!handle)
    return NULL;

  handle->in_queue = queue_create(RK_AUDIO_BUFFER_MAX_SIZE);
  handle->out_queue = queue_create(RK_AUDIO_BUFFER_MAX_SIZE);

  handle->sample_info = sample_info;
  handle->layout = layout;
  handle->stVqeConfig = *config;

  switch (config->u32VQEMode) {
  case VQE_MODE_AI_TALK:
    ret = AI_TALKVQE_Init(handle, config);
    break;
  case VQE_MODE_AO:
    ret = AO_VQE_Init(handle, config);
    break;
  case VQE_MODE_AI_RECORD:
    ret = AI_RECORDVQE_Init(handle, config);
    break;
  case VQE_MODE_BUTT:
  default:
    ret = -1;
    break;
  }
  if (ret == 0)
    return handle;

  if (handle) {
    free(handle->in_queue);
    free(handle->out_queue);
    free(handle);
  }
  return NULL;
}

int RK_AUDIO_VQE_Handle(AUDIO_VQE_S *handle, void *buffer, int bytes) {
  int16_t algo_in_bytes = handle->sample_info.channels * sizeof(short) *
                          handle->stVqeConfig.stAiTalkConfig.s32FrameSample;

#if USE_DEEP_DUMP
  if (handle->stVqeConfig.u32VQEMode == VQE_MODE_AI_TALK &&
      (RK_AUDIO_VQE_Dumping() == true)) {
    char filename[64] = {0};
    FILE *fp;

    memset(filename, 0, sizeof(filename));
    sprintf(filename, "/tmp/vqe-rkap-plus-buffer-in.pcm");
    fp = fopen(filename, "ab+");
    fwrite(buffer, 1, bytes, fp);
    fclose(fp);
  }
#endif

  // 1. data in queue
  queue_write(handle->in_queue, (unsigned char *)buffer, bytes);

  // 2. peek data from in queue, do audio process, data out queue
  for (int i = 0; i < queue_size(handle->in_queue) / algo_in_bytes; i++) {
    unsigned char in[algo_in_bytes] = {0};
    unsigned char out[algo_in_bytes] = {0};
    queue_read(handle->in_queue, in, algo_in_bytes);
    VQE_Process(handle, in, out);
    queue_write(handle->out_queue, out, algo_in_bytes);
  }

  // 3. copy the rest of the sample to the beginning of the buffer
  queue_tune(handle->in_queue);

  // 4. try to peek data from out queue
  if (queue_size(handle->out_queue) >= bytes) {
    queue_read(handle->out_queue, (unsigned char *)buffer, bytes);
    queue_tune(handle->out_queue);

#if USE_DEEP_DUMP
    if (handle->stVqeConfig.u32VQEMode == VQE_MODE_AI_TALK) {
      char filename[64] = {0};
      FILE *fp;

      memset(filename, 0, sizeof(filename));
      sprintf(filename, "/tmp/vqe-rkap-plus-buffer-out.pcm");
      fp = fopen(filename, "ab+");
      fwrite(buffer, 1, bytes, fp);
      fclose(fp);
    }
#endif
    return 0;
  } else {
    RKMEDIA_LOGE("%s: queue size %d less than %d\n", __func__,
                 queue_size(handle->out_queue), bytes);
    return -1;
  }
}

void RK_AUDIO_VQE_Deinit(AUDIO_VQE_S *handle) {
  queue_free(handle->in_queue);
  queue_free(handle->out_queue);

  switch (handle->stVqeConfig.u32VQEMode) {
  case VQE_MODE_AI_TALK:
    AI_TALKVQE_Deinit(handle);
    break;
  case VQE_MODE_AO:
    AO_VQE_Deinit(handle);
    break;
  case VQE_MODE_AI_RECORD:
    AI_RECORDVQE_Deinit(handle);
    break;
  case VQE_MODE_BUTT:
  default:
    return;
  }

  if (handle)
    free(handle);
}

#else

struct rkAUDIO_VQE_S {};
bool RK_AUDIO_VQE_Support() { return false; }
AUDIO_VQE_S *RK_AUDIO_VQE_Init(const SampleInfo &sample_info _UNUSED,
                               AI_LAYOUT_E layout _UNUSED,
                               VQE_CONFIG_S *config _UNUSED) {
  return NULL;
}
void RK_AUDIO_VQE_Deinit(AUDIO_VQE_S *handle _UNUSED) {}
int RK_AUDIO_VQE_Handle(AUDIO_VQE_S *handle _UNUSED, void *buffer _UNUSED,
                        int bytes _UNUSED) {
  return 0;
}

#endif
