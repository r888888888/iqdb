#ifndef IQDB_DEBUG_H
#define IQDB_DEBUG_H

#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#define DEBUG_OUT stderr
#define DEBUG(v) if (debug_level & DEBUG_ ## v) debug
#define DEBUG_CONT(v) if (debug_level & DEBUG_ ## v) fprintf
#define DEF_DEBUG(v, n) \
	static const int DEBUG_ ## v = 1 << n; \
	static const int DEBUG_ ## n = 1;

DEF_DEBUG(errors,	0)	// XX
DEF_DEBUG(base,		1)	// XX
DEF_DEBUG(summary,	2)	// XX
DEF_DEBUG(terse,	3)	// X
DEF_DEBUG(warnings,	4)

#ifdef DEBUG_DLER
DEF_DEBUG(transfers,	5)	// X
DEF_DEBUG(prot_err,	6)
DEF_DEBUG(connections,	7)
DEF_DEBUG(tasks,	8)

DEF_DEBUG(curl,		10)
DEF_DEBUG(sockets,	11)
DEF_DEBUG(timers,	12)
DEF_DEBUG(events,	13)
DEF_DEBUG(output,	14)
DEF_DEBUG(headers,	15)
DEF_DEBUG(redirects,	16)
DEF_DEBUG(requests,	17)
DEF_DEBUG(io,		18)
DEF_DEBUG(reads,	19)
DEF_DEBUG(protocol,	20)
DEF_DEBUG(iqdb,		21)
DEF_DEBUG(queryqueue,	27)
DEF_DEBUG(callbacks,    28)
DEF_DEBUG(queuestats,   29)
#endif
#ifdef DEBUG_IQDB
DEF_DEBUG(connections,	7)
DEF_DEBUG(images,	8)
DEF_DEBUG(dupe_finder,	10)
DEF_DEBUG(commands,	11)
#endif
#ifdef DEBUG_IPV6
DEF_DEBUG(connections,	7)
DEF_DEBUG(libevent,	10)
DEF_DEBUG(events,	13)
DEF_DEBUG(packets,	16)
DEF_DEBUG(io,		18)
DEF_DEBUG(addresses,	21)
#endif

DEF_DEBUG(resizer,	22)
DEF_DEBUG(image_info,	23)
DEF_DEBUG(prescale,	24)
DEF_DEBUG(imgdb,	25)
DEF_DEBUG(urlparse,	26)

static inline timeval now() { timeval tv; gettimeofday(&tv, NULL); return tv; }
static inline float elapsed(const timeval& from, const timeval& to = now()) {
	return (to.tv_sec - from.tv_sec) + ((float)(to.tv_usec - from.tv_usec))/1e6;
}

__attribute__ ((format (printf, 1, 2)))
void debug(const char fmt[], ...);

#endif // IQDB_DEBUG_H
