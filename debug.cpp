#include <stdarg.h>
#include <stdio.h>

#include "debug.h"

static int debug_day0 = 1000;
static int debug_days = -1;
extern int debug_level;

void debug(const char fmt[], ...) {
	char timestr[32];
	timeval t = now();
	time_t tm = time(NULL);
	struct tm* lt = localtime(&tm);

	debug_days += debug_day0 == lt->tm_yday ? 0 : debug_day0 < lt->tm_yday ? lt->tm_yday - debug_day0 : 1;
	debug_day0 = lt->tm_yday;

	timestr[sizeof(timestr)-1] = 0;
	size_t datelen = snprintf(timestr, sizeof(timestr)-1, "+%d ", debug_days);
	strftime(timestr + datelen, sizeof(timestr)-1-datelen, "%H:%M:%S", lt);
	fputs(timestr, DEBUG_OUT);
	snprintf(timestr, sizeof(timestr)-1, "%.3f ", ((float)t.tv_usec)/1e6);
	fputs(timestr+1, DEBUG_OUT);

	va_list	args;

	va_start(args, fmt);
	vfprintf(DEBUG_OUT, fmt, args);
	va_end(args);
}
