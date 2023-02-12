---------- aiosocket

---@class aiosocket
local aiosocket = {
    ---@type userdata
    childfd = nil,
    ---@type userdata
    epollfd = nil
}

function aiosocket:write(data, close)
    return net.write(self.epollfd, self.childfd, data, close or false)
end

function aiosocket:close()
    return net.close(self.epollfd, self.childfd)
end

---Close handler of socket, overridable
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
function aiosocket:on_close(epollfd, childfd)

end

---Data handler of socket, overridable
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
---@param data string stream data
---@param length integer length of data
function aiosocket:on_data(epollfd, childfd, data, length)

end

---Connect handler of socket, overridable
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
function aiosocket:on_connect(epollfd, childfd)

end

---Create new socket instance
---@param epollfd userdata
---@param childfd userdata
---@return aiosocket
function aiosocket:new(epollfd, childfd)
    local socket = { epollfd = epollfd, childfd = childfd }
    setmetatable(socket, self)
    self.__index = self
    return socket
end

-------------- aio
---@class aio
local aio = {
    sv={},
    cl={}
}

---Handler called when data is received
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
---@param data string incoming stream data
---@param len integer length of data
function aio:on_data(epollfd, childfd, data, len)
    local cl = self.cl[childfd]
    if cl ~= nil then
        if cl.on_data then
            cl:on_data(epollfd, childfd, data, len)
            return
        end
    end
    -- the data received to main HTTP server gets treated here
    local req = self.sv[childfd]
    if req == nil then
        req = { complete = data:find("\r\n\r\n"), data=data }
        self.sv[childfd] = req
    end
    if not req.complete then
        req.data = req.data .. data
        req.complete = req.data:find("\r\n\r\n")
    end
    if req.complete then
        local headers = {}
        local method, url, header, body = req.data:match("(.-) (.-) HTTP.-\r(.-)\n\r\n(.*)")

        header:gsub("\n(.-):[ ]*(.-)\r", function(key, value)
            headers[key] = value
        end)

        local response = self:on_http(method, url, headers, body)

        net.write(
            epollfd, 
            childfd, 
            string.format("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-length: %d\r\n\r\n%s", #response, response), 
            true
        )
    end
end

---Create a new TCP socket to host:port
---@param epollfd userdata epoll handle
---@param host string host name or IP address
---@param port integer port
---@return aiosocket|nil socket
---@return string|nil error
function aio:connect(epollfd, host, port)
    local sock, err = net.connect(epollfd, host, port)
    if err then
        return nil, err
    end
    self.cl[sock] = aiosocket:new(epollfd, sock)
    return self.cl[sock], nil
end

---Handler called when socket is closed
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
function aio:on_close(epollfd, childfd)
    local cl = self.cl[childfd]
    self.sv[childfd] = nil
    self.cl[childfd] = nil

    -- external sockets get treated special way
    if cl ~= nil then
        if cl.on_close then
            cl:on_close(epollfd, childfd)
        end
    end
end

---Handler called when socket connects
---@param epollfd userdata epoll handle
---@param childfd userdata socket handle
function aio:on_connect(epollfd, childfd)
    local cl = self.cl[childfd]

    -- external sockets get treated special way
    if cl ~= nil then
        if cl.on_connect then
            cl:on_connect(epollfd, childfd)
        end
    end
end

function aio:start()
    ---Init handler
    ---@param epollfd userdata
    ---@param parentfd userdata
    _G.on_init = function(epollfd, parentfd)
        if aio.on_init then
            aio:on_init(epollfd, parentfd)
        end
    end
    
    ---Data handler
    ---@param epollfd userdata
    ---@param childfd userdata
    ---@param data string
    ---@param len integer
    _G.on_data = function(epollfd, childfd, data, len)
        aio:on_data(epollfd, childfd, data, len)
    end
    
    ---Close handler
    ---@param epollfd userdata
    ---@param childfd userdata
    _G.on_close = function(epollfd, childfd)
        aio:on_close(epollfd, childfd)
    end
    
    ---Connect handler
    ---@param epollfd userdata
    ---@param childfd userdata
    _G.on_connect = function(epollfd, childfd)
        aio:on_connect(epollfd, childfd)
    end
end

---Initialization handler
---@param epollfd userdata epoll handle
---@param parentfd userdata server socket handle
function aio:on_init(epollfd, parentfd)

end

---HTTP request handler
---@param method string http method
---@param url string URL
---@param headers table headers table
---@param body string request body
---@return string response
function aio:on_http(method, url, headers, body)
    return "OK"
end

return aio