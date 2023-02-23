local mysql = require("server.mysql")

--- @alias ormentity {[string]: ormfield}

--- @class ormfield
--- @field field string
--- @field type ormtype

--- @class ormtype
local ormtype = {
    format = function()
        return "%s"
    end,
    fromstring = function(text)
        return text
    end,
    toformat = function(value)
        return tostring(value)
    end
}

local repository = {}

--- Create repository
---@param sql mysql
---@param repo {source: string, entity:ormentity, [string]: any}
function repository:create(sql, repo)
    --- @type string
    local source = repo.source
    --- @type ormentity
    local entity = repo.entity
    repo.sql = sql
    for method, params in pairs(repo) do
        if method:match("^find.*") then
            local token = method
            local args = {}
            --- @type ormtype[]
            local types = {}
            local limit = "LIMIT 1"
            local query = "SELECT * FROM " .. source
            if token:match("^findAll.*") then
                limit = ""
                token = token:gsub("^findAll", "")
            else
                token = token:gsub("^find", "")
            end
            if token:match("^By") then
                local attempts = 0
                while attempts < 10 do
                    for field, def in pairs(entity) do
                        local sub = token:sub(3, 3 + #field)
                        sub = sub:sub(1, 1):lower() .. sub:sub(2)
                        if sub == field then
                            table.insert(args, def.field .. " = '" .. def.type.format() .. "'")
                            table.insert(types, def.type)
                            token = token:sub(3 + #field + 1)
                            break
                        end
                    end
                end
                query = query .. " " .. table.concat(args, " AND ")
            end
            if #limit > 0 then
                query = query .. " " .. limit
            end
            repo[method] = function(self, ...)
                local treated = {}
                for i, v in pairs({...}) do
                    table.insert(treated, types[i].toformat(v))
                end
                return sql:select(query, unpack(treated))
            end
        end
    end
end
