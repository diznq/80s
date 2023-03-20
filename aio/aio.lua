--- @class inotify_event
--- @field name string file name
--- @field wd lightuserdata watch descriptor
--- @field dir boolean true if file is a directory
--- @field modify boolean true if file was modified
--- @field create boolean true if file as created
--- @field delete boolean true if file was deleted
--- @field clock number time since start of program

--- @class net
--- @field write fun(elfd: lightuserdata, childfd: lightuserdata, data: string, offset: integer): boolean write data to file descriptor
--- @field close fun(elfd: lightuserdata, childfd: lightuserdata): boolean close a file descriptor
--- @field connect fun(elfd: lightuserdata, host: string, port: integer): fd: lightuserdata|nil, err: string|nil open a new network connection
--- @field reload fun() reload server
--- @field listdir fun(dir: string): string[] list files in directory
--- @field inotify_init fun(elfd: lightuserdata): fd: lightuserdata|nil, error: string|nil initialize inotify
--- @field inotify_add fun(elfd: lightuserdata, childfd: lightuserdata, target: string): wd: lightuserdata add file to watchlist of inotify, returns watch descriptor
--- @field inotify_remove fun(elfd: lightuserdata, childfd: lightuserdata, wd: lightuserdata): boolean, string|nil remove watch decriptor from watchlist
--- @field inotify_read fun(data: string): inotify_event[] parse inotify events to Lua table
net = net or {}

--- @class crypto
--- @field sha1 fun(data: string): string perform sha1(data), returns bytestring with raw data
--- @field sha256 fun(data: string): string perform sha256(data), returns bytestring with raw data
--- @field cipher fun(data: string, key: string, iv: boolean, encrypt: boolean): result: string?, error: string perform encryption/decryption, if iv is false, iv is all zeros and not inserted to result, key must be at least 128 bits
--- @field to64 fun(data: string): string encode to base64
--- @field from64 fun(data: string): string decode from base64
--- @field random fun(n: integer): string generate n random bytes
crypto = crypto or {}

--- @class codec
--- @field json_encode fun(obj: table): string JSON encode object or an array
--- @field url_encode fun(text: string): string URL encode text
--- @field url_decode fun(text: string): string URL decode text
--- @field mysql_encode fun(text: string): string MySQL encode text
--- @field html_encode fun(text: string): string HTML encode text
codec = codec or {}

--- @class jit
jit = jit or nil

--- @type lightuserdata
ELFD = ELFD or nil

--- @type integer
WORKERID = WORKERID or nil

--- Aliases to be defined here
--- @alias aiostream fun() : any ... AIO input stream
--- @alias aiocor fun(stream: aiostream, resolve?: fun(value: any)|thread): nil AIO coroutine
--- @alias aioresolve fun(result: any): nil AIO resolver
--- @alias aiothen fun(on_resolved: fun(...: any)|thread) AIO then
--- @alias aiohttphandler fun(self: aiosocket, query: string, headers: {[string]: string}, body: string) AIO HTTP handler

--- @alias aiowritebuf {d: string, o: integer}

unpack = unpack or table.unpack

--- AIOsocket class
--- Provides easy wrapper to receive events per object instead of globally
---
--- @class aiosocket
local aiosocket = {
    --- @type lightuserdata socket file scriptor
    childfd = nil,
    --- @type lightuserdata event loop file descriptor
    elfd = nil,
    --- @type boolean true if close after write
    cw = false,
    --- @type boolean true if socket is connected
    co = false,
    --- @type boolean true if socket is writeable
    wr = false,
    --- @type aiowritebuf[] buffer
    buf = {},
    --- @type boolean closed
    closed = false,
}

--- Write data to network
---
--- @param data string data to write
--- @param close boolean|nil close after write
--- @return boolean
function aiosocket:write(data, close)
    if self.closed then return false end
    if close ~= nil then self.cw = close end
    if not self.wr then 
        table.insert(self.buf, {d=data, o=0})
        return true
    end
    local to_write = #data
    local ok, written = net.write(self.elfd, self.childfd, data, 0)
    if not ok then
        self:close()
        return false
    elseif written < to_write then
        self.wr = false
        table.insert(self.buf, {d=data, o=written})
        return true
    elseif self.cw then
        self.buf = {}
        self:close()
        return true
    else
        self.buf = {}
        return true
    end
end

--- Close socket
--- @return boolean
function aiosocket:close()
    if self.closed then return true end
    self.buf = {}
    return net.close(self.elfd, self.childfd)
end

--- Write HTTP respose
---@param status string status code
---@param headers {[string]: any}|string headers or content-type
---@param response string|table response body
---@return boolean
function aiosocket:http_response(status, headers, response)
    if self.closed then return false end
    local str_headers = ""
    if type(response) == "table" then
        response = codec.json_encode(response)
    end
    if type(headers) == "string" then
        str_headers = "content-type: " .. headers .. "\r\n"
    else
        for k, v in pairs(headers) do
            str_headers = str_headers .. string.format("%s: %s\r\n", k, v)
        end
    end
    return self:write(
        string.format("HTTP/1.1 %s\r\nconnection: %s\r\n%scontent-length: %d\r\n\r\n%s",
            status,
            self.cw and "close" or "keep-alive",
            str_headers,
            #response, response
        )
    )
end

--- Close handler of socket, overridable
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aiosocket:on_close(elfd, childfd)

end

--- Data handler of socket, overridable
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param data string stream data
--- @param length integer length of data
function aiosocket:on_data(elfd, childfd, data, length)

end

--- Connect handler of socket, overridable
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aiosocket:on_connect(elfd, childfd)

end

--- Writeable handler of socket, overridable
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param n_written integer bytes written (used for IOCP only)
function aiosocket:on_write(elfd, childfd, n_written)
    -- on connect is called only once
    self.wr = true
    if not self.co then
        self.co = true
        self:on_connect(elfd, childfd)
    end
    if self.closed then return end
    -- keep in mind that on_write is only triggered when socket previously failed to write part of data
    -- if there is any data remaining to be sent, try to send it
    while #self.buf > 0 do
        local item = self.buf[1]
        local to_write = #item.d - item.o
        local ok, written = true, 0
        -- if we use IOCP, we receive writes are done asynhronously
        -- in that case, we recompute the correct offset and skip
        -- to next item if we exceeded current item length
        if n_written > 0 then
            if n_written >= to_write then
                n_written = n_written - to_write
                written = 0
                to_write = 0
            else
                item.o = item.o + n_written
                to_write = to_write - n_written
            end
        end
        -- only write if there actually is something to write
        if to_write > 0 then
            ok, written = net.write(elfd, childfd, item.d, item.o)
        end
        if not ok then
            -- if sending failed completly, i.e. socket was closed, end
            self:close()
        elseif written < to_write then
            -- if we were able to send only part of data due to full buffer, equeue it for later
            self.wr = false
            item.o = item.o + written
            break
        elseif self.cw then
            -- if we sent everything and require close after write, close the socket
            self:close()
            break
        else
            table.remove(self.buf, 1)
        end
    end
end

--- Create new socket instance
---
--- @param elfd lightuserdata
--- @param childfd lightuserdata
--- @param connected boolean
--- @return aiosocket
function aiosocket:new(elfd, childfd, connected)
    local socket = { elfd = elfd, childfd = childfd, cw = false, co = connected or false, wr = connected or false }
    setmetatable(socket, self)
    self.__index = self
    return socket
end

if not aio then
    --- AIO object
    --- There can be only one instance of AIO, enabling hot-reloads
    --- as fds won't be lost during the reload
    ---
    --- @class aio
    aio = {
        --- @type {[string]: aiosocket}
        fds={},
        --- @type {[string]: {[string]: aiohttphandler}}
        http={
            --- @type {[string]: aiohttphandler}
            GET={},
            --- @type {[string]: aiohttphandler}
            POST={}
        },
        cors = 0,
        -- master key
        master_key = nil,
        ---@type {size: integer, data: table}
        cache = {},
        max_cache_size = 10000
    }
end

--- Generic handler called when data is received
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param data string incoming stream data
--- @param len integer length of data
function aio:on_data(elfd, childfd, data, len)
    local fd = self.fds[childfd]
    if fd ~= nil then
        fd:on_data(elfd, childfd, data, len)
        return
    end

    -- G as for GET, P as for POST/PUT, D as for DELETE, O as for OPTIONS, H as for HEAD
    local is_http = {G = true, P = true, D = true, O = true, H = true}
    local initial = data:sub(1, 1)
    -- detect the protocol and add correct handler
    if is_http[initial] then
        self:handle_as_http(elfd, childfd, data, len)
    end
end

--- Create new HTTP handler for network stream
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param data string incoming stream data
--- @param len integer length of data
function aio:handle_as_http(elfd, childfd, data, len)
    local fd = aiosocket:new(elfd, childfd, true)
    self.fds[childfd] = fd

    self:buffered_cor(fd, function (resolve)
        while true do
            local header = coroutine.yield("\r\n\r\n")
            if not header then
                fd:close()
                break
            end
            local length = header:match("[Cc]ontent%-[Ll]ength: (%d+)")
            local body = ""
            if length and length ~= "0" then
                body = coroutine.yield(tonumber(length))
                if not body then
                    fd:close()
                    break
                end
            end
            local method, url, headers = aio:parse_http(header)
            local close = (headers["connection"] or "close"):lower() == "close"
            fd.cw = close
            aio:on_http(fd, method, url, headers, body)
            if close then
                break
            end
        end
    end)

    -- provide data event
    fd:on_data(elfd, childfd, data, len)
end

---Parse HTTP request
---
---@param data string http request
---@return string method HTTP method
---@return string url request URL
---@return {[string]: string} headers headers table
function aio:parse_http(data)
    local headers = {}
    local method, url, header = data:match("(.-) (.-) HTTP.-\r(.*)")

    for key, value in header:gmatch("\n(.-):[ ]*(.-)\r") do
        headers[key:lower()] = value
    end

    return method, url, headers
end

--- Parse HTTP query
--- @param query string query string
--- @param private_key string|nil string decryption key for ?e
--- @return {[string]: string, e: {[string]: string}} query query params
function aio:parse_query(query, private_key)
    local params = {}
    params.e={} -- reserved for encrypted query
    query = "&" .. query
    -- match everything where first part doesn't contain = and second part doesn't contain &
    for key, value in query:gmatch("%&([^=]+)=?([^&]*)") do
        if key == "e" and private_key ~= nil then
            local value = self:decrypt(codec.url_decode(value), self:create_key(private_key))
            if value then
                local result = self:parse_query(value)
                for i, v in pairs(result) do
                    params[i] = v
                end
                params.e = result
            end
        elseif params[key] == nil then
            params[key] = codec.url_decode(value)
        end
    end
    return params
end

--- Create query string
---@param params {[string]: string} key value pairs of query params
---@param private_key string|nil private key to use to create signed query
---@param ordered boolean|false guarantee same order everytime
---@param iv boolean|false true if IV in URL is to be used
---@return string result query string
function aio:create_query(params, private_key, ordered, iv)
    iv = iv or false
    local values = {}
    for key, value in pairs(params) do
        if type(value) ~= "table" then
            table.insert(values, string.format("%s=%s", key, codec.url_encode(tostring(value))))
        end
    end
    if ordered then
        table.sort(values)
    end
    local result = table.concat(values, "&")
    if type(private_key) == "string" then
        return self:cached("url", result, function()
            local encrypted = self:encrypt(result, self:create_key(private_key), iv, false)
            if not encrypted then
                encrypted = "nil"
            end
            return string.format("e=%s", codec.url_encode(encrypted))
        end, not iv)
    end
    return result
end

--- Derive encryption key from master key and private key
---@param private_key string private key
---@return string derived key
function aio:create_key(private_key)
    if self.master_key then 
        return self.master_key .. private_key
    end
    return private_key
end

--- Cache an item or a promise
--- in case callback returns a promised, next time promise is returned as well
--- as a cached item that always maps to the same result
---
---@generic T : string
---@param cache_name string cache name
---@param key string caching key
---@param callback fun(): T producer if item is not found
---@param condition boolean|nil if false, no cache is performed
---@return T value
function aio:cached(cache_name, key, callback, condition)
    if condition == false then
        return callback()
    end
    local cache = self.cache[cache_name]
    if not cache then
        self.cache[cache_name] = {size = 0, data = {}}
        cache = self.cache[cache_name]
    end
    local hit = cache[key]
    if hit then return hit end
    if self.max_cache_size == nil or self.max_cache_size == 0 then
        return callback()
    end
    if cache.size == self.max_cache_size then
        local k, v = next(cache.data)
        cache[k] = nil
    else
        cache.size = cache.size + 1
    end
    hit = callback()
    -- in case callback is a promise, cache the value once it is resolved
    if type(hit) == "function" then
        local resolve, resolver = aio:prepare_promise()
        hit(function (value)
            cache[key] = function(cb)
                cb(value)
            end
            resolve(value)
        end)
        return resolver
    else
        cache[key] = hit
    end
    return hit
end

--- Create URL from endpoint and query params
---@param endpoint string endpoint
---@param params {[string]: string, e?: boolean, iv?: boolean, ordered?: boolean} parameters list, if e is false, no encryption is performed
---@return any
function aio:to_url(endpoint, params)
    local path = endpoint
    local private_key = aio.master_key and endpoint or nil
    if type(params) == "table" then
        local iv = params.iv or false
        local ordered = true
        if params.e == false then
            private_key = nil
        end
        if params.ordered == false then
            ordered = false
        end
        params["iv"] = nil
        params["e"] = nil
        params["ordered"] = nil
        ---@diagnostic disable-next-line: param-type-mismatch
        path = string.format("%s?%s", path, aio:create_query(params, private_key, ordered, iv))
    end
    return path
end

--- Set master key
--- Master key must be at least 16 bytes long, if it starts with b64:
--- then it's decoded from base64 in first step
---@param key string|nil key
function aio:set_master_key(key)
    if type(key) == "string" then
        if key:match("^b64:") then
            key = crypto.from64(key:sub(5))
        end
        if #key == 0 then
            key = nil
        elseif #key < 16 then
            error("master key must be at least 16 bytes long")
        end
    end
    self.master_key = key
end

--- Set max cache size
---@param size integer size
function aio:set_max_cache_size(size)
    self.max_cache_size = size
end

--- Encrypt data
---@param data string data
---@param key string key
---@param iv boolean|false if true, random IV will be used, if false IV is zeroes
---@param raw boolean|false if true, raw cipher is returned, if false, base64 encoded version is returned
---@return string|nil result encrypted data
function aio:encrypt(data, key, iv, raw)
    if iv == nil then iv = true end
    raw = raw or false
    local res, err = crypto.cipher(data, key, iv, true)
    if res then
        if not raw then res = crypto.to64(res) end
        return res
    end
    return nil
end

--- Decrypt data
---@param data string data to decrypt
---@param key string encryption key
---@param raw? boolean if true, data are not base64 decoded before decryption
---@return string|nil result decrypted data
function aio:decrypt(data, key, raw)
    raw = raw or false
    if type(data) ~= "string" then
        return nil
    end
    if not raw then data = crypto.from64(data) end
    local res, _ = crypto.cipher(data, key, true, false)
    return res
end

--- Add HTTP GET handler
--- @param url string URL
--- @param callback aiohttphandler handler
function aio:http_get(url, callback)
    self.http.GET[url] = callback
end

--- Add HTTP POST handler
--- @param url string URL
--- @param callback aiohttphandler handler
function aio:http_post(url, callback)
    self.http.POST[url] = callback
end

--- Add HTTP any handler
---@param method string HTTP method
---@param url string URL
---@param callback aiohttphandler handler
function aio:http_any(method, url, callback)
    self.http[method] = self.http[method] or {}
    self.http[method][url] = callback
end

--- Create a new TCP socket to host:port
--- @param elfd lightuserdata epoll handle
--- @param host string host name or IP address
--- @param port integer port
--- @return aiosocket|nil socket
--- @return string|nil error
function aio:connect(elfd, host, port)
    local sock, err = net.connect(elfd, host, port)
    if sock == nil then
        return nil, err
    end
    self.fds[sock] = aiosocket:new(elfd, sock, false)
    return self.fds[sock], nil
end


--- Watch for changes in file or directory
---@param elfd lightuserdata epoll handle
---@param targets string[] list of files to watch
---@param on_change fun(events: inotify_event[]) callback with changes
---@return aiosocket|nil
function aio:watch(elfd, targets, on_change)
    local fd, err = net.inotify_init(elfd)
    if fd ~= nil then
        local sock = aiosocket:new(elfd, fd, true)
        sock.watching = {}
        sock.on_data = function (self, elfd, childfd, data, length)
            local events = net.inotify_read(data)
            for _, event in ipairs(events) do
                local wd = event.wd
                if sock.watching[wd] ~= nil then
                    local base = sock.watching[wd]
                    event.name = base .. event.name
                    if event.delete then
                        sock.watching[wd] = nil
                        net.inotify_remove(elfd, fd, wd)
                    end
                end
            end
            on_change(events)
        end
        self.fds[fd] = sock
        for _, target in ipairs(targets) do
            local wd = net.inotify_add(elfd, fd, target)
            if wd ~= nil then
                sock.watching[wd] = target
            end
        end
        return sock
    else
        print(err)
    end
    return nil
end

--- Handler called when socket is closed
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aio:on_close(elfd, childfd)
    --- @type aiosocket
    local fd = self.fds[childfd]
    self.fds[childfd] = nil

    -- notify with close event, only once
    if fd ~= nil and not fd.closed then
        fd.closed = true
        fd.buf = {}
        fd:on_close(elfd, childfd)
    end
end

--- Handler called when socket is writeable
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aio:on_write(elfd, childfd, written)
    local fd = self.fds[childfd]

    -- notify with connect event
    if fd ~= nil then
        fd:on_write(elfd, childfd, written)
    end
end

--- Initialize AIO hooks
function aio:start()
    --- Init handler
    --- @param elfd lightuserdata
    --- @param parentfd lightuserdata
    _G.on_init = function(elfd, parentfd)
        if aio.on_init then
            aio:on_init(elfd, parentfd)
        end
    end
    
    --- Data handler
    --- @param elfd lightuserdata
    --- @param childfd lightuserdata
    --- @param data string
    --- @param len integer
    _G.on_data = function(elfd, childfd, data, len)
        aio:on_data(elfd, childfd, data, len)
    end
    
    --- Close handler
    --- @param elfd lightuserdata
    --- @param childfd lightuserdata
    _G.on_close = function(elfd, childfd)
        aio:on_close(elfd, childfd)
    end
    
    --- Writeable handler
    --- @param elfd lightuserdata
    --- @param childfd lightuserdata
    _G.on_write = function(elfd, childfd, written)
        aio:on_write(elfd, childfd, written)
    end
end

--- Initialization handler
---
--- @param elfd lightuserdata epoll handle
--- @param parentfd lightuserdata server socket handle
function aio:on_init(elfd, parentfd)

end

--- Default HTTP request handler
--- @param fd aiosocket file descriptor
--- @param method string http method
--- @param url string URL
--- @param headers table headers table
--- @param body string request body
function aio:on_http(fd, method, url, headers, body)
    local pivot = url:find("?", 0, true)
    local script = url:sub(0, pivot and pivot - 1 or nil)
    local query = pivot and url:sub(pivot + 1) or ""
    local handlers = self.http[method]

    if handlers ~= nil then
        local handler = handlers[script]
        if handler ~= nil then
            handler(fd, query, headers, body)
            return
        end
    end

    fd:http_response("404 Not found", "text/plain", script .. " was not found on this server")
end

---Prepare promise
---@return function|thread on_resolved callback
---@return aiothen resolver
function aio:prepare_promise()
    local early, early_val = false, nil

    --- Resolve callback with coroutine return value
    --- This code is indeed repeated 3x in this repository to avoid unnecessary
    --- encapsulation on on_resolved (as it would be changed later and reference would be lost)
    --- and save us some performance
    --- @type aiocor|thread coroutine return values
    local on_resolved = function(...) early, early_val = true, {...} end

    --- Set AIO resolver callback
    --- @type aiothen
    local resolve_event = function(callback)
        if early then
            if type(callback) == "thread" then
                local ok, err = coroutine.resume(callback, unpack(early_val))
                if not ok then
                    error(err)
                else
                    ---@diagnostic disable-next-line: redundant-return-value
                    return err
                end
            else
                return callback(unpack(early_val))
            end
        else
            on_resolved = callback
        end
    end

    return function(...) 
        if type(on_resolved) == "thread" then
            local ok, err = coroutine.resume(on_resolved, ...)
            if not ok then
                error(err)
            else
                return err
            end
        else
            return on_resolved(...)
        end
    end, resolve_event
end

--- Wrap event handlers into coroutine, example:
---
--- aio:cor(socket, "on_data", "on_close", function(stream)
---   local whole = ""
---   for item, length in stream() do
---      whole = whole .. item
---   end
---   print(whole)
--- end)
---
--- If called as aio:cor(target, callback), event_handler is assumed to be on_data
--- and close_handler is assumed to be on_close
---
--- @param target aiosocket object to be wrapped
--- @param event_handler string main event source that resumes coroutine
--- @param close_handler string|nil secondary event source that closes coroutine (sends nil data)
--- @param callback aiocor coroutine code, takes stream() that returns arguments (3, 4, ...) skipping elfd, childfd of event_handler
--- @return aiothen
function aio:cor2(target, event_handler, close_handler, callback)
    local data = nil
    local cor = self:cor0(callback)
    local on_resolved, resolve_event = self:prepare_promise()

    --- Resolver callable within coroutine
    --- @param ... any return value
    local resolver = function(...)
        on_resolved(...)
    end

    -- coroutine data iterator
    local provider = function()
        if data == nil then return end
        return unpack(data)
    end

    local running, ended = false, false

    -- main event handler that resumes coroutine as events arrive and provides data for iterator
    target[event_handler] = function(self, epfd, chdfd, ...)
        data = {...}

        -- if coroutine finished it's job, unsubscribe the event handler
        local status = coroutine.status(cor)
        if status == "dead" then
            target[event_handler] = function () end
            return
        end

        running = true
        local ok, result = coroutine.resume(cor, provider, resolver)
        running = false

        if not ok then
            print("aio.cor("..event_handler..") failed", result)
        end

        -- in case close event was invoked from the coroutine, it shall be handled here
        if ended then
            if coroutine.status(cor) ~= "dead" then
                ok, result = coroutine.resume(cor, provider, resolver)
                ended = false

                if not ok then
                    print("aio.cor(" .. event_handler .."|close) failed", result)
                end
            end
        end
    end

    -- closing event handler that sends nil signal to coroutine to terminate the iterator
    if close_handler ~= nil then
        target[close_handler] = function(self, ...)
            local status = coroutine.status(cor)
            if status == "dead" then
                target[close_handler] = function () end
                return
            end

            data = nil
            -- it might be possible that while coroutine is running, it issues a write together
            -- with close, in that case, this would be called while coroutine is still running
            -- and fail, therefore we issue ended=true signal, so after main handler finishes
            -- its job, it will close the coroutine for us instead
            
            if running then
                ended = true
            else
                local ok, result = coroutine.resume(cor, provider, resolver)
                if not ok then
                    print("aio.cor("..close_handler..") failed", result)
                end
            end
        end
    end

    return resolve_event
end

--- Wrap single event handler into a coroutine, evaluates to aio:cor2(target, event_handler, nil, callback)
---
--- @param target aiosocket object to be wrapped
--- @param event_handler string event source that resumes a coroutine
--- @param callback aiocor  coroutine code
--- @return aiothen
function aio:cor1(target, event_handler, callback)
    return self:cor2(target, event_handler, nil, callback)
end

--- Wrap aiosocket receiver into coroutine, evaluates to aio:cor2(target, "on_data", "on_close", callback)
---
--- @param target aiosocket object to be wrapped
--- @param callback aiocor coroutine code
--- @return aiothen
function aio:cor(target, callback)
    return self:cor2(target, "on_data", "on_close", callback)
end


--- Create a new coroutine
---@param callback fun(...: any): any
---@return thread coroutine
function aio:cor0(callback)
    return coroutine.create(callback)
end

--- Execute code in async environment so await can be used
---@param callback function to be ran
---@return thread coroutine
---@return boolean ok value
function aio:async(callback)
    local cor = aio:cor0(callback)
    local ok, result = coroutine.resume(cor)
    if not ok then
        print("aio.async failed: ", result)
    end
    return cor, ok
end

--- Await promise
---@param promise aiothen|thread promise object
---@return any ... response
function aio:await(promise)
    local self_cor = coroutine.running()
    local premature, yielded = nil, false
    if type(promise) == "thread" then
        local result = {coroutine.resume(promise)}
        if not result[1] then
            print("aio.await coroutine failed: ", result[2])
        end
        return unpack(result, 2)
    else
        promise(function(...)
            -- in case we receive response sooner than we actually yield
            -- consider it a premature resolve and treat it differently
            if not yielded then
                premature = {...}
                return
            end
            local ok, result = coroutine.resume(self_cor, ...)
            if not ok then
                print("aio.await failed: ", result)
            end
        end)
    end
    -- in case of premature resolve, return result right away
    if premature then
        return unpack(premature)
    end
    yielded = true
    return coroutine.yield()
end


--- Buffered reader of aio:cor. Allows to read data stream
--- in buffered manner by calling coroutine.yield(n) to receive
--- n bytes of data from network or if n is a string, coroutine
--- is resumed after delimiter n in data was encountered, which
--- is useful for tasks like get all bytes until \0 or \r\n is
--- encountered.
---
--- Example:
--- aio:buffered_cor(fd, function(resolve)
---   local length = tonumber(coroutine.yield(4))
---   local data = coroutine.yield(length)
---   resolve(data)
--- end)
---@param target aiosocket file descriptor
---@param reader fun(resolve: fun(...: any)) reader coroutine
---@return aiothen 
function aio:buffered_cor(target, reader)
    return self:cor(target, function (stream, resolve)
        local reader = self:cor0(reader)
        -- resume the coroutine the first time and receive initial
        -- requested number of bytes to be read
        local ok, requested = coroutine.resume(reader, resolve)
        local read = ""
        local req_delim = false
        local exit = requested == nil
        local nil_resolve = false

        req_delim = type(requested) == "string"

        -- if we failed in very first step, return early and resolve with nil
        if not ok then
            print("aio.buffered_cor: coroutine failed in initial run", requested)
            resolve(nil)
            return
        end

        -- iterate over bytes from network as we receive them
        for data in stream do
            local prev = #read
            --- @type string
            read = read .. data

            -- check if state is ok, and if we read >= bytes requested to read
            while not exit and ok do
                local pivot = requested
                local skip = 1
                if req_delim then
                    local off = prev - #requested
                    if off < 0 then off = 0 end
                    pivot = read:find(requested, off, true)
                    skip = #requested
                end
                if not pivot or pivot > #read then
                    break
                end
                -- iterate over all surplus we have and resume the receiver coroutine
                -- with chunks of size requested by it
                ok, requested = coroutine.resume(reader, read:sub(1, pivot))
                req_delim = type(requested) == "string"

                if not ok then
                    -- if coroutine fails, exit and print error
                    print("aio.buffered_cor: coroutine failed to resume", requested)
                    nil_resolve = true
                    exit = true
                    break
                elseif requested == nil then
                    -- if coroutine is finished, exit
                    exit = true
                    break
                end
                if requested ~= nil then
                    read = read:sub(pivot + skip)
                end
            end
            -- if we ended reading in buffered reader, exit this loop
            if exit then
                break
            end
            coroutine.yield()
        end

        -- after main stream is over, signalize end by sending nil to the reader
        if coroutine.status(reader) ~= "dead" then
            ok, requested = coroutine.resume(reader, nil, "eof")
            if not ok then
                print("aio.buffered_cor: finishing coroutine failed", requested)
            end
        end

        -- if coroutine failed, resolve with nil value
        if nil_resolve then
            resolve(nil)
        end
    end)
end

--- Gather multiple asynchronous tasks
--- @param ... aiothen coroutine resolvers
--- @return aiothen resolver values
function aio:gather(...)
    local tasks = {...}
    local counter = #tasks
    local retvals = {}
    local on_resolved, resolve_event = self:prepare_promise()

    for i, task in ipairs(tasks) do
        table.insert(retvals, nil)
        local ok, err = pcall(task, function (value)
            counter = counter - 1
            retvals[i] = value
            if counter == 0 then
                on_resolved(unpack(retvals))
            end
        end)
        if not ok then
            print("aio.gather: task " .. i .. " failed to execute", err)
        end
    end

    if #tasks == 0 then
        on_resolved()
    end

    return resolve_event
end


--- Array map
---@param array table
---@param fn function
---@return table
function aio:map(array, fn)
    local new_array = {}
    for i=1,#array do
        new_array[i] = fn(array[i])
    end
    return new_array
end

--- Chain multiple AIO operations sequentially
--- @param first aiothen
--- @param ... fun(...: any): aiothen|any
--- @return aiothen retval return value of last task
function aio:chain(first, ...)
    local callbacks = {...}
    local at = 1
    local on_resolved, resolve_event = self:prepare_promise()

    local function next_callback(...)
        if at > #callbacks then
            on_resolved(...)
        else
            local callback = callbacks[at]
            local retval = callback(...)
            at = at + 1
            if type(retval) == "function" then
                retval(function (...)
                    local ok, err = pcall(next_callback, ...)
                    if not ok then
                        print("aio.chain: retval(next_callback) failed", err)
                    end
                end)
            else
                local ok, err = pcall(next_callback, retval)
                if not ok then
                    print("aio.chain: next_callback failed", err)
                end
            end
        end
    end

    first(function (...)
        local ok, err = pcall(next_callback, ...)
        if not ok then
            print("aio.chain: first call failed", err)
        end
    end)

    return resolve_event
end

aio:start()

return aio