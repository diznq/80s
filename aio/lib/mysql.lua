require("aio.aio")

--- @alias mysqlerror {error: string} mysql error
--- @alias mysqlok {ok: boolean, affected_rows: integer, last_insert_id: integer, status_flags: integer, warnings: integer, info: string|nil} mysql ok
--- @alias mysqleof {eof: boolean}
--- @alias mysqlfielddef {catalog: string, schema: string, table: string, org_table: string, name: string, org_name: string, character_set: integer, column_length: integer, type: integer, flags: integer, decimals: integer}
--- @alias mysqlresult string[] items

--- @alias encode_le32 fun(number: integer): string encode number as 32-bit little endian
--- @alias encode_le24 fun(number: integer): string encode number as 24-bit little endian
--- @alias decode_leN fun(...: integer): integer decode little endian number from bytes
--- @alias bxor fun(a: integer, b: integer): integer compute XOR of a and b

--- @class mysql
local mysql = {
    --- @type aiosocket
    fd = nil,
    connected = false,   -- whether we are authenticated to db
    --- @type {callback: function, query: string}[]
    callbacks = {},
    --- @type {callback: function, query: string}?
    active_callback = nil,

    user = "root",
    password = "toor",
    host = "localhost",
    port = 3306,
    db = "",
    sequence_id = 0
}

local CLIENT_CONNECT_WITH_DB = 8
local CLIENT_PROTOCOL_41 = 512
local CLIENT_PLUGIN_AUTH = 524288 -- 1 << 19
local CLIENT_CONNECT_ATTRS = 1048576 -- 1 << 20
local CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 16777216 -- 1 << 24
local CLIENT_QUERY_ATTRIBUTES = 134217728 -- 1 << 27
local CONN_FLAGS = 0x0FA68D

-- to make this LuaJIT compatible as LuaJIT doesn't support bitwise ops
local codecs = [[
return function (number)
    local d = number & 255
    local c = (number >> 8) & 255
    local b = (number >> 16) & 255
    local a = (number >> 24) & 255
    return string.char(d, c, b, a)
end, function (number)
    local d = number & 255
    local c = (number >> 8) & 255
    local b = (number >> 16) & 255
    return string.char(d, c, b)
end, function (...)
    local bytes = {...}
    local value = 0
    for i=1,#bytes do
        local byte = bytes[i]
        value = value | (byte << ((i - 1) * 8))
    end
    return value
end, function(a, b)
    return a ~ b
end
]]

if type(jit) == "table" then
    codecs = [[
return function (number)
    local d = bit.band(number, 255)
    local c = bit.band(bit.rshift(number, 8), 255)
    local b = bit.band(bit.rshift(number, 16), 255)
    local a = bit.band(bit.rshift(number, 24), 255)
    return string.char(d, c, b, a)
end, function (number)
    local d = bit.band(number, 255)
    local c = bit.band(bit.rshift(number, 8), 255)
    local b = bit.band(bit.rshift(number, 16), 255)
    return string.char(d, c, b)
end, function (...)
    local bytes = {...}
    local value = 0
    for i=1,#bytes do
        local byte = bytes[i]
        value = bit.bor(value, bit.lshift(byte, ((i - 1) * 8)))
    end
    return value
end, function(a, b)
    return bit.bxor(a, b)
end
    ]]
end
local encode_le32, encode_le24, decode_leN, bxor = load(codecs)()

local function encode_varstr(str)
    return string.char(#str) .. str
end

local function encode_str(str)
    return str .. "\0"
end

---@class mysql_decoder
local mysql_decoder = {ptr=1, data=""}

--- Create new MySQL decoder instance
---@param data string packet data
---@return mysql_decoder instance
function mysql_decoder:new(data)
    local instance = {data=data, ptr=1}
    setmetatable(instance, self)
    self.__index = self
    return instance
end

--- Read data
--- @param n integer|nil
--- @return string|nil
function mysql_decoder:read(n)
    if n == nil then return nil end
    if n == 0 then return "" end
    local chunk = self.data:sub(self.ptr, self.ptr + n - 1)
    self.ptr = self.ptr + n
    return chunk
end

--- Read zero terminated string
---@return string
function mysql_decoder:zero_terminated_string()
    local start = self.data:find("\0", self.ptr, true)
    if start then
        local str = self.data:sub(self.ptr, start - 1)
        self.ptr = start + 1
        return str
    end
    return ""
end

--- Read length encoded int variable length string
---@return string|nil
function mysql_decoder:var_string()
    return self:read(self:lenint())
end

--- Read remaining contents
---@return string
function mysql_decoder:eof()
    local str = self.data:sub(self.ptr)
    self.ptr = #self.data
    return str
end

--- Read low endian integer
---@param size integer number of bytes
---@return integer result
function mysql_decoder:int(size)
    local value = decode_leN(self.data:byte(self.ptr, self.ptr + size - 1))
    self.ptr = self.ptr + size
    return value
end

--- Read length-encoded integer
---@return integer|nil result
function mysql_decoder:lenint()
    local int = self:int(1)
    if int == 251 then
        return nil
    end
    if int >= 252 then
        if int == 252 then
            return self:int(2)
        elseif int == 253 then
            return self:int(3)
        elseif int == 254 then
            return self:int(8)
        else
            -- reserved
            return 0
        end
    else
        return int
    end
end

local mysql_pool = {}
function mysql_pool:new(size)
    local connections = {}
    local utilization = {}
    for i=1, size do
        table.insert(connections, mysql:new())
        table.insert(utilization, 0)
    end

    local pick = function()
        local least = nil
        local least_conn = nil
        local least_i = nil
        for i, v in ipairs(connections) do
            local cbs = #v.callbacks + (v.active_callback and 1 or 0)
            if least == nil or cbs < least then
                least = cbs
                least_conn = v
                least_i = i
            end
        end
        ---@diagnostic disable-next-line: need-check-nil
        utilization[least_i] = utilization[least_i] + 1
        return least_conn
    end

    local connect = function(self, ...)
        local promises = {}
        for _, conn in ipairs(connections) do
            table.insert(promises, conn:connect(...))
        end
        return aio:gather(unpack(promises))
    end

    local mt = {
        __index = function(t, k)
            if k == "connect" then return connect
            elseif k == "pick" then return pick
            else
                local conn = pick()
                return function(self, ...)
                    ---@diagnostic disable-next-line: need-check-nil
                    return conn[k](conn, ...)
                end
            end
        end
    }

    return setmetatable({}, mt)
end

--- Initialize new MySQL instance
---@return mysql instance
function mysql:new()
    --- @type mysql
    local instance = {
        user = "root",
        password = "toor",
        host = "localhost",
        port = 3306,
        db = ""
    }
    setmetatable(instance, self)
    self.__index = self
    instance:reset()
    return instance
end

--- Create MySQL pool
---@param size integer pool size, default 1
function mysql:new_pool(size)
    size = size or 1
    if size == 1 then return mysql:new() end
    local pool = mysql_pool:new(size)
    return pool
end

function mysql:reset()
    if self.fd ~= nil then
        self.fd:close()
    end
    self.fd = nil
    self.sequence_id = 0
    self.connected = false
    self.callbacks = {}
    self.active_callback = nil
end

--- Connect to MySQL server, supports only mysql_native_password auth as of ow
---@param user string user name
---@param password string user password
---@param db string db to connect to
---@param host? string hostname, defaults to localhost
---@param port? integer port, defaults to 3306
---@return aiothen promise true if success, error text otherwise
function mysql:connect(user, password, db, host, port)
    self.host = host or "localhost"
    self.port = port or 3306
    self.user = user
    self.db = db
    self.password = password
    self.sequence_id = 0

    local on_resolved, resolve_event = aio:prepare_promise()

    --- @type aiosocket|nil, string|nil
    local sock, err = self.fd, nil
    
    if not sock then
        sock, err = aio:connect(ELFD, self.host, self.port)
    end

    if not sock then
        on_resolved(nil, "failed to create socket: " .. tostring(err))
    else
        self.fd = sock
        aio:buffered_cor(self.fd, function (resolve)
            local ok, err = self:handshake()
            
            if not ok then
                self.connected = false
                on_resolved(nil, err)
                self:reset()
                return
            else
                self.connected = true
                on_resolved(ok, nil)
            end

            while true do
                local seq, command = self:read_packet()
                if seq == nil then
                    print("[mysql] mysql.on_data: seq returned empty response")
                end

                -- responses from MySQL arrive sequentially, so we can call on_resolved callbacks in that fashion too
                -- in case active_callback was set and keeps returning true, it will be called until it doesn't return
                -- true anymore
                if self.active_callback ~= nil then
                    local ok, res = pcall(self.active_callback.callback, seq, command, self.active_callback.query)
                    if not ok then
                        print("[mysql] mysql.on_data: next call failed: ", res)
                        self.active_callback = nil
                    elseif res ~= true then
                        self.active_callback = nil
                    end
                elseif #self.callbacks > 0 and seq == 1 then
                    local first = self.callbacks[1]
                    table.remove(self.callbacks, 1)
                    -- use protected call, so it doesn't break our loop
                    -- if it returns true, it means more sequences are needed!
                    local ok, res = pcall(first.callback, seq, command, first.query)
                    if not ok then
                        print("[mysql] mysql.on_data: first call failed: ", res)
                    elseif res == true then
                        self.active_callback = first
                    end
                end
            end
        end)
    end

    return resolve_event
end

function mysql:debug_packet(header, packet)
    local text = ""
    for i=1, #packet do
        text = text .. (string.format("%02x " , packet:byte(i, i)))
        if i % 16 == 8 then
            text = text .. "   "
        elseif i % 16 == 0 then
            text = text .. "\n"
        end
    end
    print(header .. ":\n" .. text)
end

--- Read MySQL server packet
---@return integer|nil sequence sequence ID
---@return string packet payload
function mysql:read_packet()
    local length = coroutine.yield(4)
    if length == nil then
        self:reset()
        return nil, ""
    end
    local reader = mysql_decoder:new(length)
    length = reader:int(3)
    local seq = reader:int(1)
    local packet = coroutine.yield(length)
    if packet == nil then
        self:reset()
        return nil, ""
    end
    return seq, packet
end

--- Write MySQL client packet
---@param seq integer sequence ID
---@param packet string payload
---@return boolean ok true if write was ok
function mysql:write_packet(seq, packet)
    if not self.fd then
        print("[mysql] mysql.write_packet: self.fd is nil")
        self:reset()
        return false
    end

    local pkt = encode_le24(#packet).. string.char(seq) .. packet

    local res = self.fd:write(pkt, false)
    if not res then
        print("[mysql] mysql.write_packet: socket write failed")
        self:reset()
    end
    return res
end

--- Perform MySQL password/scramble hash
---@param password string user password
---@param scramble string nonce received from MySQL server
---@return string hash hashed password used for authentication
function mysql:native_password_hash(password, scramble)
    local shPwd = crypto.sha1(password) -- SHA1(password)
    local dshPwd = crypto.sha1(shPwd) -- SHA1(SHA1(password))
    local shJoin = crypto.sha1(scramble .. dshPwd) -- SHA1(scramble .. SHA1(SHA1(password)))
    local b1 = {shPwd:byte(1, 20)}
    local b2 = {shJoin:byte(1, 20)}
    -- SHA1(password) XOR SHA1(scramble .. SHA1(SHA1(password)))
    for i=1, 20 do
        b1[i] = bxor(b1[i], b2[i])
    end
    return string.char(unpack(b1))
end

--- Decode initial handshake from the server
---@param data string payload data
---@return string method authentication method
---@return string scramble authentication scramble (nonce)
function mysql:decode_server_handshake(data)
    local pivot = data:find("\0", 0, true)
    --- @type string
    local rest = data:sub(pivot + 1)
    local scramble1 = rest:sub(5, 12)
    local len = rest:byte(21)
    local off = 32 + math.max(13, len - 8)
    local scramble2 = rest:sub(32, off - 2)
    local scramble = scramble1 .. scramble2
    local method = rest:sub(off, #rest - 1)
    return method, scramble
end

--- Perform initial login handshale
---@return boolean status true if ok
---@return string error error message if not ok
function mysql:handshake()
    local seq, packet = self:read_packet()
    if not seq then
        return false, "connection dropped in first auth step"
    end
    local method, scramble = self:decode_server_handshake(packet)
    self:write_packet(
        1,
        encode_le32(CONN_FLAGS) .. 
        encode_le32(0xFFFFFF) ..
        string.char(45) ..
        ("\0"):rep(23) ..
        encode_str(self.user) ..
        encode_varstr(self:native_password_hash(self.password, scramble)) ..
        encode_str(self.db) ..
        encode_str(method)
    )
    local seq, response = self:read_packet()
    if not seq then
        return false, "connection dropped in seccond auth step"
    end
    local response_type = response:byte(1, 1)
    if response_type == 0 then
        return true, "ok"
    elseif response_type == 255 then
        return false, response:sub(10)
    else
        return false, response
    end
end

function mysql:escape(text)
    if type(text) == "table" then
        if #text == 0 then
            return "(NULL)"
        else
            local results = {}
            for _, v in ipairs(text) do
                table.insert(results, string.format("'%s'", self:escape(v)))
            end
            return string.format("(%s)", table.concat(results, ","))
        end
    end
    if type(text) ~= "string" then
        return text
    end
    return codec.mysql_encode(text)
end

--- Execute SQL query in raw mode
---@param query string query
---@param ... string arguments
---@return fun(on_resolved: fun(seq: integer, response: string, query: string)|thread) promise response promise
function mysql:raw_exec(query, ...)
    local params = {...}
    local on_resolved, resolve_event = aio:prepare_promise()
    local executor = function()
        local command, ok = query, true
        if #params > 0 then
            for i, param in ipairs(params) do
                params[i] = self:escape(param)
            end
            ok, command = pcall(string.format, query, unpack(params))
            if not ok then
                on_resolved(nil, {error="string format failed: " .. command .. ", query: " .. query})
                return
            end
        end
        table.insert(self.callbacks, {callback = on_resolved, query = command})
        self:write_packet(0, string.char(3) .. command)
    end
    if not self.fd then
        self:connect(self.user, self.password, self.db)(function (ok, err)
            if not ok then
                on_resolved(nil, err)
            else
                executor()
            end
        end)
    else
        executor()
    end

    return resolve_event
end


--- Execute SQL query
---@param query string query string or format
---@param ... any query parameters that will be escaped
---@return fun(on_resolved: fun(result: mysqlerror|mysqlok|mysqleof))
function mysql:exec(query, ...)
    local on_resolved, resolve_event = aio:prepare_promise()
    self:raw_exec(query, ...)(function (seq, response, query)
        if not seq and type(response) == "table" and response.error then
            response.original_query = query
            on_resolved(response)
        else
            local response = self:decode_packet(response)
            on_resolved(response)
        end
    end)
    return resolve_event
end

--- Execute SQL Select query and return results
---@param query string SQL query
---@param ... any query parameters
---@return fun(on_resolved: fun(rows: table[]|nil, errorOrColumns: string|{[string]: mysqlfielddef}|nil)) promise
function mysql:select(query, ...)
    local resolve, resolve_event = aio:prepare_promise()

    self:raw_exec(query, ...)(
        aio:cor0(function (seq, res)
            if not seq then
                resolve(nil, res)
                return
            end
            local reader = mysql_decoder:new(res)
            local is_error = res:byte(1, 1) == 255
            local n_fields = reader:lenint()
            if is_error then
                resolve(nil, self:decode_packet(res).error)
                return
            end
            if n_fields == nil then
                resolve(nil, "n_fields is nil")
                return
            end
            --- @type mysqlfielddef[]
            local fields = {}
            --- @type {[string]: mysqlfielddef}
            local by_name = {}
            local rows = {}
            for i=1, n_fields do
                local seq, field_res = coroutine.yield(true)
                if seq == nil then resolve(nil, "network error while fetching fields") return end
                local field = self:decode_field(field_res)
                table.insert(fields, field)
                by_name[field.name] = field
            end
            local _, eof = coroutine.yield(true)
            if eof:byte(1, 1) ~= 254 then
                resolve(nil, "network error, expected eof after field definitions")
                return
            end
            while true do
                seq, res = coroutine.yield(true)
                if seq == nil then resolve(nil, "network error while fetching rows") return end
                if res:byte(1, 1) == 254 then break end
                reader = mysql_decoder:new(res)
                local row = {}
                for i=1, n_fields do
                    row[fields[i].name] = reader:var_string()
                end
                table.insert(rows, row)
            end
            resolve(rows, by_name)
        end)
    )

    return resolve_event
end

--- Decode MySQL server response
---@param packet string response
---@return mysqlerror|mysqlok|nil decoded decoded response
function mysql:decode_packet(packet)
    local packet_type = packet:byte(1, 1)
    if packet_type == 255 then
        return {
            error = packet:sub(10)
        }
    elseif packet_type == 0 then
        local reader = mysql_decoder:new(packet:sub(2))
        return {
            ok = true,
            affected_rows = reader:lenint(),
            last_insert_id = reader:lenint(),
            status_flags = reader:int(2),
            warnings = reader:int(2),
            info = reader:eof()
        }
    elseif packet_type == 254 then
        return {
            eof = true
        }
    end
    return nil
end

function mysql:decode_field(packet)
    local reader = mysql_decoder:new(packet)
    return {
        catalog = reader:var_string(),
        schema = reader:var_string(),
        table = reader:var_string(),
        org_table = reader:var_string(),
        name = reader:var_string(),
        org_name = reader:var_string(),
        character_set = reader:int(2),
        column_length = reader:int(4),
        type = reader:int(1),
        flags = reader:int(2),
        decimals = reader:int(1)
    }
end

return mysql