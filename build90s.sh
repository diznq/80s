#!/usr/bin/env sh

mkdir -p bin

CXX="${CXX:-g++}"
OUT="${OUT:-bin/90s}"
DEFINES=""
FLAGS="-O2 -s -std=c++20"
LIBS="-lm -ldl -lpthread -lcrypto"

if [ $(uname) == "SunOS" ]; then
  LIBS="$LIBS -lsocket -lnsl"
fi

if [ "$DEBUG" == "true" ]; then
    DEFINES="$DEFINES -DDEBUG=1"
    FLAGS="-O0 -g"
fi

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"

$CXX src/90s.cpp \
    src/80s.c src/80s_common.c src/dynstr.c src/algo.c \
    src/serve.epoll.c src/serve.kqueue.c src/serve.port.c \
    $LIBS $DEFINES $FLAGS \
    -march=native \
    -Wno-int-to-pointer-cast \
    -o "$OUT"
