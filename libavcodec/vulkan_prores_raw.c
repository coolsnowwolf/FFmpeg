/*
 * Copyright (c) 2025 Lynne <dev@lynne.ee>
 *
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

#include <math.h>

#include "vulkan_decode.h"
#include "hwaccel_internal.h"

#include "prores_raw.h"
#include "libavutil/mem.h"
#include "libavutil/vulkan_spirv.h"

extern const char *ff_source_common_comp;
extern const char *ff_source_prores_raw_comp;

const FFVulkanDecodeDescriptor ff_vk_dec_prores_raw_desc = {
    .codec_id         = AV_CODEC_ID_PRORES_RAW,
    .decode_extension = FF_VK_EXT_PUSH_DESCRIPTOR,
    .queue_flags      = VK_QUEUE_COMPUTE_BIT,
    .decode_op        = 0,
};

typedef struct ProResRAWVulkanDecodePicture {
    FFVulkanDecodePicture vp;

    AVBufferRef *tile_data;
    uint32_t nb_tiles;
} ProResRAWVulkanDecodePicture;

typedef struct ProResRAWVulkanDecodeContext {
    FFVkSPIRVShader shd[2];
    FFVulkanPipeline pl[2];

    AVBufferPool *tile_data_pool;

    FFVkBuffer uniform_buf;
} ProResRAWVulkanDecodeContext;

typedef struct DecodePushData {
    VkDeviceAddress tile_data;
    VkDeviceAddress pkt_data;
    uint32_t frame_size[2];
    uint32_t tile_size[2];
    uint8_t  qmat[64];
} DecodePushData;

typedef struct TileData {
    int32_t pos[2];
    uint32_t offset;
    uint32_t size;
} TileData;

static int create_output_view(FFVulkanDecodeShared *ctx, AVFrame *frame,
                              FFVulkanDecodePicture *vp)
{
    VkResult ret;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    AVHWFramesContext *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVVulkanFramesContext *vkfc = frames->hwctx;
    AVVkFrame *vkf = (AVVkFrame *)frame->data[0];
    VkImageViewUsageCreateInfo usage_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = vkfc->usage,
    };
    VkImageViewCreateInfo view_create_info = {
        .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext      = &usage_info,
        .image      = vkf->img[0],
        .viewType   = VK_IMAGE_VIEW_TYPE_2D,
        .format     = VK_FORMAT_R16_UNORM,
        .components = ff_comp_identity_map,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    vp->slices_size = 0;
    vp->dpb_frame = NULL;
    vp->img_view_ref = VK_NULL_HANDLE;
    vp->img_view_out = VK_NULL_HANDLE;
    vp->img_view_dest = VK_NULL_HANDLE;
    vp->destroy_image_view = vk->DestroyImageView;
    vp->wait_semaphores = vk->WaitSemaphores;

    ret = vk->CreateImageView(ctx->s.hwctx->act_dev, &view_create_info,
                              ctx->s.hwctx->alloc, &vp->img_view_out);
    if (ret != VK_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create imageview: %s\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    vp->img_view_ref = vp->img_view_out;
    vp->img_view_dest = vp->img_view_out;
    vp->img_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    vp->img_aspect_ref = VK_IMAGE_ASPECT_COLOR_BIT;

    return 0;
}

static int vk_prores_raw_start_frame(AVCodecContext *avctx,
                                     const uint8_t *buffer,
                                     uint32_t size)
{
    int err;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx = dec->shared_ctx;
    ProResRAWVulkanDecodeContext *prv = ctx->sd_ctx;
    ProResRAWContext *prr = avctx->priv_data;
    ProResRAWVulkanDecodePicture *pp = prr->hwaccel_picture_private;

    pp->nb_tiles = 0;

    err = ff_vk_get_pooled_buffer(&ctx->s, &prv->tile_data_pool,
                                  &pp->tile_data,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  NULL, prr->nb_tiles*sizeof(TileData),
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (err < 0)
        return err;

    return create_output_view(ctx, prr->frame, &pp->vp);
}

static int vk_prores_raw_decode_slice(AVCodecContext *avctx,
                                      const uint8_t *data,
                                      uint32_t size)
{
    int err;
    ProResRAWContext *prr = avctx->priv_data;
    ProResRAWVulkanDecodePicture *pp = prr->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &pp->vp;
    FFVkBuffer *tile_data_buf = (FFVkBuffer *)pp->tile_data->data;
    TileData *td = (TileData *)tile_data_buf->mapped_mem;

    td[pp->nb_tiles].pos[0] = prr->tiles[pp->nb_tiles].x;
    td[pp->nb_tiles].pos[1] = prr->tiles[pp->nb_tiles].y;
    td[pp->nb_tiles].size = size;
    td[pp->nb_tiles].offset = vp->slices_size;

    err = ff_vk_decode_add_slice(avctx, vp, data, size, 0, &pp->nb_tiles, NULL);
    if (err < 0)
        return err;

    return 0;
}

static int vk_prores_raw_end_frame(AVCodecContext *avctx)
{
    int err = 0;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx = dec->shared_ctx;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    ProResRAWContext *prr = avctx->priv_data;
    ProResRAWVulkanDecodeContext *prv = ctx->sd_ctx;
    ProResRAWVulkanDecodePicture *pp = prr->hwaccel_picture_private;
    FFVulkanDecodePicture *vp = &pp->vp;
    FFVkBuffer *slices_buf;
    FFVkBuffer *tile_data;
    FFVkExecContext *exec = NULL;
    VkImageView out_views[AV_NUM_DATA_POINTERS] = { vp->img_view_out };
    VkImageMemoryBarrier2 img_bar[8];
    int nb_img_bar = 0;
    DecodePushData pd_decode;
    FFVulkanPipeline *pl;

    if (!vp->slices_buf || !pp->tile_data)
        return AVERROR_INVALIDDATA;

    slices_buf = (FFVkBuffer *)vp->slices_buf->data;
    tile_data = (FFVkBuffer *)pp->tile_data->data;
    pl = &prv->pl[prr->version];

    exec = ff_vk_exec_get(&dec->exec_pool);
    ff_vk_exec_start(&ctx->s, exec);

    RET(ff_vk_exec_add_dep_frame(&ctx->s, exec, prr->frame,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    RET(ff_vk_exec_add_dep_buf(&ctx->s, exec, &pp->tile_data, 1, 0));
    pp->tile_data = NULL;
    RET(ff_vk_exec_add_dep_buf(&ctx->s, exec, &vp->slices_buf, 1, 0));
    vp->slices_buf = NULL;

    ff_vk_update_descriptor_img_array(&ctx->s, pl, exec, prr->frame, out_views,
                                      0, 0, VK_IMAGE_LAYOUT_GENERAL,
                                      VK_NULL_HANDLE);
    ff_vk_exec_bind_pipeline(&ctx->s, exec, pl);

    pd_decode.tile_data = tile_data->address;
    pd_decode.pkt_data = slices_buf->address;
    pd_decode.frame_size[0] = avctx->width;
    pd_decode.frame_size[1] = avctx->height;
    pd_decode.tile_size[0] = prr->tw;
    pd_decode.tile_size[1] = prr->th;
    memcpy(pd_decode.qmat, prr->qmat, sizeof(pd_decode.qmat));

    ff_vk_update_push_exec(&ctx->s, exec, pl, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pd_decode), &pd_decode);

    ff_vk_frame_barrier(&ctx->s, exec, prr->frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pImageMemoryBarriers = img_bar,
        .imageMemoryBarrierCount = nb_img_bar,
    });

    vk->CmdDispatch(exec->buf, prr->nb_tw, prr->nb_th, 1);

    return ff_vk_exec_submit(&ctx->s, exec);

fail:
    if (exec)
        ff_vk_exec_discard_deps(&ctx->s, exec);
    return err;
}

static int init_decode_shader(AVCodecContext *avctx, FFVkSPIRVCompiler *spv,
                              ProResRAWVulkanDecodeContext *prv, int version)
{
    int err = 0;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx = dec->shared_ctx;
    FFVkSPIRVShader *shd = &prv->shd[version];
    FFVulkanPipeline *pl = &prv->pl[version];
    FFVulkanDescriptorSetBinding *desc_set;
    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    int parallel_rows = 1;

    if (ctx->s.props.properties.limits.maxComputeWorkGroupInvocations < 512 ||
        ctx->s.props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        parallel_rows = 0;

    RET(ff_vk_shader_init(pl, shd, "prores_raw",
                          VK_SHADER_STAGE_COMPUTE_BIT, 0));
    ff_vk_shader_set_compute_sizes(shd,
                                   parallel_rows ? 8 : 1,
                                   version == 0 ? 8 : 16,
                                   4);

    GLSLC(0, #extension GL_EXT_scalar_block_layout : require                  );
    GLSLC(0, #extension GL_EXT_shader_explicit_arithmetic_types_int8 : require);
    GLSLC(0, #extension GL_EXT_shader_explicit_arithmetic_types_int16 : require);
    GLSLC(0, #extension GL_EXT_shader_8bit_storage : require                  );
    GLSLC(0, #extension GL_EXT_shader_16bit_storage : require                 );
    GLSLC(0, #extension GL_EXT_null_initializer : require                     );

    if (parallel_rows)
        GLSLC(0, #define PARALLEL_ROWS                                        );

    GLSLD(ff_source_common_comp);

    GLSLC(0, layout(buffer_reference, buffer_reference_align = 16) buffer TileData { );
    GLSLC(1,     ivec2 pos;                                                        );
    GLSLC(1,     uint offset;                                                      );
    GLSLC(1,     uint size;                                                        );
    GLSLC(0, };                                                                    );
    GLSLC(0,                                                                       );
    GLSLC(0, layout(push_constant, scalar) uniform pushConstants {                 );
    GLSLC(1,     TileData tile_data;                                                );
    GLSLC(1,     u8buf pkt_data;                                                    );
    GLSLC(1,     uvec2 frame_size;                                                  );
    GLSLC(1,     uvec2 tile_size;                                                   );
    GLSLC(1,     uint8_t qmat[64];                                                  );
    GLSLC(0, };                                                                    );
    GLSLC(0,                                                                       );

    ff_vk_add_push_constant(pl, 0, sizeof(DecodePushData),
                            VK_SHADER_STAGE_COMPUTE_BIT);

    desc_set = (FFVulkanDescriptorSetBinding []) {
        {
            .name       = "dst",
            .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .mem_layout = "r16",
            .mem_quali  = "writeonly",
            .dimensions = 2,
            .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&ctx->s, pl, shd, desc_set, 1, 0, 0));

    desc_set = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "dct_scale_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "float idct_8x8_scales[64];",
        },
        {
            .name        = "scan_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "uint8_t scan[64];",
        },
        {
            .name        = "dc_cb_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "uint8_t dc_cb[13];",
        },
        {
            .name        = "ac_cb_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t ac_cb[95];",
        },
        {
            .name        = "rn_cb_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t rn_cb[28];",
        },
        {
            .name        = "ln_cb_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t ln_cb[15];",
        },
    };
    RET(ff_vk_pipeline_descriptor_set_add(&ctx->s, pl, shd, desc_set, 6, 1, 0));

    GLSLD(ff_source_prores_raw_comp);

    RET(spv->compile_shader(spv, avctx, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_create(&ctx->s, shd, spv_data, spv_len, "main"));
    RET(ff_vk_init_compute_pipeline(&ctx->s, pl, shd));
    RET(ff_vk_exec_pipeline_register(&ctx->s, &dec->exec_pool, pl));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);
    return err;
}

static void vk_decode_prores_raw_uninit(FFVulkanDecodeShared *ctx)
{
    ProResRAWVulkanDecodeContext *prv = ctx->sd_ctx;

    if (!prv)
        return;

    for (int i = 0; i < 2; i++) {
        ff_vk_pipeline_free(&ctx->s, &prv->pl[i]);
        ff_vk_shader_free(&ctx->s, &prv->shd[i]);
    }

    ff_vk_free_buf(&ctx->s, &prv->uniform_buf);
    av_buffer_pool_uninit(&prv->tile_data_pool);
    av_freep(&ctx->sd_ctx);
}

static int vk_decode_prores_raw_init(AVCodecContext *avctx)
{
    int err = 0;
    const double pi = 3.14159265358979323846;
    ProResRAWContext *prr = avctx->priv_data;
    FFVulkanDecodeContext *dec = avctx->internal->hwaccel_priv_data;
    FFVulkanDecodeShared *ctx;
    ProResRAWVulkanDecodeContext *prv;
    FFVkSPIRVCompiler *spv;
    uint8_t *uniform_buf;
    size_t cb_size[4] = {
        13*sizeof(uint8_t),
        95*sizeof(int16_t),
        28*sizeof(int16_t),
        15*sizeof(int16_t),
    };
    size_t cb_offset[4];
    size_t ua;
    double idct_8_scales[8];

    spv = ff_vk_spirv_init();
    if (!spv) {
        av_log(avctx, AV_LOG_ERROR, "Unable to initialize SPIR-V compiler!\n");
        return AVERROR_EXTERNAL;
    }

    err = ff_vk_decode_init(avctx);
    if (err < 0)
        goto fail;

    ctx = dec->shared_ctx;
    prv = av_mallocz(sizeof(*prv));
    if (!prv) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->sd_ctx = prv;
    ctx->sd_ctx_free = vk_decode_prores_raw_uninit;

    RET(init_decode_shader(avctx, spv, prv, 0));
    RET(init_decode_shader(avctx, spv, prv, 1));

    ua = ctx->s.props.properties.limits.minUniformBufferOffsetAlignment;
    cb_offset[0] = 64*sizeof(float) + 64*sizeof(uint8_t);
    cb_offset[1] = cb_offset[0] + FFALIGN(cb_size[0], ua);
    cb_offset[2] = cb_offset[1] + FFALIGN(cb_size[1], ua);
    cb_offset[3] = cb_offset[2] + FFALIGN(cb_size[2], ua);

    RET(ff_vk_create_buf(&ctx->s, &prv->uniform_buf,
                         cb_offset[3] + FFALIGN(cb_size[3], ua),
                         NULL, NULL,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    RET(ff_vk_map_buffer(&ctx->s, &prv->uniform_buf, &uniform_buf, 0));

    for (int i = 0; i < 8; i++)
        idct_8_scales[i] = cos(i*pi/16.0) / 2.0;

    for (int i = 0; i < 64; i++)
        ((float *)uniform_buf)[i] = (float)(idct_8_scales[i >> 3] *
                                            idct_8_scales[i & 7]);

    for (int i = 0; i < 64; i++)
        uniform_buf[64*sizeof(float) + prr->scan[i]] = i;

    memcpy(uniform_buf + cb_offset[0], ff_prores_raw_dc_cb, sizeof(ff_prores_raw_dc_cb));
    memcpy(uniform_buf + cb_offset[1], ff_prores_raw_ac_cb, sizeof(ff_prores_raw_ac_cb));
    memcpy(uniform_buf + cb_offset[2], ff_prores_raw_rn_cb, sizeof(ff_prores_raw_rn_cb));
    memcpy(uniform_buf + cb_offset[3], ff_prores_raw_ln_cb, sizeof(ff_prores_raw_ln_cb));

    RET(ff_vk_unmap_buffer(&ctx->s, &prv->uniform_buf, 1));

    for (int i = 0; i < 2; i++) {
        RET(ff_vk_set_descriptor_buffer(&ctx->s, &prv->pl[i], NULL, 1, 0, 0,
                                        &prv->uniform_buf,
                                        0, 64*sizeof(float),
                                        VK_FORMAT_UNDEFINED));
        RET(ff_vk_set_descriptor_buffer(&ctx->s, &prv->pl[i], NULL, 1, 1, 0,
                                        &prv->uniform_buf,
                                        64*sizeof(float), 64*sizeof(uint8_t),
                                        VK_FORMAT_UNDEFINED));
        for (int j = 0; j < 4; j++)
            RET(ff_vk_set_descriptor_buffer(&ctx->s, &prv->pl[i], NULL, 1, 2 + j, 0,
                                            &prv->uniform_buf,
                                            cb_offset[j], cb_size[j],
                                            VK_FORMAT_UNDEFINED));
    }

    spv->uninit(&spv);
    return 0;

fail:
    if (spv)
        spv->uninit(&spv);
    ff_vk_decode_uninit(avctx);
    return err;
}

static void vk_prores_raw_free_frame_priv(FFRefStructOpaque hwctx, void *data)
{
    AVHWDeviceContext *dev_ctx = hwctx.nc;
    ProResRAWVulkanDecodePicture *pp = data;

    ff_vk_decode_free_frame(dev_ctx, &pp->vp);
}

const FFHWAccel ff_prores_raw_vulkan_hwaccel = {
    .p.name                = "prores_raw_vulkan",
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_PRORES_RAW,
    .p.pix_fmt             = AV_PIX_FMT_VULKAN,
    .start_frame           = &vk_prores_raw_start_frame,
    .decode_slice          = &vk_prores_raw_decode_slice,
    .end_frame             = &vk_prores_raw_end_frame,
    .free_frame_priv       = &vk_prores_raw_free_frame_priv,
    .frame_priv_data_size  = sizeof(ProResRAWVulkanDecodePicture),
    .init                  = &vk_decode_prores_raw_init,
    .decode_params         = &ff_vk_params_invalidate,
    .flush                 = &ff_vk_decode_flush,
    .uninit                = &ff_vk_decode_uninit,
    .frame_params          = &ff_vk_frame_params,
    .priv_data_size        = sizeof(FFVulkanDecodeContext),
    .caps_internal         = HWACCEL_CAP_ASYNC_SAFE,
};