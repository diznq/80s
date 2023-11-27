#!/usr/bin/env sh
DEFAULT_C_COMPILER="gcc"
DEFAULT_COMPILER="g++"

if ! [ -x "$(command -v $DEFAULT_COMPILER)" ]; then
  DEFAULT_COMPILER="clang++"
fi

if ! [ -x "$(command -v $DEFAULT_C_COMPILER)" ]; then
  DEFAULT_COMPILER="clang"
fi

FLAGS="-s -O3"
OUT="${OUT:-bin/90s}"
CXX="${CXX:-$DEFAULT_COMPILER}"
CC="${CC:-$DEFAULT_C_COMPILER}"

mkdir -p bin
mkdir -p bin/obj

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

$CC $FLAGS -c \
    src/80s.c src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
    src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c

mv *.o bin/obj/

ar rcs bin/lib80s.a bin/obj/*.o

CPPFLAGS="-std=c++20" $CXX $FLAGS bin/lib80s.a \
    modern/90s.cpp modern/afd.cpp modern/context.cpp \
    $DEFINES \
    $LIBS \
    -o "$OUT"