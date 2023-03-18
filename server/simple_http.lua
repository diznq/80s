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

aio:http_post("/reload", function(fd, ...)
    local status = net.reload()
    if not status then
        fd:http_response("500 Internal Server Error", "text/plain", "Error: " .. WORKERID)
    else
        fd:http_response("200 OK", "text/plain", "OK: " .. WORKERID)
    end
end)

aio:http_get("/cipher", function (self, query, headers, body)
    local args = aio:parse_query(query)
    local key = crypto.sha256(args.key or "")
    local text = args.text or "Hello world!"
    local crypted, err = crypto.cipher(text, key, true)
    if not crypted then
        self:http_response("500 Internal server error", "text/plain", err)
    else
        local decrypted, err = crypto.cipher(crypto.from64(crypto.to64(crypted)), key, false)
        if not decrypted then
            self:http_response("500 Internal server error", "text/plain", "Encrypted: " .. crypto.to64(crypted) .. "\nDecryption failed: " .. err)
        else
            self:http_response("200 OK", "text/plain", "Encrypted: " .. crypto.to64(crypted) .. "\nDecrypted: " .. decrypted)
        end
    end
end)