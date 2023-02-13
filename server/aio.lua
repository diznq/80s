---------- aiosocket

--- @class aiosocket
local aiosocket = {
    --- @type userdata
    childfd = nil,
    --- @type userdata
    epollfd = nil
}

function aiosocket:write(data, close)
    return net.write(self.epollfd, self.childfd, data, close or false)
end

function aiosocket:close()
    return net.close(self.epollfd, self.childfd)
end

--- Close handler of socket, overridable
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
function aiosocket:on_close(epollfd, childfd)

end

--- Data handler of socket, overridable
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
--- @param data string stream data
--- @param length integer length of data
function aiosocket:on_data(epollfd, childfd, data, length)

end

--- Connect handler of socket, overridable
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
function aiosocket:on_connect(epollfd, childfd)

end

--- Create new socket instance
--- @param epollfd userdata
--- @param childfd userdata
--- @return aiosocket
function aiosocket:new(epollfd, childfd)
    local socket = { epollfd = epollfd, childfd = childfd }
    setmetatable(socket, self)
    self.__index = self
    return socket
end

---------- aio
--- @class aio
local aio = {
    fds={}
}

--- Handler called when data is received
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
--- @param data string incoming stream data
--- @param len integer length of data
function aio:on_data(epollfd, childfd, data, len)
    -- default socket handling
    local fd = self.fds[childfd]
    if fd ~= nil then
        fd:on_data(epollfd, childfd, data, len)
        return
    end

    -- if socket didn't exist, create it with default handler
    fd = aiosocket:new(epollfd, childfd)
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
                req.headerComplete = data:find("\r\n\r\n")
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
                local response = aio:on_http(method, url, headers, body)

                net.write(
                    epollfd, 
                    childfd, 
                    string.format("HTTP/1.1 200 OK\r\nConnection: %s\r\nContent-length: %d\r\n\r\n%s",
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
    fd:on_data(epollfd, childfd, data, len)
end

function aio:parse_http(data)
    local headers = {}
    local method, url, header, body = data:match("(.-) (.-) HTTP.-\r(.-)\n\r\n(.*)")

    header:gsub("\n(.-):[ ]*(.-)\r", function(key, value)
        headers[key] = value
    end)

    return method, url, headers, body
end

--- Create a new TCP socket to host:port
--- @param epollfd userdata epoll handle
--- @param host string host name or IP address
--- @param port integer port
--- @return aiosocket|nil socket
--- @return string|nil error
function aio:connect(epollfd, host, port)
    local sock, err = net.connect(epollfd, host, port)
    if err then
        return nil, err
    end
    self.fds[sock] = aiosocket:new(epollfd, sock)
    return self.fds[sock], nil
end

--- Handler called when socket is closed
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
function aio:on_close(epollfd, childfd)
    local fd = self.fds[childfd]
    self.fds[childfd] = nil

    -- notify with close event
    if fd ~= nil then
        fd:on_close(epollfd, childfd)
    end
end

--- Handler called when socket connects
--- @param epollfd userdata epoll handle
--- @param childfd userdata socket handle
function aio:on_connect(epollfd, childfd)
    local fd = self.fds[childfd]

    -- notify with connect event
    if fd ~= nil then
        fd:on_connect(epollfd, childfd)
    end
end

function aio:start()
    --- Init handler
    --- @param epollfd userdata
    --- @param parentfd userdata
    _G.on_init = function(epollfd, parentfd)
        if aio.on_init then
            aio:on_init(epollfd, parentfd)
        end
    end
    
    --- Data handler
    --- @param epollfd userdata
    --- @param childfd userdata
    --- @param data string
    --- @param len integer
    _G.on_data = function(epollfd, childfd, data, len)
        aio:on_data(epollfd, childfd, data, len)
    end
    
    --- Close handler
    --- @param epollfd userdata
    --- @param childfd userdata
    _G.on_close = function(epollfd, childfd)
        aio:on_close(epollfd, childfd)
    end
    
    --- Connect handler
    --- @param epollfd userdata
    --- @param childfd userdata
    _G.on_connect = function(epollfd, childfd)
        aio:on_connect(epollfd, childfd)
    end
end

--- Initialization handler
--- @param epollfd userdata epoll handle
--- @param parentfd userdata server socket handle
function aio:on_init(epollfd, parentfd)

end

--- HTTP request handler
--- @param method string http method
--- @param url string URL
--- @param headers table headers table
--- @param body string request body
--- @return string response
function aio:on_http(method, url, headers, body)
    return "OK"
end

--- Wrap handlers into coroutine, example:
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
---@param target any object to be wrapped
---@param event_handler any main event source that resumes coroutine
---@param close_handler any secondary event source that closes coroutine (sends nil data)
---@param callback any coroutine code, takes stream() that returns arguments (3, 4, ...) skipping epollfd, childfd of event_handler
---@return thread
function aio:cor(target, event_handler, close_handler, callback)
    local data = nil

    if callback == nil then
        callback = close_handler
        close_handler = nil
    end

    if type(event_handler) == "function" then
        callback = event_handler
        event_handler = "on_data"
        close_handler = "on_close"
    end

    local cor = coroutine.create(callback)

    local provider = function()
        if data == nil then return end
        return table.unpack(data)
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

return aio