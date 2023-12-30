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
LIBS="-lm -ldl -lpthread -lcrypto -lssl"

if [ "$(uname -o)" = "Msys" ]; then
  SO_EXT="dll"
  EXE_EXT=".exe"
  LIBS=$(echo "$LIBS" | sed "s/-ldl//g")
  LIBS="$LIBS -lws2_32 -lmswsock -lcrypt32"
fi

if [ "$DEBUG" = "true" ]; then
    DEFINES="$DEFINES -DS80_DEBUG=1"
    FLAGS="-O2 -ggdb"
else
    FLAGS="$FLAGS -march=native"
fi

if [ "$INFO" = "true" ]; then
    DEFINES="$DEFINES -S80_DEBUG_INFO=1"
fi

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"

arg="$1"

if [ "$1" = "clean" ]; then
  rm -rf bin/obj
  arg="$2"
fi

modtime() {
  if [ $(uname) = "FreeBSD" ]; then
    stat -f "%Sm" "$1"
  else
    stat -c %y "$1"
  fi
}

xmake() {
  cc="$1"
  flags="$2"
  libs="$3"
  static_libs="$4"
  outname="$5"
  obj_files=""
  if [ "$static_libs" = "-" ]; then
    static_libs=""
  fi

  for i in `seq 6 $#`; do
    file=$(eval echo \${$i})
    echo "> Compiling $file"
    dir=$(dirname "$file")
    mkdir -p "bin/obj/$dir"
    obj_file=$(echo "bin/obj/$file" | sed -e s/\\.c\$/.o/g | sed -e s/\\.cpp\$/.o/g)
    do_compile="1"
    # detect if the file exists and if it exists, check if .obj last modified date == .cpp last modified date
    if [ -f "$obj_file" ]; then
      script_time=$(modtime $file)
      obj_time=$(modtime $obj_file)
      if [ "$obj_time" = "$script_time" ]; then
        do_compile="0"
      fi
    fi
    # only recompile when .cpp was updated compared to .obj file
    if [ $do_compile -eq "1" ]; then
      $cc $flags -c $file -o "$obj_file"
      if [ -f "$obj_file" ]; then
        touch -r "$file" "$obj_file"
      fi
    fi
    obj_files="$obj_files $obj_file"
  done
  # link it
  if [ $(echo "$outname" | tail -c 3) = ".a" ]; then
    # if output binary ends with .a, link it as a library
    ar rcs "$outname" $obj_files
  else
    # otherwise link it as an executable or dynamic linked library
    $cc -o "$outname" $flags $obj_files $static_libs $libs
  fi
}

if [ ! -f "bin/lib80s.a" ]; then
  echo "Compiling lib80s"
  xmake "$CC" "$FLAGS $DEFINES" "$LIBS" "-" "bin/lib80s.a" \
      src/80s/80s.c src/80s/80s_common.c src/80s/80s_common.windows.c src/80s/dynstr.c src/80s/algo.c src/80s/crypto.c \
      src/80s/serve.epoll.c src/80s/serve.kqueue.c src/80s/serve.iocp.c
fi

FLAGS="$FLAGS -std=c++23 -Isrc/ -fcoroutines"

if [ ! -f "bin/template$EXE_EXT" ]; then
  echo "Compiling template compiler"
  xmake "$CXX" "$FLAGS" "$LIBS" "bin/lib80s.a" "bin/template$EXE_EXT" src/90s/httpd/template_compiler.cpp
fi

if [ "$arg" = "pages" ]; then
  # Detect all .htmls in webroot and compile them into separate .so/.dlls
  echo "Compiling webpages"
  WEB_ROOT="${WEB_ROOT:-src/90s/httpd/pages/}"
  FLAGS="$FLAGS $DEFINES -fPIC -shared"

  to_translate=$(find "$WEB_ROOT" -name *.html -type f | grep -v .priv.html)
  for file in $to_translate; do
    echo "> Translating $file"
    bin/template "$file" "$file" $(echo "$file" | sed  s/\\.html$/.html.cpp/g)
    touch -r "$file" "$(echo "$file" | sed  s/\\.html$/.html.cpp/g)"
  done

  to_compile=$(find "$WEB_ROOT" -name "*.html.cpp" -type f)
  for file in $to_compile; do
    outname=$(echo "$file" | sed s/\\.cpp$/.$SO_EXT/g)
    xmake "$CXX" "$FLAGS" "$LIBS" "-" "$outname" "$file"
  done

  # Detect all .cpps that aren't generated from .htmls and compile main lib
  to_compile=$(find "$WEB_ROOT" -name "*.cpp" -type f | grep -v ".html.cpp")
  if [ ! -z "$to_compile" ]; then
    echo "Compiling main.$SO_EXT"
    xmake "$CXX" "$FLAGS" "$LIBS" "bin/lib80s.a" "${WEB_ROOT}main.$SO_EXT" $to_compile
  fi
else
  echo "Compiling 90s web server"
  FLAGS="$FLAGS $DEFINES"
  xmake "$CXX" "$FLAGS" "$LIBS" "bin/lib80s.a" "$OUT" \
    src/90s/90s.cpp src/90s/afd.cpp src/90s/context.cpp \
    src/90s/httpd/environment.cpp src/90s/httpd/render_context.cpp src/90s/httpd/server.cpp \
    src/90s/util/util.cpp \
    src/90s/sql/mysql.cpp src/90s/sql/mysql_util.cpp \
    src/90s/mail/server.cpp src/90s/mail/indexed_mail_storage.cpp
fi