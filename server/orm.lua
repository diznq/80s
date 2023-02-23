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

--- @type {[string]: ormtype}
local ormtypes = {
    str = {
        format = function() return "%s" end,
        fromstring = function(text) return text end,
        toformat = function(value) return tostring(value) end
    },
    int = {
        format = function() return "%d" end,
        fromstring = function(text) return tonumber(text) end,
        toformat = function(value) return tonumber(value) end
    }
}


local orm = {}

--- Create repository
---@param sql mysql
---@param repo {source: string, entity:ormentity, [string]: any}
---@return {[string]: fun(...: any): fun(callback: fun(result: table|table[]|nil, error: string|nil))}
function orm:create(sql, repo)
    --- @type string
    local source = repo.source
    --- @type ormentity
    local entity = repo.entity

    -- construct lookup table for entity decoders
    --- @type {[string]: {dest: string, decode: fun(string): any}}
    local decoders = {}
    for field, def in pairs(entity) do
        decoders[def.field] = {dest = field, decode = def.type.fromstring}
    end
    
    repo.sql = sql
    for method, params in pairs(repo) do
        if method:match("^find.*") then
            local token = method
            local args = {}
            local single = true
            --- @type ormtype[]
            local types = {}
            local limit = "LIMIT 1"
            local query = "SELECT * FROM " .. source

            -- first, recognize whether we want to return single entity or all entities
            if token:match("^findAll.*") then
                single = false
                limit = ""
                token = token:gsub("^findAll", "")
            else
                token = token:gsub("^find", "")
            end

            -- then check for any WHERE filters, assume they are all AND
            if token:match("^By") then
                local attempts = 0
                -- try for 10x to match any of possible fields defined in ormentity as the
                -- starting substring of current token
                while attempts < 10 and #token > 2 do
                    for field, def in pairs(entity) do
                        -- isolate text after By... and convert first letter to lower case
                        local sub = token:sub(3, 3 + #field)
                        sub = sub:sub(1, 1):lower() .. sub:sub(2)
                        -- if we found a match, insert it to list or arguments
                        if sub == field then
                            table.insert(args, def.field .. " = '" .. def.type.format() .. "'")
                            table.insert(types, def.type)
                            token = token:sub(3 + #field + 1)
                            break
                        end
                    end
                    attempts = attempts + 1
                end
                query = query .. " WHERE " .. table.concat(args, " AND ")
            end
            if #limit > 0 then
                query = query .. " " .. limit
            end

            -- finally create the method for repository
            repo[method] = function(self, ...)
                local treated = {}
                local on_resolved, resolver = aio:prepare_promise()
                for i, v in pairs({...}) do
                    table.insert(treated, types[i].toformat(v))
                end
                sql:select(query, unpack(treated))(function (rows, errorOrColumns)
                    -- this function returns either (nil, str_error)
                    -- or (nil, nil) for single result that has not been found
                    -- or (obj) for single result that has been found
                    -- or (obj[]) for multi-result
                    if not rows then
                        on_resolved(nil, errorOrColumns)
                    else
                        local results = {}
                        for i, row in ipairs(rows) do
                            local obj = {}
                            for key, value in pairs(row) do
                                local field = decoders[key]
                                if not field then
                                    obj[key] = value
                                else
                                    obj[field.dest] = field.decode(value)
                                end
                            end
                            table.insert(results, obj)
                            if single then
                                break
                            end
                        end
                        if single then
                            local result = nil
                            if #results == 1 then result = results[1] end
                            on_resolved(result, nil)
                        else
                            on_resolved(results)
                        end
                    end
                end)
                return resolver
            end
        end
    end

    return repo
end

function aio:on_init()
    local sql = mysql:new()
    
    sql:connect("80s", "password", "db80")(function (...)
        local repo = orm:create(sql, {
            source = "posts",
            --- @type ormentity
            entity = {
                id = { field = "id", type = ormtypes.int },
                author = { field = "author", type = ormtypes.str },
                text = { field = "text", type = ormtypes.str },
            },
            findById = true,
            findAll = true
        })
        repo:findById(1)(function (result, err)
            if result then
                print(result.author .. ": " .. result.text)
            end
        end)
    end)
end

return orm