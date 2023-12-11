#!/usr/bin/env sh
DEFAULT_C_COMPILER="gcc"
DEFAULT_COMPILER="g++"
EXE_EXT=""
SO_EXT="so"

if ! [ -x "$(command -v $DEFAULT_COMPILER)" ]; then
  DEFAULT_COMPILER="clang++"
fi

if ! [ -x "$(command -v $DEFAULT_C_COMPILER)" ]; then
  DEFAULT_COMPILER="clang"
fi

FLAGS="-s -O2"
OUT="${OUT:-bin/90s}"
CXX="${CXX:-$DEFAULT_COMPILER}"
CC="${CC:-$DEFAULT_C_COMPILER}"

mkdir -p bin
mkdir -p bin/obj

DEFINES=""
LIBS="-lm -ldl -lpthread"

if [ "$(uname -o)" = "Msys" ]; then
  SO_EXT="dll"
  EXE_EXT=".exe"
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

if [ ! -f "bin/lib80s.a" ]; then
  echo "Compiling lib80s"
  $CC $FLAGS -c \
      src/80s.c src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
      src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c

  mv *.o bin/obj/

  ar rcs bin/lib80s.a bin/obj/*.o
fi

if [ ! -f "bin/template$EXE_EXT" ]; then
  echo "Compiling template compiler"
  $CXX $FLAGS -std=c++23 -Isrc/ modern/httpd/template_compiler.cpp bin/lib80s.a -O2 -o "bin/template$EXE_EXT"
fi

if [ "$1" = "pages" ]; then
  echo "Compiling webpages"
  find modern/httpd/pages -name *.html -type f -exec sh -c "bin/template {} {} \$(echo "{}" | sed  s/html/cpp/g)" \;
  find modern/httpd/pages/ -name *.cpp -type f -exec sh -c "$CXX $FLAGS -fPIC -shared -std=c++23 -fcoroutines -Isrc/ -Imodern/ {} bin/lib80s.a $DEFINES $LIBS -o \$(echo "{}" | sed  s/cpp/$SO_EXT/g)" \;
else
  echo "Compiling 90s web server"
  $CXX $FLAGS -std=c++23 -fcoroutines -Isrc/ \
    modern/90s.cpp modern/afd.cpp modern/context.cpp \
    modern/httpd/environment.cpp modern/httpd/render_context.cpp modern/httpd/server.cpp \
    bin/lib80s.a \
    $DEFINES \
    $LIBS \
    -o "$OUT"
fi