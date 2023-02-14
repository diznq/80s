---@type aio
local aio = loadfile("server/aio.lua")()

aio:start()

aiohttp:get("/haha", function (script, query, headers, body)
    return "200 OK", "text/plain", "Hi!"
end)