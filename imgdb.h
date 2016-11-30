/***************************************************************************\
    imgdb.h - iqdb library API

    Copyright (C) 2008 piespy@gmail.com

    Originally based on imgSeek code, these portions
    Copyright (C) 2003 Ricardo Niederberger Cabral.

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

#ifndef IMGDBASE_H
#define IMGDBASE_H

#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <math.h>

// STL includes
#include <map>
#include <stdexcept>
#include <vector>

// unordered_map may still be in TR1
#if defined(HAVE_UNORDERED_MAP)
#include <unordered_map>
#elif defined(HAVE_TR1_UNORDERED_MAP)
#include <tr1/unordered_map>
#endif

// Haar transform defines
#include "haar.h"

namespace imgdb {

/*
DB file layout.

Count	Size		Content
1	int32_t		DB file version and data size code
1	count_t		Number of images
1	offset_t	Offset to image signatures
98304	count_t		Bucket sizes
num_img	imageId		Image IDs
?	?		<unused space left for future image IDs up to above offset>
num_img	ImgData		Image signatures

When removing images would leave holes in the image signatures and they were
not filled by new images, signatures from the end will be relocated to fill
them. The file is not shrunk in anticipation of more images being added later.

When there is no more space for image IDs in the header, a number of
signatures are relocated from the front to the end of the file to mask space
for new image IDs.

When you need a printf statement to display a count_t, offset_t, res_t or imageId
value, use the FMT_count_t, FMT_offset_t, FMT_res_t and FMT_imageId macros
as format specifier, e.g. printf("%08" FMT_imageId, id).
*/


// Global typedefs and consts.
#ifdef FORCE_64BIT
typedef uint64_t imageId;
typedef uint64_t count_t;
typedef uint64_t offset_t;
typedef int64_t  res_t;

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#undef __STDC_FORMAT_MACROS

#define FMT_imageId PRIx64
#define FMT_count_t PRIu64
#define FMT_offset_t PRIu64
#define FMT_res_t PRId64
#else
typedef unsigned long int imageId;
typedef size_t count_t;
typedef off_t offset_t;
typedef int res_t;

#define FMT_imageId "lx"
#define FMT_count_t "zu"
#define FMT_offset_t "llu"
#define FMT_res_t "d"
#endif

// Exceptions.
class base_error : public std::exception {
public:
	~base_error() throw() { if (m_str) { delete m_str; m_str = NULL; } }
	const char* what() const throw() { return m_str ? m_str->c_str() : m_what; }
	const char* type() const throw() { return m_type; }

protected:
	base_error(const char* what, const char* type) throw() : m_what(what), m_type(type), m_str(NULL) { }
	explicit base_error(const std::string& what, const char* type) throw()
		: m_what(NULL), m_type(type), m_str(new std::string(what)) { }

	const char* m_what;
	const char* m_type;
	std::string* m_str;
};

#define DEFINE_ERROR(derived, base) \
	class derived : public base { \
	public: \
		derived(const char* what) throw() : base(what, #derived) { } \
		explicit derived(const std::string& what) throw() : base(what, #derived) { } \
	protected: \
		derived(const char* what, const char* type) throw() : base(what, type) { } \
		explicit derived(const std::string& what, const char* type) throw() : base(what, type) { } \
	};

// Fatal, cannot recover, should discontinue using the dbSpace throwing it.
DEFINE_ERROR(fatal_error,     base_error)

DEFINE_ERROR(io_error,        fatal_error)	// Non-recoverable IO error while read/writing database or cache.
DEFINE_ERROR(data_error,      fatal_error)	// Database has internally inconsistent data.
DEFINE_ERROR(memory_error,    fatal_error)	// Database could not allocate memory.
DEFINE_ERROR(internal_error,  fatal_error)	// The library code has a bug.

// Non-fatal, may retry the call after correcting the problem, and continue using the library.
DEFINE_ERROR(simple_error,    base_error)

DEFINE_ERROR(usage_error,     simple_error)	// Function call not available in current mode.
DEFINE_ERROR(param_error,     simple_error)	// An argument was invalid, e.g. non-existent image ID.
DEFINE_ERROR(image_error,     simple_error)	// Could not successfully extract image data from the given file.

// specific param_error exceptions
DEFINE_ERROR(duplicate_id,    param_error)	// Image ID already in DB.
DEFINE_ERROR(invalid_id,      param_error)	// Image ID not found in DB.

// io_error with a specific errno
class io_errno : public io_error {
public:
	io_errno(int code) throw() : io_error(NULL, "io_errno"), m_code(code) { }
	const char* what() const throw() { return strerror(m_code); }
	virtual int code() const throw() { return m_code; }

private:
	int m_code;
};

// and also a text
class io_errno_desc : public io_errno {
public:
	io_errno_desc(int code, const char* what) throw() : io_errno(code) { m_what = what; }
	const char* more() const throw() { return m_what; }
};

#ifdef INTMATH
// fixme: make Score and DScore proper classes with conversion operators
typedef int32_t Score;
typedef int64_t DScore;

const Score ScoreScale = 20;
const Score ScoreMax = (1 << ScoreScale);

template<typename T>
inline Score MakeScore(T i) { return i << imgdb::ScoreScale; }
#else
typedef float Score;
typedef float DScore;

template<typename T>
inline Score MakeScore(T i) { return i; }
#endif

typedef struct {
	Score v[3];
} lumin_native;

struct sim_value {
	sim_value(imageId i, Score s, unsigned int w, unsigned int h) : id(i), score(s), width(w), height(h) { }
	imageId id;
	Score score;
	unsigned int width, height;
};

struct image_info {
	image_info() { }
	image_info(imageId i, const lumin_native& a, int w, int h) : id(i), avgl(a), width(w), height(h) { }
	imageId id;
	lumin_native avgl;
	union {
		uint16_t width;
		uint16_t set;
	};
	union {
		uint16_t height;
		uint16_t mask;
	};

	static void avglf2i(const double avglf[3], lumin_native& avgl) {
		for (int c = 0; c < 3; c++) {
#ifdef INTMATH
			avgl.v[c] = lrint(ScoreMax * avglf[c]);
#else
			avgl.v[c] = avglf[c];
#endif
		}
	};

};

typedef std::vector<sim_value> sim_vector;
typedef std::vector<std::pair<uint32_t, size_t> > stats_t;
typedef std::vector<imageId> imageId_list;
typedef std::vector<image_info> image_info_list;
typedef Idx sig_t[NUM_COEFS];

#if defined(HAVE_UNORDERED_MAP)
template<typename T>
class imageIdMap : public std::unordered_map<imageId, T> {
};
#elif defined(HAVE_TR1_UNORDERED_MAP)
template<typename T>
class imageIdMap : public std::tr1::unordered_map<imageId, T> {
};
#else
template<typename T>
class imageIdMap : public std::map<imageId, T> {
};
#endif

struct ImgData {
	imageId id;			/* picture id */
	sig_t sig1;			/* Y positions with largest magnitude */
	sig_t sig2;			/* I positions with largest magnitude */
	sig_t sig3;			/* Q positions with largest magnitude */
	double avglf[3];		/* YIQ for position [0,0] */
	/* image properties extracted when opened for the first time */
	res_t width;			/* in pixels */
	res_t height;			/* in pixels */
};

class dbSpace;
class bloom_filter;
class db_ifstream;
class db_ofstream;

// Non-standard query arguments.
struct queryOpt {
	queryOpt(int fl = 0) : flags(fl), bfilter(NULL), mask_and(0), mask_xor(0) { }
	void filter(bloom_filter* bf) { bfilter = bf; }
	void mask(uint16_t maskAnd, uint16_t maskXor);
	void reset();

	int		flags;

	bloom_filter*	bfilter;
	uint16_t	mask_and;
	uint16_t	mask_xor;
};

// Standard query arguments.
struct queryArg : public queryOpt {
	queryArg(dbSpace* db, imageId id, unsigned int numres, int flags);
	queryArg(const ImgData& img, unsigned int numres, int flags);
	queryArg(const void* data, size_t data_size, unsigned int numres, int flags);
	queryArg(const char* filename, unsigned int numres, int flags);

	// Chainable modifier functions to set non-standard arguments.
	queryArg& filter(bloom_filter* bf) { queryOpt::filter(bf); return *this; }
	queryArg& mask(uint16_t maskAnd, uint16_t maskXor) { queryOpt::mask(maskAnd, maskXor); return *this; }

	// Copy, move and reset non-standard arguments.
	queryArg& merge(const queryOpt& q);
	queryArg& coalesce(queryOpt& q) { merge(q); q.reset(); return *this; }
	queryArg& reset() { queryOpt::reset(); return *this; }

	sig_t		sig[3];
	lumin_native	avgl;
	unsigned int	numres;
};

class dbSpace {
public:
	static const int mode_normal    = 0x00; // Full functionality, but slower queries.
	static const int mode_readonly	= 0x03; // Fast queries, cannot save back to disk.
	static const int mode_simple	= 0x02; // Fast queries, less memory, cannot save, no image ID queries.
	static const int mode_alter	= 0x04;	// Fast add/remove/info on existing DB file, no queries.
	static const int mode_imgdata	= 0x05;	// Similar to mode_alter, but read-only, to retrieve image data.

	// Image query flags.
	static const int flag_sketch	= 0x01;	// Image is a sketch, use adjusted weights.
	static const int flag_grayscale	= 0x02;	// Disregard color information from image.
		// unused at the moment	= 0x04;
	static const int flag_uniqueset	= 0x08;	// Return only best match from each set.
	static const int flag_nocommon	= 0x10;	// Disregard common coefficients (those which are present in at least 10% of the images).
	static const int flag_fast	= 0x20;	// Check only DC coefficient (luminance).

	// Used internally.
	static const int flags_internal	= 0xff000000;
	static const int flag_mask	= 0x10000000;	// Use AND and XOR masks, and only return image if result is zero.

	static int         mode_from_name(const char* mode);

	static dbSpace*    load_file(const char* filename, int mode);
	virtual void       save_file(const char* filename) = 0;

	virtual ~dbSpace();

	// Image queries.
	virtual sim_vector queryImg(const queryArg& query) = 0;

	// Image data.
	static void imgDataFromFile(const char* filename, imageId id, ImgData* img);
	static void imgDataFromBlob(const void* data, size_t data_size, imageId id, ImgData* img);

	// Initialize sig and avgl of the queryArg.
	virtual void getImgQueryArg(imageId id, queryArg* query) = 0;
	static void queryFromImgData(const ImgData& img, queryArg* query);

	// Stats.
	virtual size_t getImgCount() = 0;
	virtual stats_t getCoeffStats() = 0;
	virtual bool hasImage(imageId id) = 0;
	virtual int getImageHeight(imageId id) = 0;
	virtual int getImageWidth(imageId id) = 0;
	virtual bool isImageGrayscale(imageId id) = 0;
	virtual imageId_list getImgIdList() = 0;
	virtual image_info_list getImgInfoList() = 0;

	// DB maintenance.
	virtual void addImage(imageId id, const char* filename) = 0;
	virtual void addImageBlob(imageId id, const void *blob, size_t length) = 0;
	virtual void addImageData(const ImgData* img) = 0;
	virtual void setImageRes(imageId id, int width, int height) = 0;

	virtual void removeImage(imageId id) = 0;
	virtual void rehash() = 0;

	// Similarity.
	virtual Score calcAvglDiff(imageId id1, imageId id2) = 0;
	virtual Score calcSim(imageId id1, imageId id2, bool ignore_color = false) = 0;
	virtual Score calcDiff(imageId id1, imageId id2, bool ignore_color = false) = 0;

protected:
	dbSpace();

	virtual void load(const char* filename) = 0;

private:
	void operator = (const dbSpace&);
};

// Inline implementations.
inline void dbSpace::queryFromImgData(const ImgData& img, queryArg* query) {
	if (sizeof(query->sig) != ((char*)(img.sig3 + NUM_COEFS) - (char*)img.sig1))
		throw internal_error("query sigs and ImgData sigs packing differs.");

	memcpy(query->sig, img.sig1, sizeof(query->sig));
	image_info::avglf2i(img.avglf, query->avgl);
}

inline void queryOpt::mask(uint16_t maskAnd, uint16_t maskXor) {
	mask_and = maskAnd;
	mask_xor = maskXor;
	flags |= dbSpace::flag_mask;
}
inline void queryOpt::reset() {
	mask_and = mask_xor = 0;
	bfilter = NULL;
	flags = flags & ~dbSpace::flags_internal;
}
inline queryArg::queryArg(dbSpace* db, imageId id, unsigned int nr, int fl) : queryOpt(fl), numres(nr) {
	db->getImgQueryArg(id, this);
}
inline queryArg::queryArg(const ImgData& img, unsigned int nr, int fl) : queryOpt(fl), numres(nr) {
	dbSpace::queryFromImgData(img, this);
}
inline queryArg::queryArg(const char* filename, unsigned int nr, int fl) : queryOpt(fl), numres(nr) {
	ImgData img;
	dbSpace::imgDataFromFile(filename, 0, &img);
	dbSpace::queryFromImgData(img, this);
}
inline queryArg::queryArg(const void* data, size_t data_size, unsigned int nr, int fl) : queryOpt(fl), numres(nr) {
	ImgData img;
	dbSpace::imgDataFromBlob(data, data_size, 0, &img);
	dbSpace::queryFromImgData(img, this);
}
inline queryArg& queryArg::merge(const queryOpt& q) {
	mask_and = q.mask_and;
	mask_xor = q.mask_xor;
	bfilter = q.bfilter;
	flags = (flags & ~dbSpace::flags_internal) | (q.flags & dbSpace::flags_internal);
	return *this;
}

} // namespace

#endif
