require("aio.aio")

--- @alias httpresponse {status: integer, status_line: string, headers: table, body: string, error: string|nil}

local http_client = {
    --- @type lightuserdata
    ssl = nil
}

-- hard limits for responses fetched from remote servers
local MAX_RESPONSE_HEADER = tonumber(os.getenv("HTTP_CLIENT_MAX_HEADER")) or 65536
local MAX_RESPONSE_BODY = tonumber(os.getenv("HTTP_CLIENT_MAX_BODY")) or 16777216

--- Strip CR/LF/NUL so a value cannot inject a header or split the request
---@param value any
---@return string
local function header_safe(value)
    return (tostring(value):gsub("[\r\n%z]", " "))
end

--- Perform HTTP request
---@param params {method: string, url: string, query: table|nil, headers: table|nil, body: string|nil, response_file: file*|nil} request params
---@return aiopromise<httpresponse> response HTTP response or error
function http_client:request(params)
    local resolve, resolver = aio:prepare_promise()
    if type(params.method) ~= "string" or not params.method:match("^[A-Za-z]+$") then
        resolve(make_error("invalid HTTP method"))
        return resolver
    end
    local method = params.method:upper()
    local headers = params.headers or {}
    local full_url = params.url
    local body = params.body or ""
    local protocol, host, port, script = self:parse_url(full_url)
    if protocol and host and port and script then
        if port < 1 or port > 65535 then
            resolve(make_error("invalid port"))
            return resolver
        end
        if script:find("[%s%c]") or host:find("[%s%c]") then
            resolve(make_error("invalid URL"))
            return resolver
        end
        if protocol == "https" and self.ssl == nil then
            local ssl, err = aio:get_ssl_context()
            if not ssl then
                resolve(make_error("failed to initialize SSL context: " .. err))
                return resolver
            end
            self.ssl = ssl
        end
        aio:connect2({host = host, port = port, ssl = (protocol == "https" and self.ssl or nil)})(function (fd)
            if iserror(fd) then
                if params.response_file then params.response_file:close() end
                resolve(fd)
                return
            end
            local http_headers = {}
            local query_params = {}
            local query_sep = ""
            local query = ""

            headers["connection"] = "close"
            headers["host"] = host
            if #body > 0 or method ~= "GET" then
                headers["content-length"] = tostring(#body)
            end

            local invalid_header = nil
            for key, value in pairs(headers) do
                if type(key) ~= "string" or not key:match("^[%w%-_]+$") then
                    invalid_header = "invalid header name"
                    break
                end
                http_headers[#http_headers+1] = key .. ": " .. header_safe(value)
            end
            if invalid_header then
                fd:close()
                if params.response_file then params.response_file:close() end
                resolve(make_error(invalid_header))
                return
            end
            for key, value in pairs(params.query or {}) do
                query_params[#query_params+1] = codec.url_encode(tostring(key)) .. "=" .. codec.url_encode(tostring(value))
            end
            if #query_params > 0 then
                if script:find("%?") then
                    query_sep = "&"
                else
                    query_sep = "?"
                end
                query = table.concat(query_params, "&")
            end
            local request = string.format("%s %s HTTP/1.1\r\n%s\r\n\r\n%s", method, script .. query_sep .. query, table.concat(http_headers, "\r\n"), body)
            aio:buffered_cor(fd, function (_)
                local function fail(message)
                    fd:close()
                    if params.response_file then params.response_file:close() end
                    resolve(make_error(message))
                end

                local header = coroutine.yield("\r\n\r\n", MAX_RESPONSE_HEADER)
                if not header then
                    return fail("failed to read header")
                end
                if #header > MAX_RESPONSE_HEADER then
                    return fail("response header too large")
                end
                local http_protocol, status_line, response_headers = aio:parse_http(header, true)
                if not (http_protocol and status_line and response_headers) then
                    return fail("failed to parse response header")
                end
                if response_headers["\0framing"] then
                    response_headers["\0framing"] = nil
                    return fail("ambiguous response framing")
                end

                local response_body = ""
                local chunked = (response_headers["transfer-encoding"] or ""):lower():find("chunked", 1, true) ~= nil
                local response_length = tonumber(response_headers["content-length"] or "0")
                if response_length == nil or response_length < 0 or response_length > MAX_RESPONSE_BODY then
                    return fail("invalid response length")
                end

                local function write_file(chunk)
                    local _, err = params.response_file:write(chunk)
                    return err == nil
                end

                if chunked then
                    local chunks = {}
                    local total = 0
                    while true do
                        local line = coroutine.yield("\r\n")
                        if not line then
                            return fail("protocol error on chunked encoding")
                        end
                        if #line > 1024 then
                            return fail("chunk size line too long")
                        end
                        local size = tonumber(line:match("^%s*(%x+)") or "", 16)
                        if not size or size > MAX_RESPONSE_BODY or total + size > MAX_RESPONSE_BODY then
                            return fail("invalid chunk size")
                        end
                        if size == 0 then
                            -- consume optional trailers, terminated by an empty line
                            while true do
                                local trailer = coroutine.yield("\r\n")
                                if trailer == nil or trailer == "" then break end
                                if #trailer > MAX_RESPONSE_HEADER then
                                    return fail("response trailer too large")
                                end
                            end
                            break
                        end
                        local chunk = coroutine.yield(size)
                        if chunk == nil then
                            return fail("failed to read response chunk")
                        end
                        total = total + #chunk
                        if params.response_file then
                            if not write_file(chunk) then
                                return fail("failed to write response to file")
                            end
                        else
                            chunks[#chunks+1] = chunk
                        end
                        local terminator = coroutine.yield("\r\n")
                        if terminator ~= "" then
                            return fail("failed to read chunk terminator")
                        end
                    end
                    if not params.response_file then
                        response_body = table.concat(chunks)
                    end
                elseif response_length > 0 then
                    if params.response_file then
                        local to_read = response_length
                        while to_read > 0 do
                            local chunk = coroutine.yield(math.min(1000000, to_read))
                            if chunk == nil then
                                return fail("failed to write response stream")
                            end
                            if not write_file(chunk) then
                                return fail("failed to write response to file")
                            end
                            to_read = to_read - #chunk
                        end
                    else
                        response_body = coroutine.yield(response_length)
                        if response_body == nil then
                            return fail("failed to read response body")
                        end
                    end
                end

                fd:close()
                if params.response_file then params.response_file:close() end
                resolve({
                    status = tonumber(status_line:match("^(%d+)")),
                    status_line = status_line,
                    headers = response_headers,
                    body = response_body
                })
            end)
            local wr_ok = fd:write(request)
            if not wr_ok then
                fd:close()
                if params.response_file then params.response_file:close() end
                resolve(make_error("failed to write data to fd"))
            end
        end)
    else
        if params.response_file then params.response_file:close() end
        resolve(make_error("failed to parse URL"))
    end
    return resolver
end

--- Parse URL into protocol, host, port, script
---@param url string full URL
---@return string|nil protocol http or https
---@return string|nil host host name
---@return number|nil port port number
---@return string|nil script path after /, incl. /
function http_client:parse_url(url)
    local protocol, host_name, script = url:match("^(https?)://(.-)(/.*)$")
    if not protocol or not host_name or not script then
        return nil, nil, nil, nil
    end
    if #script == 0 then script = "/" end
    local host, port = host_name:match("^(.-):(%d+)$")
    if not host or not port then
        port = protocol == "http" and "80" or "443"
        host = host_name
    end
    port = tonumber(port)
    return protocol, host, port, script
end

return http_client