---@type aio
local aio = loadfile("server/aio.lua")()

aio:start()

aio.http:get("/haha", function (script, query, headers, body)
    local params = aio.http:parse_query(query)
    return 
        "200 OK", 
        "text/plain", 
        ("Hi, %s!"):format(params.name or "Nomad")
end)