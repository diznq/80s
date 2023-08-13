require("aio.aio")

SELF_NODE = SELF_NODE or {NODE, PORT, WORKERID}
MESSAGE_BASE = string.format("%s.%d.%d", NODE, PORT, WORKERID)

nodes = nodes or {
    names = {},
    --- @type table|nil
    master = nil,
    global = {},
    --- @type {[string]: table}
    nodes = {
        [NODE_ID] = SELF_NODE
    },
    --- @type {[table]: aiosocket}
    connections = {},
    --- @type string|nil
    authorization = "-",
    --- @type {[string]: table}
    routing = {},
    message_id = 1,
    --- @type {[string]: aiopromise}
    messages = {}
}

--- Set named file descriptor
---@param fd aiosocket file descriptor object
---@param name table name
---@param node table|nil override node value
---@return boolean success
function nodes:register(fd, name, node)
    local root = self.names
    if node then
        name = {unpack(name)}
        name[1] = node
    else
        name = {self:get_node(), unpack(name)}
    end
    if fd.name then
        self:unregister(fd)
    end
    fd.name = name
    for i, part in ipairs(name) do
        if i < #name then
            local hit = root[part]
            if (type(hit) == "table" and getmetatable(hit) == aiosocket) or (hit ~= nil and type(hit) ~= "table") then
                return false
            elseif hit == nil then
                hit = {}
                root[part] = hit
                root = hit
            else
                root = hit
            end
        else
            root[part] = fd
        end
    end
    if (not node) and self.master then
        self:announce(name, {self.master}, nil, "POST", "/nodes", "")(function (response)
            -- print("Register result", codec.json_encode(response))
        end)
    end
    return true
end

--- Unset named file descriptor
---@param fd aiosocket named fd
function nodes:unregister(fd)
    local root = self.names
    local name = fd.name
    if not name then return end
    fd.name = nil
    for i, part in ipairs(name) do
        if i == #name then
            root[part] = nil
        else
            local hit = root[part]
            if type(hit) ~= "table" or getmetatable(hit) == aiosocket then
                break
            end
            root = hit
        end
    end
end

--- Get all named FDs that match the selector
---@param selector table selector, can be either name of FD or part of name to match a group
---@return aiosocket[] fds table of all FDs that match the selector
function nodes:get_named_fds(selector)
    local root = self.names
    local matches = {}
    for i, part in ipairs(selector) do
        if i == #selector then
            local hit = root[part]
            -- check if we hit FD directly or subset of FDs
            if getmetatable(hit) == aiosocket then
                return {hit}
            else
                self:recursively_fill(matches, hit, function(entity)
                    return type(entity) == "table" and getmetatable(entity) == aiosocket
                end)
            end
        else
            local hit = root[part]
            if type(hit) ~= "table" then
                return matches
            end
            root = hit
        end
    end
    return matches
end

--- Announce message to remote endpoint
---@param from table selector
---@param selector table selector
---@param except table|string|nil except fd name
---@param method string HTTP method
---@param endpoint string HTTP endpoint
---@param message string data to broadcast
---@returns aiopromise<{status: integer, body: string, headers: {[string]: string}}> response
function nodes:announce(from, selector, except, method, endpoint, message)
    local resolve, resolver = aio:prepare_promise()
    local node = self:get_node(unpack(selector[1]))
    local host, port = node[1], node[2]
    if node[1] == "*" then
        host = self.master[1]
        port = self.master[2]
        table.remove(selector, 1)
    end
    local msg_id = string.format("%s.%d", MESSAGE_BASE, self.message_id)
    self.message_id = self.message_id + 1
    local connection = self.connections[node]
    if not connection then
        local fd, err = aio:connect(ELFD, host, port)
        if fd then
            self.connections[node] = fd
            aio:buffered_cor(fd, function (resolve)
                while true do
                    local header = coroutine.yield("\r\n\r\n")
                    if not header then break end
                    local method, _, headers = aio:parse_http(header, true)
                    local length = tonumber(headers["content-length"])
                    local body = ""
                    if length and length > 0 then
                        body = coroutine.yield(tonumber(length))
                    end
                    if not body then break end
                    local message_id = headers.message
                    if message_id then
                        local promise = self.messages[message_id]
                        if promise then
                            promise({status = tonumber(method), body = body, headers = headers})
                            self.messages[message_id] = nil
                        end
                    end
                end
                self.connections[node] = nil
            end)
            connection = fd
        else
            print("nodes.broadcast: failed to create new connection due to ", err)
        end
    end
    if connection then
        local except_str = "-"
        if type(except) == "string" then
            except_str = except
        elseif type(except) == "table" then
            except_str = codec.json_encode(except)
        end
        self.messages[msg_id] = resolve
        local formatted = string.format(
            "%s %s HTTP/1.1\r\nMessage: %s\r\nFrom: %s\r\nTo: %s\r\nExcept: %s\r\nAuthorization: %s\r\nContent-length: %d\r\nConnection: keep-alive\r\n\r\n%s", 
            method,
            endpoint,
            msg_id,
            codec.json_encode(from),
            codec.json_encode(selector),
            except_str,
            self.authorization,
            message:len(),
            message
        )
        connection:write(formatted)
    end

    return resolver
end

--- Broadcast a message
---@param selector table selector
---@param except table|string|nil except fd name
---@param message string data to broadcast
---@param close boolean|nil write close flag
function nodes:broadcast(selector, except, message, close)
    if #selector >= 1 and selector[1] ~= SELF_NODE and #selector[1] == 3 then
        if selector[1][1] == "*" then
            -- if we broadcast to everyone, get list of everyone and broadcast to them later
            self:announce(SELF_NODE, selector, nil, "GET", "/nodes", "")(function (result)
                local nodes = codec.json_decode(result.body)
                for _, node in ipairs(nodes) do
                    node.delegate[1] = self:get_node(unpack(node.delegate[1]))
                    self:broadcast(node.delegate, except, message, close)
                end
            end)
        else
            -- if we broadcast to outside directly, forward the request directly there
            self:announce(SELF_NODE, selector, except, "PUT", "/nodes", message)
        end
        return
    end
    -- otherwise broadcast it locally
    local nodes = self:get_named_fds(selector)
    for _, node in ipairs(nodes) do
        if not except or (type(except) == "table" and node.name ~= except) or (type(except) == "string" and codec.json_encode(node.name) ~= except) then
            pcall(node.write, node, message, close)
        end
    end
end

--- Get node table object
---@param node string|nil
---@param port integer|nil
---@param id integer|nil
---@return table node_ref
function nodes:get_node(node, port, id)
    if not node or node == NODE_ID or (node == NODE and port == PORT and id == WORKERID) then
        return self.nodes[NODE_ID]
    end
    port = port or PORT
    id = id or WORKERID
    local handle = string.format("%s.%d.%d", node, port, id)
    local hit = self.nodes[handle]
    if hit then return hit end
    hit = {node, port, id}
    self.nodes[handle] = hit
    return hit
end

--- Recursively fill the output with all subkeys of dict given the filter
---@param output table array output
---@param dict table dictionary
---@param filter fun(entry: any): boolean filter
function nodes:recursively_fill(output, dict, filter)
    for _, v in pairs(dict) do
        if filter(v) then
            output[#output + 1] = v
        elseif type(v) == "table" then
            self:recursively_fill(output, v, filter)
        end
    end
end

--- Validate HTTP request
---@param fd aiosocket fd
---@param headers table http headers
---@param body string|nil http request body
---@param min_body integer|nil min body length
---@return string|nil
---@return table
---@return table
function nodes:validate_request(fd, headers, body, min_body)
    local message_id = headers.message
    body = body or ""
    min_body = min_body or 0
    if self.authorization and headers.authorization ~= self.authorization then
        self:response(fd, "401 Unauthorized", message_id, "text/plain", "unauthorized")
        return nil, {}, {}
    end
    if not (headers.from and headers.to and message_id and headers.except and #body >= 0) then
        self:response(fd, "400 Bad request", message_id, "text/plain", "invalid request")
        return nil, {}, {}
    end
    local from = codec.json_decode(headers.from)
    local to = codec.json_decode(headers.to)
    if not (from and to) then
        self:response(fd, "400 Bad request", message_id, "text/plain", "invalid request")
        return nil, {}, {}
    end
    return message_id, from, to
end

--- Respond to HTTP request
---@param fd aiosocket target fd
---@param status string http status
---@param message_id string|nil message id
---@param mime string mime type
---@param message string|table response
---@return boolean
function nodes:response(fd, status, message_id, mime, message)
    return fd:http_response(status, {message = message_id, ["content-type"] = mime}, message)
end

--- Start the nodes module
---
--- Calling this will hook protocol handler for nodes protocol
--- so routing between nodes is possible, without it nodes can
--- only be use locally within node
---
---@param params {authorization: string|nil, master: table|nil} parameters
function nodes:start(params)
    self.authorization = params.authorization or "-"

    if params.master then
        self.master = self:get_node(unpack(params.master))
    end

    aio:http_get("/nodes", function (fd, query, headers, body)
        -- get list of all nodes inside the registry
        local message_id, from, to = self:validate_request(fd, headers)
        if not message_id then return end

        local nodes = self:get_named_fds({self.global, unpack(to)})
        self:response(fd, "200 OK", message_id, "application/json", nodes)
    end)

    aio:http_post("/nodes", function (fd, query, headers, body)
        -- create a new node record inside the registry
        local message_id, from, to = self:validate_request(fd, headers)
        if not message_id then return end

        local node = from[1]
        from[#from + 1] = string.format("%s.%d.%d", node[1], node[2], node[3])
        local fake_fd = {delegate=codec.json_decode(headers.from)}
        setmetatable(fake_fd, aiosocket)

        self:register(fake_fd, from, self.global)
        self:response(fd, "200 OK", message_id, "application/json", {delegate=fake_fd.delegate, name=fake_fd.name})
    end)

    aio:http_any("DELETE", "/nodes", function (fd, query, headers, body)
        -- delete the node record from the registry
        local message_id, from, to = self:validate_request(fd, headers)
        if not message_id then return end
        local original = from[1]
        from[1] = self.global
        from[#from + 1] = string.format("%s.%d.%d", original[1], original[2], original[3])
        self:unregister({name=from})
        self:response(fd, "200 OK", message_id, "application/json", {delegate=codec.json_decode(headers.from), name = from})
    end)

    aio:http_any("PUT", "/nodes", function (fd, query, headers, body)
        -- forward the message to a node
        local message_id, from, to = self:validate_request(fd, headers, body, 1)
        if not message_id then return end

        local except = nil
        if headers.except ~= "-" then
            except = headers.except
        end

        to[1] = self:get_node(unpack(to[1]))
        self:broadcast(to, except, body)
        self:response(fd, "201 Created", message_id, "text/plain", "")
    end)
end

aio:register_close_handler("nodes", function (fd)
    if fd.name and nodes.master then
        nodes:announce(fd.name, {nodes.master}, nil, "DELETE", "/nodes", "")(function (response)
            -- print("Unregister result", codec.json_encode(response))
        end)
    end 
    nodes:unregister(fd)
end)

nodes.global = nodes:get_node("*", 0, 0)
