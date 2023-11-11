require("aio.aio")

--- @alias cname {CNAME: string}
--- @alias dnssingleresponse {CNAME: string|cname|nil, MX: string|cname|nil, A: string|cname|nil, AAAA: string|cname|nil, error: string|nil}

_dns = _dns or {
    provider = "8.8.8.8",
    cache = {},
    id = 0,
}

local dns = _dns

local query_types = {
    A = 1,
    AAAA = 28,
    CNAME = 5,
    MX = 15
}

local query_types_rev = {}

for i, v in pairs(query_types) do
    query_types_rev[v] = i
end

--- Add a record
---@param host_name string host name
---@param record_type string record type
---@param value string|cname ip address or cname
function dns:add_record(host_name, record_type, value)
    self.cache[host_name] = self.cache[host_name] or {}
    self.cache[host_name][record_type] = value
end

--- Read DNS data at given offset
---@param addr string current chunk
---@param raw_data string all data
---@return string address
function dns:read_addr(addr, raw_data)
    local rem = addr
    local parts = {}
    while true and #rem > 0 do
        local size = string.byte(rem, 1)
        if size >= 192 then
            local off = string.byte(rem, 2)
            parts[#parts+1] = self:read_addr(raw_data:sub(off + 1), raw_data)
            break
        end
        if size == 0 then rem = rem:sub(2) break end
        parts[#parts+1] = rem:sub(2, size + 1)
        rem = rem:sub(2 + size)
    end
    return table.concat(parts, ".")
end

--- Read at offset where pointer points to
---@param raw_data string all data
---@param offset integer offset
---@return {name: string, addr_type: string, addr_type_num: integer, addr_class: integer} data parsed data
function dns:read_offset(raw_data, offset)
    local rem = raw_data:sub(offset + 1)
    local parts = {}
    while true and #rem > 4 do
        local size = string.byte(rem, 1)
        if size >= 192 then
            local res = self:read_offset(raw_data, string.byte(rem, 2))
            res.name = table.concat(parts, ".") .. "." .. res.name
            return res
        end
        if size == 0 then rem = rem:sub(2) break end
        parts[#parts+1] = rem:sub(2, size + 1)
        rem = rem:sub(2 + size)
    end
    local addr_type, addr_class = nil, nil
    if #rem >= 4 then
        addr_type, addr_class = string.unpack(">I2I2", rem)
    end
    return {
        name = table.concat(parts, "."),
        addr_type = query_types_rev[addr_type or -1],
        addr_type_num = addr_type,
        addr_class = addr_class
    }
end


function dns:resolve_addr(raw_data, addr)
    local ptr = string.unpack(">I2", addr)
    if ptr < 0xC000 then
        addr = {
            addr_type = "CNAME",
            addr_type_num = query_types.CNAME,
            name = self:read_addr(addr, raw_data)
        }
    else
        addr = self:read_offset(raw_data, ptr)
    end
    return addr
end


--- Perform a single DNS request
---@param host_name string host name
---@param record_type string request type
---@return aiopromise<dnssingleresponse> response
function dns:get_record(host_name, record_type)
    local resolve, resolver = aio:prepare_promise()
    record_type = record_type or "A"
    local pair = host_name
    local hit = self.cache[pair]
    if hit and hit[record_type] then
        resolve(hit)
        return resolver
    end
    if query_types[record_type] == nil then
        resolve(make_error("invalid record type: " .. record_type))
        return resolver
    end
    hit = hit or {}
    self.cache[pair] = hit
    local ip1, ip2, ip3, ip4 = host_name:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if ip1 and ip2 and ip3 and ip4 then
        hit[record_type] = host_name
        resolve(hit)
        return resolver
    end
    local qd, an, ns, ar = 1, 0, 0, 0
    local flags = 0x0100
    local parts = {}
    for match in host_name:gmatch("([^.]+)") do
        parts[#parts+1] = match
    end
    if #parts == 0 then
        resolve(make_error("failed to parse domain name into sub levels: " .. host_name))
        return resolver
    end
    self.id = (self.id + 1) % 5000
    local used_id = self.id
    local request = string.pack(">I2I2I2I2I2I2", used_id, flags, qd, an, ns, ar)
    local r = {request}
    for i, part in ipairs(parts) do
        r[#r+1] = string.char(#part)
        r[#r+1] = part
    end
    r[#r+1] = string.char(0)
    r[#r+1] = string.pack(">I2I2", query_types[record_type], 1)
    return aio:cached("dns", host_name .. record_type, function ()
        local resolve, resolver = aio:prepare_promise()
        aio:connect2({host = self.provider, port = 53, udp = true})(function (fd)
            ---@diagnostic disable-next-line: duplicate-set-field
            aio:buffered_cor(fd, function (_)
                local tx = coroutine.yield(12)
                if tx == nil then 
                    fd:close()
                    return resolve(make_error("failed to receive response back"))
                end
                local raw_data = tx
                local tx_id, flags, qd, an, ns, ar = string.unpack(">I2I2I2I2I2I2", tx)
                if tx_id ~= used_id then
                    fd:close()
                    return resolve(make_error("response ID mismatch")) 
                end
                local last_bits = flags % 16
                if last_bits ~= 0 then
                    fd:close()
                    return resolve(make_error("DNS request failed"))
                end
                if an < 1 then
                    fd:close()
                    return resolve(make_error("no DNS answer"))
                end
                local offset = 12
                local chunk = 0

                -- parse the queries
                for i=1, qd do
                    local parts = {}
                    chunk = 0
                    while true do
                        local c = coroutine.yield(1)
                        if c == nil then
                            fd:close()
                            return resolve(make_error("failed to parse DNS query"))
                        end
                        raw_data = raw_data .. c
                        chunk = chunk + 1
                        local size = string.byte(c, 1)
                        if size == 0 then break end
                        local part = coroutine.yield(size)
                        if part == nil then
                            fd:close()
                            return resolve(make_error("failed to parse DNS query"))
                        end
                        chunk = chunk + size
                        raw_data = raw_data .. part
                        parts[#parts+1] = part
                    end
                    local c = coroutine.yield(4)
                    raw_data = raw_data .. c
                    offset = offset + chunk
                end

                -- parse the answers
                for i=1, an do
                    local c = coroutine.yield(12)
                    if c == nil then
                        fd:close()
                        return resolve(make_error("failed to parse DNS answer"))
                    end
                    raw_data = raw_data .. c
                    local name, answ_type, answ_class, validity, size = string.unpack(">I2I2I2I4I2", c)
                    local offset = name % 0x1000
                    local name_type = math.floor((name - offset) / 0x1000)
                    if size == 0 then
                        fd:close()
                        return resolve(make_error("DNS answer size was 0"))
                    end
                    local addr = coroutine.yield(size)
                    if addr == nil then
                        fd:close()
                        return resolve(make_error("failed to read DNS address"))
                    end
                    raw_data = raw_data .. addr

                    if query_types_rev[answ_type] ~= nil then
                        local record = self:read_offset(raw_data, offset)
                        if record == nil then
                            fd:close()
                            return resolve(make_error("invalid DNS pointer"))
                        end
                        if answ_type == query_types.A then
                            local q1, q2, q3, q4 = string.byte(addr, 1, 4)
                            addr = string.format("%d.%d.%d.%d", q1, q2, q3, q4)
                        elseif answ_type == query_types.MX then
                            addr = addr:sub(3)
                            addr = self:resolve_addr(raw_data, addr)
                        elseif answ_type == query_types.CNAME then
                            addr = self:resolve_addr(raw_data, addr)
                        end
                        if type(addr) == "table" then
                            answ_type = addr.addr_type_num
                            addr = { CNAME = addr.name }
                        end
                        self.cache[record.name] = self.cache[record.name] or {}
                        self.cache[record.name][record_type] = addr
                    end
                end
                fd:close()
                resolve(self.cache[host_name])
            end)
            fd:write(table.concat(r))
        end)
        return resolver
    end)
end

--- Get IPv4 address to host name
---@param host_name string host name
---@param record_type string record type
---@return aiopromise<dnsresponse> response
function dns:get_ip(host_name, record_type)
    local resolve, resolver = aio:prepare_promise()
    self:get_record(host_name, record_type)(function (result)
        if iserror(result) then
            resolve(result)
        else
            if result[record_type] == nil then
                resolve(make_error("failed to find DNS entry"))
            elseif result[record_type].CNAME then
                self:get_ip(result[record_type].CNAME, "A")(function (subresult)
                    subresult["cname"] = result[record_type].CNAME
                    resolve(subresult)
                end)
            else
                resolve({ip = result[record_type]})
            end
        end
    end)
    return resolver
end

--- Initialize DNS
---@param params {provider: string|nil, host_names: {[string]: string}|nil}|nil
function dns:init(params)
    params = params or {}
    params.provider = params.provider or "8.8.8.8"
    params.host_names = params.host_names or {
        localhost = "127.0.0.1"
    }
    self.provider = params.provider
    for i, v in pairs(params.host_names) do
        self:add_record(i, "A", v)
    end
end

dns:init()

return dns