require("aio.aio")

---@alias maildetail {name: string?, email: string}
---@alias mailparam {from: maildetail, to: maildetail[], sender: table, id: string, body: string}
---@alias mailreceived fun(mail: mailparam)

smtp = {
    counter = 0,
    --- @type {[string]: mailreceived}
    on_mail_received_callbacks = {}
}

--- Initialize SMTP server
---@param params {host: string}
function smtp:init(params)
    local smtp_self = self
    aio:add_protocol_handler("smtp", {
        matches = function (data)
            return true
        end,
        on_accept = function (fd, parentfd)
            fd:write("220 " .. params.host .. " ESMTP 80s\r\n")
        end,
        handle = function (fd)
            aio:buffered_cor(fd, function (resolve)
                local ok = true
                local hello = false
                local from, message, to = nil, nil, {}
                while ok do
                    local line = coroutine.yield("\r\n")
                    if line == nil then return end
                    local msg_type = line:sub(1, 4)
                    local handled = false
                    if msg_type == "HELO" then
                        hello = true
                        fd:write("250 HELO " .. line:sub(6) .. "\r\n")
                        handled = true
                    elseif msg_type == "EHLO" then
                        hello = true
                        fd:write("250-".. params.host .. " is my domain name. Hello " .. line:sub(6) .. "!\r\n250-8BITMIME\r\n250-ENHANCEDSTATUSCODES\r\n250 SIZE 1000000\r\n")
                        handled = true
                    elseif msg_type == "MAIL" then
                        if line:sub(6, 10) == "FROM:" then
                            local raw_from = line:sub(11)
                            local parsed_from = self:extract_email(raw_from)
                            if from ~= nil then
                                fd:write("555 MAIL FROM was already sent previously\r\n")
                                handled = true
                            elseif parsed_from then
                                from = parsed_from
                                fd:write("250 OK\r\n")
                                handled = true
                            else
                                fd:write("555 Invalid address\r\n")
                                handled = true
                            end
                        else
                            fd:write("555 Invalid MAIL command\r\n")
                            handled = true
                        end
                    elseif msg_type == "RCPT" then
                        if line:sub(6, 8) == "TO:" then
                            local raw_to = line:sub(9)
                            local parsed_to = self:extract_email(raw_to)
                            if raw_to then
                                if to == nil then to = {} end
                                if #to >= 100 then
                                    fd:write("555 Limit for number of recipients is 100\r\n")
                                    handled = true
                                else
                                    to[#to + 1] = parsed_to
                                    fd:write("250 OK\r\n")
                                    handled = true
                                end
                            else
                                fd:write("555 Invalid address\r\n")
                                handled = true
                            end
                        else
                            fd:write("555 Invalid RCPT command\r\n")
                        end
                    elseif msg_type == "DATA" then
                        if from and to and #to > 0 and hello then
                            fd:write("354 Send message content; end with <CR><LF>.<CR><LF>\r\n")
                            message = coroutine.yield("\r\n.\r\n")
                            if message ~= nil then
                                self.counter = self.counter + 1
                                local messageId = NODE_ID .. "-" .. self.counter
                                for key, callback in pairs(self.on_mail_received_callbacks) do
                                    local cb_ok, result = pcall(callback, {
                                        from = from,
                                        to = to,
                                        body = message,
                                        sender = {aio:get_ip(fd)},
                                        id = messageId
                                    })
                                    if not cb_ok then
                                        print("[smtp] mail handler " .. key .. " failed with " .. result)
                                    end
                                end
                                from = nil
                                message = nil
                                to = {}
                                fd:write("250 OK: queued as " .. messageId .. "\r\n")
                                handled = true
                            else
                                fd:write("555 Message was missing\r\n")
                                handled = true
                            end
                        else
                            local errors = {"555-There were following errors:"}
                            if not hello then errors[#errors+1] = "555- No hello has been sent" end
                            if not from then errors[#errors+1] = "555- MAIL FROM has been never sent" end
                            if not to then errors[#errors+1] = "555- RCPT TO has been never sent" end
                            if to and #to == 0 then errors[#errors+1] = "555- There were zero recipients" end
                            errors[#errors+1] = "555 Please, fill the missing information"
                            fd:write(table.concat(errors, "\r\n") .. "\r\n")
                            handled = true
                        end
                    elseif msg_type == "QUIT" then
                        fd:write("221 Bye\r\n", true)
                        handled = true
                    end
                    if not handled then
                        fd:write("555 Unhandled message type\r\n")
                    end
                end
            end)
        end
    })
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
    self:init({
        host = os.getenv("SMTP_HOST") or "smtp.localhost"
    })

    self:register_handler("main", function (mail)
        print("Received mail: ", codec.json_encode(mail))
    end)
end

if not ... then
    smtp:default_initialize()
end

return smtp