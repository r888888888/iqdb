/***************************************************************************\
    imglib.h - iqdb library internal definitions

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

/**************************************************************************\
Implementation notes:

The abstract dbSpace class is implemented as two different dbSpaceImpl
classes depending on a bool template parameter indicating whether the DB is
read-only or not. The "false" version has full functionality but is not
optimized for querying. The "true" version is read-only but has much faster
queries. The read-only version can optionally discard image data not needed
for querying from external image files to further reduce memory usage. This
is called "simple mode".

There is an additional "alter" version that modifies the DB file directly
instead of first loading it into memory, like the other modes do.

The advantage of this design is maximum code re-use for the two DB usage
patterns: maintenance and querying. Both implementation classes use
different variables, and specifically different iterators to iterate over
all images. The implementation details of the iterators are of course
different but the majority of the actual code is the same for both types.
\**************************************************************************/

#ifndef IMGDBLIB_H
#define IMGDBLIB_H

#include <list>

#include <fstream>
#include <iostream>

#if LIB_GD
#include "resizer.h"
#elif LIB_ImageMagick
#include <magick/api.h>
#else
#error Unsupported image library.
#endif

#include "auto_clean.h"
#include "delta_queue.h"
#include "haar.h"
#include "imgdb.h"

namespace imgdb {

typedef unsigned int uint;

// Weights for the Haar coefficients.
// Straight from the referenced paper:
const float weightsf[2][6][3]={
		// For scanned picture (sketch=0):
		//    Y      I      Q       idx total occurs
		{{ 5.00f, 19.21f, 34.37f},  // 0   58.58      1 (`DC' component)
			{ 0.83f,  1.26f,  0.36f},  // 1    2.45      3
			{ 1.01f,  0.44f,  0.45f},  // 2    1.90      5
			{ 0.52f,  0.53f,  0.14f},  // 3    1.19      7
			{ 0.47f,  0.28f,  0.18f},  // 4    0.93      9
			{ 0.30f,  0.14f,  0.27f}}, // 5    0.71      16384-25=16359

			// For handdrawn/painted sketch (sketch=1):
			//    Y      I      Q
			{{ 4.04f, 15.14f, 22.62f},
				{ 0.78f,  0.92f,  0.40f},
				{ 0.46f,  0.53f,  0.63f},
				{ 0.42f,  0.26f,  0.25f},
				{ 0.41f,  0.14f,  0.15f},
				{ 0.32f,  0.07f,  0.38f}}
};

union image_id_index {
	imageId id;
	size_t index;
	image_id_index(imageId i) : id(i) { }
	image_id_index(size_t ind, bool) : index(ind) { }
	image_id_index() { }
	bool operator==(const image_id_index& other) const { return id == other.id; }
	bool operator!=(const image_id_index& other) const { return id != other.id; }

	operator size_t&() { return index; }
	operator imageId&() { return id; }
};

typedef std::vector<image_id_index> IdIndex_list;

template<bool is_simple> struct map_iterator;
template<> struct map_iterator<false> : public std::iterator<std::forward_iterator_tag, image_id_index> { 
	map_iterator(image_id_index* p) : m_p(p) { }
	map_iterator() { }

	image_id_index*	operator->() const { return m_p; }
	image_id_index&	operator* () const { return *m_p; }
	map_iterator&	operator++() { ++m_p; return *this; }
	map_iterator	operator++(int) { return m_p++; }
	map_iterator&	operator--() { --m_p; return *this; }
	map_iterator	operator--(int) { return m_p--; }
	bool		operator!=(const map_iterator& other) { return m_p != other.m_p; }

	image_id_index* m_p;
};
#ifdef USE_DELTA_QUEUE
#ifdef USE_DISK_CACHE
#error Sorry, delta queue and disk cache cannot be used at the same time.
#endif
template<> struct map_iterator<true> : public delta_iterator {
	typedef delta_iterator base_type;

	map_iterator(const size_t* idx) : base_type((delta_value*)idx) { }
	map_iterator(const base_type& itr) : base_type(itr) { }
	map_iterator() { }

	size_t get_index() const { return **this; }
};
#else
template<> struct map_iterator<true> : public map_iterator<false> {
	typedef map_iterator<false> base_type;

	map_iterator(const size_t* idx) : base_type((image_id_index*)idx) { }
	map_iterator(const IdIndex_list::iterator& itr) : base_type(&*itr) { }
	map_iterator() { }

	size_t get_index() const { return (*this)->index; }
};
#endif

struct mapped_file {
	mapped_file() : m_base(NULL) { }
	mapped_file(void* base, size_t len) : m_base(base), m_length(len) { }
	mapped_file(const char* fname, bool writable);

	void unmap();

	void* m_base;
	size_t m_length;
};

template<bool is_simple>
struct imageIdIndex_map : public mapped_file {
	typedef map_iterator<is_simple> iterator;
	imageIdIndex_map() { }
	imageIdIndex_map(void* base, iterator img, iterator end, size_t l)
		: mapped_file(base, l), m_img(img), m_end(end) { }
	//image_id_index& operator[] (size_t ofs) { return m_img[ofs]; }
	//bool operator! () { return !m_base; }
	iterator begin() { return m_img; }
	iterator end() { return m_end; }
	//size_t size() { return m_end - m_img; }

	iterator m_img;
	iterator m_end;
};

// Automatically unmaps the imageIdIndex_map when it goes out of scope.
template<bool is_simple>
struct AutoImageIdIndex_map : public imageIdIndex_map<is_simple> {
	typedef imageIdIndex_map<is_simple> base_type;
	AutoImageIdIndex_map(const imageIdIndex_map<is_simple>& map) : base_type(map) { }
	~AutoImageIdIndex_map() { base_type::unmap(); }
};

#if LIB_ImageMagick
// Clean up Image when it goes out of scope.
struct ImagePtr {
	ImagePtr(Image* i) : m_image(i) { }
	void destroy() { if (m_image) DestroyImage(m_image); m_image=NULL; }
	bool operator! () { return !m_image; }
	operator Image* () { return m_image; }
	operator const Image* () const { return m_image; }
	Image* m_image;
};
typedef AutoClean<ImagePtr, &ImagePtr::destroy> AutoImage;

// Same for ExceptionInfo and ImageInfo.
struct AutoExceptionInfo : public ExceptionInfo {
	AutoExceptionInfo() { GetExceptionInfo(this); }
	~AutoExceptionInfo() { DestroyExceptionInfo(this); }
};
struct ImageInfoPtr {
	ImageInfoPtr() : m_info(CloneImageInfo(NULL)) { }
	void destroy() { DestroyImageInfo(m_info); }
	ImageInfo* operator->() { return m_info; }
	operator ImageInfo* () { return m_info; }
	operator const ImageInfo* () const { return m_info; }
	ImageInfo* m_info;
};
typedef AutoClean<ImageInfoPtr, &ImageInfoPtr::destroy> AutoImageInfo;
#endif

#if LIB_GD
typedef gdImage Image;
#endif

typedef std::pair<off_t, size_t> imageIdPage;

template<bool is_simple, bool is_memory> class imageIdIndex_list;

template<>
class imageIdIndex_list<true, true> {
public:
	static const size_t threshold = 0;
#ifdef USE_DELTA_QUEUE
	class container : public delta_queue {
	public:
		struct const_iterator : public delta_iterator {
			const_iterator(const delta_iterator& itr) : delta_iterator(itr) { }
			size_t get_index() const { return **this; }
		};
	};
#else
	class container : public IdIndex_list {
	public:
		struct const_iterator : public IdIndex_list::const_iterator {
			typedef IdIndex_list::const_iterator base_type;
			const_iterator(const base_type& itr) : base_type(itr) { }
			size_t get_index() const { return (*this)->index; }
		};
	};
#endif

	imageIdIndex_map<true> map_all(bool writable) { return writable ? imageIdIndex_map<true>(NULL, m_tail.begin(), m_tail.end(), 0) : imageIdIndex_map<true>(NULL, m_base.begin(), m_base.end(), 0); };
	bool empty() { return m_tail.empty() && m_base.empty(); }
	size_t size() { return m_tail.size() + m_base.size(); }
	void reserve(size_t num) { if (num > m_base.size()) m_tail.reserve(num - m_base.size()); }
	//void resize(size_t num) { if (num <= size()) return; m_tail.resize(num - m_base.size()); }
	void loaded(size_t num) { if (num != m_tail.size()) throw data_error("Loaded incorrect number."); }
	void set_base();
	void push_back(image_id_index i) { m_tail.push_back(i.index); }
	void remove(image_id_index i); // unimplemented.

	const container& tail() { return m_tail; }

	static int fd() { return -1; }

protected:
	container m_tail;
	container m_base;
};

template<bool is_simple>
class imageIdIndex_list<is_simple, false> {
public:
	static const size_t threshold = 128;

	class container : public IdIndex_list {
	public:
		struct const_iterator : public IdIndex_list::const_iterator {
			typedef IdIndex_list::const_iterator base_type;
			const_iterator(const base_type& itr) : base_type(itr) { }
			size_t get_index() const { return (*this)->index; }
		};
	};

	imageIdIndex_list() : m_size(0), m_capacity(0) { }

	imageIdIndex_map<is_simple> map_all(bool writable);
	bool empty() { return !m_size && m_tail.empty(); }
	size_t size() { return m_size + m_tail.size(); }
	void reserve(size_t num) { resize(num); }
	void resize(size_t num);
	void loaded(size_t num) { if (num > m_capacity) throw data_error("Loaded too many."); m_size = num; }
	void set_base() { }
	void push_back(image_id_index i) { m_tail.push_back(i); if (m_tail.size() >= threshold && can_page_out()) page_out(); }
	void remove(image_id_index i);
	void clear() { m_tail.clear(); m_size = 0; }

	const container& tail() { return m_tail; }

	static int fd() { return m_fd; }

protected:
	typedef std::vector<imageIdPage> page_list;

	bool can_page_out() { return !is_simple || m_size < m_capacity; }
	void page_out();

	static int m_fd;

	page_list m_pages;
	size_t m_size;
	size_t m_capacity;
	size_t m_baseofs;
	container m_tail;
};

class bloom_filter;

/* in memory signature structure */
class SigStruct : public image_info {
public:
	size_t index;		/* index into score array for queries */

	//int_hashset* keywords;

	size_t cacheOfs;

	SigStruct(size_t ofs) : cacheOfs(ofs) { };

	SigStruct() { };

	~SigStruct()
	{
		//delete keywords;
	}

	void init(const ImgData* nsig) {
		id = nsig->id;
		height = nsig->height;
		width = nsig->width;
		avglf2i(nsig->avglf, avgl);
	}
};

/*
class KwdFrequencyStruct {
public:
	int kwdId;
	long int freq;	

	KwdFrequencyStruct(int kwdId, long int freq): kwdId(kwdId),freq(freq) {}

	bool operator< (const KwdFrequencyStruct & right) const {
		return freq > (right.freq);
	}
};
*/

// typedefs
//typedef std::map<const int, imageId> kwdFreqMap;

template<bool is_simple>
class dbSpaceImpl;

inline Score get_aspect(int width, int height) { return 0; }

template<bool is_simple> class sigMap;

template<>
class sigMap<false> : public imageIdMap<SigStruct*> {
public:
	void add_sig(imageId id, SigStruct* sig) { (*this)[id] = sig; }
	void add_index(imageId id, size_t index) { throw usage_error("Only valid in read-only mode."); }
};

template<>
class sigMap<true> : public imageIdMap<size_t> {
public:
	void add_sig(imageId id, SigStruct* sig) { throw usage_error("Not valid in read-only mode."); }
	void add_index(imageId id, size_t index) { (*this)[id] = index; }
};

template<bool is_simple>
struct index_iterator;

// In normal mode, we have image data in the sigMap, so iterate over that.
template<>
struct index_iterator<false> : public sigMap<false>::iterator {
	typedef sigMap<false>::iterator base_type;
	index_iterator(const base_type& itr, dbSpaceImpl<false>& db) : base_type(itr) { }

	imageId id() const { return (*this)->first; }
	SigStruct* sig() const { return (*this)->second; }
	size_t index() const { return sig()->index; }
	const lumin_native& avgl() const { return sig()->avgl; }
	int width() const { return sig()->width; }
	int height() const { return sig()->height; }
	uint16_t set() const { return sig()->set; }
	uint16_t mask() const { return sig()->mask; }
	size_t cOfs() const { return sig()->cacheOfs; }
	Score asp() const { return get_aspect(sig()->width, sig()->height); }
};

// In simple mode, we have only the image_info data available, so iterate over that.
// In read-only mode, we additionally have the index into the image_info array in a sigMap.
// Using functions that rely on this in simple mode will throw a usage_error.
template<>
struct index_iterator<true> : public image_info_list::iterator {
	typedef image_info_list::iterator base_type;
	index_iterator(const base_type& itr, dbSpaceImpl<true>& db) : base_type(itr), m_db(db) { }
	index_iterator(const sigMap<true>::iterator& itr, dbSpaceImpl<true>& db);	// implemented below

	imageId id() const { return (*this)->id; }
	SigStruct* sig() const { throw usage_error("Not valid in read-only mode."); }
	size_t index() const;	// implemented below
	const lumin_native& avgl() const { return (*this)->avgl; }
	int width() const { return (*this)->width; }
	int height() const { return (*this)->height; }
	uint16_t set() const { return (*this)->set; }
	uint16_t mask() const { return (*this)->mask; }
	size_t cOfs() const { return index() * sizeof(ImgData); }
	Score asp() const { return 0; }

	dbSpaceImpl<true>& m_db;
};

// Iterate over a bucket of imageIdIndex values.
template<bool is_simple, typename B>
struct id_index_iterator;

// In normal mode, the imageIdIndex_map stores image IDs. Get the index from the dbSpace's linked SigStruct.
template<typename B>
struct id_index_iterator<false,B> : public B {
	typedef B base_type;
	id_index_iterator(const base_type& itr, dbSpaceImpl<false>& db) : base_type(itr), m_db(db) { }
	size_t index() const;	// Implemented below.

	dbSpaceImpl<false>& m_db;
};

// In read-only/simple mode, the imageIdIndex_map stores the index directly.
template<typename B>
struct id_index_iterator<true,B> : public B {
	typedef B base_type;
	id_index_iterator(const base_type& itr, dbSpaceImpl<true>& db) : base_type(itr) { }
	size_t index() const { return this->get_index(); };
};

// Simplify reading/writing stream data.
#define READER_WRAPPERS \
	template<typename T> \
	T read_size(int size) { union { char s[16]; T v; } dummy; dummy.v = 0; base_type::read(dummy.s, size); return dummy.v; } \
	\
	template<typename T> \
	T read() { T dummy; base_type::read((char*) &dummy, sizeof(T)); return dummy; } \
	\
	template<typename T> \
	void read(T* t) { base_type::read((char*) t, sizeof(T)); } \
	\
	template<typename T> \
	void read(T* t, size_t n) { base_type::read((char*) t, sizeof(T) * n); }
#define WRITER_WRAPPERS \
	template<typename T> \
	void write(const T& t) { base_type::write((char*) &t, sizeof(T)); } \
	\
	template<typename T> \
	void write(const T* t, size_t n) { base_type::write((char*) t, sizeof(T) * n); }

class db_ifstream : public std::ifstream {
public:
	typedef std::ifstream base_type;
	db_ifstream(const char* fname) : base_type(fname, std::ios::binary) { }
	READER_WRAPPERS
};

class db_ofstream : public std::ofstream {
public:
	typedef std::ofstream base_type;
	db_ofstream(const char* fname) : base_type(fname, std::ios::binary) { };
	WRITER_WRAPPERS
};

class db_fstream : public std::fstream {
public:
	typedef std::fstream base_type;
	db_fstream(const char* fname) : base_type(fname, std::ios::binary | std::ios::in | std::ios::out) { };
	READER_WRAPPERS
	WRITER_WRAPPERS
};

// DB space implementations.

// Common function used by all implementations.
class dbSpaceCommon : public dbSpace {
public:
	virtual void addImage(imageId id, const char* filename);
	virtual void addImageBlob(imageId id, const void *blob, size_t length);

	static void imgDataFromFile(const char* filename, imageId id, ImgData* img);
	static void imgDataFromBlob(const void* data, size_t data_size, imageId id, ImgData* img);

	static bool is_grayscale(const lumin_native& avgl);

	virtual bool isImageGrayscale(imageId id);
	virtual Score calcAvglDiff(imageId id1, imageId id2);
	virtual Score calcSim(imageId id1, imageId id2, bool ignore_color = false);
	virtual Score calcDiff(imageId id1, imageId id2, bool ignore_color = false);

	static const int mode_mask_readonly	= 0x01;
	static const int mode_mask_simple	= 0x02;
	static const int mode_mask_alter	= 0x04;

protected:
	virtual void getImgDataByID(imageId id, ImgData* img) = 0;
	virtual void getImgAvgl(imageId id, lumin_native& avgl) = 0;

	static void sigFromImage(Image* image, imageId id, ImgData* sig);

	template<typename B>
	class bucket_set {
	public:
		static const size_t count_0 = 3;	// Colors
		static const size_t count_1 = 2;	// Coefficient signs.
		static const size_t count_2 = 16384;	// Coefficient magnitudes.

		// Do some sanity checking to ensure the array is layed out how we think it is layed out.
		// (Otherwise the iterators will be broken.)
		bucket_set() {
			if ((size_t)(end() - begin()) != count() || (size_t)((char*)end() - (char*)begin()) != size() || size() != sizeof(*this))
				throw internal_error("bucket_set array packed badly.");
		}

		typedef B* iterator;
		typedef B colbucket[count_1][count_2];

		colbucket& operator[] (size_t ind) { return buckets[ind]; }
		B& at(int col, int coef, int* idxret = NULL);

		void add(const ImgData& img, count_t index);
		void remove(const ImgData& img);

		iterator begin() { return buckets[0][0]; }
		iterator end() { return buckets[count_0][0]; }

		static count_t count() { return count_0 * count_1 * count_2; }
		static count_t size() { return count() * sizeof(B); }

	private:
		colbucket buckets[count_0];
	};

private:
	void operator = (const dbSpaceCommon&);
};

// Specific implementations.
template<bool is_simple>
class dbSpaceImpl : public dbSpaceCommon {
public:
	dbSpaceImpl(bool with_struct);
	virtual ~dbSpaceImpl();

	virtual void save_file(const char* filename);

	// Image queries.
	virtual sim_vector queryImg(const queryArg& query);

	virtual void getImgQueryArg(imageId id, queryArg* query);

	// Stats.
	virtual size_t getImgCount();
	virtual stats_t getCoeffStats();
	virtual bool hasImage(imageId id);
	virtual int getImageHeight(imageId id);
	virtual int getImageWidth(imageId id);
	virtual imageId_list getImgIdList();
	virtual image_info_list getImgInfoList();

	// DB maintenance.
	virtual void addImageData(const ImgData* img);
	virtual void setImageRes(imageId id, int width, int height);

	virtual void removeImage(imageId id);
	virtual void rehash();

private:
#ifdef USE_DISK_CACHE
	static const bool is_memory = false;
#else
	static const bool is_memory = is_simple;
#endif

	typedef index_iterator<is_simple> imageIterator;
	typedef sigMap<is_simple> image_map;
	typedef typename image_map::iterator map_iterator;

	typedef id_index_iterator<is_simple, typename imageIdIndex_map<is_simple>::iterator> idIndexIterator;
	typedef id_index_iterator<is_simple, typename imageIdIndex_list<is_simple, is_memory>::container::const_iterator> idIndexTailIterator;

	friend struct index_iterator<is_simple>;
	friend struct id_index_iterator<is_simple, typename imageIdIndex_map<is_simple>::iterator>;
	friend struct id_index_iterator<is_simple, typename imageIdIndex_list<is_simple, is_memory>::container::const_iterator>;

	image_info_list& info() { return m_info; }
	imageIterator find(imageId i);

	virtual void load(const char* filename);
	virtual void load_stream_old(db_ifstream& f, uint version);

	bool skip_image(const imageIterator& itr, const queryArg& query);

	imageIterator image_begin();
	imageIterator image_end();

	void addSigToBuckets(const ImgData* nsig);

	void getImgDataByID(imageId id, ImgData* img) { *img = get_sig_from_cache(id); }
	void getImgAvgl(imageId id, lumin_native& avgl) { avgl = find(id).avgl(); }

	size_t get_sig_cache();
	ImgData get_sig_from_cache(imageId i);
	void write_sig_cache(size_t ofs, const ImgData* sig);
	void read_sig_cache(size_t ofs, ImgData* sig);

	template<int num_colors>
	sim_vector do_query(const queryArg q);

	int m_sigFile;
	size_t m_cacheOfs;

	image_map m_images;

	size_t m_nextIndex;
	image_info_list m_info;

	/* Lists of picture ids, indexed by [color-channel][sign][position], i.e.,
	   R=0/G=1/B=2, pos=0/neg=1, (i*NUM_PIXELS+j)
	 */

	struct bucket_type : public imageIdIndex_list<is_simple, is_memory> {
		typedef imageIdIndex_list<is_simple, is_memory> base_type;
		void add(image_id_index id, count_t index) { base_type::push_back(is_simple ? image_id_index(index, true) : image_id_index(id)); }
		void remove(image_id_index id) { base_type::remove(id); }
	};
	typedef bucket_set<bucket_type> buckets_t;
	buckets_t imgbuckets;
	bool m_bucketsValid;
};

// Directly modify DB file on disk.
class dbSpaceAlter : public dbSpaceCommon {
public:
	dbSpaceAlter(bool readonly);
	virtual ~dbSpaceAlter();

	virtual void save_file(const char* filename);

	// Image queries not supported.
	virtual sim_vector queryImg(const queryArg& query) { throw usage_error("Not supported in alter mode."); }
	virtual void getImgQueryArg(imageId id, queryArg* query) { throw usage_error("Not supported in alter mode."); }

	// Stats. Partially unsupported.
	virtual size_t getImgCount();
	virtual stats_t getCoeffStats() { throw usage_error("Not supported in alter mode."); }
	virtual bool hasImage(imageId id);
	virtual int getImageHeight(imageId id);
	virtual int getImageWidth(imageId id);
	virtual imageId_list getImgIdList();
	virtual image_info_list getImgInfoList() { throw usage_error("Not supported in alter mode."); }

	// DB maintenance.
	virtual void addImageData(const ImgData* img);
	virtual void setImageRes(imageId id, int width, int height);

	virtual void removeImage(imageId id);
	virtual void rehash();

protected:
	typedef imageIdMap<size_t> ImageMap;

	ImageMap::iterator find(imageId i);
	void getImgDataByID(imageId id, ImgData* img) { *img = get_sig(m_images.find(id)->second); }
	void getImgAvgl(imageId id, lumin_native& avgl) { image_info::avglf2i(get_sig(m_images.find(id)->second).avglf, avgl); }
	ImgData get_sig(size_t ind);

	virtual void load(const char* filename);

private:
	void operator = (const dbSpaceAlter&);

	void resize_header();
	void move_deleted();

	struct bucket_type {
		void add(image_id_index id, count_t index) { size++; }
		void remove(image_id_index id) { size--; }

		count_t size;
	};

	typedef std::vector<size_t> DeletedList;

	ImageMap m_images;
	db_fstream* m_f;
	std::string m_fname;
	offset_t m_hdrOff, m_sigOff, m_imgOff;
	typedef bucket_set<bucket_type> buckets_t;
	buckets_t m_buckets;
	DeletedList m_deleted;
	bool m_rewriteIDs;
	bool m_readonly;
};

/* signature structure */
static const unsigned int AVG_IMGS_PER_DBSPACE = 20000;

// Serialization constants
static const unsigned int	SRZ_V0_5_1			= 1;
static const unsigned int	SRZ_V0_6_0			= 2;
static const unsigned int	SRZ_V0_6_1			= 3;
static const unsigned int	SRZ_V0_7_0			= 8;
static const unsigned int	SRZ_V0_9_0			= 9;

// Variable size and endianness check
static const uint32_t		SRZ_V_SZ			= (sizeof(res_t)) |
								  (sizeof(count_t) << 5) |
								  (sizeof(offset_t) << 10) |
								  (sizeof(imageId) << 15) |
								  (3 << 20);	// never matches any of the above for endian check

static const uint32_t		SRZ_V_CODE			= (SRZ_V0_9_0) | (SRZ_V_SZ << 8);

/* keyword postings structure */
static const unsigned int	 AVG_IMGS_PER_KWD	= 1000;

/*
class keywordStruct {
	//std::vector<imageId> imgIds;	// img list
public:
	keywordStruct() {
		imgIdsFilter = new bloom_filter(AVG_IMGS_PER_KWD, 1.0/(100.0 * AVG_IMGS_PER_KWD),random_bloom_seed);
	}
	bloom_filter* imgIdsFilter;

	~keywordStruct()
	{
		delete imgIdsFilter;
	}
} ;

typedef std::map<const int, keywordStruct*> keywordsMapType;
typedef std::map<const int, keywordStruct*>::iterator  keywordsMapIterator;
*/

// clustering
/* cluster list structure */
struct clustersStruct {
	imageId id;			/* representative image id */
	std::vector<imageId> imgIds;	/* img list */
	double diameter;		
};

typedef std::list<clustersStruct> cluster_list;
typedef cluster_list::iterator cluster_listIterator;

std::vector<clustersStruct> getClusterDb(const int dbId, const int numClusters);
std::vector<clustersStruct> getClusterKeywords(const int dbId, const int numClusters,std::vector<int> keywords);

// summaries
bloom_filter* getIdsBloomFilter(const int dbId);

// util
// keywordStruct* getKwdPostings(int hash);

// Delayed implementations.
inline index_iterator<true>::index_iterator(const sigMap<true>::iterator& itr, dbSpaceImpl<true>& db)
  : base_type(db.info().begin() + itr->second), m_db(db) { }
inline size_t index_iterator<true>::index() const { return *this - m_db.info().begin(); }
template<typename B> inline size_t id_index_iterator<false, B>::index() const { return m_db.find((*this)->id).index(); }

} // namespace

#endif
