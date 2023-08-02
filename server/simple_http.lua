---@type aio
require("aio.aio")

aio:http_get("/haha", function (fd, query, headers, body)
    local params = aio:parse_query(query)
    fd:http_response(
        "200 OK", 
        "text/plain; charset=utf-8", 
        ("Hi, %s!"):format(params.name or "Nomad")
    )
end)

aio:http_get("/headers", function (fd, query, headers, body)
    fd:http_response("200 OK", "application/json; charset=utf-8", headers)
end)

aio:http_get("/ip", function (fd, query, headers, body)
    local ip, port = net.sockname(fd.fd)
    fd:http_response("200 OK", "text/plain", string.format("%s:%d", ip, port))
end)

aio:http_post("/reload", function(fd, query, headers, body)
    local args = aio:parse_query(query)
    local status = args.c and aio:reload(true) or aio:reload()
    if not status then
        fd:http_response("500 Internal Server Error", "text/plain", "Error: " .. WORKERID)
    else
        fd:http_response("200 OK", "text/plain", "OK: " .. WORKERID)
    end
end)

aio:http_get("/info", function (fd, query, headers, body)
    fd:http_response("200 OK", "text/plain", net.info())
end)

aio:http_get("/cipher", function (self, query, headers, body)
    local args = aio:parse_query(query)
    local key = args.key or "1234567890123456"
    local text = args.text or "Hello world!"
    local crypted, err = crypto.cipher(text, key, true, true)
    if not crypted then
        self:http_response("500 Internal server error", "text/plain", err)
    else
        local decrypted, err = crypto.cipher(crypted, key, true, false)
        if not decrypted then
            self:http_response("500 Internal server error", "text/plain", string.format("Encrypted: %s\nDecrypted: %s", crypto.to64(crypted), err))
        else
            self:http_response("200 OK", "text/plain", string.format("Encrypted: %s\nDecrypted: %s", crypto.to64(crypted), decrypted))
        end
    end
end)

aio:http_get("/worker", function (self, query, headers, body)
    self:http_response("200 OK", "text/plain", tostring(WORKERID))
end)

aio:http_get("/visit", function (self, query, headers, body)
    local args = aio:parse_query(query)
    local url = args.url or "http://localhost:8080/haha"

    aio:popen_read(ELFD, "curl", "--silent", url)(function (contents)
        self:http_response("200 OK", "text/plain", contents or "")
    end)
end)

aio:http_get("/search", function(self, query, headers, body)
    local args = aio:parse_query(query)
    local haystack = args.haystack or ""
    local needle = args.needle or ""
    self:http_response("200 OK", "application/json", {net.partscan(haystack, needle, 0)})
end)

aio:stream_http_get("/chat", function (self, query, headers, body)
    CHAT_PARTICIPANTS = CHAT_PARTICIPANTS or {}
    self:write("Enter your name: ")
    local name = coroutine.yield("\n")
    if not name then
        return
    end
    -- clean the name
    name = name:gsub("[\r\n]", "")
    while true do
        -- add the participant to broadcast
        CHAT_PARTICIPANTS[self] = true
        self:write("You: ")
        local data = coroutine.yield("\n")
        -- remove the participant from broadcast
        if not data then
            CHAT_PARTICIPANTS[self] = nil
            break
        end
        -- broadcast it to everyone except sender
        for participant, _ in pairs(CHAT_PARTICIPANTS) do
            if participant ~= self then
                participant:write("\n" .. name .. ": " .. data .. "\nYou: ")
            end
        end
    end
end)