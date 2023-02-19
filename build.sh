LUA_LIB="/usr/local/lib/liblua.a"
LUA_INC="/usr/local/include/"
FLAGS="-s -Ofast"
OUT="${OUT:-bin/80s}"
CC="${CC:-gcc}"

if [[ "${JIT}" == "true" ]]; then
    LUA_LIB="/usr/local/lib/libluajit-5.1.a"
    LUA_INC="/usr/local/include/luajit-2.0/"
fi

mkdir -p bin

DEFINES="-DWORKERS=${WORKERS:-4} -DCRYPTOGRAPHIC_EXTENSIONS=1"
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

$CC src/main.c src/lua.c "$LUA_LIB" \
    $DEFINES \
    "-I$LUA_INC" \
    $LIBS \
    $FLAGS -march=native \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-stringop-overread \
    -o "$OUT"
