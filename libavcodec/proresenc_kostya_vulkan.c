/*
 * Apple ProRes Vulkan encoder
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/buffer.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext_vulkan.h"
#include "libavutil/vulkan_loader.h"
#include "libavutil/vulkan_spirv.h"

#include "avcodec.h"
#include "codec.h"
#include "codec_internal.h"
#include "encode.h"
#include "packet.h"
#include "profiles.h"
#include "bytestream.h"
#include "proresdata.h"
#include "proresenc_kostya_common.h"
#include "hwconfig.h"

extern const char *ff_source_common_comp;
extern const char *ff_source_dct_comp;
extern const char *ff_source_prores_ks_alpha_data_comp;
extern const char *ff_source_prores_ks_slice_data_comp;
extern const char *ff_source_prores_ks_estimate_slice_comp;
extern const char *ff_source_prores_ks_encode_slice_comp;
extern const char *ff_source_prores_ks_trellis_node_comp;

typedef struct ProresDataTables {
    int16_t qmat[128][64];
    int16_t qmat_chroma[128][64];
} ProresDataTables;

typedef struct SliceDataInfo {
    int plane;
    int line_add;
    int bits_per_sample;
} SliceDataInfo;

typedef struct EncodeSliceInfo {
    VkDeviceAddress bytestream;
    VkDeviceAddress seek_table;
} EncodeSliceInfo;

typedef struct SliceData {
    uint32_t mbs_per_slice;
    int16_t rows[MAX_PLANES * MAX_MBS_PER_SLICE * 256];
} SliceData;

typedef struct SliceScore {
    int bits[MAX_STORED_Q][4];
    int score[MAX_STORED_Q][4];
    int total_bits[MAX_STORED_Q];
    int total_score[MAX_STORED_Q];
    int overquant;
    int buf_start;
    int quant;
} SliceScore;

typedef struct VulkanEncodeProresFrameData {
    AVBufferRef *out_data_ref[2];
    AVBufferRef *slice_data_ref[2];
    AVBufferRef *slice_score_ref[2];
    AVBufferRef *frame_size_ref[2];

    VkImageView views[AV_NUM_DATA_POINTERS];
    int nb_views;

    int64_t pts;
    int64_t duration;
    void *frame_opaque;
    AVBufferRef *frame_opaque_ref;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;
    enum AVColorPrimaries color_primaries;
    int key_frame;
    int flags;
} VulkanEncodeProresFrameData;

typedef struct ProresVulkanContext {
    ProresContext ctx;

    FFVulkanContext vkctx;
    FFVkQueueFamilyCtx qf;
    FFVkExecPool e;

    AVBufferPool *pkt_buf_pool;
    AVBufferPool *slice_data_buf_pool;
    AVBufferPool *slice_score_buf_pool;
    AVBufferPool *frame_size_buf_pool;

    FFVkSPIRVShader shd_alpha_data;
    FFVulkanPipeline pl_alpha_data;

    FFVkSPIRVShader shd_slice_data[2];
    FFVulkanPipeline pl_slice_data[2];

    FFVkSPIRVShader shd_estimate_slice;
    FFVulkanPipeline pl_estimate_slice;

    FFVkSPIRVShader shd_encode_slice;
    FFVulkanPipeline pl_encode_slice;

    FFVkSPIRVShader shd_trellis_node;
    FFVulkanPipeline pl_trellis_node;

    FFVkBuffer prores_data_tables_buf;
    ProresDataTables *tables;

    int in_flight;
    int async_depth;
    AVFrame *frame;
    VulkanEncodeProresFrameData *exec_ctx_info;
} ProresVulkanContext;

static void destroy_views(ProresVulkanContext *pv, VulkanEncodeProresFrameData *pd)
{
    FFVulkanFunctions *vk = &pv->vkctx.vkfn;

    for (int i = 0; i < pd->nb_views; i++) {
        if (pd->views[i])
            vk->DestroyImageView(pv->vkctx.hwctx->act_dev, pd->views[i],
                                 pv->vkctx.hwctx->alloc);
        pd->views[i] = VK_NULL_HANDLE;
    }
    pd->nb_views = 0;
}

static int create_plane_views(ProresVulkanContext *pv, AVFrame *frame,
                              VulkanEncodeProresFrameData *pd)
{
    int err;
    int nb_planes;
    VkImageAspectFlags aspect;
    AVHWFramesContext *hwfc = (AVHWFramesContext *)frame->hw_frames_ctx->data;

    destroy_views(pv, pd);

    nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);
    for (int i = 0; i < nb_planes; i++) {
        err = ff_vk_create_imageview(&pv->vkctx, &pd->views[i], &aspect,
                                     frame, i, FF_VK_REP_FLOAT);
        if (err < 0) {
            destroy_views(pv, pd);
            return err;
        }
        pd->nb_views++;
    }

    return 0;
}

static void glsl_slice_data_struct(FFVkSPIRVShader *shd)
{
    GLSLC(0, struct SliceData {                               );
    GLSLC(1,     uint32_t mbs_per_slice;                     );
    GLSLC(1,     int16_t coeffs[4][8 * 256];                 );
    GLSLC(0, };                                              );
}

static void glsl_slice_score_struct(FFVkSPIRVShader *shd)
{
    GLSLC(0, struct SliceScore {                             );
    GLSLC(1,     ivec4 bits[16];                             );
    GLSLC(1,     ivec4 score[16];                            );
    GLSLC(1,     int total_bits[16];                         );
    GLSLC(1,     int total_score[16];                        );
    GLSLC(1,     int overquant;                              );
    GLSLC(1,     int buf_start;                              );
    GLSLC(1,     int quant;                                  );
    GLSLC(0, };                                              );
}

static int init_slice_data_pipeline(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                                    ProresVulkanContext *pv,
                                    FFVkSPIRVShader *shd, FFVulkanPipeline *pl,
                                    int blocks_per_mb)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    FFVulkanDescriptorSetBinding *desc;
    int nb_planes = av_pix_fmt_count_planes(pv->vkctx.frames->sw_format);

    RET(ff_vk_shader_init(pl, shd, "prores_ks_slice_data",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(shd, 8, blocks_per_mb, pv->ctx.mbs_per_slice);

    av_bprintf(&shd->src, "#define MAX_MBS_PER_SLICE %d\n", pv->ctx.mbs_per_slice);
    av_bprintf(&shd->src, "#define BLOCKS_PER_MB %d\n", blocks_per_mb);
    av_bprintf(&shd->src, "#define WIDTH_IN_MB %d\n", pv->ctx.mb_width);
    av_bprintf(&shd->src, "#define PICTURES_PER_FRAME %d\n", pv->ctx.pictures_per_frame);
    av_bprintf(&shd->src, "#define DCT_NB_BLOCKS %d\n",
               blocks_per_mb * pv->ctx.mbs_per_slice);

    GLSLD(ff_source_common_comp);
    GLSLD(ff_source_dct_comp);
    glsl_slice_data_struct(shd);

    GLSLC(0, layout(push_constant, scalar) uniform SliceDataInfo { );
    GLSLC(1,     int plane;                                         );
    GLSLC(1,     int line_add;                                      );
    GLSLC(1,     int bits_per_sample;                               );
    GLSLC(0, };                                                     );
    RET(ff_vk_add_push_constant(pl, 0, sizeof(SliceDataInfo),
                                VK_SHADER_STAGE_COMPUTE_BIT));

    desc = (FFVulkanDescriptorSetBinding[]) {
        {
            .name        = "SliceBuffer",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "writeonly",
            .buf_content = "SliceData slices[];",
        },
        {
            .name       = "planes",
            .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .dimensions = 2,
            .elems      = nb_planes,
            .mem_layout = "r16",
            .mem_quali  = "readonly",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&pv->vkctx, pl, shd, desc, 2, 0, 0));

    GLSLD(ff_source_prores_ks_slice_data_comp);

    RET(spv->compile_shader(spv, avctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(&pv->vkctx, shd, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&pv->vkctx, pl, shd));
    RET(ff_vk_exec_pipeline_register(&pv->vkctx, &pv->e, pl));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static int init_alpha_data_pipeline(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                                    ProresVulkanContext *pv)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    FFVulkanDescriptorSetBinding *desc;
    int nb_planes = av_pix_fmt_count_planes(pv->vkctx.frames->sw_format);
    FFVkSPIRVShader *shd = &pv->shd_alpha_data;

    RET(ff_vk_shader_init(&pv->pl_alpha_data, &pv->shd_alpha_data,
                          "prores_ks_alpha_data",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(&pv->shd_alpha_data, 16, 16, 1);

    av_bprintf(&pv->shd_alpha_data.src, "#define ALPHA_BITS %d\n", pv->ctx.alpha_bits);
    av_bprintf(&pv->shd_alpha_data.src, "#define SLICES_PER_ROW %d\n", pv->ctx.slices_width);
    av_bprintf(&pv->shd_alpha_data.src, "#define WIDTH_IN_MB %d\n", pv->ctx.mb_width);
    av_bprintf(&pv->shd_alpha_data.src, "#define MAX_MBS_PER_SLICE %d\n", pv->ctx.mbs_per_slice);

    glsl_slice_data_struct(&pv->shd_alpha_data);

    desc = (FFVulkanDescriptorSetBinding[]) {
        {
            .name        = "SliceBuffer",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "writeonly",
            .buf_content = "SliceData slices[];",
        },
        {
            .name       = "planes",
            .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            .dimensions = 2,
            .elems      = nb_planes,
            .mem_layout = "r16",
            .mem_quali  = "readonly",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&pv->vkctx, &pv->pl_alpha_data,
                                          &pv->shd_alpha_data, desc, 2, 0, 0));

    GLSLD(ff_source_prores_ks_alpha_data_comp);

    RET(spv->compile_shader(spv, avctx, &pv->shd_alpha_data, &spv_data, &spv_len,
                            "main", &spv_opaque));
    RET(ff_vk_shader_create(&pv->vkctx, &pv->shd_alpha_data, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&pv->vkctx, &pv->pl_alpha_data, &pv->shd_alpha_data));
    RET(ff_vk_exec_pipeline_register(&pv->vkctx, &pv->e, &pv->pl_alpha_data));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static int init_estimate_slice_pipeline(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                                        ProresVulkanContext *pv)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    FFVulkanDescriptorSetBinding *desc;
    int subgroup_size = 32;
    int dim_x = pv->ctx.alpha_bits ? subgroup_size : (subgroup_size / 3) * 3;
    FFVkSPIRVShader *shd = &pv->shd_estimate_slice;

    RET(ff_vk_shader_init(&pv->pl_estimate_slice, &pv->shd_estimate_slice,
                          "prores_ks_estimate_slice",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(&pv->shd_estimate_slice, dim_x, 1, 1);
    av_bprintf(&pv->shd_estimate_slice.src, "#extension GL_KHR_shader_subgroup_clustered : require\n");
    av_bprintf(&pv->shd_estimate_slice.src, "#extension GL_KHR_shader_subgroup_shuffle : require\n");
    av_bprintf(&pv->shd_estimate_slice.src, "#define MAX_MBS_PER_SLICE %d\n", pv->ctx.mbs_per_slice);
    av_bprintf(&pv->shd_estimate_slice.src, "#define CHROMA_FACTOR %d\n", pv->ctx.chroma_factor);
    av_bprintf(&pv->shd_estimate_slice.src, "#define ALPHA_BITS %d\n", pv->ctx.alpha_bits);
    av_bprintf(&pv->shd_estimate_slice.src, "#define NUM_PLANES %d\n", pv->ctx.num_planes);
    av_bprintf(&pv->shd_estimate_slice.src, "#define SLICES_PER_PICTURE %d\n", pv->ctx.slices_per_picture);
    av_bprintf(&pv->shd_estimate_slice.src, "#define MIN_QUANT %d\n",
               pv->ctx.force_quant ? 0 : pv->ctx.profile_info->min_quant);
    av_bprintf(&pv->shd_estimate_slice.src, "#define MAX_QUANT %d\n",
               pv->ctx.force_quant ? 0 : pv->ctx.profile_info->max_quant);
    av_bprintf(&pv->shd_estimate_slice.src, "#define BITS_PER_MB %d\n", pv->ctx.bits_per_mb);

    glsl_slice_data_struct(&pv->shd_estimate_slice);
    glsl_slice_score_struct(&pv->shd_estimate_slice);

    desc = (FFVulkanDescriptorSetBinding[]) {
        {
            .name        = "SliceBuffer",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "readonly",
            .buf_content = "SliceData slices[];",
        },
        {
            .name        = "SliceScores",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "writeonly",
            .buf_content = "SliceScore scores[];",
        },
        {
            .name        = "ProresDataTables",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t qmat[128][64]; int16_t qmat_chroma[128][64];",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&pv->vkctx, &pv->pl_estimate_slice,
                                          &pv->shd_estimate_slice, desc, 3, 0, 0));

    GLSLD(ff_source_prores_ks_estimate_slice_comp);

    RET(spv->compile_shader(spv, avctx, &pv->shd_estimate_slice,
                            &spv_data, &spv_len, "main", &spv_opaque));
    RET(ff_vk_shader_create(&pv->vkctx, &pv->shd_estimate_slice, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&pv->vkctx, &pv->pl_estimate_slice, &pv->shd_estimate_slice));
    RET(ff_vk_exec_pipeline_register(&pv->vkctx, &pv->e, &pv->pl_estimate_slice));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static int init_trellis_node_pipeline(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                                      ProresVulkanContext *pv)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    FFVulkanDescriptorSetBinding *desc;
    int subgroup_size = 32;
    int num_subgroups = FFALIGN(pv->ctx.mb_height, subgroup_size) / subgroup_size;
    FFVkSPIRVShader *shd = &pv->shd_trellis_node;

    RET(ff_vk_shader_init(&pv->pl_trellis_node, &pv->shd_trellis_node,
                          "prores_ks_trellis_node",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(&pv->shd_trellis_node, pv->ctx.mb_height, 1, 1);
    av_bprintf(&pv->shd_trellis_node.src, "#extension GL_KHR_shader_subgroup_arithmetic : require\n");
    av_bprintf(&pv->shd_trellis_node.src, "#define SLICES_PER_ROW %d\n", pv->ctx.slices_width);
    av_bprintf(&pv->shd_trellis_node.src, "#define NUM_SUBGROUPS %d\n", num_subgroups);
    av_bprintf(&pv->shd_trellis_node.src, "#define NUM_PLANES %d\n", pv->ctx.num_planes);
    av_bprintf(&pv->shd_trellis_node.src, "#define FORCE_QUANT %d\n", pv->ctx.force_quant);
    av_bprintf(&pv->shd_trellis_node.src, "#define MIN_QUANT %d\n", pv->ctx.profile_info->min_quant);
    av_bprintf(&pv->shd_trellis_node.src, "#define MAX_QUANT %d\n", pv->ctx.profile_info->max_quant);
    av_bprintf(&pv->shd_trellis_node.src, "#define MBS_PER_SLICE %d\n", pv->ctx.mbs_per_slice);
    av_bprintf(&pv->shd_trellis_node.src, "#define BITS_PER_MB %d\n", pv->ctx.bits_per_mb);

    glsl_slice_score_struct(&pv->shd_trellis_node);

    desc = (FFVulkanDescriptorSetBinding[]) {
        {
            .name        = "FrameSize",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "writeonly",
            .buf_content = "int frame_size;",
        },
        {
            .name        = "SliceScores",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "SliceScore scores[];",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&pv->vkctx, &pv->pl_trellis_node,
                                          &pv->shd_trellis_node, desc, 2, 0, 0));

    GLSLD(ff_source_prores_ks_trellis_node_comp);

    RET(spv->compile_shader(spv, avctx, &pv->shd_trellis_node,
                            &spv_data, &spv_len, "main", &spv_opaque));
    RET(ff_vk_shader_create(&pv->vkctx, &pv->shd_trellis_node, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&pv->vkctx, &pv->pl_trellis_node, &pv->shd_trellis_node));
    RET(ff_vk_exec_pipeline_register(&pv->vkctx, &pv->e, &pv->pl_trellis_node));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static int init_encode_slice_pipeline(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                                      ProresVulkanContext *pv)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    FFVulkanDescriptorSetBinding *desc;
    FFVkSPIRVShader *shd = &pv->shd_encode_slice;

    RET(ff_vk_shader_init(&pv->pl_encode_slice, &pv->shd_encode_slice,
                          "prores_ks_encode_slice",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(&pv->shd_encode_slice, 64, 1, 1);
    av_bprintf(&pv->shd_encode_slice.src, "#define PB_UNALIGNED\n");
    av_bprintf(&pv->shd_encode_slice.src, "#define MAX_MBS_PER_SLICE %d\n", pv->ctx.mbs_per_slice);
    av_bprintf(&pv->shd_encode_slice.src, "#define CHROMA_FACTOR %d\n", pv->ctx.chroma_factor);
    av_bprintf(&pv->shd_encode_slice.src, "#define ALPHA_BITS %d\n", pv->ctx.alpha_bits);
    av_bprintf(&pv->shd_encode_slice.src, "#define NUM_PLANES %d\n", pv->ctx.num_planes);
    av_bprintf(&pv->shd_encode_slice.src, "#define SLICES_PER_PICTURE %d\n", pv->ctx.slices_per_picture);
    av_bprintf(&pv->shd_encode_slice.src, "#define MAX_QUANT %d\n",
               pv->ctx.force_quant ? pv->ctx.force_quant : pv->ctx.profile_info->max_quant);

    GLSLD(ff_source_common_comp);
    glsl_slice_data_struct(&pv->shd_encode_slice);
    glsl_slice_score_struct(&pv->shd_encode_slice);

    GLSLC(0, layout(push_constant, scalar) uniform EncodeSliceInfo { );
    GLSLC(1,     u8buf bytestream;                                );
    GLSLC(1,     u8vec2buf seek_table;                            );
    GLSLC(0, };                                                   );
    RET(ff_vk_add_push_constant(&pv->pl_encode_slice, 0, sizeof(EncodeSliceInfo),
                                VK_SHADER_STAGE_COMPUTE_BIT));

    desc = (FFVulkanDescriptorSetBinding[]) {
        {
            .name        = "SliceBuffer",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "readonly",
            .buf_content = "SliceData slices[];",
        },
        {
            .name        = "SliceScores",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .mem_quali   = "readonly",
            .buf_content = "SliceScore scores[];",
        },
        {
            .name        = "ProresDataTables",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t qmat[128][64]; int16_t qmat_chroma[128][64];",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&pv->vkctx, &pv->pl_encode_slice,
                                          &pv->shd_encode_slice, desc, 3, 0, 0));

    GLSLD(ff_source_prores_ks_encode_slice_comp);

    RET(spv->compile_shader(spv, avctx, &pv->shd_encode_slice,
                            &spv_data, &spv_len, "main", &spv_opaque));
    RET(ff_vk_shader_create(&pv->vkctx, &pv->shd_encode_slice, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&pv->vkctx, &pv->pl_encode_slice, &pv->shd_encode_slice));
    RET(ff_vk_exec_pipeline_register(&pv->vkctx, &pv->e, &pv->pl_encode_slice));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static int submit_picture(AVCodecContext *avctx, FFVkExecContext *exec,
                          AVFrame *frame, int picture_idx)
{
    int err = 0;
    int min_quant;
    int max_quant;
    int is_chroma;
    int estimate_dim_x;
    int nb_img_bar = 0;
    ProresVulkanContext *pv = avctx->priv_data;
    ProresContext *ctx = &pv->ctx;
    VulkanEncodeProresFrameData *pd = exec->query_data;
    FFVulkanFunctions *vk = &pv->vkctx.vkfn;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pv->vkctx.frames->sw_format);
    FFVkBuffer *pkt_vk_buf;
    FFVkBuffer *slice_data_buf;
    FFVkBuffer *slice_score_buf;
    FFVkBuffer *frame_size_buf;
    SliceDataInfo slice_data_info;
    EncodeSliceInfo encode_info;
    VkImageMemoryBarrier2 img_bar[AV_NUM_DATA_POINTERS];

    min_quant = ctx->profile_info->min_quant;
    max_quant = ctx->profile_info->max_quant;
    estimate_dim_x = ctx->alpha_bits ? 32 : 30;

    RET(ff_vk_get_pooled_buffer(&pv->vkctx, &pv->pkt_buf_pool, &pd->out_data_ref[picture_idx],
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                NULL,
                                ctx->frame_size_upper_bound + FF_INPUT_BUFFER_MIN_SIZE,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    pkt_vk_buf = (FFVkBuffer *)pd->out_data_ref[picture_idx]->data;
    RET(ff_vk_exec_add_dep_buf(&pv->vkctx, exec, &pd->out_data_ref[picture_idx], 1, 1));

    RET(ff_vk_get_pooled_buffer(&pv->vkctx, &pv->slice_data_buf_pool, &pd->slice_data_ref[picture_idx],
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                NULL, ctx->slices_per_picture * sizeof(SliceData),
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    slice_data_buf = (FFVkBuffer *)pd->slice_data_ref[picture_idx]->data;
    RET(ff_vk_exec_add_dep_buf(&pv->vkctx, exec, &pd->slice_data_ref[picture_idx], 1, 1));

    RET(ff_vk_get_pooled_buffer(&pv->vkctx, &pv->slice_score_buf_pool, &pd->slice_score_ref[picture_idx],
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                NULL, ctx->slices_per_picture * sizeof(SliceScore),
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    slice_score_buf = (FFVkBuffer *)pd->slice_score_ref[picture_idx]->data;
    RET(ff_vk_exec_add_dep_buf(&pv->vkctx, exec, &pd->slice_score_ref[picture_idx], 1, 1));

    RET(ff_vk_get_pooled_buffer(&pv->vkctx, &pv->frame_size_buf_pool, &pd->frame_size_ref[picture_idx],
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                NULL, sizeof(int),
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    frame_size_buf = (FFVkBuffer *)pd->frame_size_ref[picture_idx]->data;
    RET(ff_vk_exec_add_dep_buf(&pv->vkctx, exec, &pd->frame_size_ref[picture_idx], 1, 1));

    RET(ff_vk_exec_add_dep_frame(&pv->vkctx, exec, frame,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    ff_vk_frame_barrier(&pv->vkctx, exec, frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);
    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pImageMemoryBarriers = img_bar,
        .imageMemoryBarrierCount = nb_img_bar,
    });

    slice_data_info = (SliceDataInfo) {
        .line_add = ctx->pictures_per_frame == 1 ? 0
                  : picture_idx ^ !(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST),
    };

    for (int i = 0; i < ctx->num_planes; i++) {
        is_chroma = i == 1 || i == 2;
        if (i < 3) {
            FFVulkanPipeline *pl = &pv->pl_slice_data[!is_chroma || ctx->chroma_factor == CFACTOR_Y444];
            slice_data_info.plane = i;
            slice_data_info.bits_per_sample = desc->comp[i].depth;
            RET(ff_vk_set_descriptor_buffer(&pv->vkctx, pl, exec, 0, 0, 0,
                                            slice_data_buf, 0, slice_data_buf->size,
                                            VK_FORMAT_UNDEFINED));
            ff_vk_update_descriptor_img_array(&pv->vkctx, pl, exec, frame, pd->views,
                                              0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
            ff_vk_exec_bind_pipeline(&pv->vkctx, exec, pl);
            ff_vk_update_push_exec(&pv->vkctx, exec, pl, VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(slice_data_info), &slice_data_info);
            vk->CmdDispatch(exec->buf, ctx->slices_width, ctx->mb_height, 1);
        } else {
            RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_alpha_data, exec, 0, 0, 0,
                                            slice_data_buf, 0, slice_data_buf->size,
                                            VK_FORMAT_UNDEFINED));
            ff_vk_update_descriptor_img_array(&pv->vkctx, &pv->pl_alpha_data, exec, frame, pd->views,
                                              0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
            ff_vk_exec_bind_pipeline(&pv->vkctx, exec, &pv->pl_alpha_data);
            vk->CmdDispatch(exec->buf, ctx->mb_width, ctx->mb_height, 1);
        }
    }

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers = &(VkBufferMemoryBarrier2) {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = slice_data_buf->buf,
            .offset = 0,
            .size = slice_data_buf->size,
        },
        .bufferMemoryBarrierCount = 1,
    });

    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_estimate_slice, exec, 0, 0, 0,
                                    slice_data_buf, 0, slice_data_buf->size,
                                    VK_FORMAT_UNDEFINED));
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_estimate_slice, exec, 0, 1, 0,
                                    slice_score_buf, 0, slice_score_buf->size,
                                    VK_FORMAT_UNDEFINED));
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_estimate_slice, exec, 0, 2, 0,
                                    &pv->prores_data_tables_buf, 0, pv->prores_data_tables_buf.size,
                                    VK_FORMAT_UNDEFINED));
    ff_vk_exec_bind_pipeline(&pv->vkctx, exec, &pv->pl_estimate_slice);
    vk->CmdDispatch(exec->buf,
                    FFALIGN(ctx->slices_per_picture * ctx->num_planes, estimate_dim_x) / estimate_dim_x,
                    ctx->force_quant ? 1 : (max_quant - min_quant + 1), 1);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers = &(VkBufferMemoryBarrier2) {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = slice_score_buf->buf,
            .offset = 0,
            .size = slice_score_buf->size,
        },
        .bufferMemoryBarrierCount = 1,
    });

    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_trellis_node, exec, 0, 0, 0,
                                    frame_size_buf, 0, frame_size_buf->size,
                                    VK_FORMAT_UNDEFINED));
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_trellis_node, exec, 0, 1, 0,
                                    slice_score_buf, 0, slice_score_buf->size,
                                    VK_FORMAT_UNDEFINED));
    ff_vk_exec_bind_pipeline(&pv->vkctx, exec, &pv->pl_trellis_node);
    vk->CmdDispatch(exec->buf, 1, 1, 1);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers = (VkBufferMemoryBarrier2[]) {
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = frame_size_buf->buf,
                .offset = 0,
                .size = frame_size_buf->size,
            },
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = slice_score_buf->buf,
                .offset = 0,
                .size = slice_score_buf->size,
            },
        },
        .bufferMemoryBarrierCount = 2,
    });

    encode_info.seek_table = pkt_vk_buf->address;
    encode_info.bytestream = pkt_vk_buf->address + ctx->slices_per_picture * 2;
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_encode_slice, exec, 0, 0, 0,
                                    slice_data_buf, 0, slice_data_buf->size,
                                    VK_FORMAT_UNDEFINED));
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_encode_slice, exec, 0, 1, 0,
                                    slice_score_buf, 0, slice_score_buf->size,
                                    VK_FORMAT_UNDEFINED));
    RET(ff_vk_set_descriptor_buffer(&pv->vkctx, &pv->pl_encode_slice, exec, 0, 2, 0,
                                    &pv->prores_data_tables_buf, 0, pv->prores_data_tables_buf.size,
                                    VK_FORMAT_UNDEFINED));
    ff_vk_exec_bind_pipeline(&pv->vkctx, exec, &pv->pl_encode_slice);
    ff_vk_update_push_exec(&pv->vkctx, exec, &pv->pl_encode_slice,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(encode_info), &encode_info);
    vk->CmdDispatch(exec->buf, FFALIGN(ctx->slices_per_picture, 64) / 64,
                    ctx->num_planes, 1);

fail:
    return err;
}

static int get_packet(AVCodecContext *avctx, FFVkExecContext *exec, AVPacket *pkt)
{
    int err = 0;
    int frame_size;
    int picture_size;
    int pkt_size;
    uint8_t *orig_buf;
    uint8_t *buf;
    uint8_t *slice_sizes;
    uint8_t *picture_size_pos;
    ProresVulkanContext *pv = avctx->priv_data;
    ProresContext *ctx = &pv->ctx;
    VulkanEncodeProresFrameData *pd = exec->query_data;

    pkt_size = ctx->frame_size_upper_bound + FF_INPUT_BUFFER_MIN_SIZE;
    RET(ff_get_encode_buffer(avctx, pkt, pkt_size, 0));

    pkt->pts = pd->pts;
    pkt->dts = pd->pts;
    pkt->duration = pd->duration;
    pkt->flags |= AV_PKT_FLAG_KEY * pd->key_frame;

    if (avctx->flags & AV_CODEC_FLAG_COPY_OPAQUE) {
        pkt->opaque = pd->frame_opaque;
        pkt->opaque_ref = pd->frame_opaque_ref;
        pd->frame_opaque_ref = NULL;
    }

    ff_vk_exec_wait(&pv->vkctx, exec);

    orig_buf = pkt->data;
    buf = ff_prores_kostya_write_frame_header(avctx, ctx, &orig_buf, pd->flags,
                                              pd->color_primaries, pd->color_trc,
                                              pd->colorspace);

    for (int picture_idx = 0; picture_idx < ctx->pictures_per_frame; picture_idx++) {
        FFVkBuffer *out_data_buf = (FFVkBuffer *)pd->out_data_ref[picture_idx]->data;
        FFVkBuffer *frame_size_buf = (FFVkBuffer *)pd->frame_size_ref[picture_idx]->data;

        picture_size_pos = buf + 1;
        buf = ff_prores_kostya_write_picture_header(ctx, buf);

        slice_sizes = buf;
        buf += ctx->slices_per_picture * 2;
        buf += *(int *)frame_size_buf->mapped_mem;

        memcpy(slice_sizes, out_data_buf->mapped_mem, buf - slice_sizes);

        picture_size = buf - (picture_size_pos - 1);
        bytestream_put_be32(&picture_size_pos, picture_size);

        av_buffer_unref(&pd->out_data_ref[picture_idx]);
        av_buffer_unref(&pd->slice_data_ref[picture_idx]);
        av_buffer_unref(&pd->slice_score_ref[picture_idx]);
        av_buffer_unref(&pd->frame_size_ref[picture_idx]);
    }

    orig_buf -= 8;
    frame_size = buf - orig_buf;
    bytestream_put_be32(&orig_buf, frame_size);
    av_shrink_packet(pkt, frame_size);

    destroy_views(pv, pd);

fail:
    return err;
}

static int vulkan_encode_prores_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    int err;
    ProresVulkanContext *pv = avctx->priv_data;
    FFVkExecContext *exec;
    VulkanEncodeProresFrameData *pd;
    AVFrame *frame;

    while (1) {
        exec = ff_vk_exec_get(&pv->e);
        if (exec->had_submission) {
            exec->had_submission = 0;
            pv->in_flight--;
            return get_packet(avctx, exec, pkt);
        }

        frame = pv->frame;
        err = ff_encode_get_frame(avctx, frame);
        if (err < 0 && err != AVERROR_EOF)
            return err;
        if (err == AVERROR_EOF) {
            if (!pv->in_flight)
                return err;
            continue;
        }

        pd = exec->query_data;
        pd->color_primaries = frame->color_primaries;
        pd->color_trc = frame->color_trc;
        pd->colorspace = frame->colorspace;
        pd->pts = frame->pts;
        pd->duration = frame->duration;
        pd->flags = frame->flags;
        pd->key_frame = !!(frame->flags & AV_FRAME_FLAG_KEY);
        if (avctx->flags & AV_CODEC_FLAG_COPY_OPAQUE) {
            pd->frame_opaque = frame->opaque;
            pd->frame_opaque_ref = frame->opaque_ref;
            frame->opaque_ref = NULL;
        }

        err = create_plane_views(pv, frame, pd);
        if (err < 0)
            return err;

        ff_vk_exec_start(&pv->vkctx, exec);
        err = submit_picture(avctx, exec, frame, 0);
        if (err >= 0 && pv->ctx.pictures_per_frame > 1)
            err = submit_picture(avctx, exec, frame, 1);

        if (err < 0) {
            destroy_views(pv, pd);
            return err;
        }

        RET(ff_vk_exec_submit(&pv->vkctx, exec));
        av_frame_unref(frame);
        pv->in_flight++;
        if (pv->in_flight < pv->async_depth)
            return AVERROR(EAGAIN);
    }

fail:
    return err;
}

static av_cold int encode_close(AVCodecContext *avctx)
{
    ProresVulkanContext *pv = avctx->priv_data;

    for (int i = 0; i < pv->async_depth; i++) {
        if (pv->exec_ctx_info)
            destroy_views(pv, &pv->exec_ctx_info[i]);
    }

    ff_vk_exec_pool_free(&pv->vkctx, &pv->e);

    ff_vk_pipeline_free(&pv->vkctx, &pv->pl_alpha_data);
    ff_vk_shader_free(&pv->vkctx, &pv->shd_alpha_data);
    for (int i = 0; i < 2; i++) {
        ff_vk_pipeline_free(&pv->vkctx, &pv->pl_slice_data[i]);
        ff_vk_shader_free(&pv->vkctx, &pv->shd_slice_data[i]);
    }
    ff_vk_pipeline_free(&pv->vkctx, &pv->pl_estimate_slice);
    ff_vk_shader_free(&pv->vkctx, &pv->shd_estimate_slice);
    ff_vk_pipeline_free(&pv->vkctx, &pv->pl_encode_slice);
    ff_vk_shader_free(&pv->vkctx, &pv->shd_encode_slice);
    ff_vk_pipeline_free(&pv->vkctx, &pv->pl_trellis_node);
    ff_vk_shader_free(&pv->vkctx, &pv->shd_trellis_node);

    ff_vk_free_buf(&pv->vkctx, &pv->prores_data_tables_buf);

    av_buffer_pool_uninit(&pv->pkt_buf_pool);
    av_buffer_pool_uninit(&pv->slice_data_buf_pool);
    av_buffer_pool_uninit(&pv->slice_score_buf_pool);
    av_buffer_pool_uninit(&pv->frame_size_buf_pool);

    av_freep(&pv->exec_ctx_info);
    av_frame_free(&pv->frame);

    ff_vk_uninit(&pv->vkctx);

    return 0;
}

static av_cold int encode_init(AVCodecContext *avctx)
{
    int err = 0;
    FFVkSPIRVCompiler *spv = NULL;
    ProresVulkanContext *pv = avctx->priv_data;
    ProresContext *ctx = &pv->ctx;

    RET(ff_vk_init(&pv->vkctx, avctx, NULL, avctx->hw_frames_ctx));
    RET(ff_vk_qf_init(&pv->vkctx, &pv->qf, VK_QUEUE_COMPUTE_BIT));
    RET(ff_vk_exec_pool_init(&pv->vkctx, &pv->qf, &pv->e,
                             FFMAX(pv->async_depth, 1), 0, 0, 0, NULL));

    err = ff_prores_kostya_encode_init(avctx, ctx, pv->vkctx.frames->sw_format);
    if (err < 0)
        goto fail;

    pv->frame = av_frame_alloc();
    if (!pv->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    pv->async_depth = pv->e.pool_size;
    pv->exec_ctx_info = av_calloc(pv->async_depth, sizeof(*pv->exec_ctx_info));
    if (!pv->exec_ctx_info) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    for (int i = 0; i < pv->async_depth; i++)
        pv->e.contexts[i].query_data = &pv->exec_ctx_info[i];

    spv = ff_vk_spirv_init();
    if (!spv) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    RET(init_slice_data_pipeline(avctx, spv, pv, &pv->shd_slice_data[0], &pv->pl_slice_data[0], 2));
    RET(init_slice_data_pipeline(avctx, spv, pv, &pv->shd_slice_data[1], &pv->pl_slice_data[1], 4));
    RET(init_estimate_slice_pipeline(avctx, spv, pv));
    RET(init_trellis_node_pipeline(avctx, spv, pv));
    RET(init_encode_slice_pipeline(avctx, spv, pv));
    if (ctx->alpha_bits)
        RET(init_alpha_data_pipeline(avctx, spv, pv));

    RET(ff_vk_create_buf(&pv->vkctx, &pv->prores_data_tables_buf,
                         sizeof(ProresDataTables), NULL, NULL,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    RET(ff_vk_map_buffer(&pv->vkctx, &pv->prores_data_tables_buf,
                         (uint8_t **)&pv->tables, 0));

    for (int q = 0; q < MAX_STORED_Q; q++) {
        for (int i = 0; i < 64; i++) {
            pv->tables->qmat[q][i] = ctx->quants[q][ctx->scantable[i]];
            pv->tables->qmat_chroma[q][i] = ctx->quants_chroma[q][ctx->scantable[i]];
        }
    }
    for (int q = MAX_STORED_Q; q < 128; q++) {
        for (int i = 0; i < 64; i++) {
            pv->tables->qmat[q][i] = ctx->quant_mat[ctx->scantable[i]] * q;
            pv->tables->qmat_chroma[q][i] = ctx->quant_chroma_mat[ctx->scantable[i]] * q;
        }
    }
    RET(ff_vk_unmap_buffer(&pv->vkctx, &pv->prores_data_tables_buf, 1));

fail:
    if (spv)
        spv->uninit(&spv);
    if (err < 0)
        encode_close(avctx);
    return err;
}

#define OFFSET(x) offsetof(ProresVulkanContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "mbs_per_slice", "macroblocks per slice", OFFSET(ctx.mbs_per_slice), AV_OPT_TYPE_INT, { .i64 = 8 }, 1, MAX_MBS_PER_SLICE, VE },
    { "profile", NULL, OFFSET(ctx.profile), AV_OPT_TYPE_INT, { .i64 = PRORES_PROFILE_AUTO }, PRORES_PROFILE_AUTO, PRORES_PROFILE_4444XQ, VE, .unit = "profile" },
    { "auto", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_AUTO }, 0, 0, VE, .unit = "profile" },
    { "proxy", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_PROXY }, 0, 0, VE, .unit = "profile" },
    { "lt", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_LT }, 0, 0, VE, .unit = "profile" },
    { "standard", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_STANDARD }, 0, 0, VE, .unit = "profile" },
    { "hq", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_HQ }, 0, 0, VE, .unit = "profile" },
    { "4444", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_4444 }, 0, 0, VE, .unit = "profile" },
    { "4444xq", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PRORES_PROFILE_4444XQ }, 0, 0, VE, .unit = "profile" },
    { "vendor", "vendor ID", OFFSET(ctx.vendor), AV_OPT_TYPE_STRING, { .str = "Lavc" }, 0, 0, VE },
    { "bits_per_mb", "desired bits per macroblock", OFFSET(ctx.bits_per_mb), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 8192, VE },
    { "quant_mat", "quantiser matrix", OFFSET(ctx.quant_sel), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, QUANT_MAT_DEFAULT, VE, .unit = "quant_mat" },
    { "auto", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = -1 }, 0, 0, VE, .unit = "quant_mat" },
    { "proxy", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = QUANT_MAT_PROXY }, 0, 0, VE, .unit = "quant_mat" },
    { "lt", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = QUANT_MAT_LT }, 0, 0, VE, .unit = "quant_mat" },
    { "standard", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = QUANT_MAT_STANDARD }, 0, 0, VE, .unit = "quant_mat" },
    { "hq", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = QUANT_MAT_HQ }, 0, 0, VE, .unit = "quant_mat" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = QUANT_MAT_DEFAULT }, 0, 0, VE, .unit = "quant_mat" },
    { "alpha_bits", "bits for alpha plane", OFFSET(ctx.alpha_bits), AV_OPT_TYPE_INT, { .i64 = 16 }, 0, 16, VE },
    { "async_depth", "internal parallelization depth", OFFSET(async_depth), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, INT_MAX, VE },
    { NULL }
};

static const AVClass proresenc_class = {
    .class_name = "ProRes Vulkan encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecHWConfigInternal *const prores_ks_hw_configs[] = {
    HW_CONFIG_ENCODER_FRAMES(VULKAN, VULKAN),
    HW_CONFIG_ENCODER_DEVICE(NONE, VULKAN),
    NULL,
};

static const enum AVPixelFormat prores_ks_vulkan_pix_fmts[] = {
    AV_PIX_FMT_VULKAN,
    AV_PIX_FMT_NONE,
};

const FFCodec ff_prores_ks_vulkan_encoder = {
    .p.name         = "prores_ks_vulkan",
    CODEC_LONG_NAME("Apple ProRes (iCodec Pro)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PRORES,
    .priv_data_size = sizeof(ProresVulkanContext),
    .init           = encode_init,
    .close          = encode_close,
    FF_CODEC_RECEIVE_PACKET_CB(&vulkan_encode_prores_receive_packet),
    .p.capabilities = AV_CODEC_CAP_DELAY |
                      AV_CODEC_CAP_HARDWARE |
                      AV_CODEC_CAP_ENCODER_FLUSH |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .p.pix_fmts     = prores_ks_vulkan_pix_fmts,
    .hw_configs     = prores_ks_hw_configs,
    .color_ranges   = AVCOL_RANGE_MPEG,
    .p.priv_class   = &proresenc_class,
    .p.profiles     = ff_prores_profiles,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_EOF_FLUSH,
};
