#!/usr/bin/env sh
if [ -z "${LUA_LIB_PATH}" ]; then
  if [ -z "$NODYNLINK" ]; then
    if [ -f "/usr/lib64/liblua.so" ]; then
      LUA_LIB="-llua"
      DYNLUA=1
    elif [ -f "/usr/local/lib/liblua-5.4.so" ]; then
      LUA_LIB="-llua"
      DYNLUA=1
    elif [ -f "/usr/lib/x86_64-linux-gnu/liblua5.4.so" ]; then
      LUA_LIB="/usr/lib/x86_64-linux-gnu/liblua5.4.so"
      DYNLUA=1
    elif [ -f "/run/host/usr/lib64/liblua.so" ]; then
      LUA_LIB="-L /run/host/usr/lib64/liblua.so"
      DYNLUA=1
    fi
  fi
  if [ -z "$LUA_LIB" ]; then
    LUA_LIB="/usr/local/lib/liblua.a"
  fi
else
  LUA_LIB="${LUA_LIB_PATH}"
fi

if [ -z "${LUA_INC_PATH}" ]; then
  if [ -f "/usr/include/lua.h" ]; then
    LUA_INC="/usr/include/"
  elif [ -f "/usr/local/include/lua54/lua.h" ]; then
    LUA_INC="/usr/local/include/lua54/"
  elif [ -f "/run/host/usr/include/lua.h" ]; then
    LUA_INC="/usr/include"
  else
    LUA_INC="/usr/local/include/"
  fi
else
  LUA_INC="${LUA_INC_PATH}"
fi

FLAGS="-s -O3"
OUT="${OUT:-bin/80s}"
CC="${CC:-gcc}"

if [ "$JIT" = "true" ]; then
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
  OS="$(uname -o)"
  if [ "$OS" = "Msys" ]; then
    LUA_INC="/c/Users/$USER/vcpkg/packages/lua_x64-windows/include"
  elif [ -d "/run/host" ]; then
    if [ -z "$DYNLUA" ]; then
      LUA_LIB="/run/host$LUA_LIB"
    fi
    LUA_INC="/run/host$LUA_INC"
  fi
  if [ ! -f "$LUA_INC/lua.h" ]; then
    echo "failed to locate lua.h in $LUA_INC"
    exit 1
  fi
fi

mkdir -p bin

DEFINES=""
LIBS="-lm -ldl -lpthread -lcrypto -lssl"

if [ $(uname) = "SunOS" ]; then
  LIBS="$LIBS -lsocket -lnsl"
fi

if [ "$DEBUG" = "true" ]; then
    DEFINES="$DEFINES -DDEBUG=1"
    FLAGS="-O0 -g"
fi

FLAGS="$FLAGS -march=native -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"

echo "Defines: $DEFINES"
echo "Libraries: $LIBS"
echo "Flags: $FLAGS"
echo "Lua include directory: $LUA_INC"
echo "Lua library directory: $LUA_LIB"

if [ "$DYNAMIC" = "true" ]; then
  DEFINES="$DEFINES -DS80_DYNAMIC=1"
  DEFINES="$DEFINES -DS80_SO=$OUT.so"
  $CC src/80s_common.c src/dynstr.c src/algo.c \
      src/lua.c src/lua_net.c src/lua_codec.c src/lua_crypto.c \
      src/serve.epoll.c src/serve.kqueue.c \
      -shared -fPIC \
      $LUA_LIB \
      "-I$LUA_INC" \
      $DEFINES \
      $LIBS \
      $FLAGS \
      -o "$OUT.so"
  if [ ! "$SOONLY" = "true" ]; then
    $CC src/80s.c $DEFINES $FLAGS -fPIC $LUA_LIB $LIBS -o "$OUT"
  fi
else
  $CC src/80s.c src/80s_common.c src/dynstr.c src/algo.c \
      src/lua.c src/lua_net.c src/lua_codec.c src/lua_crypto.c \
      src/serve.epoll.c src/serve.kqueue.c \
      "$LUA_LIB" \
      "-I$LUA_INC" \
      $DEFINES \
      $LIBS \
      $FLAGS \
      -o "$OUT"
fi