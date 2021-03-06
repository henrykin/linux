// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 */

#include <asm/unaligned.h>
#include <media/v4l2-mem2mem.h>
#include "rockchip_vpu_jpeg.h"
#include "rockchip_vpu.h"
#include "rockchip_vpu_common.h"
#include "rockchip_vpu_hw.h"
#include "rk3288_vpu_regs.h"

#define VEPU_JPEG_QUANT_TABLE_COUNT 16

static void rk3288_vpu_set_src_img_ctrl(struct rockchip_vpu_dev *vpu,
					struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	u32 reg;

	reg = VEPU_REG_IN_IMG_CTRL_ROW_LEN(pix_fmt->width)
		| VEPU_REG_IN_IMG_CTRL_OVRFLR_D4(0)
		| VEPU_REG_IN_IMG_CTRL_OVRFLB_D4(0)
		| VEPU_REG_IN_IMG_CTRL_FMT(ctx->vpu_src_fmt->enc_fmt);
	vepu_write_relaxed(vpu, reg, VEPU_REG_IN_IMG_CTRL);
}

static void rk3288_vpu_jpeg_enc_set_buffers(struct rockchip_vpu_dev *vpu,
					    struct rockchip_vpu_ctx *ctx,
					    struct vb2_buffer *src_buf)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	dma_addr_t src[3];

	WARN_ON(pix_fmt->num_planes > 3);

	vepu_write_relaxed(vpu, ctx->bounce_dma_addr,
			   VEPU_REG_ADDR_OUTPUT_STREAM);
	vepu_write_relaxed(vpu, ctx->bounce_size,
			   VEPU_REG_STR_BUF_LIMIT);

	if (pix_fmt->num_planes == 1) {
		src[0] = vb2_dma_contig_plane_dma_addr(src_buf, 0);
		/* single plane formats we supported are all interlaced */
		vepu_write_relaxed(vpu, src[0], VEPU_REG_ADDR_IN_PLANE_0);
	} else if (pix_fmt->num_planes == 2) {
		src[0] = vb2_dma_contig_plane_dma_addr(src_buf, 0);
		src[1] = vb2_dma_contig_plane_dma_addr(src_buf, 1);
		vepu_write_relaxed(vpu, src[0], VEPU_REG_ADDR_IN_PLANE_0);
		vepu_write_relaxed(vpu, src[1], VEPU_REG_ADDR_IN_PLANE_1);
	} else {
		src[0] = vb2_dma_contig_plane_dma_addr(src_buf, 0);
		src[1] = vb2_dma_contig_plane_dma_addr(src_buf, 1);
		src[2] = vb2_dma_contig_plane_dma_addr(src_buf, 2);
		vepu_write_relaxed(vpu, src[0], VEPU_REG_ADDR_IN_PLANE_0);
		vepu_write_relaxed(vpu, src[1], VEPU_REG_ADDR_IN_PLANE_1);
		vepu_write_relaxed(vpu, src[2], VEPU_REG_ADDR_IN_PLANE_2);
	}
}

static void
rk3288_vpu_jpeg_enc_set_qtable(struct rockchip_vpu_dev *vpu,
			       unsigned char *luma_qtable,
			       unsigned char *chroma_qtable)
{
	u32 reg, i;

	for (i = 0; i < VEPU_JPEG_QUANT_TABLE_COUNT; i++) {
		reg = get_unaligned_be32(&luma_qtable[i]);
		vepu_write_relaxed(vpu, reg, VEPU_REG_JPEG_LUMA_QUAT(i));

		reg = get_unaligned_be32(&chroma_qtable[i]);
		vepu_write_relaxed(vpu, reg, VEPU_REG_JPEG_CHROMA_QUAT(i));
	}
}

void rk3288_vpu_jpeg_enc_run(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct rockchip_vpu_jpeg_ctx jpeg_ctx;
	u32 reg;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	memset(&jpeg_ctx, 0, sizeof(jpeg_ctx));
	jpeg_ctx.buffer = vb2_plane_vaddr(&dst_buf->vb2_buf, 0);
	jpeg_ctx.width = ctx->dst_fmt.width;
	jpeg_ctx.height = ctx->dst_fmt.height;
	jpeg_ctx.quality = ctx->jpeg_quality;
	rockchip_vpu_jpeg_header_assemble(&jpeg_ctx);

	/* Switch to JPEG encoder mode before writing registers */
	vepu_write_relaxed(vpu, VEPU_REG_ENC_CTRL_ENC_MODE_JPEG,
			   VEPU_REG_ENC_CTRL);

	rk3288_vpu_set_src_img_ctrl(vpu, ctx);
	rk3288_vpu_jpeg_enc_set_buffers(vpu, ctx, &src_buf->vb2_buf);
	rk3288_vpu_jpeg_enc_set_qtable(vpu,
				       rockchip_vpu_jpeg_get_qtable(&jpeg_ctx, 0),
				       rockchip_vpu_jpeg_get_qtable(&jpeg_ctx, 1));

	reg = VEPU_REG_AXI_CTRL_OUTPUT_SWAP16
		| VEPU_REG_AXI_CTRL_INPUT_SWAP16
		| VEPU_REG_AXI_CTRL_BURST_LEN(16)
		| VEPU_REG_AXI_CTRL_OUTPUT_SWAP32
		| VEPU_REG_AXI_CTRL_INPUT_SWAP32
		| VEPU_REG_AXI_CTRL_OUTPUT_SWAP8
		| VEPU_REG_AXI_CTRL_INPUT_SWAP8;
	/* Make sure that all registers are written at this point. */
	vepu_write(vpu, reg, VEPU_REG_AXI_CTRL);

	reg = VEPU_REG_ENC_CTRL_WIDTH(JPEG_MB_WIDTH(ctx->src_fmt.width))
		| VEPU_REG_ENC_CTRL_HEIGHT(JPEG_MB_HEIGHT(ctx->src_fmt.height))
		| VEPU_REG_ENC_CTRL_ENC_MODE_JPEG
		| VEPU_REG_ENC_PIC_INTRA
		| VEPU_REG_ENC_CTRL_EN_BIT;
	/* Kick the watchdog and start encoding */
	schedule_delayed_work(&vpu->watchdog_work, msecs_to_jiffies(2000));
	vepu_write(vpu, reg, VEPU_REG_ENC_CTRL);
}
