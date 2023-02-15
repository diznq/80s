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

## Running

To run the server, execute `bin/80s examples/simple_http.lua`, optionally `bin/80s examples/simple_http.lua 8080` to specify the port. After this, the server is running and can be reloaded by calling `net.reload()` from within Lua code.

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
- `net.reload()`: reload entrypoint Lua
- `net.listdir(dir)`: list files in a directory, directories will end with `/` in returned result

## Default `examples/http.lua` as content server

Default `http.lua` comes preconfigured to serve files in `public_html` and if file name contains `.dyn.` (i.e. `index.dyn.html`), it also applies templating, which make dynamic content possible.

### Templating syntax
To insert dynamic content to the file, wrap Lua code between either `<?lu ... ?>` for synchronous code or `<?lua ... ?>` asynchronous code. All asynchronous dynamic code blocks are executed in parallel, there is no guarantee of sequential code execution.

The code must use `write(text, unsafe?=false)` to write dynamic content, which will be replaced back into original page and in case the call is asynchronous, finish the generation by calling `done()` that is available during the execution.

During code execution, several variables are set within context:
- `endpoint`: request URL without query part
- `query`: table with query parameters
- `headers`: table of request headers
- `body`: request body
- `session`: session context
- `status(http_status)`: write HTTP status
- `header(header_name, header_value)`: write HTTP header
- `write(text, unsafe?)`: writer callback
- `done()`: done signalizer

You can see examples in `examples/public_html/` directory.