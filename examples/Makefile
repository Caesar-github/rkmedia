hide := @
ECHO := echo

GCC := ../../../../../prebuilts/gcc/linux-x86/arm/gcc-arm-8.3-2019.03-x86_64-arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc-8.3.0
CFLAGS := -I../include/rkmedia \
			-I../include/rkaiq/common \
			-I../include/rkaiq/xcore \
			-I../include/rkaiq/uAPI \
			-I../include/rkaiq/algos \
			-I ../include/rkaiq/iq_parser

LIB_FILES := -L../lib

LD_FLAGS := -lpthread -leasymedia -ldrm -lrockchip_mpp \
	        -lavformat -lavcodec -lswresample -lavutil \
			-lasound -lv4l2 -lv4lconvert -lrga \
			-lRKAP_ANR -lRKAP_Common -lRKAP_3A \
			-lmd_share -lrkaiq -lod_share

CFLAGS += -DRKAIQ

SAMPLE_COMMON := common/sample_common_isp.c
all:
	$(GCC) rkmedia_audio_test.c $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_audio_test
	$(GCC) rkmedia_vi_venc_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_vi_venc_test
	$(GCC) rkmedia_venc_osd_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_venc_osd_test
	$(GCC) rkmedia_venc_cover_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_venc_cover_test
	$(GCC) rkmedia_venc_jpeg_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_venc_jpeg_test
	$(GCC) rkmedia_vi_md_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_vi_md_test
	$(GCC) rkmedia_isp_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_isp_test
	$(GCC) rkmedia_vi_multi_bind_test.c $(SAMPLE_COMMON) $(CFLAGS) $(LIB_FILES) $(LD_FLAGS) -o rkmedia_vi_multi_bind_test

	$(hide)$(ECHO) "Build Done ..."

