local aio = {
    sv={},
    cl={}
}

function aio:on_data(epollfd, childfd, data, len)
    cl = self.cl[childfd]
    if cl ~= nil then
        if cl.on_data then
            return cl:on_data(epoll, childfd, data, len)
        end
    end
    -- the data received to main HTTP server gets treated here
    req = self.sv[childfd]
    if req == nil then
        req = { complete = data:find("\r\n\r\n"), data=data }
        self.sv[childfd] = req
    end
    if not req.complete then
        req.data = req.data .. data
        req.complete = req.data:find("\r\n\r\n")
    end
    if req.complete then
        headers = {}
        method, url, header, body = req.data:match("(.-) (.-) HTTP.-\r(.-)\n\r\n(.*)")

        header:gsub("\n(.-):[ ]*(.-)\r", function(key, value)
            headers[key] = value
        end)

        response = self:on_http(method, url, headers, body)

        net.write(
            epollfd, 
            childfd, 
            string.format("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-length: %d\r\n\r\n%s", #response, response), 
            true
        )
    end
end

function aio:connect(epollfd, host, port)
    local sock, err = net.connect(epollfd, "crymp.net", 80)
    if err then
        return nil, err
    end
    self.cl[sock] = {
        childfd = sock,
        write = function(self, data, close)
            return net.write(epollfd, self.childfd, data, close or false)
        end,
        close = function(self)
            return net.close(epollfd, self.childfd)
        end
    }
    return self.cl[sock], nil
end

function aio:on_close(epollfd, childfd)
    cl = self.cl[childfd]
    self.sv[childfd] = nil

    -- external sockets get treated special way
    if cl ~= nil then
        if cl.on_close then
            return cl:on_close(epoll, childfd)
        end
    end
    self.cl[childfd] = nil
end

function aio:on_connect(epollfd, childfd)
    cl = self.cl[childfd]

    -- external sockets get treated special way
    if cl ~= nil then
        if cl.on_connect then
            return cl:on_connect(epoll, childfd)
        end
    end
end

function aio:start()
    _G.on_init = function(epollfd, parentfd)
        if aio.on_init then
            aio:on_init(epollfd, parentfd)
        end
    end
    
    _G.on_data = function(epollfd, childfd, data, len)
        aio:on_data(epollfd, childfd, data, len)
    end
    
    _G.on_close = function(epollfd, childfd)
        aio:on_close(epollfd, childfd)
    end
    
    _G.on_connect = function(epollfd, childfd)
        aio:on_connect(epollfd, childfd)
    end
end

-- overridable
function aio:on_init(epollfd, parentfd)

end

-- overridable
function aio:on_http(method, url, headers, body)
    return "OK"
end

return aio