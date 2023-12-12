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

arg="$1"

if [ "$1" = "clean" ]; then
  rm -rf bin/obj
  arg="$2"
fi

xmake() {
  cc="$1"
  flags="$2"
  rem_libs="$3"
  outname="$4"
  obj_files=""
  if [ "$rem_libs" = "-" ]; then
    rem_libs=""
  fi

  for (( i=5; i <= "$#"; i++ )); do
    file="${!i}"
    echo "> Compiling $file"
    dir=$(dirname "$file")
    mkdir -p "bin/obj/$dir"
    obj_file=$(echo "bin/obj/$file" | sed -e s/\\.c\$/.o/g | sed -e s/\\.cpp\$/.o/g)
    do_compile="1"
    if [ -f "$obj_file" ]; then
      script_time=$(stat -c %y $file)
      obj_time=$(stat -c %y $obj_file)
      if [ "$obj_time" = "$script_time" ]; then
        do_compile="0"
      fi
    fi
    if [ $do_compile -eq "1" ]; then
      $cc $flags -c $file -o "$obj_file"
      if [ -f "$obj_file" ]; then
        touch -r "$file" "$obj_file"
      fi
    fi
    obj_files="$obj_files $obj_file"
  done
  # link it
  if [ "${outname: -2}" == ".a" ]; then
    ar rcs "$outname" $obj_files
  else
    $cc -o "$outname" $obj_files $rem_libs $flags
  fi
}

if [ ! -f "bin/lib80s.a" ]; then
  echo "Compiling lib80s"
  xmake "$CC" "$FLAGS $LIBS $DEFINES" "-" "bin/lib80s.a" \
      src/80s.c src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
      src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c
fi

FLAGS="$FLAGS -std=c++23 -Isrc/ -fcoroutines -Imodern/"

if [ ! -f "bin/template$EXE_EXT" ]; then
  echo "Compiling template compiler"
  xmake "$CXX" "$FLAGS" "bin/lib80s.a" "bin/template$EXE_EXT" modern/httpd/template_compiler.cpp
fi

if [ "$arg" = "pages" ]; then
  # Detect all .htmls in webroot and compile them into separate .so/.dlls
  echo "Compiling webpages"
  WEB_ROOT="${WEB_ROOT:-modern/httpd/pages/}"
  find "$WEB_ROOT" -name *.html -type f -exec sh -c "bin/template {} {} \$(echo "{}" | sed  s/\\.html$/.html.cpp/g)" \;
  to_compile=$(find "$WEB_ROOT" -name *.html.cpp -type f)

  FLAGS="$FLAGS $LIBS $DEFINES -fPIC -shared"
  for file in $to_compile; do
    outname=$(echo "$file" | sed s/\\.cpp$/.$SO_EXT/g)
    xmake "$CXX" "$FLAGS" "-" "$outname" "$file"
  done

  # Detect all .cpps that aren't generated from .htmls and compile main lib
  to_compile=$(find "$WEB_ROOT" -name *.cpp -type f | grep -v .html.cpp)
  if [ ! -z "$to_compile" ]; then
    echo "Compiling main$SO_EXT"
    xmake "$CXX" "$FLAGS" "bin/lib80s.a" "${WEB_ROOT}main$SO_EXT" $to_compile
  fi
else
  echo "Compiling 90s web server"
  FLAGS="$FLAGS $LIBS $DEFINES"
  xmake "$CXX" "$FLAGS" "bin/lib80s.a" "$OUT" \
    modern/90s.cpp modern/afd.cpp modern/context.cpp \
    modern/httpd/environment.cpp modern/httpd/render_context.cpp modern/httpd/server.cpp modern/util/util.cpp
fi