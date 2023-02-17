# 80's

Minimalistic lock-less epoll server with Lua bindings.

## Motivation

Over time I already grew tired of insane software complexity of modern software development, that requires 300MB RAM just to return Hello world on HTTP server in Python and 500MB Docker image as an extra.

80's is a minimalistic server that leverages Lua to handle network traffic and is only 280kB when compiled yet still achieving `166 000 req/s` on single core or `300 000 req/s` on 4 cores while being fully asynchronous and free of any locks.

\* Measurements were taken with 1 000 000 requests @ 500 parallel connections using `keep-alive`. Performance without `keep-alive` is `55 000 req/s` on both single or multi-core. CPU: i7-1165G7 @ 2.80GHz

## Compiling

Prerequisites:
- installed Lua librares (lualib.a and Lua headers possibly in /usr/local/include)
- installed OpenSSL libraries if cryptograpic extensions are enabled
- Linux (as of now, only epoll is supported as event loop provider, kqueue and IOCP to be in future)

To compile the project, simply run `./build.sh`.

You can also define following environment variables before running the build to enable certain features:

- `JIT=true`: use LuaJIT instead of Lua
- `CRYPTO=true`: enable cryptographic extensions, so Lua has hashing functions

i.e. `JIT=true CRYPTO=true ./build.sh`

## Running

To run the server, execute `bin/80s server/simple_http.lua`, optionally `bin/80s server/simple_http.lua 8080` to specify the port. After this, the server is running and can be reloaded by calling `net.reload()` from within Lua code.

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

### Lua APIs
- `net.connect(elfd, hostName, port)`: create new TCP socket, returns `socket, error`
- `net.write(elfd, childfd, data, close?)`: write data on socket, if close is true, close the connection after write
- `net.close(elfd, childfd)`: close a socket
- `net.reload()`: reload entrypoint Lua
- `net.listdir(dir)`: list files in a directory, directories will end with `/` in returned result

If binary is compiled with `-DCRYPTOGRAPHIC_EXTENSIONS=1`, also following APIs are available:

- `crext.sha1(data)`: returns raw SHA1 of data
- `crext.sha256(data)`: returns raw SHA256 of data

## Async helpers

### Promises

Promises allow you to set a callback for future result, the syntax is as follows:

```lua
local result = SQL:select("SELECT * FROM users")
result(function(users, err)
    if users then
        ...
    else
        ...
    end
end)
```

To create a promise, use `aio:prepare_promise`, i.e.

```lua
function get_user_by_id(id)
    local resolve, resolver = aio:prepare_promise()
    SQL:select("SELECT * FROM users WHERE id = '%d' LIMIT 1", id)(function(users, err)
        if #users == 1 then 
            resolve(users[1])
        else
            resolve(nil)
        end
    end)
    return resolver
end

get_user_by_id(1)(function(user)
    print("Name: ", user.name)
end)
```

**Gathering promises**

To gather multiple parallel promises and get their overall results once all of them execute, you can use `aio:gather(promises...)`

```lua
aio:gather(
    http_client.GET("w3.org", "/", "text/html"), 
    http_client.GET("en.wikipedia.org", "/", "text/html")
)(function(w3, wiki)
    print("W3 response length: ", #w3)
    print("Wiki response length: ", #wiki)
end)
```

**Chaining promises**

To chain multiple promises to execute sequentially, you can use `aio:chain`, aio:chain also returns a promise that will be called with value of the last argument promise.

```lua
aio:chain(
    http_client.GET("localhost", "/users/1", "application/json"),
    function(user)
        local parsed = parse_json(user)
        return http_client.GET("localhost", "/accounts/" .. parsed.account_id, "application/json")
    end,
    function(account)
        return account.EUR
    end
)(function(eur)
    print(eur .. " EUR available in the account")
end)
```

### Coroutinization

To make coding easier, `aio:cor` and `aio:buffered_cor` exist to make code look more sequential when dealing with events of incoming data.

```lua
local sock = aio:connect("localhost", 80)
function sock:on_open() sock:write("GET / HTTP/1.1\r\n\r\n") end

--- This
function sock:on_data(elfd, childfd, data, len)
    -- somehow deal with data until we have entire response complete
    -- mind that on_data can be called multiple times as dat arrive
end

--- Can be transformed to this
aio:cor(sock, function(stream, resolve)
    local data = ""
    for chunk in stream do
        data = data .. chunk
        -- it is up to implementation when to stop
        -- by default when on_close is called, stream iterator will stop
        -- but connection doesn't always have to close this way, especially
        -- with keep-alive requests
        coroutine.yield()
    end
    resolve(data)
end)(function(response)
    print("Received full response from server: ", response)
end)
```

In similar fashion, `buffered_cor` allows to receive only defined amount of bytes or bytes until a delimiter is reached in sequential manner

```lua
aio:buffered_cor(sock, function(resolve)
    local header = coroutine.yield(4) -- read first 4 bytes of data
    local server_name = coroutine.yield("\r\n") -- read until \r\n is reached
    local length = docode_int16(coroutine.yield(2)) -- read int2 value
    local rest = coroutine.yield(length) -- read `length` bytes of remaining data
    resolve(server_name, rest)
end)(function(server_name, rest)
    print("Server: ", server_name)
    print("Data: ", data)
end)
```

### Awaiting promises
Coroutinization also allows for awaiting promises, using `aio:async` and `aio:await`. `aio:await` must always be ran from within `aio:async` context.

Example with previous `aio:gather` use case:

```lua
aio:async(function()
    local w3, wiki = aio:await(aio:gather(
        http_client.GET("w3.org", "/", "text/html"), 
        http_client.GET("en.wikipedia.org", "/", "text/html")
    ))
    print("W3 response length: ", #w3)
    print("Wiki response length: ", #wiki)
end)
```

## Default server/http.lua as content server

Default `http.lua` comes preconfigured to serve files in `public_html` and if file name contains `.dyn.` (i.e. `index.dyn.html`), it also applies templating, which make dynamic content possible. Also if file name begins with `post.`/`delete.`/`put.`, it is used for handling `POST`/`DELETE`/`PUT` requests instead of default `GET`.

### Templating syntax
To insert dynamic content to the file, wrap Lua code between either `<?lu ... ?>` for synchronous code or `<?lua ... ?>` asynchronous code. 

All asynchronous dynamic code blocks are executed in parallel and in `aio:async` context, so `aio:await` is available for use. As for order of dynamic content blocks within file, there is no guarantee of sequential code execution.

The code must use `write(text, ...)` to write dynamic content, which will be replaced back into original page and in case the call is asynchronous, finish the generation by calling `done()` that is available during the execution.

During code execution, several variables are set within context:
- `endpoint`: request URL without query part
- `query`: table with query parameters
- `headers`: table of request headers
- `body`: request body
- `session`: session context
- `status(http_status)`: write HTTP status
- `await(promise)`: awaits a promise and returns its result
- `header(header_name, header_value)`: write HTTP header
- `write(text, ...)`: writer content, if ... is present, it is equal to `string.format(text, escape(x) for x in ...)`, otherwise just text
- `escape(text)`: HTML escape the text
- `done()`: done signalizer

You can see examples in `server/public_html/` directory.

## MySQL module

Prerequisites:
- binary must be compiled with `CRYPTO=true` (or `-DCRYPTOGRAPHIC_EXTENSIONS=1`) as MySQL protocol relies on hashes for authetication.

MySQL module allows simple interaction with MySQL server, such as connecting, automatic reconnecting and text queries.
Following methods are available:

- `mysql:new()`: creates a new MySQL client object
- `mysql:connect(username, password, db_name, host?, port?)`: connects to database and returns a promise with `ok, err` return value, host defaults to `127.0.0.1`, port defaults to `3306`
- `mysql:exec(query, ...)`: sends a query to database, if ... is provided, query becomes `string.format(text, escape(x) for x in ...)`, returns a promise with decoded response equal to `mysqlerror`/`mysqlok`/`mysqleof`
- `mysql:select(query, ...)`: **shall be used** explicitly with `SELECT` queries, returns a promise with array of tables that contain results. If query fails, returned value is `nil, error string`.
- `mysql:escape(text)`: escape text for the query, not required if using `mysql:exec` or `mysql:select`
- `mysql:raw_exec(query, ...)`: sends a query to database, returns a promise that takes either callback or coroutine, if coroutine is provided, it will be resumed on each additional command that arrives from MySQL server, used as backbone for `:select`, should not be used by developer if ever. The returned/yielded value is pair of `sequence ID` and `raw packet bytes`

If connection socket disconnects while the server is still running, an attempt to reconnect will be made before executing incoming SQL queries. If that fails, it is returned as error to either :exec  or :select.