--- @class inotify_event
--- @field name string file name
--- @field wd lightuserdata watch descriptor
--- @field dir boolean true if file is a directory
--- @field modify boolean true if file was modified
--- @field create boolean true if file as created
--- @field delete boolean true if file was deleted
--- @field clock number time since start of program

--- @class net
--- @field write fun(elfd: lightuserdata, childfd: lightuserdata, fdtype: lightuserdata, data: string, offset: integer): boolean write data to file descriptor
--- @field close fun(elfd: lightuserdata, childfd: lightuserdata, fdtype: lightuserdata): boolean close a file descriptor
--- @field connect fun(elfd: lightuserdata, host: string, port: integer): fd: lightuserdata|nil, err: string|nil open a new network connection
--- @field reload fun(c_reload: lightuserdata|nil) reload server, if c_reload == S80_RELOAD, C binary is reloaded given executable was built with DYNAMIC=true
--- @field listdir fun(dir: string): string[] list files in directory
--- @field inotify_init fun(elfd: lightuserdata): fd: lightuserdata|nil, error: string|nil initialize inotify
--- @field inotify_add fun(elfd: lightuserdata, childfd: lightuserdata, target: string): wd: lightuserdata add file to watchlist of inotify, returns watch descriptor
--- @field inotify_remove fun(elfd: lightuserdata, childfd: lightuserdata, wd: lightuserdata): boolean, string|nil remove watch decriptor from watchlist
--- @field inotify_read fun(data: string): inotify_event[] parse inotify events to Lua table
--- @field partscan fun(haystack: string, needle: string, offset: integer): pos: integer, length: integer find partial substring in a string
--- @field sockname fun(fd: lightuserdata): ip: string, port: integer get ip and port of remote FD
--- @field clock fun(): number return monotonic clock in seconds
--- @field popen fun(elfd: lightuserdata, command: string, ...: string): read: lightuserdata|nil, write: lightuserdata|string process
net = net or {}

--- @class crypto
--- @field sha1 fun(data: string): string perform sha1(data), returns bytestring with raw data
--- @field sha256 fun(data: string): string perform sha256(data), returns bytestring with raw data
--- @field hmac_sha256 fun(data: string, key: string): string perform HMAC SHA256, returns bytestring with raw data
--- @field cipher fun(data: string, key: string, iv: boolean, encrypt: boolean): result: string?, error: string perform encryption/decryption, if iv is false, iv is all zeros and not inserted to result, key must be at least 128 bits
--- @field to64 fun(data: string): string encode to base64
--- @field from64 fun(data: string): string decode from base64
--- @field random fun(n: integer): string generate n random bytes
--- @field ssl_new_server fun(pubkey: string, privkey: string): lightuserdata|nil initialize new global SSL context
--- @field ssl_release fun(ssl: lightuserdata) release SSL context
--- @field ssl_bio_new fun(ssl: lightuserdata, elfd: lightuserdata, childfd: lightuserdata, ktls: boolean|nil): lightuserdata|nil initialize new non-blocking SSL BIO context
--- @field ssl_bio_release fun(bio: lightuserdata, flags: integer) release BIO context, flags, 1 to release just BIO, 2 to release just context
--- @field ssl_bio_write fun(bio: lightuserdata, data: string): integer write to BIO, returns written bytes, <0 if error
--- @field ssl_bio_read fun(bio: lightuserdata): string|nil read from BIO, nil if error
--- @field ssl_accept fun(bio: lightuserdata): boolean|nil perform SSL accept, nil if error, true if IO request, false otherwise
--- @field ssl_init_finished fun(bio: lightuserdata): boolean true if SSL accept is finished
--- @field ssl_read fun(bio: lightuserdata): string|nil, integer|nil read from SSL, returns decrypted data or nil on error, return true if write is requested
--- @field ssl_write fun(bio: lightuserdata, data: string): integer write data to SSL, encrypted data can be retrieved using bio_read later
--- @field ssl_requests_io fun(bio: lightuserdata): boolean|nil nil if error, true if IO request, false otherwise
crypto = crypto or {}

--- @class codec
--- @field json_encode fun(obj: table): string JSON encode object or an array
--- @field json_decode fun(text: string): table JSON decode text into array or object
--- @field lua_encode fun(obj: table): string Lua encode object or an array
--- @field hex_encode fun(text: string): string Hex encode text
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

--- @type lightuserdata
S80_FD_SOCKET = S80_FD_SOCKET or nil
--- @type lightuserdata
S80_FD_KTLS_SOCKET = S80_FD_KTLS_SOCKET or nil
--- @type lightuserdata
S80_FD_PIPE = S80_FD_PIPE or nil
--- @type lightuserdata
S80_FD_OTHER = S80_FD_OTHER or nil
--- @type lightuserdata
S80_RELOAD = S80_RELOAD or nil

--- @type boolean
KTLS = KTLS or false
if KTLS and (os.getenv("KTLS") or "true") == "false" then KTLS = false end
--- Aliases to be defined here
--- @alias aiostream fun() : any ... AIO input stream
--- @alias aiocor fun(stream: aiostream, resolve?: fun(value: any)|thread): nil AIO coroutine
--- @alias aioresolve fun(result: any): nil AIO resolver
--- @alias aiothen fun(on_resolved: fun(...: any)|thread) AIO then
--- @alias aiohttphandler fun(self: aiosocket, query: string, headers: {[string]: string}, body: string) AIO HTTP handler
--- @alias aiowritebuf {d: string, o: integer}
--- @alias aiohttpquery {[string]: string, e: {[string]: string}}
--- @alias aiomatches fun(data: string): boolean Matcher function
--- @alias aiohandler fun(fd: aiosocket) Handler function

--- @generic V : string
--- @alias aiopromise<V> fun(on_resolved: fun(result: V)) AIO promise

unpack = unpack or table.unpack
on_before_http = on_before_http or nil
on_after_http = on_after_http or nil

--- Check whether value is error
---@param value any
---@return boolean
function iserror(value)
    return value and type(value) == "table" and value.error
end

--- Check whether result(s) contains data and is not error
---@param ... any
---@return boolean
function ispresent(...)
    for _, value in ipairs({...}) do
        if value == nil or iserror(value) then
            return false
        end
    end
    return #{...} > 0
end

--- Create error result
---@param message string
---@return table
function make_error(message)
    return { error = message }
end

--- AIOsocket class
--- Provides easy wrapper to receive events per object instead of globally
---
--- @class aiosocket
local aiosocket = {
    --- @type lightuserdata socket file scriptor
    fd = nil,
    --- @type lightuserdata event loop file descriptor
    elfd = nil,
    --- @type lightuserdata fd type
    ft = nil,
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
    --- @type lightuserdata SSL context
    bio = nil,
    --- @type boolean|nil true if TLS is ready
    tls = nil,
    --- @type boolean|nil true if KTLS is to be used
    ktls = nil
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
    local ok, written = net.write(self.elfd, self.fd, self.ft, data, 0)
    if not ok then
        if not self.closed then
            self.closed = true
            self.buf = {}
            self:on_close(self.elfd,self.fd)
        end
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
    return net.close(self.elfd, self.fd, self.ft)
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
            str_headers = str_headers .. k .. ": " .. v .. "\r\n"
        end
    end
    return self:write(
        "HTTP/1.1 " .. status .. 
        "\r\nconnection: " .. (self.cw and "close" or "keep-alive") .. 
        "\r\n" .. str_headers .. "content-length: " .. #response .. 
        "\r\n\r\n" .. response
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
            ok, written = net.write(elfd, childfd, self.ft, item.d, item.o)
        end
        if not ok then
            -- if sending failed completly, i.e. socket was closed, end
            if not self.closed then
                self.closed = true
                self.buf = {}
                self:on_close(self.elfd, self.fd)
            end
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
--- @param fdtype lightuserdata
--- @param connected boolean
--- @return aiosocket
function aiosocket:new(elfd, childfd, fdtype, connected)
    local socket = { 
        elfd = elfd, 
        fd = childfd, 
        ft = fdtype,
        cw = false, 
        co = connected or false, 
        wr = connected or false ,
        buf = {},
        closed = false
    }
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
        -- protocol handlers
        protocols = {},
        -- master key
        master_key = nil,
        ---@type {[string]: {size: integer, data: {[string]: {expire: integer|nil, data: any}}}}
        cache = {},
        max_cache_size = 10000
    }
end

--- Generic handler called when data is received
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param fdtype lightuserdata fd type
--- @param data string incoming stream data
--- @param len integer length of data
function aio:on_data(elfd, childfd, fdtype, data, len)
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
        local fd = aiosocket:new(elfd, childfd, fdtype, true)
        self.fds[childfd] = fd
        self:handle_as_http(fd)
        fd:on_data(elfd, childfd, data, len)
    else
        for _, handler in pairs(self.protocols) do
            if handler.matches(data) then
                local fd = aiosocket:new(elfd, childfd, fdtype, true)
                self.fds[childfd] = fd
                handler.handle(fd)
                fd:on_data(elfd, childfd, data, len)
                break
            end
        end
    end
end

--- Add new protocol handler for unknown protocols
---@param name string unique name of protocol
---@param handler {matches: aiomatches, handler: aiohandler} protocol handler
function aio:add_protocol_handler(name, handler)
    self.protocols[name] = handler
end

--- Create new HTTP handler for network stream
---
--- @param fd aiosocket AIO socket to be handled
--- @return aiosocket fd stream
function aio:handle_as_http(fd)
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
    return fd
end

--- Wrap FD into TLS streama
---
--- @param fd aiosocket AIO socket to be handled
--- @param ssl lightuserdata global SSL context
function aio:wrap_tls(fd, ssl)
    local on_data = fd.on_data
    local on_close = fd.on_close
    local raw_write = fd.write
    fd.on_data = function(self, elfd, childfd, data, length)
        if self.closed then return end
        if not self.bio then
            local bio = crypto.ssl_bio_new(ssl, elfd, childfd, KTLS)
            if bio then
                self.bio = bio
                self.tls = false
            else
                self.on_data = on_data
                self.on_close = on_close
                self.write = raw_write
            end
        end
        if not self.tls then 
            crypto.ssl_bio_write(self.bio, data)
            local ok = crypto.ssl_accept(self.bio)
            if ok then
                while true do
                    local rd = crypto.ssl_bio_read(self.bio)
                    if not rd or #rd == 0 then break end
                raw_write(self, rd)
                end
            end
            self.tls = crypto.ssl_init_finished(self.bio)
            if self.tls and KTLS then
                self.ktls = true
                crypto.ssl_bio_release(self.bio, 1)
                fd.on_data = on_data
                fd.write = raw_write
            end
        else
            crypto.ssl_bio_write(self.bio, data)
            local ok = true
            while ok do
                local rd, wr = crypto.ssl_read(self.bio)
                if not rd or #rd == 0 then
                    ok = false
                else
                    pcall(on_data, self, elfd, childfd, rd, #rd)
                end
                if wr == true then
                    while true do
                        local rd = crypto.ssl_bio_read(self.bio)
                        if not rd or #rd == 0 then break end
                        raw_write(self, rd)
                    end
                end
            end
        end
    end
    fd.write = function(self, data, ...)
        if not self.tls then 
            return raw_write(self, data, ...) 
        end

        local wr = crypto.ssl_write(self.bio, data)
        while wr >= 0 and true do
            local rd = crypto.ssl_bio_read(self.bio)
            if not rd or #rd == 0 then break end
            raw_write(self, rd)
        end
    end
    fd.on_close = function(self)
        if self.bio then
            crypto.ssl_bio_release(self.bio, self.ktls and 2 or 3)
            self.bio = nil
        end
        if self.closed then return end
        pcall(on_close, self, fd.elfd, fd.fd)
        self.closed = true
    end
end

---Get IP address of a socket
---@param sock aiosocket remote socket
---@param headers table|nil http headers if available
---@return string ip address
---@return integer port port
function aio:get_ip(sock, headers)
    local fd = sock.fd
    local ip, port = net.sockname(fd)
    if headers ~= nil then
        if headers["x-real-ip"] then
            ip = headers["x-real-ip"]
        end
    end
    return ip, port
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

    for key, value in header:gmatch("\n(.-):[ ]*([^\r]+)") do
        headers[key:lower()] = value
    end

    return method, url, headers
end

--- Parse HTTP query
--- @param query string query string
--- @param private_key string|nil string decryption key for ?e
--- @return aiohttpquery query query params
function aio:parse_query(query, private_key)
    local params = {}
    params.e={} -- reserved for encrypted query
    query = "&" .. query
    -- match everything where first part doesn't contain = and second part doesn't contain &
    for key, value in query:gmatch("%&([^=&]+)=?([^&]*)") do
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
            table.insert(values, key .. "=" .. codec.url_encode(tostring(value)))
        end
    end
    if ordered then
        table.sort(values)
    end
    local result = table.concat(values, "&")
    if type(private_key) == "string" then
        return self:cached(private_key, result, function()
            local encrypted = self:encrypt(result, self:create_key(private_key), iv, false)
            if not encrypted then
                encrypted = "nil"
            end
            return "e=" .. codec.url_encode(encrypted)
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
---@param condition boolean|integer|nil if false, no cache is performed, if number, considered expire
---@param expire integer|nil expiry in seconds
---@return T value
function aio:cached(cache_name, key, callback, condition, expire)
    if expire or type(condition) == "number" then
        if not expire then
            expire = net.clock() + condition
            condition = nil
        else
            expire = net.clock() + expire
        end
    end
    if condition == false then
        return callback()
    end
    local cache = self.cache[cache_name]
    if not cache then
        self.cache[cache_name] = {size = 0, data = {}}
        cache = self.cache[cache_name]
    end
    local hit = cache.data[key]
    if hit and (hit.expire == nil or net.clock() <= hit.expire) then
        return hit.data
    end
    if self.max_cache_size == nil or self.max_cache_size == 0 then
        return callback()
    end
    if cache.size == self.max_cache_size then
        local k = next(cache.data)
        if k ~= nil then
            cache.data[k] = nil
        end
    else
        cache.size = cache.size + 1
    end
    hit = callback()
    -- in case callback is a promise, cache the value once it is resolved
    if type(hit) == "function" then
        local resolve, resolver = aio:prepare_promise()
        local requesters = {resolve}
        local result = {}
        local ready = false
        cache.data[key] = {
            data = function (cb)
                if not ready then
                    table.insert(requesters, cb)
                else
                    cb(result.value)
                end
            end,
            expire = expire
        }
        hit(function (value)
            ready = true
            result.value = value
            for _, cb in ipairs(requesters) do
                cb(value)
            end
            requesters = nil
        end)
        return resolver
    else
        cache.data[key] = {
            expire = expire,
            data = hit
        }
    end
    return hit
end

--- Create URL from endpoint and query params
---@param endpoint string endpoint
---@param params {[string]: string, e?: boolean, iv?: boolean, ordered?: boolean} parameters list, if e is false, no encryption is performed
---@param hash string|nil URL hash
---@return any
function aio:to_url(endpoint, params, hash)
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
        path = path .. "?" .. aio:create_query(params, private_key, ordered, iv)
    end
    if hash ~= nil then
        path = path .. "#" .. codec.url_encode(hash)
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
    if type(data) ~= "string" or type(key) ~= "string" then
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
    self.fds[sock] = aiosocket:new(elfd, sock, S80_FD_SOCKET, false)
    return self.fds[sock], nil
end

--- Open a process
---@param elfd lightuserdata event loop
---@param command string command
---@param ... string args
--- @return aiosocket|nil write
--- @return string|aiosocket read
function aio:popen(elfd, command, ...)
    local args = self:map({...}, tostring)
    local sock, err = net.popen(elfd, command, unpack(args))
    if sock == nil or type(err) == "string" then
        ---@diagnostic disable-next-line: return-type-mismatch
        return nil, err
    end
    self.fds[sock] = aiosocket:new(elfd, sock, S80_FD_PIPE, false)
    self.fds[err] = aiosocket:new(elfd, err, S80_FD_PIPE, false)
    return self.fds[sock], self.fds[err]
end

--- Open a process and read it's entire stdout
---@param elfd lightuserdata event loop
---@param command string command
---@param ... string args
---@return fun(resolve: fun(contents: string|nil)) promise
function aio:popen_read(elfd, command, ...)
    local resolve, resolver = self:prepare_promise()
    local rd, _ = self:popen(elfd, command, ...)
    if rd == nil then
        resolve(nil)
    else
        self:read_stream(rd)(function (result)
            resolve(result)
        end)
    end
    return resolver
end

--- Watch for changes in file or directory
---@param elfd lightuserdata epoll handle
---@param targets string[] list of files to watch
---@param on_change fun(events: inotify_event[]) callback with changes
---@return aiosocket|nil
function aio:watch(elfd, targets, on_change)
    local fd, err = net.inotify_init(elfd)
    if fd ~= nil then
        local sock = aiosocket:new(elfd, fd, S80_FD_OTHER, true)
        self.fds[fd] = sock
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
    --- @param fdtype lightuserdata
    --- @param data string
    --- @param len integer
    _G.on_data = function(elfd, childfd, fdtype, data, len)
        aio:on_data(elfd, childfd, fdtype, data, len)
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

    local running, ended, dead = false, false, false

    -- main event handler that resumes coroutine as events arrive and provides data for iterator
    target[event_handler] = function(self, epfd, chdfd, ...)
        if dead then return end
        data = {...}

        -- if coroutine finished it's job, unsubscribe the event handler
        local status = coroutine.status(cor)
        if status == "dead" then
            dead = true
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
            if dead then return end
            local status = coroutine.status(cor)
            if status == "dead" then
                dead = true
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

--- Read stream until end
---@param target aiosocket fd to be read fully
---@return fun(on_resolved: fun(result: string)|thread)
function aio:read_stream(target)
    return self:cor(target, function (stream, resolve)
        local data = {}
        for chunk in stream do
            table.insert(data, chunk)
            coroutine.yield()    
        end
        resolve(table.concat(data))
    end)
end


--- Create a new coroutine
---@param callback fun(...: any): any
---@return thread coroutine
function aio:cor0(callback)
    return coroutine.create(callback)
end

--- Execute code in async environment so await can be used
---@param callback function to be ran
---@param on_error function|nil error handler
---@return thread coroutine
---@return boolean ok value
function aio:async(callback, on_error)
    local cor = aio:cor0(callback)
    local ok, result = coroutine.resume(cor)
    if not ok then
        print("aio.async failed: ", result)
        if on_error ~= nil then
            on_error(result)
        end
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
        local read, read_len = {}, 0
        local req_delim = false
        local exit = requested == nil
        local nil_resolve = false
        local prev_match = 0

        req_delim = type(requested) == "string"

        -- if we failed in very first step, return early and resolve with nil
        if not ok then
            print("aio.buffered_cor: coroutine failed in initial run", requested)
            resolve(nil)
            return
        end

        -- iterate over bytes from network as we receive them
        for data in stream do
            table.insert(read, data)
            read_len = read_len + #data
            -- check if state is ok, and if we read >= bytes requested to read
            while not exit and ok and read_len > 0 do
                local pivot = requested
                local skip = 0
                if req_delim then
                    local full = true
                    local data = read[#read]
                    -- there are 6 possible cases total
                    if #read > 1 and prev_match > 0 then
                        full = false
                        local pos, match_len = net.partscan(data, requested:sub(prev_match + 1), 1)
                        if pos == 1 and match_len + prev_match == #requested then
                            -- 1. we already read something that ended partially with out delimiter
                            -- and now it begins with the rest!
                            pivot = read_len - #data + pos - (#requested - match_len) - 1
                            skip = #requested
                            prev_match = 0
                        elseif pos == 1 then
                            -- 2. last time it ended with our delimiter partially and now it begins partially
                            prev_match = prev_match + match_len
                            pivot = nil
                        else
                            -- 3. last time it ended with out delimiter partially, but now no match
                            -- at the beginning, in this case we move to scan as usual with full delimiter
                            prev_match = 0
                            pivot = nil
                            full = true
                        end
                    end
                    if full then
                        -- scan the string using full delimiter
                        local pos, match_len = net.partscan(data, requested, 1)
                        if match_len == #requested then
                            -- if data was found right away, set pivot
                            pivot = read_len - #data + pos - 1
                            skip = #requested
                            prev_match = 0
                        elseif match_len > 0 and pos + match_len - 1 == #data then
                            -- if delimiter was found at the end, but only partially, proceed to steps 1/2/3 next time
                            prev_match = match_len
                            pivot = nil
                        else
                            -- if delimiter wasn't found, better luck next time
                            prev_match = 0
                            pivot = nil
                        end
                    end
                    if not pivot then break end
                elseif pivot > read_len then
                    break
                end
                local str_read = table.concat(read)
                -- iterate over all surplus we have and resume the receiver coroutine
                -- with chunks of size requested by it
                ok, requested = coroutine.resume(reader, str_read:sub(1, pivot))
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
                    read = { str_read:sub(pivot + skip + 1) }
                    read_len = #read[1]
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
---@param array table array to transform
---@param fn function map function
---@param ... any additional parameters to be passed to fn
---@return table transformed array
function aio:map(array, fn, ...)
    local new_array = {}
    for i=1,#array do
        new_array[i] = fn(array[i], ...)
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
