/***************************************************************************\
    resizer.cpp - Image resizer using libgd, using pre-scaled images
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

#include <errno.h>
#include <stdio.h>

#include <arpa/inet.h>	// For ntoh*

extern "C" {
#include <jpeglib.h>
#include <jerror.h>
}

#define PNG_USE_GLOBAL_ARRAYS
#include <png.h>

#define DEBUG_RESIZER
#include "debug.h"

#include "imgdb.h"
#include "resizer.h"

extern int debug_level;

inline int get_jpeg_info(const unsigned char* data, size_t length, image_info* info) {
	while (1) {
		if (length < 2)
			return -2;

		if (data[0] != 0xff || data[1] < 0xc0) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "nope, marker is %02x%02x.\n", data[0], data[1]);
			return 0;
		}

		// RST0..RST7 have no length value.
		if (data[1] >= 0xd0 && data[1] <= 0xd7) { data += 2; length -= 2; continue; }

		// SOF markers are what we are looking for.
		switch (data[1]) {
			case 0xc0:
			case 0xc1:
			case 0xc2:
			case 0xc3:
			case 0xc5:
			case 0xc6:
			case 0xc7:
			case 0xc9:
			case 0xca:
			case 0xcb:
			case 0xcd:
			case 0xce:
			case 0xcf:
			case 0xf7:
				if (length < 9) {
					DEBUG_CONT(image_info)(DEBUG_OUT, "too short to tell.\n");
					return 9 - length;
				}
				info->height = ntohs(*(uint16_t*)((data+5)));
				info->width  = ntohs(*(uint16_t*)((data+7)));
				info->type = IMG_JPEG;
				info->mime_type = "image/jpeg";
				DEBUG_CONT(image_info)(DEBUG_OUT, "yes, %dx%d\n", info->width, info->height);
				return 0;
		}

		// Otherwise skip block.
		size_t blen = length < 2 ? 2 : ntohs(*(uint16_t*)((data+2)));
		if (length < blen + 4) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "too short to tell.\n");
			return blen + 4 - length;
		}
		data += blen + 2;
		length -= blen + 2;
	}
}

inline uint32_t get_le_32(const unsigned char* data) {
	return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}
inline uint16_t get_le_16(const unsigned char* data) {
	return data[0] | (data[1] << 8);
}

size_t get_image_info(const unsigned char* data, size_t length, image_info* info) {
	DEBUG(image_info)("Determining image info for %zd bytes at %p... ", length, data);
	info->type = IMG_UNKNOWN;
	info->mime_type = "application/octet-stream";

	if (length < 10) {
		DEBUG_CONT(image_info)(DEBUG_OUT, "too short to tell.\n");
		return 10 - length;
	}

	if (data[0] == 0xff && data[1] == 0xd8) {
		DEBUG_CONT(image_info)(DEBUG_OUT, "looks like JPEG... ");
		data += 2; length -= 2;
		return get_jpeg_info(data, length, info);

	} else if (!memcmp(data, "\x89PNG\x0D\x0A\x1A\x0A", 8)) {
		DEBUG_CONT(image_info)(DEBUG_OUT, "looks like PNG... ");
		data += 8; length -= 8;
		if (length < 16) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "too short to tell.\n");
			return 16 - length;
		}
		if (memcmp(data, "\0\0\0\x0dIHDR", 8)) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "nope, no IHDR chunk.\n");
			return 0;
		}
		info->width  = ntohl(*(uint32_t*)((data+8)));
		info->height = ntohl(*(uint32_t*)((data+12)));
		info->type = IMG_PNG;
		info->mime_type = "image/png";
		DEBUG_CONT(image_info)(DEBUG_OUT, "yes, %dx%d\n", info->width, info->height);
		return 0;

	} else if (!memcmp(data, "GIF", 3)) {
		DEBUG_CONT(image_info)(DEBUG_OUT, "looks like GIF... ");
		data += 6; length -= 6;
		info->width  = get_le_16(data);
		info->height = get_le_16(data+2);
		info->type = IMG_GIF;
		info->mime_type = "image/gif";
		DEBUG_CONT(image_info)(DEBUG_OUT, "yes, %dx%d\n", info->width, info->height);
		return 0;

	} else if (data[0] == 'B' && data[1] == 'M') {
		DEBUG_CONT(image_info)(DEBUG_OUT, "looks like BMP... ");
		if (length < 26) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "too short to tell.\n");
			return 26 - length;
		}
		data += 14; length -= 14;
		if (get_le_32(data) != 40) {
			DEBUG_CONT(image_info)(DEBUG_OUT, "nope, wrong header size.\n");
			return 0;
		}
		info->width = get_le_32(data + 4);
		info->width = get_le_32(data + 8);
		info->type = IMG_BMP;
		info->mime_type = "image/bmp";
		DEBUG_CONT(image_info)(DEBUG_OUT, "yes, %dx%d\n", info->width, info->height);
		return 0;
	}

	DEBUG_CONT(image_info)(DEBUG_OUT, "doesn't look like anything.\n");
	return 0;
}

inline unsigned int find_scale_bits(unsigned int width, unsigned int height, unsigned int thu_x, unsigned int thu_y) {
	if (width >= thu_x * 16 && height >= thu_y * 16) {
		return 3;	// 1/8
	} else if (width >= thu_x * 8 && height >= thu_y * 8) {
		return 2;	// 1/4
	} else if (width >= thu_x * 4 && height >= thu_y * 4) {
		return 1;	// 1/2
	} else {
		return 0;	// 1/1
	}
}

struct jpeg_error : public jpeg_error_mgr {
	jmp_buf handler;
	j_common_ptr info;
} jpeg_error_handler;

static void jpeg_error_exit(j_common_ptr cinfo) {
	jpeg_error* err = (jpeg_error*)cinfo->err;
	err->info = cinfo;
	longjmp(err->handler, 1);
}
static void jpeg_warning(j_common_ptr cinfo, int msg_level) {
	char msg[1024];
	(*cinfo->err->format_message)(cinfo, msg);
	DEBUG(warnings)("JPEG warning level %d: %s\n", msg_level, msg);
}

struct jpeg_data_reader : public jpeg_source_mgr {
	jpeg_data_reader(const unsigned char* data, size_t len) : m_len(len), m_data(data) {
		init_source = &init; fill_input_buffer = &fill; skip_input_data = &skip; term_source = &term;
		resync_to_restart = jpeg_resync_to_restart; bytes_in_buffer = 0; next_input_byte = NULL;
	}

	boolean do_fill(j_decompress_ptr cinfo);
	void do_skip(size_t num) { size_t n = std::min(num, bytes_in_buffer); bytes_in_buffer -= n; next_input_byte += n; }

	static void init(j_decompress_ptr cinfo) { }
	static boolean fill(j_decompress_ptr cinfo);
	static void skip(j_decompress_ptr cinfo, long num_bytes) { ((jpeg_data_reader*)cinfo->src)->do_skip(num_bytes); }
	static void term(j_decompress_ptr cinfo) { }

	size_t m_len;
	const unsigned char* m_data;

	static const JOCTET fake_eoi[2];
};

const JOCTET jpeg_data_reader::fake_eoi[2] = { (JOCTET) 0xFF, (JOCTET) JPEG_EOI };

boolean jpeg_data_reader::do_fill(j_decompress_ptr cinfo) {
	if (m_len == 0) {
		DEBUG(resizer)("jpeg_data_reader::do_fill called with no more data!\n");
		ERREXIT(cinfo, JERR_INPUT_EMPTY);
		WARNMS(cinfo, JWRN_JPEG_EOF);
		next_input_byte = fake_eoi;
		bytes_in_buffer = 2;
	} else {
		//DEBUG(resizer)("jpeg_data_reader::do_fill returning initial %zd bytes at %p.\n", m_len, m_data);
		next_input_byte = m_data;
		bytes_in_buffer = m_len;
		m_len = 0;
	}
	return TRUE;
}

// defined after do_fill so the compiler can inline it
boolean jpeg_data_reader::fill(j_decompress_ptr cinfo) { return ((jpeg_data_reader*)cinfo->src)->do_fill(cinfo); }

static boolean skip_jpeg_marker(j_decompress_ptr cinfo) {
	size_t len = ntohs(*(uint16_t*)cinfo->src->next_input_byte);
	//if (cinfo->unread_marker == JPEG_COM) {
	//	DEBUG(resizer)("JPEG comment, length %zd skipped.\n", len);
	//} else {
	//	DEBUG(resizer)("JPEG APP%d marker, length %zd skipped.\n", cinfo->unread_marker - JPEG_APP0, len);
	//}
	(*cinfo->src->skip_input_data)(cinfo, len);
	return TRUE;
}

// use libjpeg to load the image scaled 1/2, 1/4 or 1/8 as needed
gdImagePtr resize_jpeg(const unsigned char* data, size_t len, const image_info* info, unsigned int thu_x, unsigned int thu_y) {
	unsigned int scale_bits = find_scale_bits(info->width, info->height, thu_x, thu_y);
	if (!scale_bits) return NULL;

	DEBUG(resizer)("Loading JPEG rescaled to 1/%d.\n", 1<<scale_bits);

	jpeg_decompress_struct cinfo;
	jpeg_error jerr;

	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = jpeg_error_exit;
	jerr.emit_message = jpeg_warning;

	AutoGDImage img;

	bool created = 0;

	try {

	if (setjmp(jerr.handler)) {
		char msg[1024];
		(*jerr.info->err->format_message)(jerr.info, msg);
		throw imgdb::image_error(std::string(msg));
	}

	jpeg_create_decompress(&cinfo);
	created = 1;

	jpeg_data_reader reader(data, len);
	cinfo.err->trace_level = 0;
	cinfo.src = &reader;

	// skip all unhandled APP markers
	for (int i = JPEG_APP0+1; i <= JPEG_APP0+15; i++)
		if (i != JPEG_APP0+14)
			jpeg_set_marker_processor(&cinfo, i, skip_jpeg_marker);

	//fprintf(stderr, "reading header... ");
	jpeg_read_header(&cinfo, TRUE);

	cinfo.scale_num = 1;
	cinfo.scale_denom = 1<<scale_bits;
	cinfo.out_color_space = JCS_RGB;

	//fprintf(stderr, "starting decompress... ");
	jpeg_start_decompress(&cinfo);

	//fprintf(stderr, "is %d x %d x %d... ", cinfo.output_width, cinfo.output_height, cinfo.output_components);
	if (cinfo.output_components != 3)
		throw imgdb::image_error("JPEG decompress returning wrong component number.");

	img.set(gdImageCreateTrueColor(cinfo.output_width, cinfo.output_height));
	if (!img) throw imgdb::simple_error("Out of memory.");

	AutoCleanArray<unsigned char> buffer(new unsigned char[cinfo.output_width * cinfo.output_components]);
	//fprintf(stderr, "reading %d rows...\n", cinfo.output_height);
	while (cinfo.output_scanline < cinfo.output_height) {
		//fprintf(stderr, "\rrow %d... ", cinfo.output_scanline);
		unsigned char* inrow = buffer.ptr();
		int* outrow = img->tpixels[cinfo.output_scanline];
		jpeg_read_scanlines(&cinfo, &inrow, 1);
		int* outrow_end = outrow + cinfo.output_width;
		while (outrow != outrow_end) {
			*outrow++ = gdTrueColor(inrow[0], inrow[1], inrow[2]);
			inrow += 3;
		}
	}
	//fprintf(stderr, "\ndone! ");

	//fprintf(stderr, "finishing decompress... ");
	jpeg_finish_decompress(&cinfo);

	} catch (imgdb::simple_error& e) {
		// Nothing to do, just return however much we have of the image.
		DEBUG(warnings)("resize_jpeg caught %s: %s\n", e.type(), e.what());

	} catch (std::exception& e) {
		if (created) jpeg_destroy_decompress(&cinfo);
		throw;
	}

	//fprintf(stderr, "destroying decompress... ");
	if (created) jpeg_destroy_decompress(&cinfo);

	//fprintf(stderr, "returning image!\n");
	return img.detach();
}

struct png_mem_info {
	png_mem_info(const unsigned char* data, size_t len) : m_data(data), m_len(len) { }

	static void read(png_structp read_ptr, png_bytep out, png_size_t len);

private:
	const unsigned char* m_data;
	size_t m_len;
};

void png_mem_info::read(png_structp read_ptr, png_bytep out, png_size_t len) {
	png_mem_info* info = (png_mem_info*) png_get_io_ptr(read_ptr);
	//fprintf(stderr, "Returning %zd/%zd bytes from %p to %p.\n", len, info->m_len, info->m_data, out);
	if (len > info->m_len) png_error(read_ptr, "Reached end of PNG data.");
	memcpy(out, info->m_data, len);
	info->m_data += len;
	info->m_len -= len;
}

struct png_error_handler {
	static void error(png_structp png, const char *error);
	static void warning(png_structp png, const char *warning);

	const char* msg;
};

void png_error_handler::error(png_structp png, const char *error) {
	png_error_handler* handler = (png_error_handler*)png_get_error_ptr(png);
	handler->msg = error;
	//fprintf(stderr, "PNG error @%p: %s\n", handler, handler->msg);
	longjmp(png_jmpbuf(png), 1);
}
void png_error_handler::warning(png_structp png, const char *warning) {
	DEBUG(warnings)("PNG warning: %s\n", warning);
}

struct AutoPNG {
	AutoPNG();
	~AutoPNG() { if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL); png = NULL; info = NULL; }

	png_uint_32 row_bytes(){ return png_get_rowbytes(png, info); }

	void read_info();
	void setup_trans();
//	static unsigned int alpha_blend(unsigned int pixel, unsigned int bg, unsigned char alpha);
	void scale(AutoGDImage& img, unsigned int scale_bits);
	void trunc(AutoGDImage& img, unsigned int scale_bits);

	png_structp png;
	png_infop   info;

	png_error_handler handler;

	png_uint_32 width, height;
	int bit_depth, color_type, interlace_method;

	bool has_alpha;

	static png_color_16 white_background;

	union pixel {
		int i;
		png_byte b[4];
	};
};

AutoPNG::AutoPNG() {
	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &handler, png_error_handler::error, png_error_handler::warning);
	if (!png) throw imgdb::simple_error("Out of memory.");

	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		throw imgdb::simple_error("Out of memory.");
	}

}

void AutoPNG::read_info() {
	png_read_info(png, info);
	png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace_method, NULL, NULL);
}

png_color_16 AutoPNG::white_background
	= { index: 0, red: ~png_uint_16(), green: ~png_uint_16(), blue: ~png_uint_16(), gray: ~png_uint_16() };

void AutoPNG::setup_trans() {
	png_set_palette_to_rgb(png);

	has_alpha = color_type & PNG_COLOR_MASK_ALPHA;
	if (png_get_valid(png, info, PNG_INFO_tRNS)) {
		//png_set_tRNS_to_alpha(png);
		//has_alpha = true;
	}

	if (bit_depth == 16) png_set_strip_16(png);
	if (bit_depth < 8) png_set_packing(png);

	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
#if PNG_LIBPNG_VER < 10209
#warning Using libpng before 1.2.9, 1/2/4 bit grayscale images will be broken.
#else
		if (bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
#endif
		png_set_gray_to_rgb(png);
	}

	png_color_16p image_background;
	if (png_get_bKGD(png, info, &image_background))
		png_set_background(png, image_background, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);
	else
		png_set_background(png, &white_background, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);

	png_set_filler(png, 0xff, PNG_FILLER_AFTER);

	png_read_update_info(png, info);
}

/*
inline unsigned int alpha_blend(unsigned int pixel, unsigned int bg, unsigned char alpha) {
	static const unsigned int maskA = 0x00ff00ff;
	static const unsigned int maskB = 0x0000ff00;

	// Because we divide by 256 (>>8), not 255, add the mask to round up.
	// This is equivalent to dividing by 255. XXX No it is not!!
	unsigned int A = (pixel & maskA) * alpha + (bg & maskA) * (255 - alpha) + maskA;
	unsigned int B = (pixel & maskB) * alpha + (bg & maskB) * (255 - alpha) + maskB;

	return (A & (maskA << 8) | B & (maskB << 8)) >> 8;
}
*/

void AutoPNG::scale(AutoGDImage& img, unsigned int scale_bits) {
	DEBUG(resizer)("Scaling non-interlaced PNG from %ldx%ld to %dx%d.\n", (long)width, (long)height, img->sx, img->sy);

	//AutoCleanArray<png_byte> row(new png_byte[(4 * width) >> scale_bits]);
	AutoCleanArray<png_byte> row(new png_byte[row_bytes()]);
	//AutoGDImage row(gdImageCreateTrueColor(row_bytes() / 4, 1));
	//if (!row) throw imgdb::simple_error("Out of memory.");
	if (setjmp(png_jmpbuf(png))) throw imgdb::image_error(std::string(handler.msg));

	//int channels = png_get_channels(png, info);
	//fprintf(stderr, "Image has %d channels now. %ld bytes per row. Doing %lx transformations.\n", channels, row_bytes(), png->transformations);

	size_t scale = 1 << scale_bits;
	size_t mask = scale - 1;
	for (size_t i = 0; i < height; i++) {
		if (png->row_number != i) DEBUG(errors)("ERROR: We are in row %zd but PNG is in %ld!\n", i, png->row_number);

	//	png_byte* in = (png_byte*) &(img->tpixels[row_y + (row_xinc == 1 ? 0 : 1)][row_x]);
		png_byte* in = row.ptr();
	//	png_byte* in = (png_byte*) row->tpixels[0];
		//fprintf(stderr, "\rrow %zd... at %p", i, in);
		png_read_row(png, in, NULL);
		if (i & mask) continue;
//for (size_t i = 0; i < row_xnum*4; i++) fprintf(stderr, " %02x", in[i]);

		int* out = &img->tpixels[i >> scale_bits][0];
		int* end = out + img->sx;

//		if (has_alpha) while (out != end) {
//			*out = alpha_blend(gdTrueColorAlpha(in[0], in[1], in[2], 0), 0xffffff, in[3]);
//			out += row_xinc;
//			in += 4;
//
//		} else
		size_t skip = 4 * scale;
		while (out != end) {
			*out++ = gdTrueColorAlpha(in[0], in[1], in[2], 0);
			in += skip;
		}
	}
	//fprintf(stderr, "\nDone.\n");
}

void AutoPNG::trunc(AutoGDImage& img, unsigned int scale_bits) {
	// First pass = 1/8, passes 1..3 = 1/4, passes 1..5 = 1/2.
	int need_passes = 7 - scale_bits * 2;
	DEBUG(resizer)("Loading interlaced %ldx%ld PNG to 1/%d, %d passes.\n", width, height, 1<<scale_bits, need_passes);

	// Could make the temp row less wide, but libpng accidentally
	// writes the full row_bytes width even on earlier passes.
	AutoCleanArray<png_byte> row(new png_byte[row_bytes()]);
	if (setjmp(png_jmpbuf(png))) throw imgdb::image_error(std::string(handler.msg));

	//int channels = png_get_channels(png, info);
	//fprintf(stderr, "Image has %d channels now. %ld bytes per row. Doing %lx transformations.\n", channels, row_bytes(), png->transformations);

	const int png_pass_start[] = {0, 4, 0, 2, 0, 1, 0};
	const int png_pass_inc[] = {8, 8, 4, 4, 2, 2, 1};
	const int png_pass_ystart[] = {0, 0, 4, 0, 2, 0, 1};
	const int png_pass_yinc[] = {8, 8, 8, 4, 4, 2, 2};

	for (int pass = 0; pass < need_passes; pass++) {
		size_t row_x = png_pass_start[pass] >> scale_bits;
		size_t row_xinc = png_pass_inc[pass] >> scale_bits;
		size_t row_xnum = (width + png_pass_inc[pass] - 1 - png_pass_start[pass]) / png_pass_inc[pass];

		size_t row_y = png_pass_ystart[pass] >> scale_bits;
		size_t row_yinc = png_pass_yinc[pass] >> scale_bits;
		size_t row_ynum = (height + png_pass_yinc[pass] - 1 - png_pass_ystart[pass]) / png_pass_yinc[pass];

		if ((int)row_xnum > img->sx) DEBUG(errors)("ERROR! row_xnum=%zd exceeds image width %d!\n", row_xnum, img->sx);

		//fprintf(stderr, "Starting pass %d, has %zd(%ld!) rows @%zd+%zd of %zd(%ld!) pixels @%zd+%zd.\n", pass, row_ynum, png->num_rows, row_y, row_yinc, row_xnum, png->iwidth, row_x, row_xinc);
		for (size_t i = 0; i < row_ynum; i++) {
			if (png->row_number != i) DEBUG(errors)("ERROR: We are in row %zd but PNG is in %ld!\n", i, png->row_number);

		//	png_byte* in = (png_byte*) &(img->tpixels[row_y + (row_xinc == 1 ? 0 : 1)][row_x]);
			png_byte* in = row.ptr();
			//fprintf(stderr, "\rrow %zd... at %p", i, in);
			png_read_row(png, in, NULL);
//for (size_t i = 0; i < row_xnum*4; i++) fprintf(stderr, " %02x", in[i]);

			int* out = &img->tpixels[row_y][row_x];
			int* end = out + row_xnum * row_xinc;

/*			if (has_alpha) while (out != end) {
				*out = alpha_blend(gdTrueColorAlpha(in[0], in[1], in[2], 0), 0xffffff, in[3]);
				out += row_xinc;
				in += 4;

			} else
*/
			while (out != end) {
				*out = gdTrueColorAlpha(in[0], in[1], in[2], 0);
				out += row_xinc;
				in += 4;
			}

			row_y += row_yinc;
		}
		//fprintf(stderr, "\nPass done.\n");
	}
}

// use libpng to load the image; scaled 1/2, 1/4 or 1/8 as needed
gdImagePtr resize_png(const unsigned char* data, size_t len, const image_info* info, unsigned int thu_x, unsigned int thu_y) {
	unsigned int scale_bits = find_scale_bits(info->width, info->height, thu_x, thu_y);
//	if (!scale_bits) return NULL;

	AutoGDImage img;

	try {

	AutoPNG png;
	if (setjmp(png_jmpbuf(png.png))) throw imgdb::image_error(std::string(png.handler.msg));

	png_mem_info pmi(data, len);
	png_set_read_fn(png.png, &pmi, &png_mem_info::read);

	png.read_info();
	png.setup_trans();

	png_uint_32 round_up = (1 << scale_bits) - 1;
	img.set(gdImageCreateTrueColor((png.width + round_up) >> scale_bits, (png.height + round_up) >> scale_bits));
	if (!img) throw imgdb::simple_error("Out of memory.");

	if (png.interlace_method == PNG_INTERLACE_NONE)
		png.scale(img, scale_bits);
	else if (png.interlace_method == PNG_INTERLACE_ADAM7)
		png.trunc(img, scale_bits);
	else
		return NULL;

	} catch (imgdb::simple_error& e) {
		// Nothing to do, just return however much we have of the image.
		DEBUG(warnings)("resize_png caught %s: %s\n", e.type(), e.what());
	}

	return img.detach();
}

resizer_result resize_image_data(const unsigned char* data, size_t len, unsigned int thu_x, unsigned int thu_y, bool allow_prescaled) {
	image_info info;
	get_image_info(data, len, &info);

	DEBUG(resizer)("Is %s %d x %d.\n", info.mime_type, info.width, info.height);

	if (thu_y == 0) {
		if (info.width > info. height) {
			thu_y = info.height * thu_x / info.width;
		} else {
			thu_y = thu_x;
			thu_x = info.width * thu_x / info.height;
		}
	}

	AutoCleanPtrF<gdImage, &gdImageDestroy> img;

	//fprintf(stderr, "Resizing to %d x %d.\n", thu_x, thu_y);
	if (allow_prescaled) switch (info.type) {
		case IMG_JPEG:
			img.set(resize_jpeg(data, len, &info, thu_x, thu_y));
			break;
		case IMG_PNG:
			img.set(resize_png(data, len, &info, thu_x, thu_y));
			break;
		case IMG_GIF:
		default:	// just handle these below
			break;
	}

	if (img && (debug_level & DEBUG_prescale)) {
		FILE *out = fopen("prescale.jpg", "wb");
		if (out) { gdImageJpeg(img, out, 95); fclose(out); }
	}

	if (img && (unsigned int)img->sx == thu_x && (unsigned int)img->sy == thu_y) return img.detach();

	// If that failed, or not prescaling, just load the image as-is.
	AutoCleanPtrF<gdImage, &gdImageDestroy> thu(gdImageCreateTrueColor(thu_x, thu_y));
	if (!thu) throw imgdb::simple_error("Out of memory.");

	if (!img) switch (info.type) {
		case IMG_JPEG:
			img.set(gdImageCreateFromJpegPtr(len, const_cast<unsigned char*>(data)));
			break;
		case IMG_PNG:
			img.set(gdImageCreateFromPngPtr(len, const_cast<unsigned char*>(data)));
			gdImageFilledRectangle(thu, 0, 0, thu_x, thu_y, gdTrueColor(255, 255, 255));
			break;
		case IMG_GIF:
			img.set(gdImageCreateFromGifPtr(len, const_cast<unsigned char*>(data)));
			gdImageFilledRectangle(thu, 0, 0, thu_x, thu_y, gdTrueColor(255, 255, 255));
			break;
		case IMG_BMP:
		case IMG_UNKNOWN:
			throw imgdb::image_error("Unknown image format.");
	};
	if (!img) throw imgdb::image_error("Could not read image.");

	if ((unsigned int)img->sx == thu_x && (unsigned int)img->sy == thu_y) return img.detach();

	gdImageCopyResampled(thu, img, 0, 0, 0, 0, thu_x, thu_y, img->sx, img->sy);
	DEBUG(terse)("Resized %s %d x %d via %d x %d to %d x %d.\n", info.mime_type, info.width, info.height, img->sx, img->sy, thu_x, thu_y);

	// Stop autocleaning thu, and return its value instead.
	return resizer_result(thu.detach(), img->sx, img->sy);
}

