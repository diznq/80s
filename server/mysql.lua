require("aio.aio")

--- @alias mysqlerror {error: string} mysql error
--- @alias mysqlok {ok: boolean, affected_rows: integer, last_insert_id: integer, status_flags: integer, warnings: integer, info: string|nil} mysql ok

--- @class mysql
local mysql = {
    --- @type aiosocket
    fd = nil,
    connected = false,   -- whether we are authenticated to db
    callbacks = {},

    user = "root",
    password = "toor",
    host = "127.0.0.1",
    port = 3306,
    db = "",
    sequence_id = 0
}

local CLIENT_PROTOCOL_41 = 512
local CLIENT_CONNECT_WITH_DB = 8
local CLIENT_PLUGIN_AUTH = 1 << 19
local CLIENT_CONNECT_ATTRS = 1 << 20
local CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 1 << 24
local CLIENT_QUERY_ATTRIBUTES = 1 << 27
local CONN_FLAGS = 0x0FA68D

local function encode_le32(number)
    local d = number & 255
    local c = (number >> 8) & 255
    local b = (number >> 16) & 255
    local a = (number >> 24) & 255
    return string.char(d, c, b, a)
end

local function encode_le24(number)
    local d = number & 255
    local c = (number >> 8) & 255
    local b = (number >> 16) & 255
    return string.char(d, c, b)
end

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
--- @param n any
--- @return string
function mysql_decoder:read(n)
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
---@return string
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
    local bytes = {self.data:byte(self.ptr, self.ptr + size - 1)}
    local value = 0
    for i=1,#bytes do
        local byte = bytes[i]
        value = value | (byte << ((i - 1) * 8))
    end
    self.ptr = self.ptr + size
    return value
end

--- Read length-encoded integer
---@return integer result
function mysql_decoder:lenint()
    local int = self:int(1)
    if int >= 251 then
        if int == 251 then
            return self:int(2)
        elseif int == 252 then
            return self:int(3)
        elseif int == 253 then
            return self:int(8)
        else
            -- reserved
            return 0
        end
    else
        return int
    end
end

--- Initialize new MySQL instance
---@return mysql instance
function mysql:new()
    --- @type mysql
    local instance = {
        user = "root",
        password = "toor",
        host = "127.0.0.1",
        port = 3306,
        db = ""
    }
    setmetatable(instance, self)
    self.__index = self
    instance:reset()
    return instance
end

function mysql:reset()
    self.fd = nil
    self.sequence_id = 0
    self.connected = false
    self.callbacks = {}
end

--- Connect to MySQL server, supports only mysql_native_password auth as of ow
---@param user string user name
---@param password string user password
---@param db string db to connect to
---@param host? string hostname, defaults to 127.0.0.1
---@param port? integer port, defaults to 3306
---@return aiothen promise true if success, error text otherwise
function mysql:connect(user, password, db, host, port)
    self.host = host or "127.0.0.1"
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
        on_resolved("failed to create socket: " .. tostring(err))
    else
        self.fd = sock
        aio:buffered_cor(self.fd, function (resolve)
            local ok, err = self:handshake()
            if not ok then
                self.connected = false
                on_resolved(err)
            else
                self.connected = true;
                on_resolved(ok)
            end

            while true do
                local seq, command = self:read_packet()

                -- responses from MySQL arrive sequentially, so we can call on_resolved callbacks in that fashion too
                if #self.callbacks > 0 then
                    local first = self.callbacks[1]
                    table.remove(self.callbacks, 1)
                    -- use protected call, so it doesn't break our loop
                    local ok, res =  pcall(first, seq, command)
                    if not ok then
                        print(res)
                    end
                else
                    print("unhandled command arrived: ", seq)
                    self:debug_packet("Received", command)
                    --print("Command: ", command)
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
---@return integer sequence sequence ID
---@return string packet payload
function mysql:read_packet()
    local length = coroutine.yield(4)
    if length == nil then
        self:reset()
        return 0, ""
    end
    local d, c, b, a = length:byte(1, 5)
    length = b * 65536 + c * 256 + d
    local packet = coroutine.yield(length)
    if packet == nil then
        self:reset()
        return 0, ""
    end
    return a, packet
end

--- Write MySQL client packet
---@param seq integer sequence ID
---@param packet string payload
---@return boolean ok true if write was ok
function mysql:write_packet(seq, packet)
    if not self.fd then
        print("mysql.write_packet: self.fd is nil")
        self:reset()
        return false
    end

    local pkt = encode_le24(#packet).. string.char(seq) .. packet

    local res = self.fd:write(pkt, false)
    if not res then
        print("mysql.write_packet: socket write failed")
        self:reset()
    end
    return res
end

--- Perform MySQL password/scramble hash
---@param password string user password
---@param scramble string nonce received from MySQL server
---@return string hash hashed password used for authentication
function mysql:native_password_hash(password, scramble)
    local shPwd = net.sha1(password) -- SHA1(password)
    local dshPwd = net.sha1(shPwd) -- SHA1(SHA1(password))
    local shJoin = net.sha1(scramble .. dshPwd) -- SHA1(scramble .. SHA1(SHA1(password)))
    local b1 = {shPwd:byte(1, 20)}
    local b2 = {shJoin:byte(1, 20)}
    -- SHA1(password) XOR SHA1(scramble .. SHA1(SHA1(password)))
    for i=1, 20 do
        b1[i] = b1[i] ~ b2[i]
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
    local method, scramble = self:decode_server_handshake(packet)
    self:write_packet(
        seq + 1,
        encode_le32(CONN_FLAGS) .. 
        encode_le32(1 << 24) ..
        string.char(8) ..
        ("\0"):rep(23) ..
        encode_str(self.user) ..
        encode_varstr(self:native_password_hash(self.password, scramble)) ..
        encode_str(self.db) ..
        encode_str(method)
    )
    local seq, response = self:read_packet()
    local response_type = response:byte(1, 1)
    if response_type == 0 then
        return true, "ok"
    elseif response_type == 255 then
        return false, response:sub(10)
    else
        return false, response
    end
end

--- Execute SQL query
---@param query string query
---@param ... string arguments
---@return fun(on_resolved: fun(seq: integer, response: string)) promise response promise
function mysql:exec(query, ...)
    local params = {...}
    local on_resolved, resolve_event = aio:prepare_promise()
    local executor = function()
        table.insert(self.callbacks, on_resolved)
        self:write_packet(0, string.char(3) .. query)
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

--- Decode MySQL server response
---@param packet string response
---@return mysqlerror|mysqlok decoded decoded response
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
    end
    return {}
end

function aio:on_init()
    local sql = mysql:new()

    sql:connect("80s", "password", "db80")(function (ok, err)
        print("Connected: ", ok)
        sql:exec("SELECT * FROM users;")(function (seq, res)
            local result = sql:decode_packet(res)
            if result.error then
                print(result.error)
            else
                for k, v in pairs(result) do
                    print(k, v)
                end
            end
        end)
    end)
end

return mysql