---@type aio
require("aio.aio")
require("aio.lib.nodes")

nodes:start {
    authorization = "SECRET",
    master = {"localhost", 8080, 0}
}

aio:http_get("/echo", function (fd, query, headers, body)
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
    -- reloading restarts workers, so require an explicit bearer token and do
    -- not accept a cross-site form post
    local expected = os.getenv("RELOAD_TOKEN")
    if not expected or headers.authorization ~= ("Bearer " .. expected) then
        fd:http_response("403 Forbidden", "text/plain", "forbidden")
        return
    end
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
    local url = args.url or "http://localhost:8080/echo"

    -- only absolute http(s) URLs; curl options are separated with `--` and the
    -- supported protocols/redirects/output size are constrained
    if type(url) ~= "string"
        or not url:match("^https?://[%w%-%._~:/%?#%[%]@!$&'()*+,;=%%]+$") then
        self:http_response("400 Bad Request", "text/plain", "only absolute http(s) URLs are allowed")
        return
    end

    aio:popen_read(ELFD, "curl", "--silent", "--max-time", "10",
        "--proto", "=http,https", "--proto-redir", "=http,https",
        "--max-filesize", "1048576", "--globoff", "--", url)(function (contents)
        self:http_response("200 OK", "text/plain", contents or "")
    end)
end)

aio:http_get("/search", function(self, query, headers, body)
    local args = aio:parse_query(query)
    local haystack = args.haystack or ""
    local needle = args.needle or ""
    self:http_response("200 OK", "application/json", {net.partscan(haystack, needle, 1)})
end)

aio:stream_http_get("/chat", function (self, query, headers, body)
    self:write("Enter your name: ")
    local name = coroutine.yield("\n", 256)
    if not name then
        return
    end
    -- clean the name (strip control characters and cap length)
    name = name:gsub("[%z\1-\31\127]", ""):sub(1, 64)
    if #name == 0 then
        self:write("Invalid name\n")
        return
    end
    nodes:register(self, {"chat", name})
    while true do
        -- add the participant to broadcast
        self:write("You: ")
        local data = coroutine.yield("\n", 4096)
        if not data then break end
        data = data:gsub("[\r%z\1-\8\11-\31\127]", ""):sub(1, 4096)
        if nodes.master then
            -- if master is set, send the message globally!
            nodes:broadcast({nodes.global, "chat"}, self:get_name(), "\n" .. name .. ": " .. data .. "\nYou: ")
        else
            -- broadcast it to everyone except sender
            nodes:broadcast({nodes:get_node(), "chat"}, self:get_name(), "\n" .. name .. ": " .. data .. "\nYou: ")
            -- also lets try broadcasting it to 8081 node
            nodes:broadcast({nodes:get_node("localhost", PORT == 8080 and 8081 or 8080, 0), "chat"}, self:get_name(), "\n" .. name .. ": " .. data .. "\nYou: ")
        end
    end
end)