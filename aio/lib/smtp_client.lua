require("aio.aio")

local smtp_client = {
    counter = 0,
    host = "localhost",
    logging = false
}

--- @alias mailresponse {status: string, response: string, error: string|nil}

--- Send an e-mail
---@param params {from: string, to: string|string[], ssl: boolean|nil, headers: table|nil, body: string, subject: string|nil, mail_server: string|nil}
---@return aiopromise<mailresponse> response
function smtp_client:send_mail(params)
    local from, to, headers, body, ssl = params.from, params.to, params.headers, params.body, params.ssl
    headers = headers or {}
    local resolve, resolver = aio:prepare_promise()
    --- @type string[]
    local recipients = {}
    if type(to) == "string" then
        recipients = {to}
    else
        recipients = to
    end
    if #recipients == 0 then
        resolve(make_error("zero recipients"))
        return resolver
    end
    local first = recipients[1]
    local user, host = self:parse_address(first)
    if not user or not host then
        resolve(make_error("invalid first recipient"))
        return resolver
    end
    if params.mail_server ~= nil then
        host = tostring(params.mail_server)
    end
    if ssl and self.tls == nil then
        self.tls = crypto.ssl_new_client()
    end
    aio:connect2(ELFD, host, 25)(function (fd)
        if iserror(fd) then
            resolve(fd)
            return
        end
        aio:buffered_cor(fd, function (_)
            local first_line = coroutine.yield("\r\n")
            -- Hello
            local status, response = self:send_command(fd, "EHLO " .. self.host .. "\r\n")
            if not status then
                return resolve(make_error("failed to read response from server"))
            elseif status ~= "250" then
                fd:close()
                return resolve(make_error("EHLO status was " .. status .. " instead of expected 250"))
            end

            status, response = self:send_command(fd, "MAIL FROM:<" .. from .. ">")
            if not status then
                return resolve(make_error("failed to read response from server"))
            elseif status ~= "250" then
                fd:close()
                return resolve(make_error("MAIL FROM status was " .. status .. " instead of expected 250"))
            end
            
            for _, recipient in ipairs(recipients) do
                status, response = self:send_command(fd, "RCPT TO:<" .. from .. ">")
                if not status then
                    return resolve(make_error("failed to read response from server"))
                elseif status ~= "250" then
                    fd:close()
                    return resolve(make_error("RCPT TO status was " .. status .. " instead of expected 250 for recipient" .. recipient))
                end
            end

            status, response = self:send_command(fd, "DATA")
            if not status then
                return resolve(make_error("failed to read response from server"))
            elseif status ~= "354" then
                fd:close()
                return resolve(make_error("DATA status was " .. status .. " instead of expected 354"))
            end

            headers["Message-ID"] = self:generate_message_id()
            headers["From"] = from
            headers["To"] = table.concat(recipients, ", ")
            headers["Subject"] = params.subject or "No subject"

            local email_message = ""
            local headers_list = {}
            for key, value in pairs(headers) do
                headers_list[#headers_list+1] = key .. ": " .. tostring(value)
            end
            email_message = string.format("%s\r\n\r\n%s\r\n.\r\n", table.concat(headers_list, "\r\n"), body)
            fd:write(email_message)
            status, response = self:read_response()
            if not status then
                return resolve(make_error("failed to read response from server"))
            elseif status ~= "250" then
                fd:close()
                return resolve(make_error("DATA submission status was " .. status .. " instead of expected 250"))
            end

            status, response = self:send_command(fd, "QUIT")
            fd:close()
            resolve({response = response, stauts=status})
        end)
    end)
    return resolver
end

--- Send command to SMTP server
---@param fd aiosocket
---@param command string
---@return string|nil status
---@return string body
function smtp_client:send_command(fd, command)
    if self.logging then
        print("-> " .. command)
    end
    fd:write(command .. "\r\n")
    local status, response = self:read_response()
    if not status then
        fd:close()
        return nil, ""
    end
    return status, response
end

--- Read response in current coroutine
---@return string|nil status
---@return string body
function smtp_client:read_response()
    local messages = {}
    local status = ""
    while true do
        local msg = coroutine.yield("\r\n")
        if self.logging then
            print("<- " .. tostring(msg))
        end
        if msg == nil then return nil, "" end
        local code, continue, body = msg:match("^(%d+)([%- ])(.*)$")
        if code ~= nil and continue ~= nil and body ~= nil then
            messages[#messages+1] = body
            status = code
            if continue ~= "-" then break end
        else
            return nil, ""
        end
    end
    return status, table.concat(messages, "\n")
end

---Parse e-mail address
---@param address string address
---@return string|nil user
---@return string host
function smtp_client:parse_address(address)
    return address:match("^(.-)@(.+)$")
end

function smtp_client:generate_message_id()
    self.counter = self.counter + 1
    return string.format("<%s.%d.%f@%s>", NODE_ID:gsub("%/", "-"), self.counter, net.clock(), self.host)
end

--- Initialize SMTP client
---@param params {host: string|nil, logging: boolean|nil}
function smtp_client:init(params)
    self.host = params.host or "localhost"
    self.logging = params.logging or false
end

function smtp_client:default_initialize()
    self:init({
        host = os.getenv("HOST") or "localhost",
        logging = (os.getenv("LOGGING") or "false") == "true"
    })
end

return smtp_client