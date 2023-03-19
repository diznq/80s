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