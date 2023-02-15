--- @class net
--- @field write fun(elfd: lightuserdata, childfd: lightuserdata, data: string, close: boolean): boolean write tdata o file descriptor
--- @field close fun(elfd: lightuserdata, childfd: lightuserdata): boolean close a file descriptor
--- @field connect fun(elfd: lightuserdata, host: string, port: integer): fd: lightuserdata|nil, err: string|nil open a new network connection
--- @field reload fun() reload server
--- @field listdir fun(dir: string): string[] list files in directory
net = net or {}

--- @type lightuserdata
ELFD = ELFD or nil

--- @type integer
WORKERID = WORKERID or nil

--- Aliases to be defined here
--- @alias aiostream fun() : any ... AIO input stream
--- @alias aiocor fun(stream: aiostream, resolve?: fun(value: any)): nil AIO coroutine
--- @alias aioresolve fun(result: any): nil AIO resolver
--- @alias aiothen fun(on_resolved: fun(...: any)) AIO then
--- @alias aiohttphandler fun(self: aiosocket, query: string, headers: {[string]: string}, body: string) AIO HTTP handler

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
    --- @type boolean keep alive
    ka = false
}

--- Write data to network
---
--- @param data string data to write
--- @param close boolean|nil asdf
--- @return boolean
function aiosocket:write(data, close)
    return net.write(self.elfd, self.childfd, data, close or false)
end

--- Close socket
--- @return boolean
function aiosocket:close()
    return net.close(self.elfd, self.childfd)
end

--- Write HTTP respose
---@param status string status code
---@param headers {[string]: any}|string headers or content-type
---@param response string response body
---@return boolean
function aiosocket:http_response(status, headers, response)
    local str_headers = ""
    if type(headers) == "string" then
        str_headers = "Content-type: " .. headers .. "\r\n"
    else
        for k, v in pairs(headers) do
            str_headers = str_headers .. string.format("%s: %s\r\n", k, v)
        end
    end
    return net.write(
        self.elfd, 
        self.childfd, 
        string.format("HTTP/1.1 %s\r\nConnection: %s\r\n%sContent-length: %d\r\n\r\n%s",
            status,
            self.ka and "keep-alive" or "close",
            str_headers,
            #response, response
        ), 
        not self.ka
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

--- Create new socket instance
---
--- @param elfd lightuserdata
--- @param childfd lightuserdata
--- @return aiosocket
function aiosocket:new(elfd, childfd)
    local socket = { elfd = elfd, childfd = childfd, ka = false }
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
        }
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

    self:handle_as_http(elfd, childfd, data, len)
end

--- Create new HTTP handler for network stream
---
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
--- @param data string incoming stream data
--- @param len integer length of data
function aio:handle_as_http(elfd, childfd, data, len)
    local fd = aiosocket:new(elfd, childfd)
    self.fds[childfd] = fd

    self:cor(fd, function (stream)
        local req = {
            headerComplete = nil,
            bodyComplete = false, 
            bodyOverflow = false,
            bodyLength = 0, 
            data = ""
        };
        for data in stream do
            -- check for header completion
            if req.headerComplete == nil then
                req.headerComplete = data:find("\r\n\r\n", nil, true)
                if req.headerComplete then
                    req.headerComplete = req.headerComplete + #req.data
                    req.headerComplete = req.headerComplete + 3 -- offset by \r\n\r\n length
                    local length = req.data:match("[Cc]ontent%-[Ll]ength: (%d+)")
                    if length ~= nil then
                        req.bodyLength = tonumber(length)
                    end
                end
            end

            req.data = req.data .. data

            -- check for body completion
            if req.headerComplete and not req.bodyComplete then
                req.bodyComplete = #req.data - req.headerComplete >= req.bodyLength
            end

            -- if body is not complete, we yield
            if req.bodyComplete then
                local pivot = req.headerComplete + req.bodyLength
                local rest = req.data:sub(pivot + 1)
                req.data = req.data:sub(1, pivot)

                local method, url, headers, body = aio:parse_http(req.data)
                local close = (headers["connection"] or "close"):lower() == "close"
                fd.ka = not close

                aio:on_http(fd, method, url, headers, body)

                req.data = rest
                if not close then
                    req.headerComplete = nil
                    req.bodyComplete = nil
                    req.bodyLength = 0
                end
            end
            coroutine.yield(true)
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
---@return string request body
function aio:parse_http(data)
    local headers = {}
    local method, url, header, body = data:match("(.-) (.-) HTTP.-\r(.-)\n\r\n(.*)")

    for key, value in header:gmatch("\n(.-):[ ]*(.-)\r") do
        headers[key:lower()] = value
    end

    return method, url, headers, body
end

--- Parse HTTP query
--- @param query string query string
--- @return {[string]: string} query query params
function aio:parse_query(query)
    local params = {}
    query = "&" .. query
    -- match everything where first part doesn't contain = and second part doesn't contain &
    for key, value in query:gmatch("%&([^=]+)=?([^&]*)") do
        params[key] = self:parse_url(value)
    end
    return params
end


--- Parse URL encoded string
--- @param url string url encoded string
--- @return string text url decoded value
function aio:parse_url(url)
    local new = url:gsub("%+", " "):gsub("%%([0-9A-F][0-9A-F])", function(part)
        return string.char(tonumber(part, 16))
    end)
    return new
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
    self.fds[sock] = aiosocket:new(elfd, sock)
    return self.fds[sock], nil
end

--- Handler called when socket is closed
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aio:on_close(elfd, childfd)
    local fd = self.fds[childfd]
    self.fds[childfd] = nil

    -- notify with close event
    if fd ~= nil then
        fd:on_close(elfd, childfd)
    end
end

--- Handler called when socket connects
--- @param elfd lightuserdata epoll handle
--- @param childfd lightuserdata socket handle
function aio:on_connect(elfd, childfd)
    local fd = self.fds[childfd]

    -- notify with connect event
    if fd ~= nil then
        fd:on_connect(elfd, childfd)
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
    
    --- Connect handler
    --- @param elfd lightuserdata
    --- @param childfd lightuserdata
    _G.on_connect = function(elfd, childfd)
        aio:on_connect(elfd, childfd)
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
---@return function on_resolved callback
---@return fun(on_resolved: fun(...: any)) resolver
function aio:prepare_promise()
    local early, early_val = false, nil

    --- Resolve callback with coroutine return value
    --- This code is indeed repeated 3x in this repository to avoid unnecessary
    --- encapsulation on on_resolved (as it would be changed later and reference would be lost)
    --- and save us some performance
    --- @param ... any coroutine return values
    local on_resolved = function(...) early, early_val = true, {...} end

    --- Set AIO resolver callback
    --- @type aiothen
    local resolve_event = function(callback)
        if early then
            callback(unpack(early_val))
        else
            on_resolved = callback
        end
    end

    return function(...) return on_resolved(...) end, resolve_event
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
    local cor = coroutine.create(callback)
    local early, early_val = false, nil

    --- Resolve callback with coroutine return value
    --- This code is indeed repeated 3x in this repository to avoid unnecessary
    --- encapsulation on on_resolved (as it would be changed later and reference would be lost)
    --- and save us some performance
    --- @param ... any coroutine return values
    local on_resolved = function(...) early, early_val = true, {...} end

    --- Set AIO resolver callback
    --- @type aiothen
    local resolve_event = function(callback)
        if early then
            callback(unpack(early_val))
        else
            on_resolved = callback
        end
    end

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

        running = true
        local ok, result = coroutine.resume(cor, provider, resolver)
        running = false

        if not ok then
            print("error", result)
        end

        -- in case close event was invoked from the coroutine, it shall be handled here
        if ended then
            ok, result = coroutine.resume(cor, provider, resolver)
            ended = false

            if not ok then
                print("error", result)
            end
        end
    end

    -- closing event handler that sends nil signal to coroutine to terminate the iterator
    if close_handler ~= nil then
        target[close_handler] = function(self, ...)
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
                    print("error", result)
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

--- Gather multiple asynchronous tasks
--- @param ... aiothen coroutine resolvers
--- @return aiothen resolver values
function aio:gather(...)
    local tasks = {...}
    local counter = #{...}
    local retvals = {}
    local early, early_val = false, nil

    --- Resolve callback with coroutine return value
    --- @param ... any coroutine return values
    local on_resolved = function(...) early, early_val = true, {...} end

    --- Set AIO resolver callback
    --- @type aiothen
    local resolve_event = function(callback)
        if early then
            callback(unpack(early_val))
        else
            on_resolved = callback
        end
    end

    for i, task in ipairs(tasks) do
        table.insert(retvals, nil)
        task(function (value)
            counter = counter - 1
            retvals[i] = value
            if counter == 0 then
                on_resolved(unpack(retvals))
            end
        end)
    end

    return resolve_event
end

--- Chain multiple AIO operations sequentially
--- @param first aiothen
--- @param ... fun(...: any): aiothen|any
--- @return aiothen retval return value of last task
function aio:chain(first, ...)
    local callbacks = {...}
    local at = 1
    local early, early_val = false, nil
    --- Resolve callback with coroutine return value
    --- @param ... any coroutine return values
    local on_resolved = function(...) early, early_val = true, {...} end

    --- Set AIO resolver callback
    --- @type aiothen
    local resolve_event = function(callback)
        if early then
            callback(unpack(early_val))
        else
        on_resolved = callback
        end
    end

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
                        print(err)
                    end
                end)
            else
                local ok, err = pcall(next_callback, retval)
                if not ok then
                    print(err)
                end
            end
        end
    end

    first(function (...)
        local ok, err = pcall(next_callback, ...)
        if not ok then
            print(err)
        end
    end)

    return resolve_event
end

aio:start()

return aio