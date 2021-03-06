/* Phaethon - A FLOSS resource explorer for BioWare's Aurora engine games
 *
 * Phaethon is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * Phaethon is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * Phaethon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phaethon. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  Image related utility functions.
 */

#ifndef IMAGES_UTIL_H
#define IMAGES_UTIL_H

#include <cassert>
#include <cstring>

#include <memory>

#include "src/common/types.h"
#include "src/common/util.h"
#include "src/common/maths.h"
#include "src/common/error.h"

#include "src/images/types.h"

namespace Images {

/** Return the number of bytes per pixel in this format. */
static inline int getBPP(PixelFormat format) {
	switch (format) {
		case kPixelFormatR8G8B8:
		case kPixelFormatB8G8R8:
			return 3;

		case kPixelFormatR8G8B8A8:
		case kPixelFormatB8G8R8A8:
			return 4;

		case kPixelFormatA1R5G5B5:
		case kPixelFormatR5G6B5:
		case kPixelFormatDepth16:
			return 2;

		default:
			return 0;
	}
}

/** Return the number of bytes necessary to hold an image of these dimensions
  * and in this format. */
static inline uint32 getDataSize(PixelFormat format, int32 width, int32 height) {
	if ((width < 0) || (width >= 0x8000) || (height < 0) || (height >= 0x8000))
		throw Common::Exception("Invalid dimensions %dx%d", width, height);

	switch (format) {
		case kPixelFormatR8G8B8:
		case kPixelFormatB8G8R8:
			return width * height * 3;

		case kPixelFormatR8G8B8A8:
		case kPixelFormatB8G8R8A8:
			return width * height * 4;

		case kPixelFormatA1R5G5B5:
		case kPixelFormatR5G6B5:
		case kPixelFormatDepth16:
			return width * height * 2;

		case kPixelFormatDXT1:
			return MAX<uint32>( 8, ((width + 3) / 4) * ((height + 3) / 4) *  8);

		case kPixelFormatDXT3:
		case kPixelFormatDXT5:
			return MAX<uint32>(16, ((width + 3) / 4) * ((height + 3) / 4) * 16);

		default:
			break;
	}

	throw Common::Exception("Invalid pixel format %u", (uint) format);
}

/** Are these image dimensions valid for this format? */
static inline bool hasValidDimensions(PixelFormat format, int32 width, int32 height) {
	if ((width < 0) || (width >= 0x8000) || (height < 0) || (height >= 0x8000))
		return false;

	switch (format) {
		case kPixelFormatR8G8B8:
		case kPixelFormatB8G8R8:
		case kPixelFormatR8G8B8A8:
		case kPixelFormatB8G8R8A8:
		case kPixelFormatA1R5G5B5:
		case kPixelFormatR5G6B5:
		case kPixelFormatDepth16:
		case kPixelFormatDXT1:
		case kPixelFormatDXT3:
		case kPixelFormatDXT5:
			return true;

		default:
			break;
	}

	return false;
}

/** Flip an image horizontally. */
static inline void flipHorizontally(byte *data, int width, int height, int bpp) {
	if ((width <= 0) || (height <= 0) || (bpp <= 0))
		return;

	const size_t halfWidth = width / 2;
	const size_t pitch     = bpp * width;

	std::unique_ptr<byte[]> buffer = std::make_unique<byte[]>(bpp);

	while (height-- > 0) {
		byte *dataStart = data;
		byte *dataEnd   = data + pitch - bpp;

		for (size_t j = 0; j < halfWidth; j++) {
			memcpy(buffer.get(), dataStart   , bpp);
			memcpy(dataStart   , dataEnd     , bpp);
			memcpy(dataEnd     , buffer.get(), bpp);

			dataStart += bpp;
			dataEnd   -= bpp;
		}

		data += pitch;
	}
}

/** Flip an image vertically. */
static inline void flipVertically(byte *data, int width, int height, int bpp) {
	if ((width <= 0) || (height <= 0) || (bpp <= 0))
		return;

	const size_t pitch = bpp * width;

	byte *dataStart = data;
	byte *dataEnd   = data + (pitch * height) - pitch;

	std::unique_ptr<byte[]> buffer = std::make_unique<byte[]>(pitch);

	size_t halfHeight = height / 2;
	while (halfHeight--) {
		memcpy(buffer.get(), dataStart   , pitch);
		memcpy(dataStart   , dataEnd     , pitch);
		memcpy(dataEnd     , buffer.get(), pitch);

		dataStart += pitch;
		dataEnd   -= pitch;
	}
}

/** Rotate a square image in 90° steps, clock-wise. */
static inline void rotate90(byte *data, int width, int height, int bpp, int steps) {
	if ((width <= 0) || (height <= 0) || (bpp <= 0))
		return;

	assert(width == height);

	while (steps-- > 0) {
		const size_t n = width;

		const size_t w =  n      / 2;
		const size_t h = (n + 1) / 2;

		for (size_t x = 0; x < w; x++) {
			for (size_t y = 0; y < h; y++) {
				const size_t d0 = ( y          * n +  x         ) * bpp;
				const size_t d1 = ((n - 1 - x) * n +  y         ) * bpp;
				const size_t d2 = ((n - 1 - y) * n + (n - 1 - x)) * bpp;
				const size_t d3 = ( x          * n + (n - 1 - y)) * bpp;

				for (size_t p = 0; p < (size_t) bpp; p++) {
					const byte tmp = data[d0 + p];

					data[d0 + p] = data[d1 + p];
					data[d1 + p] = data[d2 + p];
					data[d2 + p] = data[d3 + p];
					data[d3 + p] = tmp;
				}
			}
		}

	}
}

/** De-"swizzle" a texture pixel offset. */
static inline uint32 deSwizzleOffset(uint32 x, uint32 y, uint32 width, uint32 height) {
	width  = Common::intLog2(width);
	height = Common::intLog2(height);

	uint32 offset     = 0;
	uint32 shiftCount = 0;

	while (width | height) {
		if (width) {
			offset |= (x & 0x01) << shiftCount;

			x >>= 1;

			shiftCount++;
			width--;
		}

		if (height) {
			offset |= (y & 0x01) << shiftCount;

			y >>= 1;

			shiftCount++;
			height--;
		}
	}

	return offset;
}

} // End of namespace Images

#endif // IMAGES_UTIL_H
