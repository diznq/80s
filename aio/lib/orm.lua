local mysql = require("aio.lib.mysql")

--- Convert to integer
---@param x string|number|nil
---@return integer|nil
local function tointeger(x)
    local num = tonumber(x)
    if num ~= nil then return math.floor(num) end
end

--- @alias ormentity {[string]: ormfield}

--- @class ormfield
--- @field field string|nil
--- @field type ormtype

--- @alias ormerror {error: string}
--- @alias ormcount fun(...): fun(on_resolved: fun(result: integer|nil|ormerror))
--- @alias ormone fun(...): fun(on_resolved: fun(result: table|nil|ormerror))
--- @alias ormall fun(...): fun(on_resolved: fun(result: table[]|ormerror))
--- @alias orminsert fun(...): fun(on_resolved: fun(result: mysqlerror|mysqlok|mysqleof))
--- @alias ormdelete fun(...): fun(on_resolved: fun(result: mysqlerror|mysqlok|mysqleof))
--- @alias ormrepo {all: {[string]: ormall}, one: {[string]: ormone}, deleteOne: {[string]: ormdelete}, deleteAll: {[string]: ormdelete}, count: {[string]: ormcount}, entity: ormentity, source: string, insert: orminsert, replace: orminsert, update: ormupdate, sql: mysql}
--- @alias ormupdate fun(self, entity: {[string]: any}, update: {[string]: any}): fun(on_resolved: fun(result: mysqlerror|mysqlok|mysqleof))
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
    text = {
        format = function() return "%s" end,
        fromstring = function(text) return text end,
        toformat = function(value) return tostring(value) end
    },
    int = {
        format = function() return "%d" end,
        fromstring = function(text) return tointeger(text) end,
        toformat = function(value) return tointeger(value) end
    },
    double = {
        format = function() return "%f" end,
        fromstring = function(text) return tonumber(text) end,
        toformat = function(value) return tonumber(value) end
    },
    bit = {
        format = function() return "%c" end,
        fromstring = function(text) return text:byte(1, 1) ~= 0 end,
        toformat = function(value) return value and 1 or 0 end
    },
    ---@diagnostic disable-next-line: assign-type-mismatch
    varchar = function(size)
        return {
            format = function() return "%s" end,
            fromstring = function(text) return text end,
            toformat = function(text)
                if #text > size then return text:sub(1, size) end
                return text
            end
        }
    end,
    datetime = {
        format = function() return "%s" end,
        fromstring = function(text)
            local Y,M,D,H,I,s = text:match("^(%d-)%-(%d-)%-(%d-) (%d-):(%d-):(%d+)")
            if Y and M and D and H and I and s then
                return {
                    year = tonumber(Y),
                    month = tonumber(M),
                    day = tonumber(D),
                    hour = tonumber(H),
                    min = tonumber(I),
                    sec = tonumber(s)
                }
            end
            return nil
        end,
        toformat = function(value)
            if type(value) == "string" then
                return value
            end
            if type(value) == "number" then
                value = os.date("*t", value)
            end
            if type(value) ~= "table" then
                value = os.date("*t")
            end
            return string.format("%04d-%02d-%02d %02d:%02d:%02d.%03d", value.year, value.month, value.day, value.hour, value.min, value.sec, 0)
        end
    }
}


local orm = {
    t = ormtypes
}

--- Create repository
---@param sql mysql
---@param repo {source: string, index: string[]|string, entity:ormentity, [string]: any}
---@return ormrepo
function orm:create(sql, repo)
    --- @type string
    local source = repo.source
    --- @type ormentity
    local entity = repo.entity
    --- @type string[]
    local index = repo.index or "id"
    local insert_fields = {}
    local insert_order = {}

    if type(index) == "string" then
        index = {index}
    end

    -- construct lookup table for entity decoders
    --- @type {[string]: {dest: string, decode: fun(string): any}}
    local decoders = {}
    for field, def in pairs(entity) do
        if type(def) == "table" then
            def.field = def.field or field
            decoders[def.field] = {dest = field, decode = def.type.fromstring}
            table.insert(insert_fields, def.field)
            table.insert(insert_order, field)
        end
    end

    local insert_base_query = string.format(" INTO %s (%s) VALUES ", source, table.concat(insert_fields, ","))
    local update_base_query = string.format("UPDATE %s SET %%s WHERE %%s LIMIT 1",source)
    
    repo.sql = sql
    repo.one = {}
    repo.all = {}
    repo.deleteOne = {}
    repo.deleteAll = {}
    repo.count = {}

    local inserter = function(self, kw, ...)
        local tuples = {}
        local params = {}
        for i, item in ipairs({...}) do
            local values = {}
            for j, key in ipairs(insert_order) do
                local value = item[key]
                local def = entity[key]
                if def == nil then
                    print("[orm] orm.repo(" .. source .. ").save: missing entity definition for " .. key .. ", using NULL for insert")
                    value = "NULL"
                elseif value == nil then
                    value = "NULL"
                else
                    table.insert(params, def.type.toformat(value))
                    value = "'" .. def.type:format() .. "'"
                end
                table.insert(values, value)
            end
            table.insert(tuples, "(" .. table.concat(values, ",") .. ")" )
        end
        if #tuples == 0 then
            local resolve, resolver = aio:prepare_promise()
            resolve({ok = true, affected_rows = 0})
            return resolver
        end
        local final_query = kw .. insert_base_query .. " " ..  table.concat(tuples, ",")
        return sql:exec(final_query, unpack(params))
    end

    repo.sql = sql

    repo.insert = function (self, ...)
        return inserter(self, "INSERT", ...)
    end

    repo.replace = function(self, ...)
        return inserter(self, "REPLACE", ...)
    end

    repo.update = function(self, object, update)
        local sets = {}
        local where = {}
        local params = {}
        for k, v in pairs(update) do
            table.insert(sets, entity[k].field .."='" .. entity[k].type.format() .. "'")
            table.insert(params, entity[k].type.toformat(v))
        end
        for _, key in ipairs(index) do
            table.insert(where, entity[key].field .. "='".. entity[key].type.format() .."'")
            table.insert(params, entity[key].type.toformat(object[key]))
        end
        local final_query = string.format(update_base_query, table.concat(sets, ","), table.concat(where, " AND "))
        return sql:exec(final_query, unpack(params))
    end

    for method, params in pairs(repo) do
        if method:match("^find.*") or method:match("^by.*") then
            local token = method
            local args = {}
            --- @type ormtype[]
            local types = {}
            local query = "SELECT * FROM " .. source

            -- first, recognize whether we want to return single entity or all entities
            token = token:gsub("^find", "")

            -- then check for any WHERE filters, assume they are all AND
            if type(params) ~= "string" and token:match("^By") then
                local attempts = 0
                -- try for 10x to match any of possible fields defined in ormentity as the
                -- starting substring of current token
                token = token:sub(3)
                local it = token == "IpPort"
                while attempts < 10 and #token > 0 do
                    for field, def in pairs(entity) do
                        if type(def) == "table" then
                            -- isolate text after By... and convert first letter to lower case
                            local sub = token:sub(1, #field)
                            sub = sub:sub(1, 1):lower() .. sub:sub(2)
                            -- if we found a match, insert it to list or arguments
                            if sub == field then
                                table.insert(args, def.field .. " = '" .. def.type.format() .. "'")
                                table.insert(types, def.type)
                                token = token:sub(1 + #field)
                                break
                            end
                        end
                    end
                    attempts = attempts + 1
                end
                query = query
                if #args > 0 then
                    query = query .. " WHERE " .. table.concat(args, " AND ")
                end
            elseif type(params) == "string" then
                if params:lower():match("^select") then
                    query = params
                else
                    query = query .. " " .. params
                end
            end

            -- finally create the method for repository
            method = method:gsub("findB", "b")
            repo.all[method] = self:create_method(sql, query, types, decoders, false, false, repo.all)
            repo.one[method] = self:create_method(sql, query, types, decoders, true, false, repo.one)
            repo.deleteAll[method] = self:create_method(sql, query:gsub("^SELECT %*", "DELETE"), types, decoders, false, false, repo.deleteAll, true)
            repo.deleteOne[method] = self:create_method(sql, query:gsub("^SELECT %*", "DELETE"), types, decoders, true, false, repo.deleteOne, true)
            repo.count[method] = self:create_method(sql, query:gsub("^SELECT %*", "SELECT COUNT(*) AS c"), types, decoders, true, true, repo.count)
        end
    end

    ---@diagnostic disable-next-line: return-type-mismatch
    return repo
end

---Create new ORM method for repository
---@param sql mysql
---@param query string
---@param types ormtype[]
---@param decoders {[string]: {dest: string, decode: fun(string): any}}
---@param single boolean
---@param count boolean
---@param parent table
---@param delete boolean|nil
---@return ormone|ormall|ormcount
function orm:create_method(sql, query, types, decoders, single, count, parent, delete)
    return function(...)
        local treated = {}
        local extras = {}
        local real_i = 1
        local has_limit = false
        local on_resolved, resolver = aio:prepare_promise()
        for i, v in pairs({...}) do
            -- do this so if it is called as :method, it still works
            local special = false
            if type(v) == "table" and v.orderBy then
                table.insert(extras, "ORDER BY " .. v.orderBy)
                special = true
            end
            if type(v) == "table" and v.limit then
                table.insert(extras, "LIMIT " .. v.limit)
                has_limit = true
                special = true
            end
            if not special then
                if i ~= 1 or v ~= parent then
                    local type_def = types[real_i]
                    if type_def ~= nil then
                        local result = type_def.toformat(v)
                        if result == nil then
                            on_resolved(make_error("invalid value: " .. tostring(v)))
                            return resolver
                        end
                        table.insert(treated, result)
                        real_i = real_i + 1
                    else
                        if v == nil then
                            on_resolved(make_error("nil passed to select as argument"))
                            return resolver
                        end
                        table.insert(treated, v)
                        real_i = real_i + 1
                    end
                end
            end
        end
        if single and not has_limit then
            table.insert(extras, "LIMIT 1")
        end
        local final_query = query .. " " .. table.concat(extras, " ")
        if delete == true then
            sql:exec(final_query, unpack(treated))(function (result)
                on_resolved(result)
            end)
            return resolver
        end
        sql:select(final_query, unpack(treated))(function (rows, errorOrColumns)
            -- this function returns either (nil, str_error)
            -- or (nil, nil) for single result that has not been found
            -- or (obj) for single result that has been found
            -- or (obj[]) for multi-result
            if not rows then
                ---@diagnostic disable-next-line: need-check-nil, param-type-mismatch
                on_resolved(make_error(errorOrColumns["error"] or "select error"))
            else
                local results = {}
                for _, row in ipairs(rows) do
                    -- in case of count, directly create the object only with count
                    if count then
                        table.insert(results, {c = tonumber(row.c)})
                        break
                    else
                        -- otherwise remap from raw field names to nice field names defined by entity
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
                end
                if single then
                    local result = nil
                    if #results == 1 then result = results[1] end
                    ---@diagnostic disable-next-line: need-check-nil
                    if count then result = result["c"] end
                    on_resolved(result)
                else
                    on_resolved(results)
                end
            end
        end)
        return resolver
    end
end

return orm