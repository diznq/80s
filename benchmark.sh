P="1;2;4;8;16;32;64;128;256;512"
Bin="bin/80s server/http.lua;bin/80sJIT server/http.lua;bin/80s8 server/http.lua;bin/80s8JIT server/http.lua;bin/svgo;node private/server.js;java -jar bin/spring.jar;bin/beast 0.0.0.0 8080;uvicorn private.server:app --port 8080 --workers 4 --log-level critical;php"
IFS=";"

for b in $Bin; do
    id=$(echo "$b" | sed 's/\//_/g' | sed 's/ /_/g')
    eval "($b)&"
    pid=$!
    echo "PID: $pid"
    sleep 5

    for p in $P; do
	echo "Executing $b, $p"
        ab -c $p -n 1000000 -k "http://localhost:8080/haha?name=Abcde" > "results/$p,$id.txt"
	    sleep 2
    done

    kill $pid
done
