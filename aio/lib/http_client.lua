require("aio.aio")

--- @alias httpresponse {status: integer, status_line: string, headers: table, body: string, error: string|nil}

local http_client = {
    --- @type lightuserdata
    ssl = nil
}

--- Perform HTTP request
---@param params {method: string, url: string, query: table|nil, headers: table|nil, body: string|nil, response_file: file*|nil} request params
---@return aiopromise<httpresponse> response HTTP response or error
function http_client:request(params)
    local resolve, resolver = aio:prepare_promise()
    local method = params.method:upper()
    local headers = params.headers or {}
    local full_url = params.url
    local body = params.body or ""
    local protocol, host, port, script = self:parse_url(full_url)
    if protocol and host and port and script then
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

            for key, value in pairs(headers) do
                http_headers[#http_headers+1] = key .. ": " .. codec.url_encode(tostring(value))
            end
            for key, value in pairs(params.query or {}) do
                query_params[#query_params+1] = key .. "=" .. codec.url_encode(tostring(value))
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
                local header = coroutine.yield("\r\n\r\n")
                if not header then
                    fd:close()
                    if params.response_file then params.response_file:close() end
                    resolve(make_error("failed to read header"))
                end
                local http_protocol, status_line, response_headers = aio:parse_http(header, true)
                local response_body = ""
                if http_protocol and status_line and response_headers then
                    local response_length = tonumber(response_headers["content-length"] or "0")
                    if response_length > 0 then
                        if params.response_file then
                            local to_read = response_length
                            local did_read = 0
                            while true do
                                local chunk_size = math.min(1000000, to_read)
                                local chunk = coroutine.yield(chunk_size)
                                if chunk ~= nil then
                                    local f, err = params.response_file:write(chunk)
                                    if err then
                                        fd:close()
                                        params.response_file:close()
                                        resolve(make_error("failed to write response to file"))
                                        return
                                    end
                                    did_read = did_read + #chunk
                                    to_read = to_read - #chunk
                                else
                                    params.response_file:close()
                                    fd:close()
                                    resolve(make_error("failed to write response stream"))
                                    return
                                end
                                if to_read ~= 0 then
                                    fd:close()
                                    params.response_file:close()
                                    resolve(make_error("repsonse seems to be corrupted, read length: " .. tostring(did_read) .. ", requested length: " .. tostring(response_length)))
                                    return
                                else
                                    params.response_file:close()
                                    break
                                end
                            end
                        else
                            response_body = coroutine.yield(response_length)
                        end
                        if response_body == nil then
                            fd:close()
                            if params.response_file then params.response_file:close() end
                            resolve(make_error("failed to read response body"))
                            return
                        end
                    end
                    fd:close()
                    resolve({
                        status = tonumber(status_line:match("^(%d+)")),
                        status_line = status_line,
                        headers = response_headers,
                        body = response_body
                    })
                else
                    fd:close()
                    if params.response_file then params.response_file:close() end
                    resolve(make_error("failed to parse response header"))
                end
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