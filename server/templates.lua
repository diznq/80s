require("aio.aio")

--- @class templates
local templates = {}

--- @class render_result
--- @field headers {[string]: string} headers table
--- @field status string response status
--- @field content string rendered template

--- HTML escape text
---@param text string text
---@return string text escaped text
---@return integer matches
local function escape(text)
    return text:gsub("%&", "&amp;"):gsub("%\"", "&quot;"):gsub("%<", "&lt;"):gsub("%>", "&gt;")
end

local function await(callback)
    return aio:await(callback)
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
---@param content string original file content
---@return fun(on_resolved: fun(result: render_result)) promise
function templates:render(session, headers, body, endpoint, query, mime, content)
    local parts = {}
    local writeHeaders = {["Content-type"] = mime}
    local status = "200 OK"
    local on_resolved, resolve_event = aio:prepare_promise()

    local new = content:gsub("<%?lu(a?)(.-)%?>", function (async, match)
        local code = load("return function(session, headers, body, endpoint, query, write, escape, await, header, status, done)" .. match .. "end")()
        local data = ""
        table.insert(parts, 
        function(done)
            aio:async(function()
                code(
                    session,
                    headers,
                    body,
                    endpoint, 
                    query,
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
                    function(name, value)
                        writeHeaders[name] = value
                    end,
                    function(value)
                        status = value
                    end,
                    function ()
                        done(data)
                    end)
                if #async == 0 then
                    done(data)
                end
            end)
        end)
        return "<?l" .. tostring(#parts) .. "?>"
    end)
    aio:gather(unpack(parts))(function (...)
        local responses = {...}
        local response = new:gsub("<%?l([0-9]+)%?>", function(match)
            return tostring(responses[tonumber(match, 10)])
        end)
        on_resolved({
            status=status,
            headers=writeHeaders,
            content=response
        })
    end)
    return resolve_event
end

return templates