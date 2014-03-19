#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "imgdb.h"
#include "resizer.h"

int debug_level = -1;

int main(int argc, char** argv) {
	if (argc < 3) exit(1);
	const char* thumb = argv[1];
	const char* outname = argv[2];
	int thudim = strtol(argv[3], NULL, 0);

	int fd = -1;
	unsigned char* map = NULL;
	size_t len = 0;

	try {
		struct stat st;
		if ((fd = open(thumb, O_RDONLY)) == -1 || fstat(fd, &st) ||
				(map = (unsigned char*)mmap(NULL, len = st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
			throw imgdb::io_errno(errno);

		fprintf(stderr, "Mapped %s at %p:%zd.\n", thumb, map, len);
		AutoGDImage thu(resize_image_data(map, len, thudim, 0, 12*thudim));

		FILE *out = fopen(outname, "wb");
		if (!out) throw imgdb::io_errno(errno);

		gdImageJpeg(thu, out, 95);
		fclose(out);

		fprintf(stderr, "OK:%d %d\n", thu->sx, thu->sy);

	} catch (std::exception& e) {
		fprintf(stderr, "Resizer caught exception, what=%s.\n", e.what());
	}

	if (map) munmap(map, len);
	if (fd != -1) close(fd);

	return 0;
}
