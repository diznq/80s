---@type aio
require("aio.aio")

SSL = SSL or crypto.ssl_new_server("private/pubkey.crt", "private/privkey.pem")

aio:add_protocol_handler("tls", {
    matches = function()
        return true
    end,
    handle = function(fd)
        fd.on_data = function(self, elfd, childfd, data, length)
            if not self.bio then
                self.bio = crypto.ssl_new_bio(SSL)
            end
            
            crypto.ssl_bio_write(self.bio, data)
            if not crypto.ssl_init_finished(self.bio) then
                local ok = crypto.ssl_accept(self.bio)
                if ok then
                    while true do
                        local rd = crypto.ssl_bio_read(self.bio)
                        if not rd or #rd == 0 then break end
                        self:write(rd)
                    end
                end
                if crypto.ssl_init_finished(self.bio) then
                    print("handshake finished")
                end
            end
        end
        fd.on_close = function(self)
            if self.bio then
                crypto.ssl_release_bio(self.bio)
                self.bio = nil
            end
        end
    end,
})

aio:http_get("/headers", function (fd, query, headers, body)
    fd:http_response("200 OK", "application/json; charset=utf-8", headers)
end)