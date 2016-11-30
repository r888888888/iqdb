/***************************************************************************\
    iqdb.cpp - iqdb server (database maintenance and queries)

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

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>

#ifdef MEMCHECK
#include <malloc.h>
#include <mcheck.h>
#endif

#include <algorithm>
#include <list>
#include <vector>

#include "auto_clean.h"
#define DEBUG_IQDB
#include "debug.h"
#include "imgdb.h"

int debug_level = DEBUG_errors | DEBUG_base | DEBUG_summary | DEBUG_connections | DEBUG_images | DEBUG_imgdb; // | DEBUG_dupe_finder; // | DEBUG_resizer;

static void die(const char fmt[], ...) __attribute__ ((format (printf, 1, 2))) __attribute__ ((noreturn));
static void die(const char fmt[], ...) {
	va_list args;

	va_start(args, fmt);
	fflush(stdout);
	vfprintf(stderr, fmt, args);
	va_end(args);

	exit(1);
}

class dbSpaceAuto : public AutoCleanPtr<imgdb::dbSpace> {
public:
	dbSpaceAuto() { };
	dbSpaceAuto(const char* filename, int mode) : AutoCleanPtr<imgdb::dbSpace>(loaddb(filename, mode)), m_filename(filename) { };
	dbSpaceAuto(const dbSpaceAuto& other) { if (other != NULL) throw imgdb::internal_error("Can't copy-construct dbSpaceAuto."); }

	void save() { (*this)->save_file(m_filename.c_str()); }
	void load(const char* filename, int mode) { this->set(loaddb(filename, mode)); m_filename = filename; }
	void clear() { this->set(NULL); }

	const std::string& filename() const { return m_filename; }

private:
	static imgdb::dbSpace* loaddb(const char* fn, int mode) {
		imgdb::dbSpace* db = imgdb::dbSpace::load_file(fn, mode);
		DEBUG(summary)("Database loaded from %s, has %zd images.\n", fn, db->getImgCount());
		return db;
	}

	std::string m_filename;
};

class dbSpaceAutoMap {
	typedef std::list<dbSpaceAuto> list_type;
	typedef std::vector<dbSpaceAuto*> array_type;

public:
	dbSpaceAutoMap(int ndbs, int mode, const char* const * filenames) {
		m_array.reserve(ndbs);
		while (ndbs--) (*m_array.insert(m_array.end(), &*m_list.insert(m_list.end(), dbSpaceAuto())))->load(*filenames++, mode);
	}

	dbSpaceAuto& at(unsigned int dbid, bool append = false) {
		while (append && size() <= dbid) m_array.insert(m_array.end(), &*m_list.insert(m_list.end(), dbSpaceAuto()));
		if (dbid >= size() || (!append && !*m_array[dbid])) throw imgdb::param_error("dbId out of range.");
		return *m_array[dbid];
	}

	dbSpaceAuto& operator [] (unsigned int dbid) {
		return *m_array[dbid];
	}

	size_t size() const { return m_array.size(); }

private:
	array_type m_array;
	list_type  m_list;
};

#ifdef INTMATH
#define ScD(x) ((double)(x)/imgdb::ScoreMax)
#define DScD(x) ScD(((x)/imgdb::ScoreMax))
#define DScSc(x) ((x) >> imgdb::ScoreScale)
#else
#define ScD(x) (x)
#define DScD(x) (x)
#define DScSc(x) (x)
#endif

// Minimum score to consider an image a relevant match. Starting from the end of the list
// (least similar), average and standard deviation are computed. When the std.dev. exceeds
// the min_stddev value, the minimum score is then avg + stddev * stddev_fract.
template<typename C>
imgdb::Score min_sim(const C& sim, imgdb::Score min_stddev, imgdb::Score stddev_frac) {
	imgdb::DScore min_sqd = (imgdb::DScore)min_stddev * min_stddev;
	imgdb::DScore sum = 0;
	imgdb::DScore sqsum = 0;
	int cnt = 0;
	if (sim.size() < 2) return -1;
	for (typename C::const_iterator itr = sim.end(); itr != sim.begin(); ) {
		--itr;
		if (itr->score < 0) continue;

		cnt++;
		sum += itr->score;
		sqsum += (imgdb::DScore)itr->score * itr->score;

		if (cnt < 2) continue;

		imgdb::DScore avg = sum / cnt;
		imgdb::DScore sqd = sqsum - sum * avg;
		//imgdb::Score stddev = lrint(sqrt((double)sqd/cnt));
		//imgdb::Score min_sim = avg + (((imgdb::DScore)stddev_frac * stddev) >> imgdb::ScoreScale);
//fprintf(stderr, "Got %d. sum=%.1f avg=%.1f sqsum=%.1f² stddev=%.1f.\n", cnt, ScD(sum), ScD(avg), ScD(sqrt(sqsum)), ScD(sqrt((double)sqd/cnt)));
		if (sqd > min_sqd * cnt) {
#ifdef INTMATH
			return avg + lrint((double)stddev_frac * sqrt(sqd/cnt) / imgdb::ScoreMax);
#else
			return avg + stddev_frac * sqrt(sqd/cnt);
#endif
		}
	}
	return -1;
}

template<typename C>
void stddev_limit(C& sim, uint mindev) {
	imgdb::Score min = min_sim(sim, imgdb::MakeScore(mindev), imgdb::MakeScore(1) / 2);
	if (min == -1)
		min = imgdb::MakeScore(90);

	for (typename C::iterator itr = sim.begin(); itr != sim.end(); ++itr)
		if (itr->score < min) {
			sim.erase(itr, sim.end());
			break;
		}
}

typedef std::list<imgdb::imageId> dupe_list;

struct dupe_result {
	dupe_result(imgdb::imageId id_) : id(id_), score(0) { }

	imgdb::imageId id;
	imgdb::Score score;

	bool operator< (const dupe_result& other) const { return score < other.score; }

};

class dupe_map : public imgdb::imageIdMap<dupe_list*> {
public:
	typedef imgdb::imageId imageId;

	typedef imgdb::imageIdMap<dupe_list*> base_type;
	typedef std::list<dupe_list> group_list;

	void link(imageId one, imageId two);

	group_list::const_iterator group_begin() const { return m_groups.begin(); }
	group_list::const_iterator group_end() const   { return m_groups.end(); }

private:
	iterator insert(imageId one, dupe_list* group);

	group_list m_groups;
};

inline dupe_map::iterator dupe_map::insert(imageId one, dupe_list* group) {
	group->push_back(one);
	return base_type::insert(std::make_pair(one, group)).first;
}

void dupe_map::link(imageId one, imageId two) {
	if (one == two) return;
///	if (one > two) std::swap(one, two);

	DEBUG(dupe_finder)("\nLinking %08" FMT_imageId" -> %08" FMT_imageId ": ", one, two);
	iterator itrOne = find(one);
	iterator itrTwo = find(two);
	if (itrOne == end()) {
		std::swap(one, two);
		std::swap(itrOne, itrTwo);
	}
	if (itrOne == end()) {
		dupe_list* group = &*m_groups.insert(m_groups.end(), dupe_list());
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "neither ID found, making new group %p.\n", group);
		insert(one, group);
		insert(two, group);
		return;

/*
	} else if (itrOne == end()) {
		if (one < *itrTwo->second.pparent) {
			DEBUG_CONT(dupe_finder)(DEBUG_OUT, "making %08lx new parent of %08lx.\n", one, two);
			insert(one, *itrTwo->second.pparent, itrTwo->second.pparent);
			*itrTwo->second.pparent = one;
			return;
		}
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "inserting in group %08lx of %08lx.\n", *itrTwo->second.pparent, two);
		dupe_entry& parent = at(*itrTwo->second.pparent);
		insert(one, parent.next, itrTwo->second.pparent);
		parent.next = one;
		return;
*/

	} else if (itrTwo == end()) {
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "inserting in group %p of %08" FMT_imageId ".\n", itrOne->second, one);
		if (itrOne->second->empty()) throw imgdb::internal_error("Group is empty!");
		insert(two, itrOne->second);
		return;

	} else if (itrOne->second == itrTwo->second) {
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "already grouped in %p.\n", itrOne->second);
		if (itrOne->second->empty()) throw imgdb::internal_error("Group is empty!");
		return;
	}

	// Both exist in different groups. Merge groups.
	dupe_list* gfrom = itrTwo->second;
	dupe_list* gto = itrOne->second;
	DEBUG_CONT(dupe_finder)(DEBUG_OUT, "merging group %p of %08" FMT_imageId " into %p of %08" FMT_imageId "...", gfrom, two, gto, one);
	if (gfrom->empty() || gto->empty()) throw imgdb::internal_error("Group is empty!");

	for (dupe_list::iterator lItr = gfrom->begin(); lItr != gfrom->end(); ++lItr) {
		iterator itr = find(*lItr);
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, " %08" FMT_imageId "(%p->%p)", *lItr, itr == end() ? NULL : itr->second, gto);
		if (itr == end()) throw imgdb::internal_error("Dupe link not found!");
		itr->second = gto;
	}

	gto->splice(gto->end(), *gfrom);
	if (!gfrom->empty() || gto->empty()) throw imgdb::internal_error("Wrong group is empty!");
/*
	dupe_list* group = itrOne->second.pparent;
	iterator mergeItr = find(*itrTwo->second.pparent);
	if (mergeItr == end()) throw imgdb::internal_error("Can't find parent to be merged.");
	if ((itrOne = find(*itrOne->second.pparent)) == end()) throw imgdb::internal_error("Parent not found!");
	if (itrOne->second.pparent < itrTwo->second.pparent) {
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "merging group %08lx of %08lx into %08lx of %08lx...",
						*itrOne->second.pparent, one, *itrTwo->second.pparent, two);
		pparent = itrOne->second.pparent;
		mergeItr = find(*itrTwo->second.pparent);
	} else {
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, "merging group %08lx of %08lx into %08lx of %08lx...",
						*itrTwo->second.pparent, two, *itrOne->second.pparent, one);
		pparent = itrTwo->second.pparent;
		mergeItr = find(*itrOne->second.pparent);
	}
	parent_list::iterator pItr = std::find_if(m_parents.begin(), m_parents.end(), std::bind2nd(std::equal_to<imageId>(), mergeItr->first));
	if (pItr == m_parents.end()) throw imgdb::internal_error("Can't find parent to be merged in list.");
	DEBUG_CONT(dupe_finder)(DEBUG_OUT, " erasing group %p=%08lx", &*pItr, *pItr);
	m_parents.erase(pItr);

	DEBUG_CONT(dupe_finder)(DEBUG_OUT, " %08lx(%p->%p)", mergeItr->first, mergeItr->second.pparent, pparent);
	mergeItr->second.pparent = pparent;
	while (mergeItr->second.next != none) {
		mergeItr = find(mergeItr->second.next);
		if (mergeItr == end()) break;
		DEBUG_CONT(dupe_finder)(DEBUG_OUT, " %08lx(%p->%p)", mergeItr->first, mergeItr->second.pparent, pparent);
		mergeItr->second.pparent = pparent;
	}

	mergeItr->second.next = itrOne->second.next;
	itrOne->second.next = mergeItr->first;
*/
}

void find_duplicates(const char* fn, int mindev) {
	/* for testing stddev code...
	int scores[] = { 84, 71, 67, 52, 43, 41, 40, 40, 39, 39, 39, 39, 38, 38, 38, 38, 38 };
	imgdb::sim_vector sim;
	for (unsigned int i =0; i < sizeof(scores)/sizeof(scores[0]); i++)
		sim.push_back(imgdb::sim_value(i, scores[i] << imgdb::ScoreScale, 0, 0));
	imgdb::Score m = min_sim(sim, 5 << imgdb::ScoreScale, imgdb::ScoreMax / 2);
fprintf(stderr, "Min score: %.1f\n", ScD(m));
	return;
	*/
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_readonly);

	dupe_map dupes;

	DEBUG(dupe_finder)("Finding std.dev=%d dupes from %zd images.\n", mindev, db->getImgCount());
	imgdb::imageId_list images = db->getImgIdList();
	for (imgdb::imageId_list::const_iterator itr = images.begin(); itr != images.end(); ++itr) {
		DEBUG(dupe_finder)("%3zd%%\r", 100*(itr - images.begin())/images.size());
		imgdb::sim_vector sim = db->queryImg(imgdb::queryArg(db, *itr, 16, 0)); // imgdb::dbSpace::flag_nocommon));

		imgdb::Score min = min_sim(sim, imgdb::MakeScore(mindev), imgdb::MakeScore(1) / 2);
//fprintf(stderr, "%08lx got %4.1f +/- %4.1f: ", *itr, ScD(min), 0.0);
//for (imgdb::sim_vector::iterator sItr = sim.begin(); sItr != sim.end(); ++sItr) fprintf(stderr, "%08lx:%4.1f ", sItr->id, ScD(sItr->score));
//fprintf(stderr, "\n");
		if (min < 0) continue;

		for (imgdb::sim_vector::iterator sItr = sim.begin(); sItr != sim.end() && sItr->score >= min; ++sItr)
			dupes.link(*itr, sItr->id);
	}

	typedef std::vector<dupe_result> out_list;
	typedef std::multimap<double, std::pair<imgdb::imageId, out_list> > lists_list;
	lists_list lists;

	for (dupe_map::group_list::const_iterator itr = dupes.group_begin(); itr != dupes.group_end(); ++itr) {
		if (itr->empty()) {
			DEBUG(dupe_finder)("Skipping empty group %p.\n", &*itr);
			continue;
		}

		out_list out;
		DEBUG(dupe_finder)("Processing group %p: ", &*itr);
		for (dupe_list::const_iterator dItr = itr->begin(); dItr != itr->end(); ++dItr) {
			DEBUG(dupe_finder)(" %08" FMT_imageId, *dItr);
			out.push_back(*dItr);
			dupe_map::iterator mItr = dupes.find(*dItr);
			if (mItr == dupes.end()) throw imgdb::internal_error("Could not find linked dupe.");
			if (mItr->second != &*itr) throw imgdb::internal_error("Linked dupe has wrong group!");
			dupes.erase(mItr);
		}
		//fprintf(stderr, "\nGetting scores:");
		for (out_list::iterator one = out.begin(); one != out.end(); ++one)
			for (out_list::iterator two = one + 1; two != out.end(); ++two) {
				//fprintf(stderr, " %08lx<->%08lx:", one->id, two->id);
				imgdb::Score score = db->calcSim(one->id, two->id, false);
				//fprintf(stderr, "%.1f", ScD(score));
				one->score += score;
				two->score += score;
			}
		std::make_heap(out.begin(), out.end());
		imgdb::imageId ref = out.front().id;
		std::pop_heap(out.begin(), out.end());
		out.pop_back();
		//fprintf(stderr, "\nReference: %08lx", ref);
		for (out_list::iterator one = out.begin(); one != out.end(); ++one) {
			imgdb::Score score = db->calcSim(one->id, ref, false);
			//fprintf(stderr, " %08lx<->%08lx:%.1f", one->id, ref, ScD(score));
			one->score = score;
		}
		std::make_heap(out.begin(), out.end());
		lists_list::iterator itr2 = lists.insert(std::make_pair(db->calcSim(out.front().id, ref, false), std::make_pair(ref, out_list())));
		itr2->second.second.swap(out);
		//fprintf(stderr, "Group %p has max similarity %.2f\n", &itr->second.second, itr->first);
	}

	for (lists_list::reverse_iterator itr = lists.rbegin(); itr != lists.rend(); ++itr) {
		//fprintf(stderr, "\nPrinting: %08lx", ref);
		printf("202 %08" FMT_imageId "=%.1f", itr->second.first, 0.0);
		//fprintf(stderr, "Showing group %p with similarity %.2f\n", &itr->second.second, itr->first);
		while (!itr->second.second.empty()) {
			//imgdb::Score score = db->calcSim(itr->second.second.front().id, itr->second.first, false);
			//fprintf(stderr, " %08lx<->%08lx:%.1f", out.front().id, ref, ScD(score));
			printf(" %08" FMT_imageId ":%.1f",itr->second.second.front().id, ScD(itr->second.second.front().score));
			std::pop_heap(itr->second.second.begin(), itr->second.second.end());
			itr->second.second.pop_back();
		}
		printf("\n");
	}

	if (!dupes.empty()) throw imgdb::internal_error("Orphaned dupe!");
}

void add(const char* fn) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_alter);
	while (!feof(stdin)) {
		char fn[1024];
		char line[1024];
		imgdb::imageId id;
		int width = -1, height = -1;
		if (!fgets(line, sizeof(line), stdin)) {
			DEBUG(errors)("Read error.\n");
			continue;
		}
		if (sscanf(line, "%" FMT_imageId " %d %d:%1023[^\r\n]\n", &id, &width, &height, fn) != 4  &&
		    sscanf(line, "%" FMT_imageId ":%1023[^\r\n]\n", &id, fn) != 2) {
			DEBUG(errors)("Invalid line %s\n", line);
			continue;
		}
		try {
			if (!db->hasImage(id)) {
				DEBUG(images)("Adding %s = %08" FMT_imageId "...\r", fn, id);
				db->addImage(id, fn);
			}
			if (width != -1 && height != -1)
				db->setImageRes(id, width, height);
		} catch (const imgdb::simple_error& err) {
                	DEBUG(errors)("%s: %s %s\n", fn, err.type(), err.what());
		}
	}
	db.save();
}

void list(const char* fn) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_alter);
	imgdb::imageId_list list = db->getImgIdList();
	for (imgdb::imageId_list::iterator itr = list.begin(); itr != list.end(); ++itr) printf("%08" FMT_imageId "\n", *itr);
}

void rehash(const char* fn) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_normal);
	db->rehash();
	db.save();
}

void stats(const char* fn) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_simple);
	size_t count = db->getImgCount();
	imgdb::stats_t stats = db->getCoeffStats();
	for (imgdb::stats_t::const_iterator itr = stats.begin(); itr != stats.end(); ++itr) {
		printf("c=%d\ts=%d\ti=%d\t%zd = %zd\n", itr->first >> 24, (itr->first >> 16) & 0xff, itr->first & 0xffff, itr->second, 100 * itr->second / count);
	}
}

void count(const char* fn) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_simple);
	printf("%zd images\n", db->getImgCount());
}

void query(const char* fn, const char* img, int numres, int flags) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_simple);
	imgdb::sim_vector sim = db->queryImg(imgdb::queryArg(img, numres, flags));
	for (size_t i = 0; i < sim.size(); i++)
		printf("%08" FMT_imageId " %lf %d %d\n", sim[i].id, ScD(sim[i].score), sim[i].width, sim[i].height);
}

void diff(const char* fn, imgdb::imageId id1, imgdb::imageId id2) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_readonly);
	double diff = db->calcDiff(id1, id2);
	printf("%08" FMT_imageId " %08" FMT_imageId " %lf\n", id1, id2, diff);
}

void sim(const char* fn, imgdb::imageId id, int numres) {
	dbSpaceAuto db(fn, imgdb::dbSpace::mode_readonly);
	imgdb::sim_vector sim = db->queryImg(imgdb::queryArg(db, id, numres, 0));
	for (size_t i = 0; i < sim.size(); i++)
		printf("%08" FMT_imageId " %lf %d %d\n", sim[i].id, ScD(sim[i].score), sim[i].width, sim[i].height);
}

enum event_t { DO_QUITANDSAVE };

#define DB dbs.at(dbid)

struct sim_db_value : public imgdb::sim_value {
	sim_db_value(const imgdb::sim_value& sim, int _db) : imgdb::sim_value(sim), db(_db) {}
	int db;
};
struct cmp_sim_high : public std::binary_function<sim_db_value,sim_db_value,bool> {
	bool operator() (const sim_db_value& one, const sim_db_value& two) { return two.score < one.score; }
};
struct query_t { unsigned int dbid, numres, flags; };

std::pair<char*, size_t> read_blob(const char* size_arg, FILE* rd) {
	size_t blob_size = strtoul(size_arg, NULL, 0);
	char* blob = new char[blob_size];
	if (fread(blob, 1, blob_size, rd) != blob_size)
		throw imgdb::param_error("Error reading literal image data");

	return std::make_pair(blob, blob_size);
}

DEFINE_ERROR(command_error,   imgdb::param_error)

void do_commands(FILE* rd, FILE* wr, dbSpaceAutoMap& dbs, bool allow_maint) {
	struct customOpt : public imgdb::queryOpt {
		customOpt() : mindev(0) {}

		uint mindev;
	} queryOpt;

	while (!feof(rd)) try {
		fprintf(wr, "000 iqdb ready\n");
		fflush(wr);

		char command[1024];
		if (!fgets(command, sizeof(command), rd)) {
			if (feof(rd)) {
				fprintf(wr, "100 EOF detected.\n");
				DEBUG(warnings)("End of input\n");
				return;
			} else if (ferror(rd)) {
				fprintf(wr, "300 File error %s\n", strerror(errno));
				DEBUG(errors)("File error %s\n", strerror(errno));
				return;
			} else {
				fprintf(wr, "300 Unknown file error.\n");
				DEBUG(warnings)("Unknown file error.\n");
			}
			continue;
		}
		//fprintf(stderr, "Command: %s", command);
		char *arg = strchr(command, ' ');
		if (!arg) arg = strchr(command, '\n');
		if (!arg) {
			fprintf(wr, "300 Invalid command: %s\n", command);
			continue;
		}

		*arg++ = 0;
		DEBUG(commands)("Command: %s. Arg: %s", command, arg);

		#ifdef MEMCHECK
		struct mallinfo mi1 = mallinfo();
		#endif
		#if MEMCHECK>1
		mtrace();
		#endif

		if (!strcmp(command, "quit")) {
			if (!allow_maint) throw imgdb::usage_error("Not authorized");
			fprintf(wr, "100 Done.\n");
			fflush(wr);
			throw DO_QUITANDSAVE;

		} else if (!strcmp(command, "done")) {
			return;

		} else if (!strcmp(command, "list")) {
			int dbid;
			if (sscanf(arg, "%i\n", &dbid) != 1) throw imgdb::param_error("Format: list <dbid>");
			imgdb::imageId_list list = DB->getImgIdList();
			for (size_t i = 0; i < list.size(); i++) fprintf(wr, "100 %08" FMT_imageId "\n", list[i]);

		} else if (!strcmp(command, "count")) {
			int dbid;
			if (sscanf(arg, "%i\n", &dbid) != 1) throw imgdb::param_error("Format: count <dbid>");
			fprintf(wr, "101 count=%zd\n", DB->getImgCount());

		} else if (!strcmp(command, "query_opt")) {
			char *opt_arg = strchr(arg, ' ');
			if (!opt_arg) throw imgdb::param_error("Format: query_opt <option> <arguments...>");
			*opt_arg++ = 0;
			if (!strcmp(arg, "mask")) {
				int mask_and, mask_xor;
				if (sscanf(opt_arg, "%i %i\n", &mask_and, &mask_xor) != 2) throw imgdb::param_error("Format: query_opt mask AND XOR");
				queryOpt.mask(mask_and, mask_xor);
				fprintf(wr, "100 Using mask and=%d xor=%d\n", mask_and, mask_xor);
			} else if (!strcmp(arg, "mindev")) {
				if (sscanf(opt_arg, "%u\n", &queryOpt.mindev) != 1) throw imgdb::param_error("Format: query_opt mindev STDDEV");
			} else {
				throw imgdb::param_error("Unknown query option");
			}

		} else if (!strcmp(command, "query")) {
			char filename[1024];
			int dbid, flags, numres;
			if (sscanf(arg, "%i %i %i %1023[^\r\n]\n", &dbid, &flags, &numres, filename) != 4)
				throw imgdb::param_error("Format: query <dbid> <flags> <numres> <filename>");

			std::pair<char*, size_t> blob_info = filename[0] == ':' ? read_blob(filename + 1, rd) : std::make_pair<char*, size_t>(NULL, 0);
			imgdb::sim_vector sim = DB->queryImg(blob_info.first ? imgdb::queryArg(blob_info.first, blob_info.second, numres, flags) : imgdb::queryArg(filename, numres, flags).coalesce(queryOpt));
			delete[] blob_info.first;
			if (queryOpt.mindev > 0)
				stddev_limit(sim, queryOpt.mindev);
			fprintf(wr, "101 matches=%zd\n", sim.size());
			for (size_t i = 0; i < sim.size(); i++)
				fprintf(wr, "200 %08" FMT_imageId " %lf %d %d\n", sim[i].id, ScD(sim[i].score), sim[i].width, sim[i].height);

			queryOpt.reset();

		} else if (!strcmp(command, "multi_query")) {
			int count;
			typedef std::vector<query_t> query_list;
			query_list queries;
			customOpt multiOpt = queryOpt;

			do {
				query_t query;
				if (sscanf(arg, "%i %i %i %n", &query.dbid, &query.flags, &query.numres, &count) != 3)
					throw imgdb::param_error("Format: multi_query <dbid> <flags> <numres> [+ <dbid2> <flags2> <numres2>...] <filename>");

				queries.push_back(query);
				arg += count;
			} while (arg[0] == '+' && ++arg);
			char* eol = strchr(arg, '\n'); if (eol) *eol = 0;

			imgdb::ImgData img;
			if (arg[0] == ':') {
				std::pair<char*, size_t> blob_info = read_blob(arg + 1, rd);
				imgdb::dbSpace::imgDataFromBlob(blob_info.first, blob_info.second, 0, &img);
				delete[] blob_info.first;
			} else {
				imgdb::dbSpace::imgDataFromFile(arg, 0, &img);
			}

			std::vector<sim_db_value> sim;
			imgdb::Score merge_min = imgdb::MakeScore(100);
			for (query_list::iterator itr = queries.begin(); itr != queries.end(); ++itr) {
				imgdb::sim_vector dbsim = dbs.at(itr->dbid)->queryImg(imgdb::queryArg(img, itr->numres + 1, itr->flags).merge(multiOpt));
				if (dbsim.empty()) continue;

				// Scale it so that DBs with different noise levels are all normalized:
				// Pull score of following hit down to 0%, keeping 100% a fix point, then
				// merge and sort list, and pull up 0% to the minimum noise level again.
				// This assumes that result numres+1 is indeed noise, so numres must not
				// be too small. And if we got fewer images than we requested, the DB
				// doesn't even have that many and hence the noise floor is zero.
				imgdb::Score sim_min = dbsim.back().score;
//fprintf(stderr, "DB %d: %zd/%d results, noise %f\n", itr->dbid, dbsim.size(), itr->numres, ScD(sim_min));
				if (dbsim.size() < itr->numres + 1)
					sim_min = 0;
				else
					dbsim.pop_back();

				merge_min = std::min(merge_min, sim_min);
#ifdef INTMATH
				imgdb::DScore slope = sim_min == 100 * imgdb::ScoreMax ? imgdb::ScoreMax : 
					(((imgdb::DScore)100 * imgdb::ScoreMax) << imgdb::ScoreScale) / (100 * imgdb::ScoreMax - sim_min);
#else
				imgdb::DScore slope = sim_min == 100 ? 1 : 100 / (100 - sim_min);
#endif
//fprintf(stderr, "Slope is %f=(100/100-%f)\n", ScD(slope), ScD(sim_min));
				imgdb::DScore offset = - slope * sim_min;
//fprintf(stderr, "Offset is %f=-%f*%f\n", DScD(offset), ScD(slope), ScD(sim_min));

				for (imgdb::sim_vector::iterator sitr = dbsim.begin(); sitr != dbsim.end(); ++sitr) {
//imgdb::Score o=sitr->score;
					sitr->score = DScSc(slope * sitr->score + offset);
//fprintf(stderr, "   %f -> %f\n", ScD(o), ScD(sitr->score));
					sim.push_back(sim_db_value(*sitr, itr->dbid));
				}
			}

			std::sort(sim.begin(), sim.end(), cmp_sim_high());
			imgdb::DScore slope = imgdb::MakeScore(1) - merge_min / 100;
			if (queryOpt.mindev > 0)
				stddev_limit(sim, queryOpt.mindev);
			fprintf(wr, "101 matches=%zd\n", sim.size());
			for (size_t i = 0; i < sim.size(); i++)
				fprintf(wr, "201 %d %08" FMT_imageId " %f %d %d\n", sim[i].db, sim[i].id, DScD((slope * sim[i].score)) + ScD(merge_min), sim[i].width, sim[i].height);

			queryOpt.reset();

		} else if (!strcmp(command, "sim")) {
			int dbid, flags, numres;
			imgdb::imageId id;
			if (sscanf(arg, "%i %i %i %" FMT_imageId "\n", &dbid, &flags, &numres, &id) != 4)
				throw imgdb::param_error("Format: sim <dbid> <flags> <numres> <imageId>");

			imgdb::sim_vector sim = DB->queryImg(imgdb::queryArg(DB, id, numres, flags).coalesce(queryOpt));
			if (queryOpt.mindev > 0)
				stddev_limit(sim, queryOpt.mindev);
			fprintf(wr, "101 matches=%zd\n", sim.size());
			for (size_t i = 0; i < sim.size(); i++)
				fprintf(wr, "200 %08" FMT_imageId " %lf %d %d\n", sim[i].id, ScD(sim[i].score), sim[i].width, sim[i].height);

			queryOpt.reset();

		} else if (!strcmp(command, "add")) {
			char fn[1024];
			imgdb::imageId id;
			int dbid;
			int width = -1, height = -1;
			if (sscanf(arg, "%d %" FMT_imageId " %d %d:%1023[^\r\n]\n", &dbid, &id, &width, &height, fn) != 5  &&
			    sscanf(arg, "%d %" FMT_imageId ":%1023[^\r\n]\n", &dbid, &id, fn) != 3)
				throw imgdb::param_error("Format: add <dbid> <imgid>[ <width> <height>]:<filename>");

			// Could just catch imgdb::param_error, but this is so common here that handling it explicitly is better.
			if (!DB->hasImage(id)) {
				fprintf(wr, "100 Adding %s = %d:%08" FMT_imageId "...\n", fn, dbid, id);
				DB->addImage(id, fn);
			}

			if (width > 0 && height > 0)
				DB->setImageRes(id, width, height);

		} else if (!strcmp(command, "remove")) {
			imgdb::imageId id;
			int dbid;
			if (sscanf(arg, "%d %" FMT_imageId, &dbid, &id) != 2)
				throw imgdb::param_error("Format: remove <dbid> <imgid>");

			fprintf(wr, "100 Removing %d:%08" FMT_imageId "...\n", dbid, id);
			DB->removeImage(id);

		} else if (!strcmp(command, "set_res")) {
			imgdb::imageId id;
			int dbid, width, height;
			if (sscanf(arg, "%d %" FMT_imageId " %d %d\n", &dbid, &id, &width, &height) != 4)
				throw imgdb::param_error("Format: set_res <dbid> <imgid> <width> <height>");

			fprintf(wr, "100 Setting %d:%08" FMT_imageId " = %d:%d...\r", dbid, id, width, height);
			DB->setImageRes(id, width, height);

		} else if (!strcmp(command, "list_info")) {
			int dbid;
			if (sscanf(arg, "%i\n", &dbid) != 1) throw imgdb::param_error("Format: list_info <dbid>");
			imgdb::image_info_list list = DB->getImgInfoList();
			for (imgdb::image_info_list::iterator itr = list.begin(); itr != list.end(); ++itr)
				fprintf(wr, "100 %08" FMT_imageId " %d %d\n", itr->id, itr->width, itr->height);

		} else if (!strcmp(command, "rehash")) {
			if (!allow_maint) throw imgdb::usage_error("Not authorized");
			int dbid;
			if (sscanf(arg, "%d", &dbid) != 1)
				throw imgdb::param_error("Format: rehash <dbid>");

			fprintf(wr, "100 Rehashing %d...\n", dbid);
			DB->rehash();

		} else if (!strcmp(command, "coeff_stats")) {
			int dbid;
			if (sscanf(arg, "%d", &dbid) != 1)
				throw imgdb::param_error("Format: coeff_stats <dbid>");

			fprintf(wr, "100 Retrieving coefficient stats for %d...\n", dbid);
			imgdb::stats_t stats = DB->getCoeffStats();
			for (imgdb::stats_t::iterator itr = stats.begin(); itr != stats.end(); ++itr)
				fprintf(wr, "100 %d %zd\n", itr->first, itr->second);

		} else if (!strcmp(command, "saveas")) {
			if (!allow_maint) throw imgdb::usage_error("Not authorized");
			char fn[1024];
			int dbid;
			if (sscanf(arg, "%d %1023[^\r\n]\n", &dbid, fn) != 2)
				throw imgdb::param_error("Format: saveas <dbid> <file>");

			fprintf(wr, "100 Saving DB %d to %s...\n", dbid, fn);
			DB.save();

		} else if (!strcmp(command, "load")) {
			if (!allow_maint) throw imgdb::usage_error("Not authorized");
			char fn[1024], mode[32];
			int dbid;
			if (sscanf(arg, "%d %31[^\r\n ] %1023[^\r\n]\n", &dbid, mode, fn) != 3)
				throw imgdb::param_error("Format: load <dbid> <mode> <file>");
			if ((size_t)dbid < dbs.size() && dbs[dbid])
				throw imgdb::param_error("Format: dbid already in use.");

			fprintf(wr, "100 Loading DB %d from %s...\n", dbid, fn);
			dbs.at(dbid, true).load(fn, imgdb::dbSpace::mode_from_name(mode));

		} else if (!strcmp(command, "drop")) {
			if (!allow_maint) throw imgdb::usage_error("Not authorized");
			int dbid;
			if (sscanf(arg, "%d", &dbid) != 1)
				throw imgdb::param_error("Format: drop <dbid>");

			DB.clear();
			fprintf(wr, "100 Dropped DB %d.\n", dbid);

		} else if (!strcmp(command, "db_list")) {
			for (size_t i = 0; i < dbs.size(); i++) if (dbs[i]) fprintf(wr, "102 %zd %s\n", i, dbs[i].filename().c_str());

		} else if (!strcmp(command, "ping")) {
			fprintf(wr, "100 Pong.\n");

		} else if (!strcmp(command, "debuglevel")) {
			if (strlen(arg))
				debug_level = strtol(arg, NULL, 16);
			fprintf(wr, "100 Debug level %x.\n", debug_level);

		} else if (command[0] == 0) {
			fprintf(wr, "100 NOP.\n");

		} else {
			throw command_error(command);
		}

		DEBUG(commands)("Command completed successfully.\n");

		#if MEMCHECK>1
		muntrace();
		#endif
		#ifdef MEMCHECK
		struct mallinfo mi2 = mallinfo();
		if (mi2.uordblks != mi1.uordblks) {
			FILE* f = fopen("memleak.log", "a");
			fprintf(f, "Command used %d bytes of memory: %s %s", mi2.uordblks - mi1.uordblks, command, arg);
			fclose(f);
		}
		#endif

	} catch (const imgdb::simple_error& err) {
		fprintf(wr, "301 %s %s\n", err.type(), err.what());
		fflush(wr);
	}
}

void command(int numfiles, char** files) {
	dbSpaceAutoMap dbs(numfiles, imgdb::dbSpace::mode_alter, files);

	try {
		do_commands(stdin, stdout, dbs, true);

	} catch (const event_t& event) {
		if (event != DO_QUITANDSAVE) return;
		for (int dbid = 0; dbid < numfiles; dbid++)
			DB.save();
	}

	DEBUG(commands)("End of commands.\n");
}

DEFINE_ERROR(network_error,   imgdb::base_error)

// Attach rd/wr FILE to fd and automatically close when going out of scope.
struct socket_stream {
	socket_stream() : socket(-1), rd(NULL), wr(NULL) { }
	socket_stream(int sock) : socket(-1), rd(NULL), wr(NULL) { set(sock); }
	void set(int sock) {
		close();

	  	socket = sock;
		rd = fdopen(sock, "r");
		wr = fdopen(sock, "w");

	  	if (sock == -1 || !rd || !wr) {
			close();
			throw network_error("Cannot fdopen socket.");
		}
	}
	~socket_stream() { close(); }
	void close() {
		if (rd) fclose(rd);
		rd=NULL;
		if (wr) fclose(wr);
		wr=NULL;
		if (socket != -1) ::close(socket);
		socket=-1;
	}

	int socket;
	FILE* rd;
	FILE* wr;
};

bool set_socket(int fd, struct sockaddr_in& bindaddr, int force) {
	if (fd == -1) die("Can't create socket: %s\n", strerror(errno));

	int opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) die("Can't set SO_REUSEADDR: %s\n", strerror(errno));
	if (bind(fd, (struct sockaddr*) &bindaddr, sizeof(bindaddr)) ||
	    listen(fd, 64)) {
		if (force) die("Can't bind/listen: %s\n", strerror(errno));
		DEBUG(base)("Socket in use, will replace server later.\n");
		return false;
	} else {
		DEBUG(base)("Listening on port %d.\n", ntohs(bindaddr.sin_port));
		return true;
	}
}

void rebind(int fd, struct sockaddr_in& bindaddr) {
	int retry = 0;
	DEBUG(base)("Binding to %08x:%d... ", ntohl(bindaddr.sin_addr.s_addr), ntohs(bindaddr.sin_port));
	while (bind(fd, (struct sockaddr*) &bindaddr, sizeof(bindaddr))) {
		if (retry++ > 60) die("Could not bind: %s.\n", strerror(errno));
		DEBUG_CONT(base)(DEBUG_OUT, "Can't bind yet: %s.\n", strerror(errno));
		sleep(1);
		DEBUG(base)("%s", "");
	}
	DEBUG_CONT(base)(DEBUG_OUT, "bind ok.\n");
	if (listen(fd, 64))
		die("Can't listen: %s.\n", strerror(errno));

	DEBUG(base)("Listening on port %d.\n", ntohs(bindaddr.sin_port));
}

void server(const char* hostport, int numfiles, char** files, bool listen2) {
	int port;
	char dummy;
	char host[1024];

	std::map<in_addr_t, bool> source_addr;
	struct addrinfo hints;
	struct addrinfo* ai;

	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

        int ret = sscanf(hostport, "%1023[^:]:%i%c", host, &port, &dummy);
	if (ret != 2) { strcpy(host, "localhost"); ret = 1 + sscanf(hostport, "%i%c", &port, &dummy); }
	if (ret != 2) die("Can't parse host/port `%s', got %d.\n", hostport, ret);

	int replace = 0;
	while (numfiles > 0) {
		if (!strcmp(files[0], "-r")) {
			replace = 1;
			numfiles--;
			files++;
		} else if (!strncmp(files[0], "-s", 2)) {
			struct sockaddr_in addr;
			if (int ret = getaddrinfo(files[0] + 2, NULL, &hints, &ai)) 
				die("Can't resolve host %s: %s\n", files[0] + 2, gai_strerror(ret));

			memcpy(&addr, ai->ai_addr, std::min<size_t>(sizeof(addr), ai->ai_addrlen));
			DEBUG(connections)("Restricting connections. Allowed from %s\n", inet_ntoa(addr.sin_addr));
			source_addr[addr.sin_addr.s_addr] = true;
			freeaddrinfo(ai);

			numfiles--;
			files++;
		} else {
			break;
		}
	}

	if (int ret = getaddrinfo(host, NULL, &hints, &ai)) die("Can't resolve host %s: %s\n", host, gai_strerror(ret));

	struct sockaddr_in bindaddr_low;
	struct sockaddr_in bindaddr_high;
	memcpy(&bindaddr_low, ai->ai_addr, std::min<size_t>(sizeof(bindaddr_low), ai->ai_addrlen));
	memcpy(&bindaddr_high, ai->ai_addr, std::min<size_t>(sizeof(bindaddr_high), ai->ai_addrlen));
	bindaddr_low.sin_port = htons(port);
	bindaddr_high.sin_port = htons(port - listen2);
	freeaddrinfo(ai);

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) die("Can't ignore SIGPIPE: %s\n", strerror(errno));

	int fd_high = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	int fd_low = listen2 ? socket(PF_INET, SOCK_STREAM, IPPROTO_TCP) : -1;
	int fd_max = listen2 ? std::max(fd_high, fd_low) : fd_high;
	bool success = set_socket(fd_high, bindaddr_high, !replace);
	if (listen2 && set_socket(fd_low, bindaddr_low, !replace) != success)
		die("Only one socket failed to bind, this is weird, aborting!\n");

	dbSpaceAutoMap dbs(numfiles, imgdb::dbSpace::mode_simple, files);

	if (!success) {
		int other_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (other_fd == -1)
			die("Can't create socket: %s.\n", strerror(errno));
		if (connect(other_fd, (struct sockaddr*) &bindaddr_high, sizeof(bindaddr_high))) {
			DEBUG(warnings)("Can't connect to old server: %s.\n", strerror(errno));
		} else {
		socket_stream stream(other_fd);
		DEBUG(base)("Sending quit command.\n");
		fputs("quit now\n", stream.wr); fflush(stream.wr);

		char buf[1024];
		while (fgets(buf, sizeof(buf), stream.rd))
			DEBUG(base)(" --> %s", buf);
		}

		if (listen2) rebind(fd_low, bindaddr_low);
		rebind(fd_high, bindaddr_high);
	}

	fd_set read_fds;
	FD_ZERO(&read_fds);

	while (1) {
		FD_SET(fd_high, &read_fds);
		if (listen2) FD_SET(fd_low,  &read_fds);

		int nfds = select(fd_max + 1, &read_fds, NULL, NULL, NULL);
		if (nfds < 1) die("select() failed: %s\n", strerror(errno));

		struct sockaddr_in client;
		socklen_t len = sizeof(client);

		bool is_high = FD_ISSET(fd_high, &read_fds);

		int fd = accept(is_high ? fd_high : fd_low, (struct sockaddr*) &client, &len);
		if (fd == -1) {
			DEBUG(errors)("accept() failed: %s\n", strerror(errno));
			continue;
		}

		if (!source_addr.empty() && source_addr.find(client.sin_addr.s_addr) == source_addr.end()) {
			DEBUG(connections)("REFUSED connection from %s:%d\n", inet_ntoa(client.sin_addr), client.sin_port);
			close(fd);
			continue;
		}

		DEBUG(connections)("Accepted %s connection from %s:%d\n", is_high ? "high priority" : "normal", inet_ntoa(client.sin_addr), client.sin_port);

		struct timeval tv = { 5, 0 };	// 5 seconds
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
		    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
			DEBUG(errors)("Can't set SO_RCVTIMEO/SO_SNDTIMEO: %s\n", strerror(errno));
		}

		socket_stream stream;

		try {
			stream.set(fd);
			do_commands(stream.rd, stream.wr, dbs, is_high);

		} catch (const event_t& event) {
			if (event == DO_QUITANDSAVE) return;

		} catch (const network_error& err) {
			DEBUG(connections)("Connection %s:%d network error: %s.\n",
				inet_ntoa(client.sin_addr), client.sin_port, err.what());

		// Unhandled imgdb::base_error means it was fatal or completely unknown.
		} catch (const imgdb::base_error& err) {
			fprintf(stream.wr, "302 %s %s\n", err.type(), err.what());
			fprintf(stderr, "Caught base_error %s: %s\n", err.type(), err.what());
			throw;

		} catch (const std::exception& err) {
			fprintf(stream.wr, "300 Caught unhandled exception!\n");
			fprintf(stderr, "Caught unhandled exception: %s\n", err.what());
			throw;
		}

		DEBUG(connections)("Connection %s:%d closing.\n", inet_ntoa(client.sin_addr), client.sin_port);
	}
}

void help() {
	printf(	"Usage: iqdb add|list|help args...\n"
		"\tadd dbfile - Read images to add in the form ID:filename from stdin.\n"
		"\tlist dbfile - List all images in database.\n"
		"\tquery dbfile imagefile [numres] - Find similar images.\n"
		"\tsim dbfile id [numres] - Find images similar to given ID.\n"
		"\tdiff dbfile id1 id2 - Compute difference between image IDs.\n"
		"\tlisten [host:]port dbfile... - Listen on given host/port.\n"
		"\thelp - Show this help.\n"
	);
	exit(1);
}

int main(int argc, char** argv) {
  try {
//	open_swap();
	if (argc < 2) help();

	if (!strncmp(argv[1], "-d=", 3)) {
		debug_level = strtol(argv[1]+3, NULL, 0);
		DEBUG(base)("Debug level set to %x\n", debug_level);
		argv++;
		argc--;
	}

	const char* filename = argv[2];
	int flags = 0;

	if (!strcasecmp(argv[1], "add")) {
		add(filename);
	} else if (!strcasecmp(argv[1], "list")) {
		list(filename);
	} else if (!strncasecmp(argv[1], "query", 5)) {
		if (argv[1][5] == 'u') flags |= imgdb::dbSpace::flag_uniqueset;

		const char* img = argv[3];
		int numres = argc < 6 ? -1 : strtol(argv[4], NULL, 0);
		if (numres < 1) numres = 16;
		query(filename, img, numres, flags);
	} else if (!strcasecmp(argv[1], "diff")) {
		imgdb::imageId id1 = strtoll(argv[3], NULL, 0);
		imgdb::imageId id2 = strtoll(argv[4], NULL, 0);
		diff(filename, id1, id2);
	} else if (!strcasecmp(argv[1], "sim")) {
		imgdb::imageId id = strtoll(argv[3], NULL, 0);
		int numres = argc < 6 ? -1 : strtol(argv[4], NULL, 0);
		if (numres < 1) numres = 16;
		sim(filename, id, numres);
	} else if (!strcasecmp(argv[1], "rehash")) {
		rehash(filename);
	} else if (!strcasecmp(argv[1], "find_duplicates")) {
		int mindev = argc < 4 ? 10 : strtol(argv[3], NULL, 0);
		if (mindev < 1 || mindev > 99) mindev = 10;
		find_duplicates(filename, mindev);
	} else if (!strcasecmp(argv[1], "command")) {
		command(argc-2, argv+2);
	} else if (!strcasecmp(argv[1], "listen")) {
		server(argv[2], argc-3, argv+3, false);
	} else if (!strcasecmp(argv[1], "listen2")) {
		server(argv[2], argc-3, argv+3, true);
	} else if (!strcasecmp(argv[1], "statistics")) {
		stats(filename);
	} else if (!strcasecmp(argv[1], "count")) {
		count(filename);
	} else {
		help();
	}

	//closeDbase();
	//fprintf(stderr, "database closed.\n");

  // Handle this specially because it means we need to fix the DB before restarting :(
  } catch (const imgdb::data_error& err) {
	DEBUG(errors)("Data error: %s.\n", err.what());
	exit(10);

  } catch (const imgdb::base_error& err) {
	DEBUG(errors)("Caught error %s: %s.\n", err.type(), err.what());
	if (errno) perror("Last system error");
  }

  return 0;
}
