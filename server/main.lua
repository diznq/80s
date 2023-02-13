---@type aio
local aio = loadfile("server/aio.lua")()

aio:start()

function aio:on_http(method, url, headers, body)
    return "Hi"
end

function aio:on_init(elfd, parentfd)
    if false then
        -- TCP socket example
        local sock, err = aio:connect(elfd, "crymp.net", 80)
        if sock then
            -- coroutinized version
            aio:cor1(sock, "on_connect", function()
                sock:write("GET /api/servers HTTP/1.1\r\nHost: crymp.net\r\nAccept: application/json\r\nConnection: close\r\n\r\n")
            end)

            aio:cor(sock, function(stream)
                local data = ""
                for chunk, length in stream do
                    print("#" .. WORKERID .. ": Received", length, "bytes")
                    data = data .. chunk
                    coroutine.yield()
                end
            end)
        else
            print("failed to connect: ", err)
        end
    end
end