/***************************************************************************\
    resizer.h - Image resizer using libgd, using pre-scaled images
		from libjpeg/libpng to be faster and use less memory.

    Copyright (C) 2008 piespy@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

#include <gd.h>

#include "auto_clean.h"

enum image_types { IMG_UNKNOWN, IMG_JPEG, IMG_PNG, IMG_GIF, IMG_BMP };
struct image_info {
	unsigned int width, height;
	const char* mime_type;
	image_types type;
};

struct resizer_result {
	gdImagePtr image;
	unsigned int via_x, via_y;

	operator gdImagePtr() { return image; }

	resizer_result(gdImagePtr i) : image(i), via_x(0), via_y(0) {}
	resizer_result(gdImagePtr i, unsigned int x, unsigned int y) : image(i), via_x(x), via_y(y) {}
};

// Use this to wrap the return code of e.g. resize_image_data
// to make sure it is always cleaned up, even with exceptions.
typedef AutoCleanPtrF<gdImage, &gdImageDestroy> AutoGDImage;

// Find type and dimensions from image data, which may be incomplete.
// Only reads the header, does not do any data validation.
// Return values:
// 0   Success, image_info structure now holds valid information.
//     (Note that the type may still be IMG_UNKNOWN.)
// >0  Cannot determine type, need at this many more bytes before
//     more information may be available.
size_t get_image_info(const unsigned char* data, size_t length, image_info* info);

// Take image data at given memory location and length, and resize
// to thu_x*thu_y (or, if thu_y = 0, the dimensions with the right
// aspect ratio fitting in box thu_x*thu_x), and return it.
// If allow_prescaled is set, large images will be prescaled while
// loading, and not loaded at full resolution. The minimum resolution
// at which this happens depends on the image format and what loading
// scalers it supports. It will always load it at least at twice
// the desired thumbnail resolution so that the final downscaling
// does not look blocky.
// JPEG		4*dim
// PNG		
// GIF
resizer_result resize_image_data(const unsigned char* data, size_t len, unsigned int thu_x, unsigned int thu_y, bool allow_prescaled);

