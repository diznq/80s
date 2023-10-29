require("aio.aio")

smtp = {
    counter = 0,
}

local phases = {
    START = "EHLO",
    EHLO = "MAIL",
    HELO = "MAIL",
    MAIL = "RCPT",
    RCPT = "DATA"
}

---comment
---@param params {host: string}
function smtp:init(params)
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
                local await_state = phases.START
                local from, to, message = "", "", ""
                while ok do
                    local line = coroutine.yield("\r\n")
                    if line == nil then return end
                    local msg_type = line:sub(1, 4)
                    if #msg_type == 4 and msg_type == await_state then
                        if await_state == "HELO" then
                            fd:write("250 HELO " .. line:sub(6) .. "\r\n")
                        elseif await_state == "EHLO" then
                            fd:write("250-".. params.host .. " is my domain name. Hello " .. line:sub(6) .. "!\r\n250-8BITMIME\r\n250 SIZE 1000000\r\n")
                        elseif await_state == "MAIL" then
                            if line:sub(6, 9) == "FROM" then
                                from = line:sub(11)
                                fd:write("250 OK\r\n")
                            end
                        elseif await_state == "RCPT" then
                            if line:sub(6, 7) == "TO" then
                                to = line:sub(9)
                                fd:write("250 OK\r\n")
                            end
                        elseif await_state == "DATA" then
                            fd:write("354 Send message content; end with <CR><LF>.<CR><LF>\r\n")
                            message = coroutine.yield("\r\n.\r\n")
                            if message ~= nil then
                                self.counter = self.counter + 1
                                local messageId = NODE .. "-" .. self.counter
                                print("Received message")
                                print("From", from)
                                print("To", to)
                                print("-----------")
                                print(message)
                                fd:write("250 OK: queued as " .. messageId .. "\r\n")
                            end
                        elseif await_state == "QUIT" then
                            fd:write("221 Bye\r\n", true)
                        end
                        await_state = phases[msg_type]
                    elseif msg_type == "QUIT" then
                        fd:write("221 Bye\r\n", true)
                    end
                end
            end)
        end
    })
end

function smtp:default_initialize()
    self:init({
        host = os.getenv("SMTP_HOST") or "smtp.localhost"
    })
end

if not ... then
    smtp:default_initialize()
end

return smtp