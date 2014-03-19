#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "imgdb.h"
#include "resizer.h"
#include "debug.h"

int debug_level = DEBUG_errors | DEBUG_base | DEBUG_summary;

int main(int argc, char** argv) {
	if (argc < 3) return 1;
	const char* thumb = argv[1];
	const char* outname = argv[2];
	int thudim = argc < 4 ? 0 : strtol(argv[3], NULL, 0);
	if (thudim < 8) thudim = 150;
	int qual = argc < 5 ? 0 : strtol(argv[4], NULL, 0);
	if (qual < 10) qual = 80;
	//fprintf(stderr, "%s -> %s %d %d\n", thumb, outname, thudim, qual);

	int fd = -1;
	unsigned char* map = NULL;
	size_t len = 0;

	try {
		struct stat st;
		if ((fd = open(thumb, O_RDONLY)) == -1 || fstat(fd, &st) ||
				(map = (unsigned char*)mmap(NULL, len = st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
			throw imgdb::io_errno(errno);

		if (map[0] == 'B' && map[1] == 'M') {
			fprintf(stderr, "Bitmap file, ignoring.\n");
			exit(64);
		}

		AutoGDImage thu(resize_image_data(map, len, thudim, 0, 12*thudim));

		FILE *out = fopen(outname, "wb");
		if (!out) throw imgdb::io_errno(errno);

		gdImageJpeg(thu, out, qual);
		fclose(out);


	} catch (std::exception& e) {
		fprintf(stderr, "Resizer caught exception, what=%s.\n", e.what());
		return 1;
	}

	if (map) munmap(map, len);
	if (fd != -1) close(fd);

	return 0;
}
