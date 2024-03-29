# 80's

Minimalistic lock-less epoll/kqueue server with Lua bindings and full hot-code reloading.

## Motivation

Over time I already grew tired of insane software complexity of modern software development, that requires 300MB RAM just to return Hello world on HTTP server in Python and 500MB Docker image as an extra.

80's is a minimalistic server that leverages Lua to handle network traffic and is only 280kB when compiled yet still achieving `166 000 req/s` on single core or `330 000 req/s` on 4 cores while being fully asynchronous and free of any locks.

\* Measurements were taken with 1 000 000 requests @ 500 parallel connections using `keep-alive`. Performance without `keep-alive` is `55 000 req/s` on both single or multi-core. CPU: i7-1165G7 @ 2.80GHz

## Compiling

### Using local system dependencies

Prerequisites:
- installed Lua librares (lualib.a and Lua headers possibly in /usr/local/include)
- installed OpenSSL libraries
- Linux, FreeBSD or Windows (IOCP for Windows is supported only partially for basic tasks)
- if building on Windows, Msys2 is recommended as building environment

To compile the project, simply run `./80.sh`.

You can also define following environment variables before running the build to enable certain features:

- `JIT=true`: use LuaJIT instead of Lua
- `DEBUG=true`: compile in debug mode
- `LINK=static/dynamic`: link type, if dynamic live binary reload (not just Lua) is enabled
- `SOONLY=true`: build only .so file if LINK is dynamic for live reloads

i.e. `JIT=true ./80.sh`

### Using VSCode devcontainer

Open VSCode and run command _Reopen in Container_

## Running

To run the server, execute `bin/80s server/simple_http.lua`, optionally `bin/80s server/simple_http.lua -p 8080 -c 4` to specify the port and number of CPUs (workers). After this, the server is running and can be reloaded by calling `net.reload()` from within Lua code.

### Available flags

- `-6`: bind to IPv6 address (defaults to not set)
- `-h address`: set server bind address (defaults to 8080)
- `-p port_no`: set server port address (defaults to either 0.0.0.0 or ::)
- `-m module1.so,module2.so,...`: list of comma separated modules to be loaded (default empty)
- `-c concurrency`: set concurrency level (defaults to number of machine CPUs)

## Benchmark

By using `server/simple_http.lua` as basis for simple benchmarking, that is:

- request goes to `/echo?name=Abcde`
- server responds with `Hi, Abcde!`

Result of this "benchmark" were as follows:

|    # |  FastAPI |     Node |      PHP |        80s + JIT |       80s |    C++ Beast |  Spring |      Go |
|-----:|---------:|---------:|---------:|-----------------:|----------:|-------------:|--------:|--------:|
|  1   | 2 270.82 | 11 181.6 | 15 319.3 |       **78 329** |    70 351 |     33 563.1 |  31 017 |  68 263 |
|  2   | 3 750.01 | 20 542.3 | 25 414.5 |      **140 055** |   121 880 |     51 785.3 |  61 441 | 110 083 |
|  4   | 5 598.95 | 29 734.2 | 32 766.4 |      **208 887** |   191 456 |     51 172.2 |  93 701 | 193 670 |
|  8   | 6 559.85 | 34 952.2 | 36 700.2 |      **276 499** |   259 212 |     52 010.9 | 108 544 | 214 954 |
|  16  | 6 998.02 | 34 783.7 | 38 593.3 |      **274 336** |   257 560 |     53 727.8 | 126 530 | 221 350 |
|  32  | 7 330.34 | 35 172.2 | 35 751.2 |      **303 348** |   257 959 |     53 842.5 | 132 428 | 228 555 |
|  64  | 7 563.92 | 35 189.6 | 35 601.7 |      **280 517** |   256 301 |     53 386.0 | 133 973 | 233 014 |
|  128 | 7 764.70 | 34 766.8 | 34 281.1 |      **282 553** |   262 609 |     53 338.6 | 137 394 | 223 966 |
|  256 | 7 554.97 | 34 406.2 | 35 200.1 |      **274 152** |   257 229 |     52 433.6 | 140 158 | 226 288 |
|  512 | 7 431.89 | 34 126.9 | 32 067.4 |      **276 537** |   244 457 |     49 276.8 | 136 063 | 208 429 |
| 10000| 6 194.70 |	33 774.3 |	    N/A |	   **180 599** |   145 858 |	 40 492.6 |	119 237 | 157 191 |

\* # = number of parallel connections using `ab -c # -n 1000000 -k http://localhost:8080/echo?name=Abcde`

\* bold cell = highest requests/s

Also it is important to notice the difference in memory consumption between top performers on C10K beng:

- 80s + JIT: 70.6 MB RAM
- Go: 253.3MB RAM
- 80s: 106.4 MB RAM
- Spring: 681 MB RAM

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
- `_G.on_write(elfd, childfd)`: called when socket sbecomes writeable (also on connect)
- `_G.on_init(elfd, parentfd)`: called when event loop is initialized

## Creating custom C modules

To extend the C functionality, custom modules can be created that contain `on_load` and `on_unload` procedures.

Example:

```c
#include <stdio.h> 
#include "../src/80s.h"

int on_load(void *ctx, serve_params *params, int reload) {
    printf("on load, worker: %d\n", params->workerid);
}

int on_unload(void *ctx, serve_params *params, int reload) {
    printf("on unload, worker: %d\n", params->workerid);
}
```


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
local sock = aio:connect(ELFD, "localhost", 80)
function sock:on_connect() sock:write("GET / HTTP/1.1\r\n\r\n") end

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

### Awaiting promises

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

Default `http.lua` comes preconfigured to serve files in `public_html`.
In case there is `main.lua` in parent directory of `public_html`, it is loaded on server start-up.
The file rendering follows these rules:

- If file name contains `.dyn.` (i.e. `index.dyn.html`), it is considered dynamic contain and therefore `<?lu ... ?>`, `<?lua ... ?>` and `<?include ... ?>` will be replaced with rendered content on page request
- If path to the file contains directory called `static/`, even if name contains `.dyn.`, it is considered static content
- If file name contains `.priv.`, it is not assigned to any handler, but can be used for including
- If file starts with `post.`, it is assigned to `POST` handler
- If file starts with `put.`, it is assigned to `PUT` handler
- If file starts with `delete.`, it is assigned to `DELETE` handler
- If directory name begins with `i.`, it is ignored

Live reloading can be enabled by setting `RELOAD=true` environmet variable. For now that works only on systems that support inotify API.

To change default public_html folder, set `PUBLIC_HTML` envirnoment variable, i.e. `PUBLIC_HTML=server/www/` will use `server/www/main.lua` + `server/www/public_html/*`.

To set master key for URLs encryption, set `MASTER_KEY` environment variable, after that all URLs generated with `to_url` will be encrypted and all query parameters passed in `e` will be decrypted automatically.


### Templating syntax
To insert dynamic content to the file, wrap Lua code between either `<?lu ... ?>` for synchronous code or `<?lua ... ?>` asynchronous code. 

To include another file into the current file, use `<?include ... ?>` with path relative to current file, i.e. `<?include ../header.priv.html ?>`. Maximum inclusion depth is up to 64 files.

All asynchronous dynamic code blocks are executed in **sequential order** and in `aio:async` context, so `aio:await` is available for use. In case that file begins with `#! parallel` (either before or after resolving all includes), execution becomes parallel and there is no guaranteed order of execution of each block.

The code must use `write(text, ...)` to write dynamic content, which will be replaced back into original page and in case the call is asynchronous, finish the generation by calling `done()` that is available during the execution.

There is also sugar syntax for write in form of line that begins with `| ` (the space **must follow** `|`), where `| Text` will be transformed to `write([[Text]])` and if text contains `#[[Argument]]` or `#[[Argument:Format]]`, it will be appended to list of otherwise arguments for write. If no format is provided, it is evaluated as `s` that translates to `%s`.

Example:

```html
| <div class="message">
|   Hello there, #[[name]]. Random number of today is: #[[math.random():.3f]]
| </div>
```

That translates to

```lua
write([[<div class="message">]])
write([[Hello there, %s. Random number of today is: %.3f]], name, math.random())
write([[</div>]])
```

Same rules apply for text between \`\`\`...\`\`\`, so previous example could be further simplified to just

```html
\`\`\`
<div class="message">
  Hello there, #[[name]]. Random number of today is: #[[math.random():.3f]]
</div>
\`\`\`
```

During code execution, several variables are set within context:
- `session`: session context
- `locals`: a table where scripts can store intermediate data that they can share across them while entire page renders
- `endpoint`: request URL without query part
- `query`: table with query parameters, encrypted parameters available under `query.e` and should be used if possible instead as just `query` can be overriden in case of `?e=...&param1=...&param2=...`
- `headers`: table of request headers
- `body`: request body
- `status(http_status)`: write HTTP status
- `await(promise)`: awaits a promise and returns its result
- `header(header_name, header_value)`: write HTTP header
- `write(text, ...)`: writer content, if ... is present, it is equal to `string.format(text, escape(x) for x in ...)`, otherwise just text
- `escape(text)`: HTML escape the text
- `post_render(handler)`: add callback `fun(rendered_page: string): string new_page` that will be called after page is rendered and can alter the page content
- `done()`: done signalizer
- `to_url(endpoint, params)`: create URL, i.e. `to_url("/profile", {id=user.id})`, if aio master key is set, query parameters will be encrypted into `?e` by using `master_key .. endpoint` as a key. If `params.e` is set to false, no encryption is performed and if `params.iv` exists, IV will be available in encrypted URL based on it's value, by default it won't

You can see examples in `server/public_html/` directory.

### Best practices

- **Never** define global `function`s in `<?lu(a) ... ?>` to prevent undefined behaviour! Always use `local function` instead!
- For multi line text outputs, prefer \`\`\`...\`\`\` syntax over multple `| ...` lines syntax **if code has fewer formatting arguments**
- Leverage `aio:cached(cache_name, key, fun(): any)` to accelerate generation of known static content

## MySQL module

MySQL module allows simple interaction with MySQL server, such as connecting, automatic reconnecting and text queries.
Following methods are available:

- `mysql:new()`: creates a new MySQL client object
- `mysql:connect(username, password, db_name, host?, port?)`: connects to database and returns a promise with `ok, err` return value, host defaults to `127.0.0.1`, port defaults to `3306`
- `mysql:exec(query, ...)`: sends a query to database, if ... is provided, query becomes `string.format(text, escape(x) for x in ...)`, returns a promise with decoded response equal to `mysqlerror`/`mysqlok`/`mysqleof`
- `mysql:select(query, ...)`: **shall be used** explicitly with `SELECT` queries, returns a promise with array of tables that contain results. If query fails, returned value is `nil, error string`.
- `mysql:escape(text)`: escape text for the query, not required if using `mysql:exec` or `mysql:select`
- `mysql:raw_exec(query, ...)`: sends a query to database, returns a promise that takes either callback or coroutine, if coroutine is provided, it will be resumed on each additional command that arrives from MySQL server, used as backbone for `:select`, should not be used by developer if ever. The returned/yielded value is pair of `sequence ID` and `raw packet bytes`

If connection socket disconnects while the server is still running, an attempt to reconnect will be made before executing incoming SQL queries. If that fails, it is returned as error to either :exec  or :select.

## ORM module

ORM module allows to build upon MySQL module to simplify working with database and provide simple `repository` implementation.

To create a Java JPA-like repository, `orm:create` can be used:

```lua
local repo = orm:create(SQL, {
    source = "posts",
    --- @type ormentity
    entity = {
        id = { field = "id", type = orm.t.int },
        author = { field = "author", type = orm.t.text },
        text = { field = "text", type = orm.t.text },
    },
    findById = true,
    findBy = true
})
```

The following code will create repository with following methods:

```lua
-- SELECT * FROM posts WHERE id = ?
repo.all:byId(id: int): Posts[]?, error?
-- SELECT * FROM posts
repo.all:by(): Posts[]?, error?
-- SELECT * FROM posts WHERE id = ? LIMIT 1
repo.one:byId(id: int): Post?, error?
-- SELECT * FROM posts LIMIT 1
repo.one:by(id: int): Post?, error?
-- SELECT COUNT(*) FROM posts WHERE id = ? LIMT 1
repo.count:byId(id: int): int?, error?
-- SELECT COUNT(*) FROM posts WHERE id = ?
repo.count:by(): int?, error?
```

Optionally `{orderBy: string, limit: string}` can be passed to all select methods, i.e. `repo.all:by({orderBy = "author", limit = 2})`. **Keep in mind that `orderBy` and `limit` aren't escaped in any way**.

Method names always use keys specified in `entity` object using during creation and not `.field` that maps to raw database. Same goes for `repo:save(object)`. All generated queries use `AND` to filter for the result, i.e. `findByIpAndPort` would be `WHERE ip = ? AND port = ?`.