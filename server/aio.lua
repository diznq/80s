--- Aliases to be defined here
--- @alias aiostream fun() : any[] AIO input stream
--- @alias aiocor fun(stream: aiostream): nil AIO coroutine
--- @alias aiohttphandler fun(script: string, query: string, headers: {[string]: string}, body: string): string, string AIO HTTP handler

unpack = unpack or table.unpack

--- AIOsocket class
--- Provides easy wrapper to receive events per object instead of globally
---
--- @class aiosocket
local aiosocket = {
    --- @type userdata
    childfd = nil,
    --- @type userdata
    elfd = nil
}

--- Write data to network
---
--- @param data string data to write
--- @param close boolean|nil asdf
--- @return boolean
function aiosocket:write(data, close)
    return net.write(self.elfd, self.childfd, data, close or false)
end

function aiosocket:close()
    return net.close(self.elfd, self.childfd)
end

--- Close handler of socket, overridable
---
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
function aiosocket:on_close(elfd, childfd)

end

--- Data handler of socket, overridable
---
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
--- @param data string stream data
--- @param length integer length of data
function aiosocket:on_data(elfd, childfd, data, length)

end

--- Connect handler of socket, overridable
---
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
function aiosocket:on_connect(elfd, childfd)

end

--- Create new socket instance
---
--- @param elfd userdata
--- @param childfd userdata
--- @return aiosocket
function aiosocket:new(elfd, childfd)
    local socket = { elfd = elfd, childfd = childfd }
    setmetatable(socket, self)
    self.__index = self
    return socket
end

if not aiohttp then
    --- AIO default HTTP server
    ---
    --- @class aiohttp
    aiohttp = {
        GET={},
        POST={}
    }
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
        ---@type aiohttp
        http=aiohttp
    }
end

--- Generic handler called when data is received
---
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
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
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
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
            req.data = req.data .. data
            -- check for header completion
            if req.headerComplete == nil then
                req.headerComplete = data:find("\r\n\r\n", nil, true)
                if req.headerComplete then
                    req.headerComplete = req.headerComplete + 3 -- offset by \r\n\r\n length
                    local length = req.data:match("[Cc]ontent%-[Ll]ength: (%d+)")
                    if length ~= nil then
                        req.bodyLength = tonumber(length)
                    end
                end
            end

            -- check for body completion
            if req.headerComplete and not req.bodyComplete then
                req.bodyComplete = #req.data - req.headerComplete >= req.bodyLength
            end

            -- if body is not complete, we yield
            if req.bodyComplete then
                local pivot = req.headerComplete + req.bodyLength
                local rest = req.data:sub(pivot + 1)

                req.data = req.data:sub(1, pivot)

                --print("---------" .. req.data:len() / 110 .. "/" .. #rest / 110 .. "+++++++++")
                local method, url, headers, body = aio:parse_http(req.data)
                local close = (headers["Connection"] or "close"):lower() == "close"
                local status, response = aio:on_http(method, url, headers, body)

                net.write(
                    elfd, 
                    childfd, 
                    string.format("HTTP/1.1 %s\r\nConnection: %s\r\nContent-length: %d\r\n\r\n%s",
                        status,
                        close and "close" or "keep-alive",
                        #response, response
                    ), 
                    close
                )

                req.data = rest
                if #rest > 0 and not close then
                    req.headerComplete = nil
                    req.bodyComplete = nil
                    req.bodyLength = 0
                end
            end
            coroutine.yield()
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
---@return string|nil request body
function aio:parse_http(data)
    local headers = {}
    local method, url, header, body = data:match("(.-) (.-) HTTP.-\r(.-)\n\r\n(.*)")

    for key, value in header:gmatch("\n(.-):[ ]*(.-)\r") do
        headers[key] = value
    end

    return method, url, headers, body
end

--- Create a new TCP socket to host:port
--- @param elfd userdata epoll handle
--- @param host string host name or IP address
--- @param port integer port
--- @return aiosocket|nil socket
--- @return string|nil error
function aio:connect(elfd, host, port)
    local sock, err = net.connect(elfd, host, port)
    if err then
        return nil, err
    end
    self.fds[sock] = aiosocket:new(elfd, sock)
    return self.fds[sock], nil
end

--- Handler called when socket is closed
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
function aio:on_close(elfd, childfd)
    local fd = self.fds[childfd]
    self.fds[childfd] = nil

    -- notify with close event
    if fd ~= nil then
        fd:on_close(elfd, childfd)
    end
end

--- Handler called when socket connects
--- @param elfd userdata epoll handle
--- @param childfd userdata socket handle
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
    --- @param elfd userdata
    --- @param parentfd userdata
    _G.on_init = function(elfd, parentfd)
        if aio.on_init then
            aio:on_init(elfd, parentfd)
        end
    end
    
    --- Data handler
    --- @param elfd userdata
    --- @param childfd userdata
    --- @param data string
    --- @param len integer
    _G.on_data = function(elfd, childfd, data, len)
        aio:on_data(elfd, childfd, data, len)
    end
    
    --- Close handler
    --- @param elfd userdata
    --- @param childfd userdata
    _G.on_close = function(elfd, childfd)
        aio:on_close(elfd, childfd)
    end
    
    --- Connect handler
    --- @param elfd userdata
    --- @param childfd userdata
    _G.on_connect = function(elfd, childfd)
        aio:on_connect(elfd, childfd)
    end
end

--- Initialization handler
---
--- @param elfd userdata epoll handle
--- @param parentfd userdata server socket handle
function aio:on_init(elfd, parentfd)

end

--- Default HTTP request handler
--- @param method string http method
--- @param url string URL
--- @param headers table headers table
--- @param body string|nil request body
--- @return string statusCode status code
--- @return string response response text
function aio:on_http(method, url, headers, body)
    local pivot = url:find("?", 0, true)
    local script = url:sub(0, pivot and pivot - 1 or nil)
    local query = pivot and url:sub(pivot + 1) or ""
    local handlers = self.http[method]
    if handlers ~= nil then
        local handler = handlers[script]
        if handler ~= nil then
            return handler(script, query, headers, body)
        end
    end
    return "404 Not found", script .. " was not found on this server"
end

--- Add HTTP GET handler
--- @param url string URL
--- @param callback aiohttphandler handler
function aiohttp:get(url, callback)
    self.GET[url] = callback
end

--- Add HTTP POST handler
--- @param url string URL
--- @param callback aiohttphandler handler
function aiohttp:post(url, callback)
    self.POST[url] = callback
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
--- @return thread
function aio:cor2(target, event_handler, close_handler, callback)
    local data = nil
    local cor = coroutine.create(callback)

    local provider = function()
        if data == nil then return end
        return unpack(data)
    end

    target[event_handler] = function(self, epfd, chdfd, ...)
        data = {...}
        local ok, result = coroutine.resume(cor, provider)
        if not ok then
            print("error", result)
        end
    end

    if close_handler ~= nil then
        target[close_handler] = function(self, ...)
            data = nil
            coroutine.resume(cor, provider)
        end
    end

    return cor
end

--- Wrap single event handler into a coroutine, evaluates to aio:cor2(target, event_handler, nil, callback)
---
--- @param target aiosocket object to be wrapped
--- @param event_handler string event source that resumes a coroutine
--- @param callback aiocor  coroutine code
--- @return thread
function aio:cor1(target, event_handler, callback)
    return self:cor2(target, event_handler, nil, callback)
end

--- Wrap aiosocket receiver into coroutine, evaluates to aio:cor2(target, "on_data", "on_close", callback)
---
--- @param target aiosocket object to be wrapped
--- @param callback aiocor coroutine code
--- @return thread
function aio:cor(target, callback)
    return self:cor2(target, "on_data", "on_close", callback)
end

return aio