#!/usr/bin/env sh

if [[ -z "${LUA_LIB_PATH}" ]]; then
  LUA_LIB="/usr/local/lib/liblua.a"
else
  LUA_LIB="${LUA_LIB_PATH}"
fi

if [[ -z "${LUA_INC_PATH}" ]]; then
  LUA_INC="/usr/local/include/"
else
  LUA_INC="${LUA_INC_PATH}"
fi

FLAGS="-s -O2"
OUT="${OUT:-bin/80s}"
CC="${CC:-gcc}"

if [ "${JIT}" == "true" ]; then

    if [ -z "${LUA_JIT_LIB_PATH}" ]; then
      LUA_LIB="/usr/local/lib/libluajit-5.1.a"
    else
      LUA_LIB="${LUA_JIT_LIB_PATH}"
    fi

    if [ -z "${LUA_JIT_INC_PATH}" ]; then
      LUA_INC="/usr/local/include/luajit-2.1/"
    else
      LUA_INC="${LUA_JIT_INC_PATH}"
    fi
fi

if [ ! -f "$LUA_INC/lua.h" ]; then
  LUA_INC="/run/host$LUA_INC"
  LUA_LIB="/run/host$LUA_LIB"
  if [ ! -f "$LUA_INC/lua.h" ]; then
    echo "failed to locate lua.h in $LUA_INC"
    exit 1
  fi
fi

mkdir -p bin

DEFINES=""
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
echo "Lua include directory: $LUA_INC"
echo "Lua library directory: $LUA_LIB"

$CC src/main.c src/dynstr.c \
    src/lua.c src/lua_codec.c src/lua_crypto.c \
    src/serve.epoll.c src/serve.kqueue.c src/serve.port.c \
    "$LUA_LIB" \
    $DEFINES \
    "-I$LUA_INC" \
    $LIBS \
    $FLAGS -march=native \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -o "$OUT"
