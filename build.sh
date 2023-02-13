STATIC=""
LUA_LIB="lua"
LUA_INC="/usr/local/include/"


if [[ "${JIT}" == "true" ]]; then
    LUA_LIB="luajit-5.1"
    LUA_INC="/usr/local/include/luajit-2.0/"
    STATIC="-static"
fi

gcc src/main.c src/lua.c "-I$LUA_INC" $STATIC "-l$LUA_LIB" -lm -s -march=native -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Ofast -o bin/server