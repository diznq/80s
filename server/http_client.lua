---@type aio
require("aio.aio")

--- @class http_client
local http_client = {}

function http_client:GET(host, script, accept)
    local sock, err = aio:connect(ELFD, host, 80)
    if sock then
        -- coroutinized version
        aio:cor1(sock, "on_connect", function(_, done)
            sock:write(
                string.format(
                    "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: %s\r\nConnection: close\r\n\r\n", 
                    script,
                    host,
                    accept
                )
            )
            done(true)
        end)

        return aio:cor(sock, function(stream, done)
            local data = ""
            for chunk in stream do
                data = data .. chunk
                coroutine.yield()
            end
            done(data)
        end)
    else
        print("failed to connect: ", err)
    end
end

function aio:on_init(elfd, parentfd)
    aio:gather(
        http_client:GET("w3.org", "/", "text/html"), 
        http_client:GET("en.wikipedia.org", "/", "text/html")
    )(function(w3, wiki)
        print("W3 response length: ", #w3)
        print("Wiki response length: ", #wiki)
    end)
end

return http_client