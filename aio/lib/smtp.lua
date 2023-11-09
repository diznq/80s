require("aio.aio")

---@alias maildetail {name: string?, email: string}
---@alias mailparam {from: maildetail, to: maildetail[], sender: table, id: string, body: string}
---@alias mailreceived fun(mail: mailparam): aiopromise?

local smtp = {
    counter = 0,
    host = "localhost",
    --- @type lightuserdata|nil
    tls = nil,
    --- @type {[string]: mailreceived}
    on_mail_received_callbacks = {}
}

--- Initialize SMTP server
---@param params {host: string, tls: boolean|nil, privkey: string|nil, pubkey: string|nil, server: boolean|nil}
function smtp:init(params)
    self.host = params.host
    if params.tls and params.privkey and params.pubkey then
        local SSL, err = aio:get_ssl_context({server = true, pubkey = params.pubkey, privkey = params.privkey})
        if not SSL then
            print("[smtp] Failed to initialize TLS: " .. tostring(err))
        else
            self.tls = SSL
        end
    end
    aio:add_protocol_handler("smtp", {
        matches = function (data)
            return true
        end,
        on_accept = function (fd, parentfd)
            fd:write("220 " .. params.host .. " ESMTP 80s\r\n")
        end,
        handle = function (fd)
            self:handle_as_smtp(fd)
        end
    })
end

function smtp:handle_as_smtp(fd)
    aio:buffered_cor(fd, function (_)
        local ok = true
        local hello = false
        local from, message, to = nil, nil, {}
        local tls_capability = ""
        if self.tls then
            tls_capability = "250-STARTTLS\r\n"
        end
        while ok do
            --- @type string|nil
            local line = coroutine.yield("\r\n")
            if line == nil then return end
            --- @type string|nil
            local msg_type = line:match("^([A-Za-z]+)")
            local handled = false
            if msg_type ~= nil then msg_type = msg_type:upper() end
            if msg_type == nil then
                fd:write("502 Invalid command\r\n")
                handled = true
            elseif msg_type == "HELO" then
                hello = true
                fd:write("250 HELO " .. line:sub(6) .. "\r\n")
                handled = true
            elseif msg_type == "EHLO" then
                hello = true
                fd:write("250-".. self.host .. " is my domain name. Hello " .. line:sub(6) .. "!\r\n250-8BITMIME\r\n" .. tls_capability .. "250 SIZE 1000000\r\n")
                handled = true
            elseif msg_type == "STARTTLS" then
                if self.tls then
                    if hello then
                        fd:write("220 Go ahead\r\n")
                        aio:wrap_tls(fd, self.tls)
                        handled = true
                        hello = false
                    else
                        fd:write("503 HELO or EHLO was not sent previously!\r\n")
                        handled = true
                    end
                end
            elseif msg_type == "MAIL" then
                if line:sub(6, 10) == "FROM:" then
                    local raw_from = line:sub(11)
                    local parsed_from = self:extract_email(raw_from)
                    if from ~= nil then
                        fd:write("503 MAIL FROM was already sent previously\r\n")
                        handled = true
                    elseif parsed_from then
                        from = parsed_from
                        fd:write("250 OK\r\n")
                        handled = true
                    else
                        fd:write("501 Invalid address\r\n")
                        handled = true
                    end
                else
                    fd:write("500 Invalid MAIL command\r\n")
                    handled = true
                end
            elseif msg_type == "RCPT" then
                if line:sub(6, 8) == "TO:" then
                    local raw_to = line:sub(9)
                    local parsed_to = self:extract_email(raw_to)
                    if raw_to then
                        if to == nil then to = {} end
                        if #to >= 100 then
                            fd:write("501 Limit for number of recipients is 100\r\n")
                            handled = true
                        else
                            to[#to + 1] = parsed_to
                            fd:write("250 OK\r\n")
                            handled = true
                        end
                    else
                        fd:write("501 Invalid address\r\n")
                        handled = true
                    end
                else
                    fd:write("500 Invalid RCPT command\r\n")
                end
            elseif msg_type == "DATA" then
                if from and to and #to > 0 and hello then
                    fd:write("354 Send message content; end with <CR><LF>.<CR><LF>\r\n")
                    message = coroutine.yield("\r\n.\r\n")
                    if message ~= nil then
                        self.counter = self.counter + 1
                        local messageId = NODE_ID .. "-" .. self.counter
                        local handle_ok = true
                        for key, callback in pairs(self.on_mail_received_callbacks) do
                            local cb_ok, result = pcall(callback, {
                                from = from,
                                to = to,
                                body = message,
                                sender = {aio:get_ip(fd)},
                                id = messageId
                            })
                            if cb_ok and type(result) == "function" then
                                result = aio:await(result)
                                if type(result) == "table" and result.error then
                                    result = result.error
                                    cb_ok = false
                                end
                            end
                            if not cb_ok then
                                handle_ok = false
                                print("[smtp] mail handler " .. key .. " failed with " .. result)
                            end
                         end
                        from = nil
                        message = nil
                        to = {}
                        if handle_ok then
                            fd:write("250 OK: queued as " .. messageId .. "\r\n")
                        else
                            fd:write("451 Server failed to handle the message, try again later\r\n")
                        end
                        handled = true
                    else
                        fd:write("500 Message was missing\r\n")
                        handled = true
                    end
                else
                    local errors = {"503-There were following errors:"}
                    if not hello then errors[#errors+1] = "503- No hello has been sent" end
                    if not from then errors[#errors+1] = "503- MAIL FROM has been never sent" end
                    if not to then errors[#errors+1] = "503- RCPT TO has been never sent" end
                    if to and #to == 0 then errors[#errors+1] = "503- There were zero recipients" end
                    errors[#errors+1] = "503 Please, fill the missing information"
                    fd:write(table.concat(errors, "\r\n") .. "\r\n")
                    handled = true
                end
            elseif msg_type == "QUIT" then
                fd:write("221 Bye\r\n", true)
                handled = true
            end
            if not handled then
                fd:write("502 Command not implemented\r\n")
            end
        end
    end)
end

---Extract information about sender
---@param raw string
---@return {name: string?, email: string}? details 
function smtp:extract_email(raw)
    local simple = raw:match("^<([a-zA-Z0-9.]+@[a-z0-9.]+)>.*$")
    if simple then
        return {email = simple}
    end
end

--- Register on mail received handler
---@param name string handler name
---@param handler mailreceived handler
function smtp:register_handler(name, handler)
    self.on_mail_received_callbacks[name] = handler
end

function smtp:default_initialize()
    local use_tls = (os.getenv("TLS") or "false") == "true"
    local tls_pubkey = os.getenv("TLS_PUBKEY") or nil
    local tls_privkey = os.getenv("TLS_PRIVKEY") or nil

    self:init({
        host = os.getenv("SMTP_HOST") or "smtp.localhost",
        tls = use_tls,
        privkey = tls_privkey,
        pubkey = tls_pubkey,
        server = true
    })

    self:register_handler("main", function (mail)
        print("Received mail: ", codec.json_encode(mail))
    end)
end

if not ... then
    smtp:default_initialize()
end

return smtp