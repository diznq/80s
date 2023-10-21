---@type aio
require("aio.aio")

SSL, _ = crypto.ssl_new_server("private/pubkey.crt", "private/privkey.pem")

if SSL then
    aio:add_protocol_handler("tls", {
        matches = function()
            return true
        end,
        handle = function(fd)
            aio:wrap_tls(aio:handle_as_http(fd), SSL)
        end,
    })
end

aio:http_get("/headers", function (fd, query, headers, body)
    fd:http_response("200 OK", "application/json; charset=utf-8", headers)
end)
