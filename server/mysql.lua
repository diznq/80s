require("aio.aio")

--- @class mysql
local mysql = {
    --- @type aiosocket
    fd = nil,

    user = "root",
    password = "toor",
    host = "127.0.0.1",
    port = 3306,
    db = ""
}

local CLIENT_PROTOCOL_41 = 512
local CLIENT_CONNECT_WITH_DB = 8
local CLIENT_PLUGIN_AUTH = 1 << 19
local CLIENT_CONNECT_ATTRS = 1 << 20
local CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 1 << 24

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

--- Initialize new MySQL instance
---@return mysql instance
function mysql:new()
    local instance = {
        --- @type aiosocket
        fd = nil,
        user = "root",
        password = "toor",
        host = "127.0.0.1",
        port = 3306,
        db = ""
    }
    setmetatable(instance, self)
    self.__index = self
    return instance
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

    local on_resolved, resolve_event = aio:prepare_promise()

    local sock, err = aio:connect(ELFD, self.host, self.port)

    if not sock then
        on_resolved("failed to create socket: " .. tostring(err))
    else
        self.fd = sock
        function sock:on_connect(elfd, childfd)

        end
        aio:buffered_cor(self.fd, function (resolve)
            local ok, err = self:handshake()
            if not ok then
                on_resolved(err)
            else
                on_resolved(ok)
            end

            while true do
                local seq, command = self:read_packet()
                self:debug_packet("Received", command)
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
    local d, c, b, a = length:byte(1, 5)
    length = b * 65536 + c * 256 + d
    return a, coroutine.yield(length)
end

--- Write MySQL client packet
---@param seq integer sequence ID
---@param packet string payload
---@return boolean ok true if write was ok
function mysql:write_packet(seq, packet)
    local pkt = encode_le24(#packet).. string.char(seq) .. packet
    return self.fd:write(pkt, false)
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
        encode_le32(0x0FA68D) .. 
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

function aio:on_init()
    local sql = mysql:new()

    sql:connect("80s", "password", "db80")(function (...)
        print("Connection status: ", ...)
    end)
end

return mysql