requests = {}

function requests:on_data(epollfd, childfd, data)
    req = requests[childfd]
    if req == nil then
        req = { complete = data:find("\r\n\r\n"), data=data }
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

        net_write(
            epollfd, 
            childfd, 
            string.format("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-length: %d\r\n\r\n%s", #response, response), 
            true
        )
    end
end

function requests:on_http(method, url, headers, body)
    kv = ""
    for k, v in pairs(headers) do
        kv = kv .. " " .. k .. ": " .. v .. "\n"
    end
    return string.format("Method: %s\nURL: %s\nHeaders:\n%s", method, url, kv)
end

function on_data(epollfd, childfd, data, len)
    requests:on_data(epollfd, childfd, data)
end