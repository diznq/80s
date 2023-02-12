# 80's

Minimalistic epoll server with bindings to Lua

## Motivation

Over time I already grew tired of insane software complexity of modern software development, that requires 300MB RAM just to return Hello world on HTTP server in Python and 500MB Docker image as an extra.

80's is a minimalistic server that leverages Lua to handle network traffic and is only 270kB when compiled yet still achieving 37 000 req/s on single core with async IO thanks to epoll!

## Compiling

Prerequisites:
- installed Lua librares (lualib.a)

To compile the project, simply run `gcc main.c -llua -lm -O3 -s -march=native -o bin/server`

## Exposed APIs
- `server.lua/on_data(epollfd, childfd, data, length)` is called by C server on each incoming packet of data
- `net_write(epollfd, childfd, data, close?)`: write data on socket, if close is true, close the connection after write
- `net_close(epollfd, childfd)`: close a socket