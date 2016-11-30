// Little program to test various imgdb operations.
// Compile with "make test-db" and then just run it,
// it needs a readable image in test.jpg to run.
// If it runs without throwing any exceptions, it all works fine!

#include <stdlib.h>
#include <stdio.h>
#include <tr1/unordered_map>
#include "delta_queue.h"
#include "debug.h"
#include "imgdb.h"

int debug_level = DEBUG_errors | DEBUG_base | DEBUG_summary | DEBUG_resizer | DEBUG_image_info;

static const char* fn = "test-db.idb";

class DeltaTest : public delta_queue {
public:
	static void test();
};

#define S std::string()+
std::string operator +(const std::string& str, size_t v) {
	std::string out;
	out.reserve(str.length() + 20);
	out.append(str);
	char buf[20];
	snprintf(buf, 20, "%zd", v);
	out.append(buf);
	return out;
}

void DeltaTest::test() {
	DeltaTest delta;
	std::vector<size_t> comp;

	printf("Testing delta queue...\nStoring");
	size_t last = 0;
	delta.reserve(100000);
	for (int i = 0; i < 100000; i++) {
		if (i < 300) {
			int c = 0;
			iterator itr;
			for (itr = delta.begin(); itr != delta.end() && c < i; ++itr, ++c);
			if (c != i) throw imgdb::internal_error(S"\nFailed! Reached end() at "+c+" not "+i+"!\n");
			if (itr != delta.end()) throw imgdb::internal_error(S"\nFailed! Did not reach end() at "+c+"/"+i+"!\n");
		}
		bool type = (rand() & 0xff) < 10;
		size_t val = 1 + (rand() & (type ? 0xffff: 0xfd)) + (type * 254);
		if ((val > 254) != type) throw imgdb::internal_error(S"Bad value! type="+type+" val="+val+"\n");
		last += val;
		if (i < 10) printf(" %zu=%zd ", last, val);
		delta.push_back(last);
		comp.push_back(last);
	}
	printf("\nFirst elements:"); for (int i = 0; i < 10; i++) printf(" %08zx", delta.m_base[i].full);
	printf("\n%zd values, %zd used in container (capacity %zd, %zd%%/%zd%%). Verifying... ", delta.size(), delta.m_base.size(), delta.m_base.capacity(), delta.m_base.capacity()*100/delta.size(), delta.m_base.size()*100/delta.size());
	iterator itr = delta.begin();
	std::vector<size_t>::iterator cItr = comp.begin();
	int i = 0;
	for (; itr != delta.end() && cItr != comp.end(); ++itr, ++cItr, ++i) {
		if (*itr != *cItr) throw imgdb::internal_error(S"\nFailed! Element "+i+" is "+*itr+" but should be "+*cItr+"!\n");
	}
	if (itr != delta.end()) throw imgdb::internal_error(S"\nFailed! Did not reach end at element "+i+"!\n");
	if (cItr != comp.end()) throw imgdb::internal_error(S"\nFailed! Reached end prematurely at element "+i+"!\n");
	printf("OK.\n");
}

inline Idx shuffle(Idx old, int add) {
	return (old < 0 ? -(-old + add - 1) % 16000 - 1 : (old + add - 1) % 16000 + 1);
}

typedef std::map<imgdb::imageId,bool> deleted_t;

imgdb::ImgData data, org;
imgdb::ImgData* make_data(int id) {
	data.id = id;
	for (int i = 0; i < NUM_COEFS; i++) {
		data.sig1[i] = shuffle(org.sig1[i], id);
		data.sig2[i] = shuffle(org.sig2[i], id);
		data.sig3[i] = shuffle(org.sig3[i], id);
	}
	for (int i = 1; i < 4; i++)
		data.avglf[i-1] = org.avglf[i-1] * (1000.0 * i / (1000.0 * i + id));
	data.width = 800 + id;
	data.height = 600 + id;
	return &data;
}

void check(imgdb::dbSpace* db, int range, const deleted_t& removed) {
	int error = 0;
	typedef std::tr1::unordered_map<imgdb::imageId, int> id_map;
	id_map ids;
	for (int i = 1; i <= range; i++)
		ids[i] = 0;
	for (deleted_t::const_iterator itr = removed.begin(); itr != removed.end(); ++itr)
		ids[itr->first] = 2;

	imgdb::imageId_list imgs = db->getImgIdList();
	for (imgdb::imageId_list::const_iterator itr = imgs.begin(); itr != imgs.end(); ++itr) {
		id_map::iterator img = ids.find(*itr);
		if (img == ids.end())
			error |= fprintf(stderr, "ERROR: DB returned unknown ID %08llx!\n", (long long)*itr);
		else if (img->second == 1)
			error |= fprintf(stderr, "ERROR: DB returned duplicate ID %08llx!\n", (long long)*itr);
		else if (img->second == 2)
			error |= fprintf(stderr, "ERROR: DB returned deleted ID %08llx!\n", (long long)*itr);
		else
			img->second = 1;
	}

	for (id_map::iterator itr = ids.begin(); itr != ids.end(); ++itr)
		if (itr->second == 0)
			error |= fprintf(stderr, "ERROR: DB did not return ID %08llx!\n", (long long) itr->first);

	if (error) throw imgdb::internal_error("Failed!");
}

void docheck(int range, int mode, const char* name, const deleted_t& removed) {
	imgdb::dbSpace* db = imgdb::dbSpace::load_file(fn, mode);
	fprintf(stderr, "OK, checking with %s... ", name);
	check(db, range, removed);
	fprintf(stderr, "OK.\n");
	delete db;
}

void query(imgdb::dbSpace* db, unsigned int id, const deleted_t& removed) {
	fprintf(stderr, "q%d ", id);
	imgdb::sim_vector res = db->queryImg(imgdb::queryArg(*make_data(id), 8, 0));
	bool shouldfail = removed.find(id) != removed.end();
	if (shouldfail != (res[0].id != id || res[0].width != 800+id || res[0].height != 600+id || imgdb::MakeScore(res[0].score) * 9 / 10)) {
		fprintf(stderr, "%s: id=%lld %dx%d %.1f\n", shouldfail ? "FOUND DELETED IMAGE" : "NOT FOUND", (long long) res[0].id, res[0].width, res[0].height, 1.0*res[0].score/imgdb::MakeScore(1));
		throw imgdb::internal_error("Failed!");
	}
}

#define CHECK(range, mode) docheck(range, imgdb::dbSpace::mode_ ## mode, #mode, removed)
#define DELETE(i) \
	{ fprintf(stderr, "-%lld ", (long long) i); \
	try { db->removeImage(i); removed[i]; } catch (const imgdb::invalid_id& e) { fprintf(stderr, "!! "); } }
#define ADD(i) \
	{ fprintf(stderr, "%lld ", (long long) i); \
	try { db->addImageData(make_data(i)); } catch (const imgdb::duplicate_id& e) { fprintf(stderr, "!! "); } }

int main() {
	DeltaTest::test();

	deleted_t removed;
	imgdb::dbSpace::imgDataFromFile("test.jpg", 0, &org);
	fprintf(stderr, "test.jpg avgl: %f %f %f\n", org.avglf[0], org.avglf[1], org.avglf[2]);
	if (!org.avglf[0] || !org.avglf[1] || !org.avglf[2]) { DEBUG(errors)("Image loading failed!\n"); exit(1); }
	unlink(fn);
	fprintf(stderr, "Creating new DB %s... ", fn);
	imgdb::dbSpace* db = imgdb::dbSpace::load_file(fn, imgdb::dbSpace::mode_alter);
	fprintf(stderr, "done.\n");
	fprintf(stderr, "Adding 10-10 images... ");
	for (int i = 1; i <= 10; i++) ADD(i);
	for (int i = 1; i <= 10; i++) DELETE(i);
	check(db, 10, removed);
	//fprintf(stderr, "Saving. ");
	//db->save_file(fn);
	delete db;
	fprintf(stderr, "Done.\n");
	CHECK(10, alter);
	CHECK(10, simple);
	removed.clear();
	db = imgdb::dbSpace::load_file(fn, imgdb::dbSpace::mode_alter);
	fprintf(stderr, "Adding 50-4+50 images... ");
	for (int i = 1; i <= 50; i++) ADD(i);
	DELETE(4); DELETE(27); DELETE(15); DELETE(48);
	ADD(24); DELETE(4); DELETE(27);
	for (int i = 51; i <= 100; i++) ADD(i);
	check(db, 100, removed);
	fprintf(stderr, "Saving. ");
	db->save_file(fn);
	delete db;
	fprintf(stderr, "Done.\n");
	CHECK(100, alter);
	CHECK(100, simple);
	db = imgdb::dbSpace::load_file(fn, imgdb::dbSpace::mode_alter);
	fprintf(stderr, "Deleting 20 adding 4 images... ");
	DELETE(16); DELETE(19); DELETE(21); DELETE(24); 
	DELETE(26); DELETE(29); DELETE(31); DELETE(34); 
	DELETE(36); DELETE(39); DELETE(41); DELETE(44); 
	DELETE(46); DELETE(49); DELETE(51); DELETE(54); 
	DELETE(66); DELETE(69); DELETE(71); DELETE(74); 
	ADD(101); ADD(102); ADD(103); ADD(104);
	check(db, 104, removed);
	//fprintf(stderr, "Saving. ");
	//db->save_file(fn);
	delete db;
	fprintf(stderr, "Done.\n");
	CHECK(104, simple);
	db = imgdb::dbSpace::load_file(fn, imgdb::dbSpace::mode_alter);
	check(db, 104, removed);
	fprintf(stderr, "Adding 2000-10+1-1 images... ");
	for (int i = 105; i <= 2100; i++) ADD(i);
	DELETE(4); DELETE(148); DELETE(2100); DELETE(2099);
	DELETE(204); DELETE(2000); DELETE(1999); DELETE(1489);
	DELETE(2099); DELETE(2001); DELETE(999); DELETE(2098);
	ADD(2101);
	DELETE(314);
	check(db, 2101, removed);
	db->save_file(fn);
	delete db;
	CHECK(2101, simple);
	db = imgdb::dbSpace::load_file(fn, imgdb::dbSpace::mode_simple);
	fprintf(stderr, "Querying... ");
	query(db, 1, removed); query(db, 314, removed); query(db, 2101, removed); query(db, 2000, removed);
	for (int i = 1; i < 50; i++) query(db, 1 + rand() % 2101, removed);
	fprintf(stderr, "\nOK. Adding/querying/removing/querying in simple mode.\n");
	removed[2103];query(db, 2103, removed);removed.erase(2103);
	ADD(2102); ADD(2103); ADD(2104); ADD(2102);
	query(db, 2103, removed); query(db, 2104, removed);
	DELETE(2103);
	query(db, 2103, removed);
	DELETE(2103);
	ADD(2103);removed.erase(2103);
	query(db, 2103, removed); query(db, 2104, removed);
	fprintf(stderr, "\nDone!\n");
}
