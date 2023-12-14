require("aio.aio")


---@alias maildetail {name: string?, email: string}
---@alias mailparam {from: maildetail, to: maildetail[], inbound: boolean|nil, sender: string, error: string|nil, id: string, subject: string|nil, time: string|nil, subfolder: string|nil, received: osdate, body: string, unread: boolean|nil}
---@alias okhandle fun(): any
---@alias errhandle fun(reason: string|nil): any
---@alias mailreceived fun(mail: mailparam, handle: {ok: okhandle, error: errhandle}): aiopromise?

local smtp = {
    counter = 0,
    host = "localhost",
    logging = false,
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
            self:log("[smtp] Failed to initialize TLS: " .. tostring(err))
        else
            self.tls = SSL
        end
    end
    aio:add_protocol_handler("smtp", {
        matches = function (data)
            return true
        end,
        on_accept = function (fd, parentfd)
            self:write(fd, "220 " .. params.host .. " ESMTP 80s\r\n")
        end,
        handle = function (fd)
            self:handle_as_smtp(fd)
        end
    })
end

function smtp:log(...)
    if not self.logging then return end
    print(...)
end

--- Set loggng state
---@param state boolean
function smtp:set_logging(state)
    self.logging = state
end

function smtp:read(data)
    local data = coroutine.yield(data)
    if data ~= nil then
        self:log("[smtp] <- " .. data)
    end
    return data
end

function smtp:write(fd, data, close)
    if data ~= nil then
        self:log("[smtp] -> " .. data)
    end
    return fd:write(data, close)
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
            local line = self:read("\r\n")
            if line == nil then return end
            --- @type string|nil
            local msg_type = line:match("^([A-Za-z]+)")
            local handled = false
            if msg_type ~= nil then msg_type = msg_type:upper() end
            if msg_type == nil then
                self:write(fd, "502 Invalid command\r\n")
                handled = true
            elseif msg_type == "HELO" then
                hello = true
                self:write(fd, "250 HELO " .. line:sub(6) .. "\r\n")
                handled = true
            elseif msg_type == "EHLO" then
                hello = true
                self:write(fd, "250-".. self.host .. " is my domain name. Hello " .. line:sub(6) .. "!\r\n250-8BITMIME\r\n" .. tls_capability .. "250 SIZE 1000000\r\n")
                handled = true
            elseif msg_type == "STARTTLS" then
                if self.tls then
                    if hello then
                        self:write(fd, "220 Go ahead\r\n")
                        aio:wrap_tls(fd, self.tls)
                        handled = true
                        hello = false
                    else
                        self:write(fd, "503 HELO or EHLO was not sent previously!\r\n")
                        handled = true
                    end
                end
            elseif msg_type == "MAIL" then
                if line:sub(6, 10) == "FROM:" then
                    local raw_from = line:sub(11)
                    local parsed_from = self:extract_email(raw_from)
                    if from ~= nil then
                        self:write(fd, "503 MAIL FROM was already sent previously\r\n")
                        handled = true
                    elseif parsed_from then
                        from = parsed_from
                        self:write(fd, "250 OK\r\n")
                        handled = true
                    else
                        self:write(fd, "501 Invalid address\r\n")
                        handled = true
                    end
                else
                    self:write(fd, "500 Invalid MAIL command\r\n")
                    handled = true
                end
            elseif msg_type == "RCPT" then
                if line:sub(6, 8) == "TO:" then
                    local raw_to = line:sub(9)
                    local parsed_to = self:extract_email(raw_to)
                    if raw_to then
                        if to == nil then to = {} end
                        if #to >= 100 then
                            self:write(fd, "501 Limit for number of recipients is 100\r\n")
                            handled = true
                        else
                            to[#to + 1] = parsed_to
                            self:write(fd, "250 OK\r\n")
                            handled = true
                        end
                    else
                        self:write(fd, "501 Invalid address\r\n")
                        handled = true
                    end
                else
                    self:write(fd, "500 Invalid RCPT command\r\n")
                end
            elseif msg_type == "DATA" then
                if from and to and #to > 0 and hello then
                    self:write(fd, "354 Send message content; end with <CR><LF>.<CR><LF>\r\n")
                    message = self:read("\r\n.\r\n")
                    if message ~= nil then
                        local email_subject = message:match("Subject: (.-)\r\n") or "No subject"
                        local messageId = self:generate_message_id()
                        local handle_ok = true
                        local write_response = true
                        for key, callback in pairs(self.on_mail_received_callbacks) do
                            local sender_ip, sender_port = aio:get_ip(fd)
                            local cb_ok, result = pcall(callback, {
                                from = from,
                                to = to,
                                body = message,
                                subject = email_subject,
                                sender = string.format("%s,%d", sender_ip, sender_port),
                                id = messageId,
                                ---@diagnostic disable-next-line: assign-type-mismatch
                                received = os.date("*t"),
                                inbound = true,
                                unread = true
                            }, {
                                ok = function ()
                                    self:write(fd, "250 OK: queued as " .. messageId .. "\r\n")
                                end,
                                error = function (message)
                                    if message ~= nil then
                                        self:write(fd, "451 Server failed to handle the message: " .. message .. ", try again later\r\n")
                                    else
                                        self:write(fd, "451 Server failed to handle the message, try again later\r\n")
                                    end
                                end
                            })
                            if not cb_ok then
                                handle_ok = false
                                self:log("[smtp] mail handler " .. key .. " failed with " .. result)
                            else
                                write_response = false
                            end
                        end
                        from = nil
                        message = nil
                        to = {}
                        if write_response then
                            if handle_ok then
                                self:write(fd, "250 OK: queued as " .. messageId .. "\r\n")
                            else
                                self:write(fd, "451 Server failed to handle the message, try again later\r\n")
                            end
                        end
                        handled = true
                    else
                        self:write(fd, "500 Message was missing\r\n")
                        handled = true
                    end
                else
                    local errors = {"503-There were following errors:"}
                    if not hello then errors[#errors+1] = "503- No hello has been sent" end
                    if not from then errors[#errors+1] = "503- MAIL FROM has been never sent" end
                    if not to then errors[#errors+1] = "503- RCPT TO has been never sent" end
                    if to and #to == 0 then errors[#errors+1] = "503- There were zero recipients" end
                    errors[#errors+1] = "503 Please, fill the missing information"
                    self:write(fd, table.concat(errors, "\r\n") .. "\r\n")
                    handled = true
                end
            elseif msg_type == "QUIT" then
                self:write(fd, "221 Bye\r\n", true)
                handled = true
            end
            if not handled then
                self:write(fd, "502 Command not implemented\r\n")
            end
        end
    end)
end

---Extract information about sender
---@param raw string
---@return {name: string?, email: string}? details 
function smtp:extract_email(raw)
    local simple = raw:match("^<([a-zA-Z0-9.%-_=+]+@[a-z0-9.%-_]+)>.*$")
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

function smtp:generate_message_id()
    self.counter = self.counter + 1
    local date = os.date("%Y%m%d%H%I%S")
    return string.format("<%s.%s.%d@%s>", NODE_ID:gsub("%/", "-"), date, self.counter, self.host)
end

function smtp:default_initialize()
    local use_tls = (os.getenv("TLS") or "false") == "true"
    local tls_pubkey = os.getenv("TLS_PUBKEY") or nil
    local tls_privkey = os.getenv("TLS_PRIVKEY") or nil

    self:init({
        host = os.getenv("SMTP_HOST") or "smtp.localhost",
        loggng = (os.getenv("SMTP_LOGGING") or "true") == "true",
        tls = use_tls,
        privkey = tls_privkey,
        pubkey = tls_pubkey,
        server = true
    })

    self:register_handler("main", function (mail)
        self:log("Received mail: ", codec.json_encode(mail))
    end)
end

if not ... then
    smtp:default_initialize()
end

return smtp