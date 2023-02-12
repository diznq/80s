---@type aio
local aio = loadfile("server/aio.lua")()

aio:start()

function aio:on_http(method, url, headers, body)
    local kv = ""
    for k, v in pairs(headers) do
        kv = kv .. " " .. k .. ": " .. v .. "\n"
    end
    return string.format("Method: %s\nURL: %s\nHeaders:\n%s", method, url, kv)
end

function aio:on_init(epollfd, parentfd)
    if true then
        -- TCP socket example
        local sock, err = aio:connect(epollfd, "crymp.net", 80)
        if sock then
            -- coroutinized version
            aio:cor(sock, "on_connect", function(data)
                sock:write("GET /api/servers HTTP/1.1\r\nHost: crymp.net\r\nAccept: application/json\r\nConnection: close\r\n\r\n")
            end)

            aio:cor(sock, "on_data", "on_close", function(_)
                local data = ""
                while _.data ~= nil do
                    local received = _.data[3]
                    data = data .. received
                    coroutine.yield(true)
                end
                print("Data: ", data)
            end)
        else
            print("failed to connect: ", err)
        end
    end
end