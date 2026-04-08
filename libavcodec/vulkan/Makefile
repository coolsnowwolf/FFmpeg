GEN_CLEANSUFFIXES = *.o *.c *.d

clean::
	$(RM) $(GEN_CLEANSUFFIXES:%=libavcodec/vulkan/%)

OBJS-$(CONFIG_PRORES_RAW_VULKAN_HWACCEL) += vulkan/common.o \
					    vulkan/prores_raw.o

OBJS-$(CONFIG_PRORES_VULKAN_HWACCEL) += vulkan/common.o \
                                        vulkan/prores_reset.o \
                                        vulkan/prores_vld.o \
                                        vulkan/prores_idct.o

OBJS-$(CONFIG_PRORES_KS_VULKAN_ENCODER) += vulkan/common.o \
                                           vulkan/dct.o \
                                           vulkan/prores_ks_alpha_data.o \
                                           vulkan/prores_ks_slice_data.o \
                                           vulkan/prores_ks_estimate_slice.o \
                                           vulkan/prores_ks_encode_slice.o \
                                           vulkan/prores_ks_trellis_node.o

VULKAN = $(subst $(SRC_PATH)/,,$(wildcard $(SRC_PATH)/libavcodec/vulkan/*.comp))
.SECONDARY: $(VULKAN:.comp=.c)
libavcodec/vulkan/%.c: TAG = VULKAN
libavcodec/vulkan/%.c: $(SRC_PATH)/libavcodec/vulkan/%.comp
	$(M)$(SRC_PATH)/tools/source2c $< $@
