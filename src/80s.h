#ifndef __80S_H__
#define __80S_H__

void error(const char *msg);

#ifdef DEBUG
#  define dbg(message) puts(message)
#else
#  define dbg(message)
#endif

#endif