LUA_LIB="/usr/local/lib/liblua.a"
LUA_INC="/usr/local/include/"

if [[ "${JIT}" == "true" ]]; then
    LUA_LIB="/usr/local/lib/libluajit-5.1.a"
    LUA_INC="/usr/local/include/luajit-2.0/"
fi

mkdir -p bin

gcc src/main.c src/lua.c "-I$LUA_INC" "$LUA_LIB" -lm -ldl -lpthread -s -Ofast -march=native -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -o bin/80s