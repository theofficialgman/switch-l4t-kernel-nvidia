/*
 * cursor.c: Cursor functions for tegradc ext interface.
 *
 * Copyright (c) 2011-2019, NVIDIA CORPORATION, All rights reserved.
 *
 * Author: Robert Morell <rmorell@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/dma-buf.h>
#include <uapi/video/tegra_dc_ext.h>
#include "tegra_dc_ext_priv.h"

/* ugh */
#include "../dc.h"
#include "../dc_priv.h"
#include "../dc_reg.h"

static size_t cursor_buf_size(unsigned int n, enum CURSOR_COLOR_FORMAT colorfmt)
{
	if (colorfmt == a1r5g5b5)
		return (size_t)n * n * 2;
	/* legacy, r8g8b8a8, a8r8g8b8 — all 32bpp in hardware */
	return (size_t)n * n * 4;
}

/*
 * Rotate an N×N cursor bitmap to match the display rotation, reading from
 * the unmodified user buffer (src) and writing into a DMA-coherent output
 * buffer (dst).  Never touches the user buffer after reading it.
 *
 * Rotation angles match dc->out->rotation:
 *   90  → 90° CCW  (bottom-left of original goes to top-left of output)
 *   180 → 180°
 *   270 → 90° CW   (top-right of original goes to top-left of output)
 */
static void tegra_dc_ext_rotate_cursor(const void *src, void *dst,
				       unsigned int n,
				       enum CURSOR_COLOR_FORMAT colorfmt,
				       int rotation)
{
	unsigned int r, c;
	size_t sz = cursor_buf_size(n, colorfmt);

#define ROTATE_PIXELS(type)						\
	do {								\
		const type *s = src;					\
		type *d = dst;						\
		switch (rotation) {					\
		case 90: /* CCW */					\
			for (r = 0; r < n; r++)				\
				for (c = 0; c < n; c++)			\
					d[(n-1-c)*n + r] = s[r*n + c]; \
			break;						\
		case 180:						\
			for (r = 0; r < n; r++)				\
				for (c = 0; c < n; c++)			\
					d[(n-1-r)*n + (n-1-c)] = s[r*n + c]; \
			break;						\
		case 270: /* CW */					\
			for (r = 0; r < n; r++)				\
				for (c = 0; c < n; c++)			\
					d[c*n + (n-1-r)] = s[r*n + c]; \
			break;						\
		default:						\
			memcpy(d, s, sz);				\
			break;						\
		}							\
	} while (0)

	switch (colorfmt) {
	case legacy:
	case r8g8b8a8:
	case a8r8g8b8:
		ROTATE_PIXELS(u32);
		break;
	case a1r5g5b5:
		ROTATE_PIXELS(u16);
		break;
	}
#undef ROTATE_PIXELS
}

void tegra_dc_ext_cursor_cleanup(struct tegra_dc_ext *ext)
{
	if (ext->cursor.rot_vaddr) {
		dma_free_coherent(&ext->dc->ndev->dev, ext->cursor.rot_size,
				  ext->cursor.rot_vaddr, ext->cursor.rot_dma);
		ext->cursor.rot_vaddr = NULL;
		ext->cursor.rot_dma   = 0;
		ext->cursor.rot_size  = 0;
	}
}

int tegra_dc_ext_get_cursor(struct tegra_dc_ext_user *user)
{
	struct tegra_dc_ext *ext = user->ext;
	int ret = 0;

	mutex_lock(&ext->cursor.lock);

	if (!ext->cursor.user)
		ext->cursor.user = user;
	else if (ext->cursor.user != user)
		ret = -EBUSY;

	mutex_unlock(&ext->cursor.lock);

	return ret;
}

int tegra_dc_ext_put_cursor(struct tegra_dc_ext_user *user)
{
	struct tegra_dc_ext *ext = user->ext;
	int ret = 0;

	mutex_lock(&ext->cursor.lock);

	if (ext->cursor.user == user)
		ext->cursor.user = NULL;
	else
		ret = -EACCES;

	mutex_unlock(&ext->cursor.lock);

	return ret;
}

int tegra_dc_ext_set_cursor_image(struct tegra_dc_ext_user *user,
				  struct tegra_dc_ext_cursor_image *args)
{
	struct tegra_dc_ext *ext = user->ext;
	struct tegra_dc *dc = ext->dc;
	struct tegra_dc_dmabuf *handle, *old_handle;
	dma_addr_t phys_addr;
	int ret;
	u32 extformat = TEGRA_DC_EXT_CURSOR_FORMAT_FLAGS(args->flags);
	u32 clrformat = TEGRA_DC_EXT_CURSOR_COLORFMT_FLAGS(args->flags);
	u32 fg = CURSOR_COLOR(args->foreground.r,
			      args->foreground.g,
			      args->foreground.b);
	u32 bg = CURSOR_COLOR(args->background.r,
			      args->background.g,
			      args->background.b);
	unsigned extsize = TEGRA_DC_EXT_CURSOR_IMAGE_FLAGS_SIZE(args->flags);
	enum tegra_dc_cursor_size size;
	enum tegra_dc_cursor_blend_format blendfmt;
	enum CURSOR_COLOR_FORMAT colorfmt;
	unsigned int cursor_n;

	switch (extsize) {
	case TEGRA_DC_EXT_CURSOR_IMAGE_FLAGS_SIZE_32x32:
		size = TEGRA_DC_CURSOR_SIZE_32X32;
		cursor_n = 32;
		break;
	case TEGRA_DC_EXT_CURSOR_IMAGE_FLAGS_SIZE_64x64:
		size = TEGRA_DC_CURSOR_SIZE_64X64;
		cursor_n = 64;
		break;
	case TEGRA_DC_EXT_CURSOR_IMAGE_FLAGS_SIZE_128x128:
		size = TEGRA_DC_CURSOR_SIZE_128X128;
		cursor_n = 128;
		break;
	case TEGRA_DC_EXT_CURSOR_IMAGE_FLAGS_SIZE_256x256:
		size = TEGRA_DC_CURSOR_SIZE_256X256;
		cursor_n = 256;
		break;
	default:
		return -EINVAL;
	}

	switch (extformat) {
	case TEGRA_DC_EXT_CURSOR_FORMAT_2BIT_LEGACY:
		blendfmt = TEGRA_DC_CURSOR_FORMAT_2BIT_LEGACY;
		break;
	case TEGRA_DC_EXT_CURSOR_FORMAT_RGBA_NON_PREMULT_ALPHA:
		blendfmt = TEGRA_DC_CURSOR_FORMAT_RGBA_NON_PREMULT_ALPHA;
		break;
	case TEGRA_DC_EXT_CURSOR_FORMAT_RGBA_PREMULT_ALPHA:
		blendfmt = TEGRA_DC_CURSOR_FORMAT_RGBA_PREMULT_ALPHA;
		break;
	case TEGRA_DC_EXT_CURSOR_FORMAT_RGBA_XOR:
		blendfmt = TEGRA_DC_CURSOR_FORMAT_RGBA_XOR;
		break;
	default:
		return -EINVAL;
	}
	switch (clrformat) {
	case TEGRA_DC_CURSOR_COLORFMT_LEGACY:
		colorfmt = legacy;
		break;
	case TEGRA_DC_CURSOR_COLORFMT_R8G8B8A8: /* normal */
		colorfmt = r8g8b8a8;
		break;
	case  TEGRA_DC_CURSOR_COLORFMT_A1R5G5B5:
		colorfmt = a1r5g5b5;
		break;
	case TEGRA_DC_CURSOR_COLORFMT_A8R8G8B8:
		colorfmt = a8r8g8b8;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&ext->cursor.lock);

	if (ext->cursor.user != user) {
		ret = -EACCES;
		goto unlock;
	}

	if (!ext->enabled) {
		ret = -ENXIO;
		goto unlock;
	}

	old_handle = ext->cursor.cur_handle;

	ret = tegra_dc_ext_pin_window(user, args->buff_id, &handle, &phys_addr);
	if (ret)
		goto unlock;

	/*
	 * Rotate cursor bitmap to match the display rotation.  A persistent
	 * DMA-coherent scratch buffer is used so the user buffer is never
	 * modified — re-reading the original data each call means repeated
	 * set_cursor_image calls on the same buffer don't accumulate rotations.
	 */
	{
		int display_rotation = (dc->out) ? dc->out->rotation : 0;
		size_t rot_sz = cursor_buf_size(cursor_n, colorfmt);
		struct device *dma_dev = &dc->ndev->dev;
		dma_addr_t hw_phys = phys_addr; /* fallback if rotation fails */

		if (display_rotation != 0) {
			if (ext->cursor.rot_size < rot_sz) {
				if (ext->cursor.rot_vaddr)
					dma_free_coherent(dma_dev,
							  ext->cursor.rot_size,
							  ext->cursor.rot_vaddr,
							  ext->cursor.rot_dma);
				ext->cursor.rot_vaddr =
					dma_alloc_coherent(dma_dev, rot_sz,
							   &ext->cursor.rot_dma,
							   GFP_KERNEL);
				ext->cursor.rot_size =
					ext->cursor.rot_vaddr ? rot_sz : 0;
				if (!ext->cursor.rot_vaddr)
					dev_warn(dma_dev,
						 "cursor: alloc %zu byte rot buf failed\n",
						 rot_sz);
			}

			if (ext->cursor.rot_vaddr) {
				int cret = dma_buf_begin_cpu_access(
						handle->buf, 0, rot_sz,
						DMA_BIDIRECTIONAL);
				if (!cret) {
					void *uva = dma_buf_vmap(handle->buf);

					if (uva) {
						tegra_dc_ext_rotate_cursor(
							uva,
							ext->cursor.rot_vaddr,
							cursor_n, colorfmt,
							display_rotation);
						dma_buf_vunmap(handle->buf, uva);
						hw_phys = ext->cursor.rot_dma;
						dev_dbg(dma_dev,
							 "cursor rotate: %ddeg n=%u fmt=%d sz=%zu\n",
							 display_rotation,
							 cursor_n,
							 (int)colorfmt,
							 rot_sz);
					} else {
						dev_warn(dma_dev,
							 "cursor rotate: vmap failed\n");
					}
					dma_buf_end_cpu_access(handle->buf, 0,
							       rot_sz,
							       DMA_BIDIRECTIONAL);
				} else {
					dev_warn(dma_dev,
						 "cursor rotate: begin_cpu_access failed: %d\n",
						 cret);
				}
			}
		}

		tegra_dc_scrncapt_disp_pause_lock(dc);

		ext->cursor.cur_handle = handle;
		ret = tegra_dc_cursor_image(dc, blendfmt, size, fg, bg, hw_phys,
					    colorfmt, args->alpha, args->flags,
					    !!old_handle);

		tegra_dc_scrncapt_disp_pause_unlock(dc);
		mutex_unlock(&ext->cursor.lock);

		if (old_handle) {
			dma_buf_unmap_attachment(old_handle->attach,
				old_handle->sgt, DMA_TO_DEVICE);
			dma_buf_detach(old_handle->buf, old_handle->attach);
			dma_buf_put(old_handle->buf);
			kfree(old_handle);
		}

		return ret;
	}

unlock:
	mutex_unlock(&ext->cursor.lock);

	return ret;
}

int tegra_dc_ext_set_cursor(struct tegra_dc_ext_user *user,
			    struct tegra_dc_ext_cursor *args)
{
	struct tegra_dc_ext *ext = NULL;
	struct tegra_dc *dc = NULL;
	bool enable;
	int ret;

	if (!args)
		return -EINVAL;

	if (!user || !user->ext || !user->ext->dc)
		return -ENODEV;

	ext = user->ext;
	dc = ext->dc;

	mutex_lock(&ext->cursor.lock);
	if (ext->cursor.user != user) {
		ret = -EACCES;
		goto unlock;
	}

	if (!ext->enabled) {
		ret = -ENXIO;
		goto unlock;
	}

	enable = !!(args->flags & TEGRA_DC_EXT_CURSOR_FLAGS_VISIBLE);
	if (enable && !ext->cursor.cur_handle) {
		ret = -EFAULT;
		goto unlock;
	}

	tegra_dc_scrncapt_disp_pause_lock(dc);

	{
		int phys_x = args->x;
		int phys_y = args->y;
		int cursor_n;

		switch (dc->cursor.size) {
		case TEGRA_DC_CURSOR_SIZE_32X32:   cursor_n = 32;  break;
		case TEGRA_DC_CURSOR_SIZE_64X64:   cursor_n = 64;  break;
		case TEGRA_DC_CURSOR_SIZE_128X128: cursor_n = 128; break;
		case TEGRA_DC_CURSOR_SIZE_256X256: cursor_n = 256; break;
		default:                           cursor_n = 0;   break;
		}

		if (dc->out) {
			switch (dc->out->rotation) {
			case 90:
				/*
				 * Bitmap rotated 90deg CCW: original top-left
				 * hot-spot moves to bottom-left of rotated bitmap.
				 * Correct: pull phys_y back by (n-1).
				 */
				phys_x = args->y;
				phys_y = (int)dc->mode.v_active - 1 - args->x
					 - (cursor_n - 1);
				break;
			case 180:
				/*
				 * Bitmap rotated 180deg: hot-spot moves to
				 * bottom-right.  Correct both axes.
				 */
				phys_x = (int)dc->mode.h_active - 1 - args->x
					 - (cursor_n - 1);
				phys_y = (int)dc->mode.v_active - 1 - args->y
					 - (cursor_n - 1);
				break;
			case 270:
				/*
				 * Bitmap rotated 90deg CW: hot-spot moves to
				 * top-right of rotated bitmap.  Correct phys_x.
				 */
				phys_x = (int)dc->mode.h_active - 1 - args->y
					 - (cursor_n - 1);
				phys_y = args->x;
				break;
			}
		}

		dev_dbg(&dc->ndev->dev,
			 "cursor set: logical=(%d,%d) phys=(%d,%d) n=%d rot=%d\n",
			 args->x, args->y, phys_x, phys_y, cursor_n,
			 dc->out ? dc->out->rotation : 0);

		ret = tegra_dc_cursor_set(dc, enable, phys_x, phys_y);
	}

	tegra_dc_scrncapt_disp_pause_unlock(dc);
	mutex_unlock(&ext->cursor.lock);

	return ret;

unlock:
	mutex_unlock(&ext->cursor.lock);

	return ret;
}

int tegra_dc_ext_cursor_clip(struct tegra_dc_ext_user *user,
			    int *args)
{
	struct tegra_dc_ext *ext = user->ext;
	struct tegra_dc *dc = ext->dc;
	int ret;

	mutex_lock(&ext->cursor.lock);

	if (ext->cursor.user != user) {
		ret = -EACCES;
		goto unlock;
	}

	if (!ext->enabled) {
		ret = -ENXIO;
		goto unlock;
	}

	ret = tegra_dc_cursor_clip(dc, *args);

	mutex_unlock(&ext->cursor.lock);

	return ret;

unlock:
	mutex_unlock(&ext->cursor.lock);

	return ret;
}
