require("aio.aio")

SELF_NODE = SELF_NODE or {NODE, PORT, WORKERID}

nodes = nodes or {
    names = {},
    --- @type {[string]: table}
    nodes = {
        [NODE_ID] = SELF_NODE
    },
    --- @type {[table]: aiosocket}
    connections = {},
    --- @type string|nil
    authorization = "-"
}

--- Set named file descriptor
---@param fd aiosocket file descriptor object
---@param name table name
---@return boolean success
function nodes:register(fd, name)
    local root = self.names
    name = {self:get_node(), unpack(name)}
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

--- Broadcast a message
---@param selector table selector
---@param except table|string|nil except fd name
---@param message string data to broadcast
---@param close boolean|nil write close flag
function nodes:broadcast(selector, except, message, close)
    if #selector >= 1 and selector[1] ~= SELF_NODE and #selector[1] == 3 then
        -- if we broadcast outside
        local node = self:get_node(unpack(selector[1]))
        local connection = self.connections[node]
        if not connection then
            local fd, err = aio:connect(ELFD, node[1], node[2])
            if fd then
                self.connections[node] = fd
                fd.on_close = function ()
                    self.connections[node] = nil
                end
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
            local formatted = string.format(
                "POST /nodes HTTP/1.1\r\nFrom: %s\r\nTo: %s\r\nExcept: %s\r\nAuthorization: %s\r\nContent-length: %d\r\nConnection: keep-alive\r\n\r\n%s", 
                codec.json_encode(SELF_NODE),
                codec.json_encode(selector),
                except_str,
                self.authorization,
                message:len(),
                message
            )
            connection:write(formatted)
        end
        return
    end
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
    local handle = string.format("%s/%d/%d", node, port, id)
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

--- Start the nodes module
---
--- Calling this will hook protocol handler for nodes protocol
--- so routing between nodes is possible, without it nodes can
--- only be use locally within node
---
---@param authorization string|nil authorization header requirement
function nodes:start(authorization)
    self.authorization = authorization or "-"
    aio:http_post("/nodes", function (fd, query, headers, body)
        if not (headers.from and headers.to and headers.except and #body > 0) then
            fd:http_response("400 Bad request", "text/plain", "invalid request")
            return
        end
        if authorization and headers.authorization ~= authorization then
            fd:http_response("401 Unauthorized", "text/plain", "unauthorized")
            return
        end
        --local from = codec.json_decode(headers.from)
        local to = codec.json_decode(headers.to)
        local except = nil
        if headers.except ~= "-" then
            except = headers.except
        end
        to[1] = self:get_node(unpack(to[1]))
        self:broadcast(to, except, body)
    end)
end

aio:register_close_handler("nodes", function (fd)
    nodes:unregister(fd)
end)