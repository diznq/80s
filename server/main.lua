---@type aio
local aio = loadfile("server/aio.lua")()

aio:start()

aio:http_get("/haha", function (fd, query, headers, body)
    local params = aio:parse_query(query)
    fd:http_response(
        "200 OK", 
        "text/plain", 
        ("Hi, %s!"):format(params.name or "Nomad")
    )
end)

aio:http_post("/reload", function(fd, ...)
    local status = net.reload()
    if not status then
        fd:http_response("500 Internal Server Error", "text/plain", "Error: " .. WORKERID)
    else
        fd:http_response("200 OK", "text/plain", "OK: " .. WORKERID)
    end
end)