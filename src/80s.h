#ifndef __80S_H__
#define __80S_H__

void error(const char *msg);

#ifdef DEBUG
#  define dbg(message) printf("%s: %s\n", message, strerror(errno))
#else
#  define dbg(message)
#endif

#endif