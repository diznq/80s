#! /bin/bash
NUM_HCPU="$(nproc)"
NUM_CPU="$(($NUM_HCPU / 2))"

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

FLAGS="-s -Ofast"
OUT="${OUT:-bin/80s}"
CC="${CC:-gcc}"

if [[ "${JIT}" == "true" ]]; then

    if [[ -z "${LUA_JIT_LIB_PATH}" ]]; then
      LUA_LIB="/usr/local/lib/libluajit-5.1.a"
    else
      LUA_LIB="${LUA_JIT_LIB_PATH}"
    fi

    if [[ -z "${LUA_JIT_INC_PATH}" ]]; then
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

DEFINES="-DCRYPTOGRAPHIC_EXTENSIONS=true"
LIBS="-lm -ldl -lpthread -lcrypto"

if [[ "$NOCRYPTO" == "true" ]]; then
    DEFINES=$(echo "$DEFINES" | sed 's/-DCRYPTOGRAPHIC_EXTENSIONS=true//g')
    LIBS=$(echo "$LIBS" | sed 's/-lcrypto//g')
fi

if [[ "$IPV6" == "true" ]]; then
    DEFINES="$DEFINES -DALLOW_IPV6=1"
fi

if [[ "$DEBUG" == "true" ]]; then
    DEFINES="$DEFINES -DDEBUG=1"
    FLAGS="-O0 -g"
fi

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"
echo "Lua include directory: $LUA_INC"
echo "Lua library directory: $LUA_LIB"

$CC src/main.c src/lua.c src/lua_codec.c src/dynstr.c src/serve.epoll.c src/serve.kqueue.c "$LUA_LIB" \
    $DEFINES \
    "-I$LUA_INC" \
    $LIBS \
    $FLAGS -march=native \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -o "$OUT"
