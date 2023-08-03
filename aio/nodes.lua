require("aio.aio")

SELF_NODE = SELF_NODE or {NODE, PORT, WORKERID}

nodes = nodes or {
    names = {},
    nodes = {
        [NODE_ID] = SELF_NODE
    }
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
---@param except table|nil except fd name
---@param message string data to broadcast
---@param close boolean|nil write close flag
function nodes:broadcast(selector, except, message, close)
    local nodes = self:get_named_fds(selector)
    for _, node in ipairs(nodes) do
        if node.name ~= except then
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
    if not node or node == NODE_ID or (node == NODE and port == PORT or id == WORKERID) then
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
function nodes:start()

end

aio:register_close_handler("nodes", function (fd)
    nodes:unregister(fd)
end)