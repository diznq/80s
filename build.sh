#!/usr/bin/env sh
INC_SEARCH_PATH="$LUA_INC_PATH /usr/include/lua.h /usr/include/lua5.4/lua.h /usr/local/include/lua54/lua.h /usr/local/include/lua.h /mingw64/include/lua.h"
JIT_INC_SEARCH_PATH="$LUA_INC_PATH /usr/local/include/luajit-2.1/lua.h /mingw64/include/luajit-2.1/lua.h"
SO_SEARCH_PATH="$LUA_LIB_PATH /usr/lib64/liblua.so /usr/local/lib/liblua-5.4.so /usr/lib/x86_64-linux-gnu/liblua5.4.so /mingw64/lib/liblua.dll.a"
JIT_SO_SEARCH_PATH="$LUA_LIB_PATH /usr/lib64/libluajit-5.1.so* /usr/local/lib/libluajit-5.1.so*"
LIB_SEARCH_PATH="$LUA_LIB_PATH /usr/local/lib/liblua.a /usr/local/lib/liblua-5.4.a /mingw64/lib/liblua.a"
JIT_LIB_SEARCH_PATH="$LUA_LIB_PATH /usr/local/lib/libluajit-5.1.a /mingw64/lib/libluajit-5.1.a /mingw64/lib/libluajit-5.1.dll.a"
SO_EXT="so"

if [ "$JIT" = "true" ]; then
  INC_SEARCH_PATH="$JIT_INC_SEARCH_PATH"
fi

if [ "$LINK" = "dynamic" ]; then
  if [ "$JIT" = "true" ]; then
    LIB_SEARCH_PATH="$JIT_SO_SEARCH_PATH"
  else
    LIB_SEARCH_PATH="$SO_SEARCH_PATH"
  fi
else
  if [ "$JIT" = "true" ]; then
    LIB_SEARCH_PATH="$JIT_LIB_SEARCH_PATH"
  fi
fi

for path in $INC_SEARCH_PATH; do
  if [ -f "$path" ]; then
    LUA_INC="$path"
    break
  fi
done

for path in $LIB_SEARCH_PATH; do
  if ls "$path" 1> /dev/null 2>&1; then
    LUA_LIB="$path"
  fi
done

if [ -z "$LUA_LIB" ]; then
  echo "error: failed to find lua library, searched $LIB_SEARCH_PATH"
  exit 1
fi

if [ -z "$LUA_INC" ]; then
  echo "error: failed to find lua include, searched $INC_SEARCH_PATH"
  exit 1
fi

if [ "$LINK" = "dynamic" ]; then
  LUA_LIB=$(echo "$LUA_LIB" | sed 's/.*[/]lib/-l/g' | sed 's/.so.*//g' | sed 's/.dll.*//g')
fi

FLAGS="-s -O3"
OUT="${OUT:-bin/80s}"
CC="${CC:-gcc}"

mkdir -p bin

DEFINES=""
LIBS="-lm -ldl -lpthread -lcrypto -lssl"

if [ "$(uname -o)" = "Msys" ]; then
  SO_EXT="dll"
  LIBS=$(echo "$LIBS" | sed "s/-ldl//g")
  LIBS="$LIBS -lws2_32 -lmswsock"
fi

if [ "$DEBUG" = "true" ]; then
    DEFINES="$DEFINES -DS80_DEBUG=1"
    FLAGS="-O0 -g"
fi

if [ "$JIT" = "true" ]; then
  DEFINES="$DEFINES -DS80_JIT=1"
fi

LUA_INC=$(echo "$LUA_INC" | sed 's/lua.h//g')

FLAGS="$FLAGS -march=native"

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"
echo "Lua include directory: $LUA_INC"
echo "Lua library directory: $LUA_LIB"

if [ "$LINK" = "dynamic" ]; then
  DEFINES="$DEFINES -DS80_DYNAMIC=1"
  DEFINES="$DEFINES -DS80_DYNAMIC_SO=\"$OUT.$SO_EXT\""
  $CC src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
      src/lua.c src/lua_net.c src/lua_codec.c src/lua_crypto.c \
      src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c \
      -shared -fPIC \
      $LUA_LIB \
      "-I$LUA_INC" \
      $DEFINES \
      $LIBS \
      $FLAGS \
      -o "$OUT.$SO_EXT"
  if [ ! "$SOONLY" = "true" ]; then
    $CC src/80s.c $DEFINES $FLAGS -fPIC $LUA_LIB $LIBS -o "$OUT"
  fi
else
  $CC src/80s.c src/80s_common.c src/80s_common.windows.c src/dynstr.c src/algo.c \
      src/lua.c src/lua_net.c src/lua_codec.c src/lua_crypto.c \
      src/serve.epoll.c src/serve.kqueue.c src/serve.iocp.c \
      "$LUA_LIB" \
      "-I$LUA_INC" \
      $DEFINES \
      $LIBS \
      $FLAGS \
      -o "$OUT"
fi