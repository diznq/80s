require("aio.aio")

_smtp_client = _smtp_client or {
    counter = 0,
    host = "localhost",
    logging = false,
    --- @type lightuserdata|nil
    tls = nil,
    ssl_enforced = false
}

local smtp_client = _smtp_client

--- @alias mailresponse {status: string, response: string, error: string|nil}

--- Send an e-mail
---@param params {from: string, to: string|string[], ssl: boolean|nil, test_ssl: boolean|nil, headers: table|nil, body: string, subject: string|nil, mail_server: string|nil}
---@return aiopromise<mailresponse> response
function smtp_client:send_mail(params)
    local from, to, headers, body, ssl, subject = params.from, params.to, params.headers, params.body, params.ssl, params.subject
    body = body or ""
    subject = subject or ""
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
    if self.ssl_enforced then ssl = true end
    if ssl and self.tls == nil then
        local ssl_result, err = aio:get_ssl_context()
        if not ssl_result then
            resolve(make_error("failed to initialize SSL context: " .. err))
            return resolver
        end
        self.tls = ssl_result
    end
    if self.logging then print("= Submitting new mail") end
    aio:connect2({host = host, port = 25, dns_type = "MX", cname_ssl = true})(function (fd)
        if iserror(fd) then
            if self.logging then print("= Failed to connect to MX " .. host .. ":25") end
            resolve(make_error("failed to connect to SMTP server: " .. host .. ":25, reason: " .. fd.error))
            return
        end
        if self.logging then print("= Successfuly connected to MX " .. host .. ":25") end
        aio:buffered_cor(fd, function (_)
            if self.logging then print("= Starting the mail flow") end
            -- first line can be ignored
            local first_line = coroutine.yield("\r\n")
            -- Hello
            local status, response = self:send_command(fd, "EHLO " .. self.host)
            if not status then
                return resolve(make_error("EHLO failed to read response from server"))
            elseif status ~= "250" then
                fd:close()
                return resolve(make_error("EHLO status was " .. status .. " instead of expected 250"))
            end

            if self.tls and self.ssl_enforced and not response:find("STARTTLS") then
                fd:close()
                return resolve(make_error("EHLO server doesn't support STARTTLS, but client enforces TLS"))
            elseif self.tls and response:find("STARTTLS") ~= nil then
                status, response = self:send_command(fd, "STARTTLS")
                if not status then
                    return resolve(make_error("STARTTLS failed to read response from server"))
                elseif status ~= "220" then
                    fd:close()
                    return resolve(make_error("STARTTLS status was " .. status .. " instead of expected 220"))
                end
                aio:wrap_tls(fd, self.tls, fd.host, true)(function (result)
                    if not result then
                        fd:close()
                        resolve(make_error("STARTTLS failed to wrap TLS"))
                        return
                    elseif iserror(result) then
                        fd:close()
                        resolve(make_error("STARTTLS failed on " .. result.error))
                        return
                    end
                    if params.test_ssl then
                        fd:close()
                        resolve({ok = true})
                        return
                    end
                    aio:buffered_cor(fd, function (_)
                        status, response = self:send_command(fd, "EHLO " .. self.host)
                        if not status then
                            return resolve(make_error("STARTTLS EHLO failed to read response from server"))
                        elseif status ~= "250" then
                            fd:close()
                            return resolve(make_error("STARTTLS EHLO status was " .. status .. " instead of expected 250"))
                        end
                        self:mail_flow(fd, from, recipients, headers, subject, body, resolve)
                    end)
                end)
            else
                self:mail_flow(fd, from, recipients, headers, subject, body, resolve)
            end
        end)
    end)
    return resolver
end

--- Encode e-mail message
---@param from string sender
---@param recipients string[] list of recipients
---@param headers table headers table
---@param subject string|nil email subject
---@param body string email body
---@return string
function smtp_client:encode_message(from, recipients, headers, subject, body)
    if headers["Message-ID"] == nil then
        headers["Message-ID"] = self:generate_message_id()
    end
    headers["From"] = from
    headers["To"] = table.concat(recipients, ", ")
    headers["Subject"] = subject or "No subject"

    local canon = {}
    local header_keys = {}
    local header_values = {}

    local headers_list = {}
    for key, value in pairs(headers) do
        header_keys[#header_keys+1] = key
        headers_list[#headers_list+1] = key .. ": " .. tostring(value)
    end

    local headers_str = table.concat(headers_list, "\r\n")

    body = body:gsub("^([ \r\n\t]+)", ""):gsub("([ \r\n\t]+)$", "")

    local result = string.format("%s\r\n\r\n%s", headers_str, body)

    if self.logging then print("= E-mail payload generated") end
    if self.dkim_domain and self.dkim_privkey and self.dkim_selector then
        if self.logging then print("= Generating DKIM signature") end
        local canonized = headers_str
        local issued = os.time()
        local valid_to = os.time() + 3600
        local bh = crypto.to64(crypto.sha256(body .. "\r\n"))
        local dkim_val = string.format(
            "v=1; a=rsa-sha256; c=simple/simple; d=%s; s=%s; t=%d; x=%d; h=%s; bh=%s; b=",
            self.dkim_domain, self.dkim_selector, issued, valid_to, table.concat(header_keys, ":"), bh
        )
        canonized = canonized .. "\r\nDKIM-Signature: " .. dkim_val
        local ok, err = crypto.rsa_sha256(self.dkim_privkey, canonized)
        if ok then
            result = "DKIM-Signature: " .. dkim_val .. crypto.to64(ok) .. "\r\n" .. result
            if self.logging then print("= DKIM generated successfuly") end
        else
            if self.logging then print("= Failed to generate DKIM: ", err) end
        end
    end

    return result
end

--- Perform mail SMTP send mail flow
---@param fd aiosocket client socket
---@param from string sender address
---@param recipients string[] recipient addresses
---@param headers table headers table
---@param subject string subject
---@param body string e-mail body including headers
---@param resolve fun(result: any)|thread continuation
---@return any
function smtp_client:mail_flow(fd, from , recipients, headers, subject, body, resolve)
    local status, response = self:send_command(fd, "MAIL FROM:<" .. from .. ">")
    if not status then
        return resolve(make_error("MAIL FROM failed to read response from server"))
    elseif status ~= "250" then
        fd:close()
        return resolve(make_error("MAIL FROM status was " .. status .. " instead of expected 250"))
    end
    
    for _, recipient in ipairs(recipients) do
        status, response = self:send_command(fd, "RCPT TO:<" .. recipient .. ">")
        if not status then
            return resolve(make_error("RECPT TO failed to read response from server"))
        elseif status ~= "250" then
            fd:close()
            return resolve(make_error("RCPT TO status was " .. status .. " instead of expected 250 for recipient " .. recipient))
        end
    end

    status, response = self:send_command(fd, "DATA")
    if not status then
        return resolve(make_error("DATA failed to read response from server"))
    elseif status ~= "354" then
        fd:close()
        return resolve(make_error("DATA status was " .. status .. " instead of expected 354"))
    end

    local email_message = self:encode_message(from, recipients, headers, subject, body)

    if self.logging then
        print("----------------------->")
        print(email_message)
        print("-----------------------/")
    end
    fd:write(email_message .. "\r\n.\r\n")
    status, response = self:read_response()
    if not status then
        return resolve(make_error("DATA submission failed to read response from server"))
    elseif status ~= "250" then
        fd:close()
        return resolve(make_error("DATA submission status was " .. status .. " instead of expected 250"))
    end

    status, response = self:send_command(fd, "QUIT")
    fd:close()
    resolve({response = response, status=status})
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
    local date = os.date("%Y%m%d%H%I%S")
    return string.format("<%s.%s.%d@%s>", NODE_ID:gsub("%/", "-"), date, self.counter, self.host)
end

--- Initialize SMTP client
---@param params {host: string|nil, logging: boolean|nil, ssl: boolean|nil, dkim_privkey: string|nil, dkim_selector: string|nil, dkim_domain: string|nil}
function smtp_client:init(params)
    self.host = params.host or "localhost"
    self.logging = params.logging or false
    self.dkim_domain = params.dkim_domain
    self.dkim_privkey = params.dkim_privkey
    self.dkim_selector = params.dkim_selector
    if params.ssl then
        self.ssl_enforced = true
    end
end

function smtp_client:default_initialize()
    self:init({
        host = os.getenv("HOST") or "localhost",
        logging = (os.getenv("LOGGING") or "false") == "true",
        ssl = (os.getenv("TLS") or "false") == "true"
    })
end

return smtp_client