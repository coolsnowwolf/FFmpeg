/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "proresdec.h"
#include "vulkan_decode.h"
#include "hwaccel_internal.h"

#include "libavutil/mem.h"
#include "libavutil/vulkan_loader.h"
#include "libavutil/vulkan_spirv.h"

extern const char *ff_source_common_comp;
extern const char *ff_source_prores_reset_comp;
extern const char *ff_source_prores_vld_comp;
extern const char *ff_source_prores_idct_comp;

const FFVulkanDecodeDescriptor ff_vk_dec_prores_desc = {
    .codec_id    = AV_CODEC_ID_PRORES,
    .queue_flags = VK_QUEUE_COMPUTE_BIT,
};

typedef struct ProresVulkanDecodePicture {
    FFVulkanDecodePicture vp;

    AVBufferRef *slice_offset_buf;
    uint32_t slice_num;
    uint32_t bitstream_size;
} ProresVulkanDecodePicture;

typedef struct ProresVulkanShaderVariants {
    FFVkSPIRVShader shd_reset;
    FFVulkanPipeline pl_reset;

    FFVkSPIRVShader shd_vld;
    FFVulkanPipeline pl_vld;

    FFVkSPIRVShader shd_idct;
    FFVulkanPipeline pl_idct;
} ProresVulkanShaderVariants;

typedef struct ProresVulkanDecodeContext {
    ProresVulkanShaderVariants shaders[2];
    AVBufferPool *slice_offset_pool;
} ProresVulkanDecodeContext;

typedef struct ProresVkParameters {
    VkDeviceAddress slice_data;
    uint32_t bitstream_size;

    uint16_t width;
    uint16_t height;
    uint16_t mb_width;
    uint16_t mb_height;
    uint16_t slice_width;
    uint16_t slice_height;
    uint8_t  log2_slice_width;
    uint8_t  log2_chroma_w;
    uint8_t  depth;
    uint8_t  alpha_info;
    uint8_t  bottom_field;

    uint8_t  qmat_luma[64];
    uint8_t  qmat_chroma[64];
} ProresVkParameters;

static int vk_prores_start_frame(AVCodecContext *avctx,
                                 const uint8_t *buffer,
                                 uint32_t size)
{
    int err;
    ProresContext *pr = avctx->priv_data;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx = dec->shared_ctx;
    ProresVulkanDecodeContext *pv = ctx->sd_ctx;
    ProresVulkanDecodePicture *pp = pr->hwaccel_picture_private;

    pp->slice_num = 0;
    pp->bitstream_size = 0;

    err = ff_vk_get_pooled_buffer(&ctx->s, &pv->slice_offset_pool,
                                  &pp->slice_offset_buf,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  NULL, (pr->slice_count + 1) * sizeof(uint32_t),
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (err < 0)
        return err;

    return ff_vk_decode_prepare_frame_sdr(dec, pr->frame, &pp->vp, 1,
                                          FF_VK_REP_NATIVE, 0);
}

static int vk_prores_decode_slice(AVCodecContext *avctx,
                                  const uint8_t *data,
                                  uint32_t size)
{
    int err;
    ProresContext *pr = avctx->priv_data;
    ProresVulkanDecodePicture *pp = pr->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &pp->vp;
    FFVkBuffer *slice_offset = (FFVkBuffer *)pp->slice_offset_buf->data;

    AV_WN32(slice_offset->mapped_mem + pp->slice_num * sizeof(uint32_t),
            vp->slices_size);

    err = ff_vk_decode_add_slice(avctx, vp, data, size, 0, &pp->slice_num, NULL);
    if (err < 0)
        return err;

    AV_WN32(slice_offset->mapped_mem + pp->slice_num * sizeof(uint32_t),
            vp->slices_size);
    pp->bitstream_size = vp->slices_size;

    return 0;
}

static int vk_prores_end_frame(AVCodecContext *avctx)
{
    int err = 0;
    ProresContext *pr = avctx->priv_data;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx = dec->shared_ctx;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    ProresVulkanDecodeContext *pv = ctx->sd_ctx;
    ProresVulkanDecodePicture *pp = pr->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &pp->vp;
    FFVkBuffer *slice_data;
    FFVkBuffer *slice_offsets;
    FFVkExecContext *exec = NULL;
    VkImageView out_views[AV_NUM_DATA_POINTERS] = { 0 };
    VkImageMemoryBarrier2 img_bar[AV_NUM_DATA_POINTERS];
    VkBufferMemoryBarrier2 buf_bar[2];
    ProresVulkanShaderVariants *shaders;
    ProresVkParameters pd;
    int nb_img_bar = 0;
    int nb_buf_bar = 0;
    const AVPixFmtDescriptor *pix_desc;

    if (!pp->slice_num)
        return 0;

    pix_desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
    if (!pix_desc)
        return AVERROR(EINVAL);

    if (!vp->slices_buf || !pp->slice_offset_buf)
        return AVERROR_INVALIDDATA;

    slice_data = (FFVkBuffer *)vp->slices_buf->data;
    slice_offsets = (FFVkBuffer *)pp->slice_offset_buf->data;
    shaders = &pv->shaders[pr->frame_type != 0];

    pd = (ProresVkParameters) {
        .slice_data       = slice_data->address,
        .bitstream_size   = pp->bitstream_size,
        .width            = avctx->width,
        .height           = avctx->height,
        .mb_width         = pr->mb_width,
        .mb_height        = pr->mb_height,
        .slice_width      = pr->slice_count / pr->mb_height,
        .slice_height     = pr->mb_height,
        .log2_slice_width = av_log2(pr->slice_mb_width),
        .log2_chroma_w    = pix_desc->log2_chroma_w,
        .depth            = avctx->bits_per_raw_sample,
        .alpha_info       = pr->alpha_info,
        .bottom_field     = pr->first_field ^ (pr->frame_type == 1),
    };

    memcpy(pd.qmat_luma, pr->qmat_luma, sizeof(pd.qmat_luma));
    memcpy(pd.qmat_chroma, pr->qmat_chroma, sizeof(pd.qmat_chroma));

    exec = ff_vk_exec_get(&dec->exec_pool);
    ff_vk_exec_start(&ctx->s, exec);

    RET(ff_vk_exec_add_dep_frame(&ctx->s, exec, pr->frame,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    RET(ff_vk_exec_mirror_sem_value(&ctx->s, exec, &vp->sem, &vp->sem_value,
                                    pr->frame));
    RET(ff_vk_exec_add_dep_buf(&ctx->s, exec,
                               (AVBufferRef *[]){ vp->slices_buf, pp->slice_offset_buf },
                               2, 0));

    vp->slices_buf = NULL;
    pp->slice_offset_buf = NULL;

    RET(ff_vk_create_imageviews(&ctx->s, exec, out_views, pr->frame));

    ff_vk_frame_barrier(&ctx->s, exec, pr->frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers    = buf_bar,
        .bufferMemoryBarrierCount = nb_buf_bar,
        .pImageMemoryBarriers     = img_bar,
        .imageMemoryBarrierCount  = nb_img_bar,
    });
    nb_img_bar = nb_buf_bar = 0;

    ff_vk_update_descriptor_img_array(&ctx->s, &shaders->pl_reset, exec,
                                      pr->frame, out_views, 0, 0,
                                      VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    ff_vk_exec_bind_pipeline(&ctx->s, exec, &shaders->pl_reset);
    ff_vk_update_push_exec(&ctx->s, exec, &shaders->pl_reset,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);

    vk->CmdDispatch(exec->buf, pr->mb_width << 1, pr->mb_height << 1, 1);

    ff_vk_frame_barrier(&ctx->s, exec, pr->frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers    = buf_bar,
        .bufferMemoryBarrierCount = nb_buf_bar,
        .pImageMemoryBarriers     = img_bar,
        .imageMemoryBarrierCount  = nb_img_bar,
    });
    nb_img_bar = nb_buf_bar = 0;

    RET(ff_vk_set_descriptor_buffer(&ctx->s, &shaders->pl_vld, exec, 0, 0, 0,
                                    slice_offsets, 0,
                                    (pp->slice_num + 1) * sizeof(uint32_t),
                                    VK_FORMAT_UNDEFINED));
    ff_vk_update_descriptor_img_array(&ctx->s, &shaders->pl_vld, exec,
                                      pr->frame, out_views, 0, 1,
                                      VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    ff_vk_exec_bind_pipeline(&ctx->s, exec, &shaders->pl_vld);
    ff_vk_update_push_exec(&ctx->s, exec, &shaders->pl_vld,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);

    vk->CmdDispatch(exec->buf,
                    AV_CEIL_RSHIFT(pr->slice_count / pr->mb_height, 3),
                    AV_CEIL_RSHIFT(pr->mb_height, 3),
                    3 + !!pr->alpha_info);

    ff_vk_frame_barrier(&ctx->s, exec, pr->frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers    = buf_bar,
        .bufferMemoryBarrierCount = nb_buf_bar,
        .pImageMemoryBarriers     = img_bar,
        .imageMemoryBarrierCount  = nb_img_bar,
    });

    ff_vk_update_descriptor_img_array(&ctx->s, &shaders->pl_idct, exec,
                                      pr->frame, out_views, 0, 0,
                                      VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    ff_vk_exec_bind_pipeline(&ctx->s, exec, &shaders->pl_idct);
    ff_vk_update_push_exec(&ctx->s, exec, &shaders->pl_idct,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);

    vk->CmdDispatch(exec->buf, AV_CEIL_RSHIFT(pr->mb_width, 1), pr->mb_height, 3);

    RET(ff_vk_exec_submit(&ctx->s, exec));

fail:
    if (err < 0 && exec)
        ff_vk_exec_discard_deps(&ctx->s, exec);
    return err;
}

static int add_push_data(FFVulkanPipeline *pl, FFVkSPIRVShader *shd)
{
    GLSLC(0, layout(push_constant, scalar) uniform pushConstants { );
    GLSLC(1,    u8buf    slice_data;                               );
    GLSLC(1,    uint     bitstream_size;                           );
    GLSLC(0,                                                       );
    GLSLC(1,    uint16_t width;                                    );
    GLSLC(1,    uint16_t height;                                   );
    GLSLC(1,    uint16_t mb_width;                                 );
    GLSLC(1,    uint16_t mb_height;                                );
    GLSLC(1,    uint16_t slice_width;                              );
    GLSLC(1,    uint16_t slice_height;                             );
    GLSLC(1,    uint8_t  log2_slice_width;                         );
    GLSLC(1,    uint8_t  log2_chroma_w;                            );
    GLSLC(1,    uint8_t  depth;                                    );
    GLSLC(1,    uint8_t  alpha_info;                               );
    GLSLC(1,    uint8_t  bottom_field;                             );
    GLSLC(0,                                                       );
    GLSLC(1,    uint8_t  qmat_luma[64];                            );
    GLSLC(1,    uint8_t  qmat_chroma[64];                          );
    GLSLC(0, };                                                    );

    return ff_vk_add_push_constant(pl, 0, sizeof(ProresVkParameters),
                                   VK_SHADER_STAGE_COMPUTE_BIT);
}

static int init_shader(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                       FFVulkanDecodeShared *ctx, FFVkExecPool *pool,
                       FFVkSPIRVShader *shd, FFVulkanPipeline *pl,
                       const char *name,
                       FFVulkanDescriptorSetBinding *descs, int num_descs,
                       const char *source,
                       int local_size_x, int local_size_y, int local_size_z,
                       int interlaced)
{
    int err = 0;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;

    RET(ff_vk_shader_init(pl, shd, name, VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(shd, local_size_x, local_size_y, local_size_z);

    av_bprintf(&shd->src, "#define GET_BITS_SMEM\n");
    if (interlaced)
        av_bprintf(&shd->src, "#define INTERLACED\n");

    GLSLD(ff_source_common_comp);

    RET(add_push_data(pl, shd));
    RET(ff_vk_pipeline_descriptor_set_add(&ctx->s, pl, shd, descs, num_descs, 0, 0));

    GLSLD(source);

    RET(spv->compile_shader(spv, avctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(&ctx->s, shd, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&ctx->s, pl, shd));
    RET(ff_vk_exec_pipeline_register(&ctx->s, pool, pl));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static void vk_decode_prores_uninit(FFVulkanDecodeShared *ctx)
{
    int i;
    ProresVulkanDecodeContext *pv = ctx->sd_ctx;

    if (!pv)
        return;

    for (i = 0; i < FF_ARRAY_ELEMS(pv->shaders); i++) {
        ff_vk_pipeline_free(&ctx->s, &pv->shaders[i].pl_reset);
        ff_vk_shader_free(&ctx->s, &pv->shaders[i].shd_reset);

        ff_vk_pipeline_free(&ctx->s, &pv->shaders[i].pl_vld);
        ff_vk_shader_free(&ctx->s, &pv->shaders[i].shd_vld);

        ff_vk_pipeline_free(&ctx->s, &pv->shaders[i].pl_idct);
        ff_vk_shader_free(&ctx->s, &pv->shaders[i].shd_idct);
    }

    av_buffer_pool_uninit(&pv->slice_offset_pool);
    av_freep(&ctx->sd_ctx);
}

static int vk_decode_prores_init(AVCodecContext *avctx)
{
    int err = 0;
    int i;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx;
    ProresVulkanDecodeContext *pv;
    FFVkSPIRVCompiler *spv;
    FFVulkanDescriptorSetBinding *desc_set;
    AVHWFramesContext *out_frames_ctx;

    spv = ff_vk_spirv_init();
    if (!spv) {
        av_log(avctx, AV_LOG_ERROR, "Unable to initialize SPIR-V compiler!\n");
        return AVERROR_EXTERNAL;
    }

    err = ff_vk_decode_init(avctx);
    if (err < 0)
        goto fail;

    ctx = dec->shared_ctx;
    pv = av_mallocz(sizeof(*pv));
    if (!pv) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->sd_ctx = pv;
    ctx->sd_ctx_free = vk_decode_prores_uninit;
    out_frames_ctx = (AVHWFramesContext *)avctx->hw_frames_ctx->data;

    for (i = 0; i < FF_ARRAY_ELEMS(pv->shaders); i++) {
        desc_set = (FFVulkanDescriptorSetBinding []) {
            {
                .name       = "dst",
                .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .dimensions = 2,
                .mem_layout = ff_vk_shader_rep_fmt(out_frames_ctx->sw_format),
                .mem_quali  = "writeonly",
                .elems      = av_pix_fmt_count_planes(out_frames_ctx->sw_format),
                .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        RET(init_shader(avctx, spv, ctx, &dec->exec_pool,
                        &pv->shaders[i].shd_reset, &pv->shaders[i].pl_reset,
                        "prores_dec_reset", desc_set, 1,
                        ff_source_prores_reset_comp, 8, 8, 1, i));

        desc_set = (FFVulkanDescriptorSetBinding []) {
            {
                .name        = "slice_offsets_buf",
                .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
                .mem_layout  = "scalar",
                .mem_quali   = "readonly",
                .buf_content = "uint32_t slice_offsets[];",
            },
            {
                .name       = "dst",
                .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .dimensions = 2,
                .mem_layout = ff_vk_shader_rep_fmt(out_frames_ctx->sw_format),
                .mem_quali  = "writeonly",
                .elems      = av_pix_fmt_count_planes(out_frames_ctx->sw_format),
                .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        RET(init_shader(avctx, spv, ctx, &dec->exec_pool,
                        &pv->shaders[i].shd_vld, &pv->shaders[i].pl_vld,
                        "prores_dec_vld", desc_set, 2,
                        ff_source_prores_vld_comp, 8, 8, 1, i));

        desc_set = (FFVulkanDescriptorSetBinding []) {
            {
                .name       = "dst",
                .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .dimensions = 2,
                .mem_layout = ff_vk_shader_rep_fmt(out_frames_ctx->sw_format),
                .elems      = av_pix_fmt_count_planes(out_frames_ctx->sw_format),
                .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        RET(init_shader(avctx, spv, ctx, &dec->exec_pool,
                        &pv->shaders[i].shd_idct, &pv->shaders[i].pl_idct,
                        "prores_dec_idct", desc_set, 1,
                        ff_source_prores_idct_comp, 32, 2, 1, i));
    }

fail:
    if (spv)
        spv->uninit(&spv);
    return err;
}

static void vk_prores_free_frame_priv(FFRefStructOpaque hwctx, void *data)
{
    AVHWDeviceContext *dev_ctx = hwctx.nc;
    ProresVulkanDecodePicture *pp = data;

    ff_vk_decode_free_frame(dev_ctx, &pp->vp);
}

const FFHWAccel ff_prores_vulkan_hwaccel = {
    .p.name                = "prores_vulkan",
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_PRORES,
    .p.pix_fmt             = AV_PIX_FMT_VULKAN,
    .start_frame           = &vk_prores_start_frame,
    .decode_slice          = &vk_prores_decode_slice,
    .end_frame             = &vk_prores_end_frame,
    .free_frame_priv       = &vk_prores_free_frame_priv,
    .frame_priv_data_size  = sizeof(ProresVulkanDecodePicture),
    .init                  = &vk_decode_prores_init,
    .update_thread_context = &ff_vk_update_thread_context,
    .decode_params         = &ff_vk_params_invalidate,
    .flush                 = &ff_vk_decode_flush,
    .uninit                = &ff_vk_decode_uninit,
    .frame_params          = &ff_vk_frame_params,
    .priv_data_size        = sizeof(FFVulkanDecodeContext),
    .caps_internal         = HWACCEL_CAP_ASYNC_SAFE | HWACCEL_CAP_THREAD_SAFE,
};
