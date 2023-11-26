#!/usr/bin/env sh
DEFAULT_COMPILER="g++"

if ! type "$DEFAULT_COMPILER" &> /dev/null; then
  DEFAULT_COMPILER="clang++"
fi

FLAGS="-s -O3"
OUT="${OUT:-bin/90s}"
CC="${CC:-$DEFAULT_COMPILER}"

mkdir -p bin

DEFINES=""
LIBS="-lm -ldl -lpthread"

if [ "$(uname -o)" = "Msys" ]; then
  SO_EXT="dll"
  LIBS=$(echo "$LIBS" | sed "s/-ldl//g")
  LIBS="$LIBS -lws2_32 -lmswsock"
fi

if [ "$DEBUG" = "true" ]; then
    DEFINES="$DEFINES -DS80_DEBUG=1"
    FLAGS="-O0 -g"
fi

FLAGS="$FLAGS -march=native"

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"

CPPFLAGS="-std=c++20" $CC $FLAGS \
    src/80s.c src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
    src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c \
    modern/90s.cpp \
    $DEFINES \
    $LIBS \
    -o "$OUT"