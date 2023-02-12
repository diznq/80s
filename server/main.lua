local aio = loadfile("server/aio.lua")()

aio:start()

function aio:on_http(method, url, headers, body)
    kv = ""
    for k, v in pairs(headers) do
        kv = kv .. " " .. k .. ": " .. v .. "\n"
    end
    return string.format("Method: %s\nURL: %s\nHeaders:\n%s", method, url, kv)
end

function aio:on_init(epollfd, parentfd)
    --[[
    -- TCP socket example
    local sock, err = aio:connect(epollfd, "crymp.net", 80)
    if sock then
        function sock:on_connect(epollfd, childfd)
            self:write("GET /api/servers HTTP/1.1\r\nHost: crymp.net\r\nAccept: application/json\r\nConnection: close\r\n\r\n")
        end
        function sock:on_data(epollfd, childfd, data, length)
            print("Received " .. tostring(length) .. " of data ", data)
        end
        function sock:on_close(epollfd, childfd)
            print("Closed")
        end
    end
    --]]
end