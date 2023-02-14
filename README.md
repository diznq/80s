# 80's

Minimalistic lock-less epoll server with Lua bindings.

## Motivation

Over time I already grew tired of insane software complexity of modern software development, that requires 300MB RAM just to return Hello world on HTTP server in Python and 500MB Docker image as an extra.

80's is a minimalistic server that leverages Lua to handle network traffic and is only 280kB when compiled yet still achieving `166 000 req/s` on single core or `300 000 req/s` on 4 cores while being fully asynchronous and free of any locks.

\* Measurements were taken with 1 000 000 requests @ 500 parallel connections using `keep-alive`. Performance without `keep-alive` is `55 000 req/s` on both single or multi-core. CPU: i7-1165G7 @ 2.80GHz

## Compiling

Prerequisites:
- installed Lua librares (lualib.a and Lua headers possibly in /usr/local/include)
- Linux (as of now, only epoll is supported as event loop provider, kqueue and IOCP to be in future)

To compile the project, simply run `./build.sh` or `JIT=true ./build.sh` to build with LuaJIT.

## API
C server exposes several APIs for Lua for both input and output.

Naming conventions:
- `elfd`: event loop file descriptor
- `childfd`: child file descriptor (remote socket)
- `parentfd`: main server file descriptor
- `data`: received/to be sent data, always a `string`

### Input APIs
- `_G.on_data(elfd, childfd, data, length)`: called on each incoming packet of data
- `_G.on_close(elfd, childfd)`: called when socket is closed
- `_G.on_connect(elfd, childfd)`: called when socket successfuly connects
- `_G.on_init(elfd, parentfd)`: called when epoll is initialized

### Output APIs
- `net.connect(elfd, hostName, port)`: create new TCP socket, returns `socket, error`
- `net.write(elfd, childfd, data, close?)`: write data on socket, if close is true, close the connection after write
- `net.close(elfd, childfd)`: close a socket