require("aio.aio")

--- @class templates
local templates = {}

--- @class templateinput
--- @field session table
--- @field headers table
--- @field locals table
--- @field body string
--- @field endpoint string
--- @field query table

--- @class templateoutput
--- @field headers table
--- @field status string

--- @class templatectx
--- @field content string
--- @field parts fun(input: templateinput, output: templateoutput, done: aiothen)[]
--- @field parallel boolean

--- @class render_result
--- @field headers {[string]: string} headers table
--- @field status string response status
--- @field content string rendered template

--- HTML escape text
---@param text any text
---@return any text escaped text
---@return integer matches
local function escape(text)
    if type(text) == "string" then
        return text:gsub("%&", "&amp;"):gsub("%\"", "&quot;"):gsub("%<", "&lt;"):gsub("%>", "&gt;")
    else
        return text, 0
    end
end

local function await(callback)
    return aio:await(callback)
end

---comment
---@param content string template file
---@param base string directory that current file is located in
---@return templatectx
function templates:prepare(content, base)
    local context = { content = "", parts = {}, parallel = false }
    local depth, max_depth, matches = 0, 32, 1

    while depth < max_depth and matches > 0 do
        content = content:gsub("^#!%s*parallel%s-\n", function ()
            context.parallel = true;
            return "";
        end)

        content, matches = content:gsub("<%?%s*include%s+%\"?(.-)\"?%s*%?>", function(path)
            if not path:match("^/") then
                path = base .. path
            end
            local f, err = io.open(path, "r")
            if not f or err then
                return "failed to include " .. path
            end
            local content = f:read("*all")
            f:close()
            return content
        end)

        depth = depth + 1
    end

    context.content = content:gsub("<%?lu(a?)(.-)%?>", function (async, match)
        local compiled, err = load("return function(session, locals, headers, body, endpoint, query, write, escape, await, header, status, done)" .. match .. "end")
        if not compiled then
            table.insert(context.parts, function (input, output, done)
                done(err)
            end)
            return "<?l" .. tostring(#context.parts) .. "?>"
        end
        local code = compiled()
        table.insert(context.parts, 
        function(input, output, done)
            aio:async(function()
                local data = ""
                code(
                    input.session,
                    input.locals,
                    input.headers,
                    input.body,
                    input.endpoint, 
                    input.query,
                    function(text, ...)
                        local params = {...}
                        if #params > 0 then
                            for i, v in ipairs(params) do
                                params[i] = escape(v)
                            end
                            data = data .. string.format(text, unpack(params))
                        else
                            data = data .. text
                        end
                    end, 
                    escape,
                    await,
                    function(name, value) output.headers[name] = value end,
                    function(value) output.status = value end,
                    function() done(data) end
                )
                if #async == 0 then
                    done(data)
                end
            end)
        end)
        return "<?l" .. tostring(#context.parts) .. "?>"
    end)

    return context
end

--- Render a dynamic template file
--- Syntax for dynamic content is:
--- <?lu ... ?> for synchronous functions that don't have to use done() at the end
--- <?lua ... ?> for asynchronous functions that must signalize they are done by making done() call at end
--- Further description can be found in README.md
---
---@param session {[string]: string} user session
---@param headers {[string]: string} headers table
---@param endpoint string request URL
---@param query {[string]: string} query parameters
---@param mime string expected mime type
---@param ctx templatectx prepared template context
---@return fun(on_resolved: fun(result: render_result)) promise
function templates:render(session, headers, body, endpoint, query, mime, ctx)
    local on_resolved, resolve_event = aio:prepare_promise()

    -- if there is no dynamic content, resolve immediately
    if #ctx.parts == 0 then
        on_resolved(ctx.content)
        return resolve_event
    end

    local output = {
        headers = {
            ["Content-type"] = mime
        },
        status = "200 OK"
    }

    local input = {
        session = session,
        headers = headers,
        locals = {},
        body = body,
        endpoint = endpoint,
        query = query
    }

    local mapper = function(fn)
        return function(...)
            return fn(input, output, ...)
        end
    end

    if ctx.parallel then
        aio:gather(unpack(aio:map(ctx.parts, mapper)))(function (...)
            local responses = {...}
            local response = ctx.content:gsub("<%?l([0-9]+)%?>", function(match)
                return tostring(responses[tonumber(match, 10)])
            end)
            on_resolved({
                status=output.status,
                headers=output.headers,
                content=response
            })
        end)
    else
        local responses = {}
        aio:chain(mapper(ctx.parts[1]), unpack(aio:map(ctx.parts, function(fn)
            return function(res)
                table.insert(responses, res)
                return mapper(fn)
            end
        end), 2))(function (res)
            table.insert(responses, res)
            local response = ctx.content:gsub("<%?l([0-9]+)%?>", function(match)
                return tostring(responses[tonumber(match, 10)])
            end)
            on_resolved({
                status=output.status,
                headers=output.headers,
                content=response
            })
        end)
    end
    return resolve_event
end

return templates