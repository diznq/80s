require("aio.aio")

local http_client = require("aio.lib.http_client")

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

--- Get IPv4 address to host name
---@param host_name string host name
---@param record_type string record type
---@return aiopromise<dnsresponse> response
function dns:get_ip(host_name, record_type)
    local a, b, c, d = host_name:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if a and b and c and d then
        local resolve, resolver = aio:prepare_promise()
        resolve({ip = host_name})
        return resolver
    elseif self.cache[host_name] and self.cache[host_name].A then
        local resolve, resolver = aio:prepare_promise()
        resolve({ip = self.cache[host_name].A})
        return resolver
    end
    return aio:cached("dns", host_name .. ":" .. record_type, function ()
        local resolve, resolver = aio:prepare_promise()
        http_client:request({
            method = "GET",
            url = "https://dns.google/resolve?name=" .. host_name .. "&type=" .. record_type
        })(function (result)
            if result.status ~= 200 then
                resolve(make_error("failed to resolve " .. host_name .. ":" .. record_type))
            else
                local data = codec.json_decode(result.body)
                if not data then
                    resolve(make_error("failed to read response from DNS"))
                    return
                end
                if not data.Answer or #data.Answer == 0 then
                    resolve(make_error("no DNS records for " .. host_name .. ":" .. record_type))
                else
                    local first = data.Answer[1]
                    if first.type == 1 then
                        resolve({ip=first.data})
                    elseif first.type == 15 then
                        local prio, what = first.data:match("(%d+) ([^ ]+)%.$")
                        if not what then
                            resolve(make_error("failed to parse MX address: " .. first.data))
                        else
                            self:get_ip(what, "A")(function (result)
                                if iserror(result) then
                                    resolve(result)
                                else
                                    resolve({ip = result.ip, cname=what})
                                end
                            end)
                        end
                    else
                        local what = first.data:match("([^ ]+)")
                        if not what then
                            resolve(make_error("failed to parse MX address: " .. first.data))
                        else
                            self:get_ip(what, "A")(function (result)
                                if iserror(result) then
                                    resolve(result)
                                else
                                    resolve({ip = result.ip, cname=what})
                                end
                            end)
                        end
                    end
                end
            end
        end)
        return resolver
    end)
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
    self:add_record("dns.google", "A", "8.8.4.4")
end

dns:init()

dns:get_ip("gmail.com", "MX")(function (result)
    print(codec.json_encode(result))
end)

return dns