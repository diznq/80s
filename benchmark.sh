P="1;2;4;8;16;32;64;128;256;512;10000"
Bin="bin/80s server/simple_http.lua;bin/80sJIT server/simple_http.lua;bin/80s8 server/simple_http.lua;bin/80s8JIT server/simple_http.lua"
Bin="$Bin;bin/svgo;node private/server.js;bin/beast 0.0.0.0 8080;uvicorn private.server:app --port 8080 --workers 4 --log-level critical;php"
Bin="$Bin;java -jar bin/spring.jar"
#Bin="php"
IFS=";"

for b in $Bin; do
    id=$(echo "$b" | sed 's/\//_/g' | sed 's/ /_/g')
    eval "($b)&"
    pid=$!
    echo "PID: $pid"
    sleep 5

    for p in $P; do
	echo "Executing $b, $p"
        ab -c $p -n 1000000 -k "http://localhost:8080/haha?name=Abcde" > "private/ben/$p,$id.txt"
	sleep 2
    done

    kill $pid
    sleep 5
done
