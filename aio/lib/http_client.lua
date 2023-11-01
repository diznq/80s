require("aio.aio")

--- @alias httpresponse {status: integer, status_line: string, headers: table, body: string, error: string|nil}

local http_client = {
    --- @type lightuserdata
    ssl = nil
}

--- Perform HTTP request
---@param params {method: string, url: string, query: table|nil, headers: table|nil, body: string|nil}
---@return aiopromise<httpresponse> response
function http_client:request(params)
    local resolve, resolver = aio:prepare_promise()
    local method = params.method:upper()
    local headers = params.headers or {}
    local full_url = params.url
    local body = params.body or ""
    local protocol, host, port, script = self:parse_url(full_url)
    if protocol and host and port and script then
        if protocol == "https" and self.ssl == nil then
            self.ssl = crypto.ssl_new_client()
        end
        aio:connect2(ELFD, host, port, protocol == "https" and self.ssl or nil)(function (fd)
            if iserror(fd) then
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
                    resolve(make_error("failed to read header"))
                end
                local http_protocol, status_line, response_headers = aio:parse_http(header, true)
                local response_body = ""
                if http_protocol and status_line and response_headers then
                    local response_length = tonumber(response_headers["content-length"] or "0")
                    if response_length > 0 then
                        response_body = coroutine.yield(response_length)
                        print(response_body)
                        if response_body == nil then
                            fd:close()
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
                    resolve(make_error("failed to parse response header"))
                end
            end)
            local wr_ok = fd:write(request)
            if not wr_ok then
                fd:close()
                resolve(make_error("failed to write data to fd"))
            end
        end)
    else
        resolve(make_error("failed to parse URL"))
    end
    return resolver
end

---comment
---@param url any
---@return string|nil
---@return string|nil
---@return number|nil
---@return string|nil
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