/*
 * Vulkan
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Chia-I Wu <olv@lunarg.com>
 */

#include "buf.h"
#include "img.h"
#include "mem.h"
#include "state.h"
#include "cmd_priv.h"
#include "fb.h"

static VkResult cmd_meta_create_buf_view(struct intel_cmd *cmd,
                                           VkBuffer buf,
                                           VkDeviceSize range,
                                           VkFormat format,
                                           struct intel_buf_view **view)
{
    VkBufferViewCreateInfo info;
    VkDeviceSize stride;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    info.buffer = buf;
    info.viewType = VK_BUFFER_VIEW_TYPE_FORMATTED;
    info.format = format;
    info.range = range;

    /*
     * We do not rely on the hardware to avoid out-of-bound access.  But we do
     * not want the hardware to ignore the last element either.
     */
    stride = icd_format_get_size(format);
    if (info.range % stride)
        info.range += stride - (info.range % stride);

    return intel_buf_view_create(cmd->dev, &info, view);
}

static void cmd_meta_set_src_for_buf(struct intel_cmd *cmd,
                                     const struct intel_buf *buf,
                                     VkFormat format,
                                     struct intel_cmd_meta *meta)
{
    struct intel_buf_view *view;
    VkResult res;

    res = cmd_meta_create_buf_view(cmd, (VkBuffer) buf,
            buf->size, format, &view);
    if (res != VK_SUCCESS) {
        cmd_fail(cmd, res);
        return;
    }

    meta->src.valid = true;

    memcpy(meta->src.surface, view->cmd,
            sizeof(view->cmd[0]) * view->cmd_len);
    meta->src.surface_len = view->cmd_len;

    intel_buf_view_destroy(view);

    meta->src.reloc_target = (intptr_t) buf->obj.mem->bo;
    meta->src.reloc_offset = 0;
    meta->src.reloc_flags = 0;
}

static void cmd_meta_set_dst_for_buf(struct intel_cmd *cmd,
                                     const struct intel_buf *buf,
                                     VkFormat format,
                                     struct intel_cmd_meta *meta)
{
    struct intel_buf_view *view;
    VkResult res;

    res = cmd_meta_create_buf_view(cmd, (VkBuffer) buf,
            buf->size, format, &view);
    if (res != VK_SUCCESS) {
        cmd_fail(cmd, res);
        return;
    }

    meta->dst.valid = true;

    memcpy(meta->dst.surface, view->cmd,
            sizeof(view->cmd[0]) * view->cmd_len);
    meta->dst.surface_len = view->cmd_len;

    intel_buf_view_destroy(view);

    meta->dst.reloc_target = (intptr_t) buf->obj.mem->bo;
    meta->dst.reloc_offset = 0;
    meta->dst.reloc_flags = INTEL_RELOC_WRITE;
}

static void cmd_meta_set_src_for_img(struct intel_cmd *cmd,
                                     const struct intel_img *img,
                                     VkFormat format,
                                     VkImageAspect aspect,
                                     struct intel_cmd_meta *meta)
{
    VkImageViewCreateInfo info;
    struct intel_img_view *view;
    VkResult ret;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = (VkImage) img;

    if (img->array_size == 1) {
        switch (img->type) {
        case VK_IMAGE_TYPE_1D:
            info.viewType = VK_IMAGE_VIEW_TYPE_1D;
            break;
        case VK_IMAGE_TYPE_2D:
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            break;
        default:
            break;
        }
    } else {
        switch (img->type) {
        case VK_IMAGE_TYPE_1D:
            info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            break;
        case VK_IMAGE_TYPE_2D:
            info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            break;
        case VK_IMAGE_TYPE_3D:
            info.viewType = VK_IMAGE_VIEW_TYPE_3D;
            break;
        default:
            break;
        }
    }

    info.format = format;
    info.channels.r = VK_CHANNEL_SWIZZLE_R;
    info.channels.g = VK_CHANNEL_SWIZZLE_G;
    info.channels.b = VK_CHANNEL_SWIZZLE_B;
    info.channels.a = VK_CHANNEL_SWIZZLE_A;
    info.subresourceRange.aspect = aspect;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.mipLevels = VK_LAST_MIP_LEVEL;
    info.subresourceRange.baseArraySlice = 0;
    info.subresourceRange.arraySize = VK_LAST_ARRAY_SLICE;

    ret = intel_img_view_create(cmd->dev, &info, &view);
    if (ret != VK_SUCCESS) {
        cmd_fail(cmd, ret);
        return;
    }

    meta->src.valid = true;

    memcpy(meta->src.surface, view->cmd,
            sizeof(view->cmd[0]) * view->cmd_len);
    meta->src.surface_len = view->cmd_len;

    meta->src.reloc_target = (intptr_t) img->obj.mem->bo;
    meta->src.reloc_offset = 0;
    meta->src.reloc_flags = 0;

    intel_img_view_destroy(view);
}

static void cmd_meta_adjust_compressed_dst(struct intel_cmd *cmd,
                                           const struct intel_img *img,
                                           struct intel_cmd_meta *meta)
{
    int32_t w, h, layer;
    unsigned x_offset, y_offset;

    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        w = GEN_EXTRACT(meta->dst.surface[2], GEN7_SURFACE_DW2_WIDTH);
        h = GEN_EXTRACT(meta->dst.surface[2], GEN7_SURFACE_DW2_HEIGHT);
        layer = GEN_EXTRACT(meta->dst.surface[4],
                GEN7_SURFACE_DW4_MIN_ARRAY_ELEMENT);
    } else {
        w = GEN_EXTRACT(meta->dst.surface[2], GEN6_SURFACE_DW2_WIDTH);
        h = GEN_EXTRACT(meta->dst.surface[2], GEN6_SURFACE_DW2_HEIGHT);
        layer = GEN_EXTRACT(meta->dst.surface[4],
                GEN6_SURFACE_DW4_MIN_ARRAY_ELEMENT);
    }

    /* note that the width/height fields have the real values minus 1 */
    w = (w + img->layout.block_width) / img->layout.block_width - 1;
    h = (h + img->layout.block_height) / img->layout.block_height - 1;

    /* adjust width and height */
    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        meta->dst.surface[2] &= ~(GEN7_SURFACE_DW2_WIDTH__MASK |
                                  GEN7_SURFACE_DW2_HEIGHT__MASK);
        meta->dst.surface[2] |= GEN_SHIFT32(w, GEN7_SURFACE_DW2_WIDTH) |
                                GEN_SHIFT32(h, GEN7_SURFACE_DW2_HEIGHT);
    } else {
        meta->dst.surface[2] &= ~(GEN6_SURFACE_DW2_WIDTH__MASK |
                                  GEN6_SURFACE_DW2_HEIGHT__MASK);
        meta->dst.surface[2] |= GEN_SHIFT32(w, GEN6_SURFACE_DW2_WIDTH) |
                                GEN_SHIFT32(h, GEN6_SURFACE_DW2_HEIGHT);
    }

    if (!layer)
        return;

    meta->dst.reloc_offset = intel_layout_get_slice_tile_offset(&img->layout,
            0, layer, &x_offset, &y_offset);

    /*
     * The lower 2 bits (or 1 bit for Y) are missing.  This may be a problem
     * for small images (16x16 or smaller).  We will need to adjust the
     * drawing rectangle instead.
     */
    x_offset = (x_offset / img->layout.block_width) >> 2;
    y_offset = (y_offset / img->layout.block_height) >> 1;

    /* adjust min array element and X/Y offsets */
    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        meta->dst.surface[4] &= ~GEN7_SURFACE_DW4_MIN_ARRAY_ELEMENT__MASK;
        meta->dst.surface[5] |= GEN_SHIFT32(x_offset, GEN7_SURFACE_DW5_X_OFFSET) |
                                GEN_SHIFT32(y_offset, GEN7_SURFACE_DW5_Y_OFFSET);
    } else {
        meta->dst.surface[4] &= ~GEN6_SURFACE_DW4_MIN_ARRAY_ELEMENT__MASK;
        meta->dst.surface[5] |= GEN_SHIFT32(x_offset, GEN6_SURFACE_DW5_X_OFFSET) |
                                GEN_SHIFT32(y_offset, GEN6_SURFACE_DW5_Y_OFFSET);
    }
}

static void cmd_meta_set_dst_for_img(struct intel_cmd *cmd,
                                     const struct intel_img *img,
                                     VkFormat format,
                                     uint32_t lod, uint32_t layer,
                                     struct intel_cmd_meta *meta)
{
    VkColorAttachmentViewCreateInfo info;
    struct intel_att_view *view;
    VkResult ret;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COLOR_ATTACHMENT_VIEW_CREATE_INFO;
    info.image = (VkImage) img;
    info.format = format;
    info.mipLevel = lod;
    info.baseArraySlice = layer;
    info.arraySize = 1;

    ret = intel_att_view_create_for_color(cmd->dev, &info, &view);
    if (ret != VK_SUCCESS) {
        cmd_fail(cmd, ret);
        return;
    }

    meta->dst.valid = true;

    memcpy(meta->dst.surface, view->att_cmd,
            sizeof(view->att_cmd[0]) * view->cmd_len);
    meta->dst.surface_len = view->cmd_len;

    meta->dst.reloc_target = (intptr_t) img->obj.mem->bo;
    meta->dst.reloc_offset = 0;
    meta->dst.reloc_flags = INTEL_RELOC_WRITE;

    if (icd_format_is_compressed(img->layout.format))
        cmd_meta_adjust_compressed_dst(cmd, img, meta);

    intel_att_view_destroy(view);
}

static void cmd_meta_set_src_for_writer(struct intel_cmd *cmd,
                                        enum intel_cmd_writer_type writer,
                                        VkDeviceSize size,
                                        VkFormat format,
                                        struct intel_cmd_meta *meta)
{
    struct intel_buf_view *view;
    VkResult res;

    res = cmd_meta_create_buf_view(cmd, (VkBuffer) VK_NULL_HANDLE,
            size, format, &view);
    if (res != VK_SUCCESS) {
        cmd_fail(cmd, res);
        return;
    }

    meta->src.valid = true;

    memcpy(meta->src.surface, view->cmd,
            sizeof(view->cmd[0]) * view->cmd_len);
    meta->src.surface_len = view->cmd_len;

    intel_buf_view_destroy(view);

    meta->src.reloc_target = (intptr_t) writer;
    meta->src.reloc_offset = 0;
    meta->src.reloc_flags = INTEL_CMD_RELOC_TARGET_IS_WRITER;
}

static void cmd_meta_set_ds_view(struct intel_cmd *cmd,
                                 const struct intel_img *img,
                                 uint32_t lod, uint32_t layer,
                                 struct intel_cmd_meta *meta)
{
    VkDepthStencilViewCreateInfo info;
    struct intel_att_view *view;
    VkResult ret;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DEPTH_STENCIL_VIEW_CREATE_INFO;
    info.image = (VkImage) img;
    info.mipLevel = lod;
    info.baseArraySlice = layer;
    info.arraySize = 1;

    ret = intel_att_view_create_for_ds(cmd->dev, &info, &view);
    if (ret != VK_SUCCESS) {
        cmd_fail(cmd, ret);
        return;
    }

    meta->ds.view = view;
}

static void cmd_meta_set_ds_state(struct intel_cmd *cmd,
                                  VkImageAspect aspect,
                                  uint32_t stencil_ref,
                                  struct intel_cmd_meta *meta)
{
    meta->ds.stencil_ref = stencil_ref;
    meta->ds.aspect = aspect;
}

static enum intel_dev_meta_shader get_shader_id(const struct intel_dev *dev,
                                                const struct intel_img *img,
                                                bool copy_array)
{
    enum intel_dev_meta_shader shader_id;

    switch (img->type) {
    case VK_IMAGE_TYPE_1D:
        shader_id = (copy_array) ?
            INTEL_DEV_META_FS_COPY_1D_ARRAY : INTEL_DEV_META_FS_COPY_1D;
        break;
    case VK_IMAGE_TYPE_2D:
        shader_id = (img->samples > 1) ? INTEL_DEV_META_FS_COPY_2D_MS :
                    (copy_array) ?  INTEL_DEV_META_FS_COPY_2D_ARRAY :
                    INTEL_DEV_META_FS_COPY_2D;
        break;
    case VK_IMAGE_TYPE_3D:
    default:
        shader_id = INTEL_DEV_META_FS_COPY_2D_ARRAY;
        break;
    }

    return shader_id;
}

static bool cmd_meta_mem_dword_aligned(const struct intel_cmd *cmd,
                                       VkDeviceSize src_offset,
                                       VkDeviceSize dst_offset,
                                       VkDeviceSize size)
{
    return !((src_offset | dst_offset | size) & 0x3);
}

static VkFormat cmd_meta_img_raw_format(const struct intel_cmd *cmd,
                                          VkFormat format)
{
    switch (icd_format_get_size(format)) {
    case 1:
        format = VK_FORMAT_R8_UINT;
        break;
    case 2:
        format = VK_FORMAT_R16_UINT;
        break;
    case 4:
        format = VK_FORMAT_R32_UINT;
        break;
    case 8:
        format = VK_FORMAT_R32G32_UINT;
        break;
    case 16:
        format = VK_FORMAT_R32G32B32A32_UINT;
        break;
    default:
        assert(!"unsupported image format for raw blit op");
        format = VK_FORMAT_UNDEFINED;
        break;
    }

    return format;
}

ICD_EXPORT void VKAPI vkCmdCopyBuffer(
    VkCmdBuffer                 cmdBuffer,
    VkBuffer                    srcBuffer,
    VkBuffer                    destBuffer,
    uint32_t                    regionCount,
    const VkBufferCopy*         pRegions)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_buf *src = intel_buf(srcBuffer);
    struct intel_buf *dst = intel_buf(destBuffer);
    struct intel_cmd_meta meta;
    VkFormat format;
    uint32_t i;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_VS_POINTS;

    meta.height = 1;
    meta.samples = 1;

    format = VK_FORMAT_UNDEFINED;

    for (i = 0; i < regionCount; i++) {
        const VkBufferCopy *region = &pRegions[i];
        VkFormat fmt;

        meta.src.x = region->srcOffset;
        meta.dst.x = region->destOffset;
        meta.width = region->copySize;

        if (cmd_meta_mem_dword_aligned(cmd, region->srcOffset,
                    region->destOffset, region->copySize)) {
            meta.shader_id = INTEL_DEV_META_VS_COPY_MEM;
            meta.src.x /= 4;
            meta.dst.x /= 4;
            meta.width /= 4;

            /*
             * INTEL_DEV_META_VS_COPY_MEM is untyped but expects the stride to
             * be 16
             */
            fmt = VK_FORMAT_R32G32B32A32_UINT;
        } else {
            if (cmd_gen(cmd) == INTEL_GEN(6)) {
                intel_dev_log(cmd->dev, VK_DBG_REPORT_ERROR_BIT,
                        &cmd->obj.base, 0, 0,
                        "unaligned vkCmdCopyBuffer unsupported");
                cmd_fail(cmd, VK_ERROR_UNKNOWN);
                continue;
            }

            meta.shader_id = INTEL_DEV_META_VS_COPY_MEM_UNALIGNED;

            /*
             * INTEL_DEV_META_VS_COPY_MEM_UNALIGNED is untyped but expects the
             * stride to be 4
             */
            fmt = VK_FORMAT_R8G8B8A8_UINT;
        }

        if (format != fmt) {
            format = fmt;

            cmd_meta_set_src_for_buf(cmd, src, format, &meta);
            cmd_meta_set_dst_for_buf(cmd, dst, format, &meta);
        }

        cmd_draw_meta(cmd, &meta);
    }
}

ICD_EXPORT void VKAPI vkCmdCopyImage(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                   srcImage,
    VkImageLayout                            srcImageLayout,
    VkImage                                   destImage,
    VkImageLayout                            destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                       pRegions)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_img *src = intel_img(srcImage);
    struct intel_img *dst = intel_img(destImage);
    struct intel_cmd_meta meta;
    VkFormat raw_format;
    bool raw_copy = false;
    uint32_t i;

    if (src->type != dst->type) {
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    if (src->layout.format == dst->layout.format) {
        raw_copy = true;
        raw_format = cmd_meta_img_raw_format(cmd, src->layout.format);
    } else if (icd_format_is_compressed(src->layout.format) ||
               icd_format_is_compressed(dst->layout.format)) {
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_FS_RECT;

    cmd_meta_set_src_for_img(cmd, src,
            (raw_copy) ? raw_format : src->layout.format,
            VK_IMAGE_ASPECT_COLOR, &meta);

    meta.samples = dst->samples;

    for (i = 0; i < regionCount; i++) {
        const VkImageCopy *region = &pRegions[i];
        uint32_t j;

        meta.shader_id = get_shader_id(cmd->dev, src,
                (region->extent.depth > 1));

        meta.src.lod = region->srcSubresource.mipLevel;
        meta.src.layer = region->srcSubresource.arraySlice +
            region->srcOffset.z;
        meta.src.x = region->srcOffset.x;
        meta.src.y = region->srcOffset.y;

        meta.dst.lod = region->destSubresource.mipLevel;
        meta.dst.layer = region->destSubresource.arraySlice +
                region->destOffset.z;
        meta.dst.x = region->destOffset.x;
        meta.dst.y = region->destOffset.y;

        meta.width = region->extent.width;
        meta.height = region->extent.height;

        if (raw_copy) {
            const uint32_t block_width =
                icd_format_get_block_width(raw_format);

            meta.src.x /= block_width;
            meta.src.y /= block_width;
            meta.dst.x /= block_width;
            meta.dst.y /= block_width;
            meta.width /= block_width;
            meta.height /= block_width;
        }

        for (j = 0; j < region->extent.depth; j++) {
            cmd_meta_set_dst_for_img(cmd, dst,
                    (raw_copy) ? raw_format : dst->layout.format,
                    meta.dst.lod, meta.dst.layer, &meta);

            cmd_draw_meta(cmd, &meta);

            meta.src.layer++;
            meta.dst.layer++;
        }
    }
}

ICD_EXPORT void VKAPI vkCmdBlitImage(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                  srcImage,
    VkImageLayout                            srcImageLayout,
    VkImage                                  destImage,
    VkImageLayout                            destImageLayout,
    uint32_t                                 regionCount,
    const VkImageBlit*                       pRegions,
    VkTexFilter                              filter)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    /*
     * TODO: Implement actual blit function.
     */
    cmd_fail(cmd, VK_ERROR_UNAVAILABLE);
}

ICD_EXPORT void VKAPI vkCmdCopyBufferToImage(
    VkCmdBuffer                              cmdBuffer,
    VkBuffer                                  srcBuffer,
    VkImage                                   destImage,
    VkImageLayout                            destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                pRegions)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_buf *buf = intel_buf(srcBuffer);
    struct intel_img *img = intel_img(destImage);
    struct intel_cmd_meta meta;
    VkFormat format;
    uint32_t block_width, i;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_FS_RECT;

    meta.shader_id = INTEL_DEV_META_FS_COPY_MEM_TO_IMG;
    meta.samples = img->samples;

    format = cmd_meta_img_raw_format(cmd, img->layout.format);
    block_width = icd_format_get_block_width(img->layout.format);
    cmd_meta_set_src_for_buf(cmd, buf, format, &meta);

    for (i = 0; i < regionCount; i++) {
        const VkBufferImageCopy *region = &pRegions[i];
        uint32_t j;

        meta.src.x = region->bufferOffset / icd_format_get_size(format);

        meta.dst.lod = region->imageSubresource.mipLevel;
        meta.dst.layer = region->imageSubresource.arraySlice +
            region->imageOffset.z;
        meta.dst.x = region->imageOffset.x / block_width;
        meta.dst.y = region->imageOffset.y / block_width;

        meta.width = region->imageExtent.width / block_width;
        meta.height = region->imageExtent.height / block_width;

        for (j = 0; j < region->imageExtent.depth; j++) {
            cmd_meta_set_dst_for_img(cmd, img, format,
                    meta.dst.lod, meta.dst.layer, &meta);

            cmd_draw_meta(cmd, &meta);

            meta.src.x += meta.width * meta.height;
            meta.dst.layer++;
        }
    }
}

ICD_EXPORT void VKAPI vkCmdCopyImageToBuffer(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                   srcImage,
    VkImageLayout                            srcImageLayout,
    VkBuffer                                  destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                pRegions)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_img *img = intel_img(srcImage);
    struct intel_buf *buf = intel_buf(destBuffer);
    struct intel_cmd_meta meta;
    VkFormat img_format, buf_format;
    uint32_t block_width, i;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_VS_POINTS;

    img_format = cmd_meta_img_raw_format(cmd, img->layout.format);
    block_width = icd_format_get_block_width(img_format);

    /* buf_format is ignored by hw, but we derive stride from it */
    switch (img_format) {
    case VK_FORMAT_R8_UINT:
        meta.shader_id = INTEL_DEV_META_VS_COPY_R8_TO_MEM;
        buf_format = VK_FORMAT_R8G8B8A8_UINT;
        break;
    case VK_FORMAT_R16_UINT:
        meta.shader_id = INTEL_DEV_META_VS_COPY_R16_TO_MEM;
        buf_format = VK_FORMAT_R8G8B8A8_UINT;
        break;
    case VK_FORMAT_R32_UINT:
        meta.shader_id = INTEL_DEV_META_VS_COPY_R32_TO_MEM;
        buf_format = VK_FORMAT_R32G32B32A32_UINT;
        break;
    case VK_FORMAT_R32G32_UINT:
        meta.shader_id = INTEL_DEV_META_VS_COPY_R32G32_TO_MEM;
        buf_format = VK_FORMAT_R32G32B32A32_UINT;
        break;
    case VK_FORMAT_R32G32B32A32_UINT:
        meta.shader_id = INTEL_DEV_META_VS_COPY_R32G32B32A32_TO_MEM;
        buf_format = VK_FORMAT_R32G32B32A32_UINT;
        break;
    default:
        img_format = VK_FORMAT_UNDEFINED;
        buf_format = VK_FORMAT_UNDEFINED;
        break;
    }

    if (img_format == VK_FORMAT_UNDEFINED ||
        (cmd_gen(cmd) == INTEL_GEN(6) &&
         icd_format_get_size(img_format) < 4)) {
        intel_dev_log(cmd->dev, VK_DBG_REPORT_ERROR_BIT,
                      &cmd->obj.base, 0, 0,
                      "vkCmdCopyImageToBuffer with bpp %d unsupported",
                      icd_format_get_size(img->layout.format));
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    cmd_meta_set_src_for_img(cmd, img, img_format,
            VK_IMAGE_ASPECT_COLOR, &meta);
    cmd_meta_set_dst_for_buf(cmd, buf, buf_format, &meta);

    meta.samples = 1;

    for (i = 0; i < regionCount; i++) {
        const VkBufferImageCopy *region = &pRegions[i];
        uint32_t j;

        meta.src.lod = region->imageSubresource.mipLevel;
        meta.src.layer = region->imageSubresource.arraySlice +
            region->imageOffset.z;
        meta.src.x = region->imageOffset.x / block_width;
        meta.src.y = region->imageOffset.y / block_width;

        meta.dst.x = region->bufferOffset / icd_format_get_size(img_format);
        meta.width = region->imageExtent.width / block_width;
        meta.height = region->imageExtent.height / block_width;

        for (j = 0; j < region->imageExtent.depth; j++) {
            cmd_draw_meta(cmd, &meta);

            meta.src.layer++;
            meta.dst.x += meta.width * meta.height;
        }
    }
}

ICD_EXPORT void VKAPI vkCmdUpdateBuffer(
    VkCmdBuffer                              cmdBuffer,
    VkBuffer                                  destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_buf *dst = intel_buf(destBuffer);
    struct intel_cmd_meta meta;
    VkFormat format;
    uint32_t *ptr;
    uint32_t offset;

    /* must be 4-byte aligned */
    if ((destOffset | dataSize) & 3) {
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    /* write to dynamic state writer first */
    offset = cmd_state_pointer(cmd, INTEL_CMD_ITEM_BLOB, 32,
            (dataSize + 3) / 4, &ptr);
    memcpy(ptr, pData, dataSize);

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_VS_POINTS;

    meta.shader_id = INTEL_DEV_META_VS_COPY_MEM;

    meta.src.x = offset / 4;
    meta.dst.x = destOffset / 4;
    meta.width = dataSize / 4;
    meta.height = 1;
    meta.samples = 1;

    /*
     * INTEL_DEV_META_VS_COPY_MEM is untyped but expects the stride to be 16
     */
    format = VK_FORMAT_R32G32B32A32_UINT;

    cmd_meta_set_src_for_writer(cmd, INTEL_CMD_WRITER_STATE,
            offset + dataSize, format, &meta);
    cmd_meta_set_dst_for_buf(cmd, dst, format, &meta);

    cmd_draw_meta(cmd, &meta);
}

ICD_EXPORT void VKAPI vkCmdFillBuffer(
    VkCmdBuffer                              cmdBuffer,
    VkBuffer                                  destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_buf *dst = intel_buf(destBuffer);
    struct intel_cmd_meta meta;
    VkFormat format;

    /* must be 4-byte aligned */
    if ((destOffset | fillSize) & 3) {
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_VS_POINTS;

    meta.shader_id = INTEL_DEV_META_VS_FILL_MEM;

    meta.clear_val[0] = data;

    meta.dst.x = destOffset / 4;
    meta.width = fillSize / 4;
    meta.height = 1;
    meta.samples = 1;

    /*
     * INTEL_DEV_META_VS_FILL_MEM is untyped but expects the stride to be 16
     */
    format = VK_FORMAT_R32G32B32A32_UINT;

    cmd_meta_set_dst_for_buf(cmd, dst, format, &meta);

    cmd_draw_meta(cmd, &meta);
}

static void cmd_meta_clear_image(struct intel_cmd *cmd,
                                 struct intel_img *img,
                                 VkFormat format,
                                 struct intel_cmd_meta *meta,
                                 const VkImageSubresourceRange *range)
{
    uint32_t mip_levels, array_size;
    uint32_t i, j;

    if (range->baseMipLevel >= img->mip_levels ||
        range->baseArraySlice >= img->array_size)
        return;

    mip_levels = img->mip_levels - range->baseMipLevel;
    if (mip_levels > range->mipLevels)
        mip_levels = range->mipLevels;

    array_size = img->array_size - range->baseArraySlice;
    if (array_size > range->arraySize)
        array_size = range->arraySize;

    for (i = 0; i < mip_levels; i++) {
        meta->dst.lod = range->baseMipLevel + i;
        meta->dst.layer = range->baseArraySlice;

        /* TODO INTEL_CMD_META_DS_HIZ_CLEAR requires 8x4 aligned rectangle */
        meta->width = u_minify(img->layout.width0, meta->dst.lod);
        meta->height = u_minify(img->layout.height0, meta->dst.lod);

        if (meta->ds.op != INTEL_CMD_META_DS_NOP &&
            !intel_img_can_enable_hiz(img, meta->dst.lod))
            continue;

        for (j = 0; j < array_size; j++) {
            if (range->aspect == VK_IMAGE_ASPECT_COLOR) {
                cmd_meta_set_dst_for_img(cmd, img, format,
                        meta->dst.lod, meta->dst.layer, meta);

                cmd_draw_meta(cmd, meta);
            } else {
                cmd_meta_set_ds_view(cmd, img, meta->dst.lod,
                        meta->dst.layer, meta);
                cmd_meta_set_ds_state(cmd, range->aspect,
                        meta->clear_val[1], meta);

                cmd_draw_meta(cmd, meta);

                intel_att_view_destroy(meta->ds.view);
            }

            meta->dst.layer++;
        }
    }
}

void cmd_meta_ds_op(struct intel_cmd *cmd,
                    enum intel_cmd_meta_ds_op op,
                    struct intel_img *img,
                    const VkImageSubresourceRange *range)
{
    struct intel_cmd_meta meta;

    if (img->layout.aux != INTEL_LAYOUT_AUX_HIZ)
        return;
    if (range->aspect != VK_IMAGE_ASPECT_DEPTH)
        return;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_DEPTH_STENCIL_RECT;
    meta.samples = img->samples;

    meta.ds.aspect = VK_IMAGE_ASPECT_DEPTH;
    meta.ds.op = op;
    meta.ds.optimal = true;

    cmd_meta_clear_image(cmd, img, img->layout.format, &meta, range);
}

void cmd_meta_clear_color_image(
    VkCmdBuffer                         cmdBuffer,
    VkImage                             image,
    VkImageLayout                       imageLayout,
    const VkClearColorValue            *pClearColor,
    uint32_t                            rangeCount,
    const VkImageSubresourceRange      *pRanges)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_img *img = intel_img(image);
    struct intel_cmd_meta meta;
    VkFormat format;
    uint32_t i;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_FS_RECT;

    meta.shader_id = INTEL_DEV_META_FS_CLEAR_COLOR;
    meta.samples = img->samples;

    meta.clear_val[0] = pClearColor->u32[0];
    meta.clear_val[1] = pClearColor->u32[1];
    meta.clear_val[2] = pClearColor->u32[2];
    meta.clear_val[3] = pClearColor->u32[3];
    format = img->layout.format;

    for (i = 0; i < rangeCount; i++) {
        cmd_meta_clear_image(cmd, img, format, &meta, &pRanges[i]);
    }
}

ICD_EXPORT void VKAPI vkCmdClearColorImage(
    VkCmdBuffer                         cmdBuffer,
    VkImage                             image,
    VkImageLayout                       imageLayout,
    const VkClearColorValue            *pClearColor,
    uint32_t                            rangeCount,
    const VkImageSubresourceRange      *pRanges)
{
    cmd_meta_clear_color_image(cmdBuffer, image, imageLayout, pClearColor, rangeCount, pRanges);
}

void cmd_meta_clear_depth_stencil_image(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                   image,
    VkImageLayout                            imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*          pRanges)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_img *img = intel_img(image);
    struct intel_cmd_meta meta;
    uint32_t i;

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_DEPTH_STENCIL_RECT;

    meta.shader_id = INTEL_DEV_META_FS_CLEAR_DEPTH;
    meta.samples = img->samples;

    meta.clear_val[0] = u_fui(depth);
    meta.clear_val[1] = stencil;

    if (imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
        meta.ds.optimal = true;
    }

    for (i = 0; i < rangeCount; i++) {
        const VkImageSubresourceRange *range = &pRanges[i];

        cmd_meta_clear_image(cmd, img, img->layout.format,
                &meta, range);
    }
}

ICD_EXPORT void VKAPI vkCmdClearDepthStencilImage(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                   image,
    VkImageLayout                            imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*          pRanges)
{
    cmd_meta_clear_depth_stencil_image(cmdBuffer, image, imageLayout, depth, stencil, rangeCount, pRanges);
}

ICD_EXPORT void VKAPI vkCmdClearColorAttachment(
    VkCmdBuffer                             cmdBuffer,
    uint32_t                                colorAttachment,
    VkImageLayout                           imageLayout,
    const VkClearColorValue                *pColor,
    uint32_t                                rectCount,
    const VkRect3D                         *pRects)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    const struct intel_render_pass_subpass *subpass =
        cmd->bind.render_pass_subpass;
    const struct intel_fb *fb = cmd->bind.fb;
    const struct intel_att_view *view =
        fb->views[subpass->color_indices[colorAttachment]];

    /* Convert each rect3d to clear into a subresource clear.
     * TODO: this currently only supports full layer clears --
     * cmd_meta_clear_color_image does not provide a means to
     * specify the xy bounds.
     */
    for (uint32_t i = 0; i < rectCount; i++) {
           VkImageSubresourceRange range = {
               VK_IMAGE_ASPECT_COLOR,
               view->mipLevel,
               1,
               pRects[i].offset.z,
               pRects[i].extent.depth
           };

           cmd_meta_clear_color_image(cmdBuffer, (VkImage) view->img,
                                      imageLayout,
                                      pColor,
                                      1,
                                      &range);
    }
}

ICD_EXPORT void VKAPI vkCmdClearDepthStencilAttachment(
    VkCmdBuffer                             cmdBuffer,
    VkImageAspectFlags                      imageAspectMask,
    VkImageLayout                           imageLayout,
    float                                   depth,
    uint32_t                                stencil,
    uint32_t                                rectCount,
    const VkRect3D                         *pRects)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    const struct intel_render_pass_subpass *subpass =
        cmd->bind.render_pass_subpass;
    const struct intel_fb *fb = cmd->bind.fb;
    const struct intel_att_view *view = fb->views[subpass->ds_index];

    /* Convert each rect3d to clear into a subresource clear.
     * TODO: this currently only supports full layer clears --
     * cmd_meta_clear_depth_stencil_image does not provide a means to
     * specify the xy bounds.
     */
    for (uint32_t i = 0; i < rectCount; i++) {
           VkImageSubresourceRange range = {
               VK_IMAGE_ASPECT_DEPTH,
               0, /* ds->mipLevel, */
               1,
               pRects[i].offset.z,
               pRects[i].extent.depth
           };

           if (imageAspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
               cmd_meta_clear_depth_stencil_image(cmdBuffer,
                       (VkImage) view->img, imageLayout,
                       depth, stencil, 1, &range);
           }
           if (imageAspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
               range.aspect = VK_IMAGE_ASPECT_STENCIL;
               cmd_meta_clear_depth_stencil_image(cmdBuffer,
                       (VkImage) view->img, imageLayout,
                       depth, stencil, 1, &range);
           }
    }
}

ICD_EXPORT void VKAPI vkCmdResolveImage(
    VkCmdBuffer                              cmdBuffer,
    VkImage                                   srcImage,
    VkImageLayout                            srcImageLayout,
    VkImage                                   destImage,
    VkImageLayout                            destImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                    pRegions)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_img *src = intel_img(srcImage);
    struct intel_img *dst = intel_img(destImage);
    struct intel_cmd_meta meta;
    VkFormat format;
    uint32_t i;

    if (src->samples <= 1 || dst->samples > 1 ||
        src->layout.format != dst->layout.format) {
        cmd_fail(cmd, VK_ERROR_UNKNOWN);
        return;
    }

    memset(&meta, 0, sizeof(meta));
    meta.mode = INTEL_CMD_META_FS_RECT;

    switch (src->samples) {
    case 2:
    default:
        meta.shader_id = INTEL_DEV_META_FS_RESOLVE_2X;
        break;
    case 4:
        meta.shader_id = INTEL_DEV_META_FS_RESOLVE_4X;
        break;
    case 8:
        meta.shader_id = INTEL_DEV_META_FS_RESOLVE_8X;
        break;
    case 16:
        meta.shader_id = INTEL_DEV_META_FS_RESOLVE_16X;
        break;
    }

    meta.samples = 1;

    format = cmd_meta_img_raw_format(cmd, src->layout.format);
    cmd_meta_set_src_for_img(cmd, src, format, VK_IMAGE_ASPECT_COLOR, &meta);

    for (i = 0; i < regionCount; i++) {
        const VkImageResolve *region = &pRegions[i];
        int arraySlice;

        for(arraySlice = 0; arraySlice < region->extent.depth; arraySlice++) {
            meta.src.lod = region->srcSubresource.mipLevel;
            meta.src.layer = region->srcSubresource.arraySlice + arraySlice;
            meta.src.x = region->srcOffset.x;
            meta.src.y = region->srcOffset.y;

            meta.dst.lod = region->destSubresource.mipLevel;
            meta.dst.layer = region->destSubresource.arraySlice + arraySlice;
            meta.dst.x = region->destOffset.x;
            meta.dst.y = region->destOffset.y;

            meta.width = region->extent.width;
            meta.height = region->extent.height;

            cmd_meta_set_dst_for_img(cmd, dst, format,
                    meta.dst.lod, meta.dst.layer, &meta);

            cmd_draw_meta(cmd, &meta);
        }
    }
}
