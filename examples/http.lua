---@type aio
local aio = loadfile("aio/aio.lua")()

aio:start()

--- Process a dynamic output template
--- Syntax for dynamic content is:
--- <?lu ... ?> for synchronous functions that don't have to use done() at the end
--- <?lua ... ?> for asynchronous functions that must signalize they are done by making done() call at end
--- Further description can be found in README.md
---
---@param res aiosocket socket handle
---@param headers {[string]: string} headers table
---@param endpoint string request URL
---@param query {[string]: string} query parameters
---@param mime string expected mime type
---@param content string original file content
local function process_dynamic(res, headers, body, endpoint, query, mime, content)
    local parts = {}
    local session = {}
    local writeHeaders = {["Content-type"] = mime}
    local status = "200 OK"
    local new = content:gsub("<%?lu(a?)(.-)%?>", function (async, match)
        local code = load("return function(session, headers, body, endpoint, query, write, header, status, done)" .. match .. "end")()
        local data = ""
        table.insert(parts, 
        function(done)
            code(
                session,
                headers,
                body,
                endpoint, 
                query, 
                function(text, unsafe)
                    if not unsafe then
                        text = text:gsub("%&", "&amp;"):gsub("%\"", "&quot;"):gsub("%<", "&lt;"):gsub("%>", "&gt;") 
                    end
                    data = data .. text 
                end, 
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
        return "<?l" .. tostring(#parts) .. "?>"
    end)
    --print(new)
    aio:gather(unpack(parts))(function (...)
        local responses = {...}
        local response = new:gsub("<%?l([0-9]+)%?>", function(match)
            return tostring(responses[tonumber(match, 10)])
        end)
        res:http_response(status, writeHeaders, response)
    end)
end

local function create_endpoint(endpoint, mime, content, dynamic)
    aio:http_get(endpoint, function (self, query, headers, body)
        if dynamic then
            query = aio:parse_query(query)
            process_dynamic(self, headers, body, endpoint, query, mime, content)
        else
            self:http_response("200 OK", mime, content)
        end
    end)
end

local function init_dir(base, prefix)
    prefix = prefix or "/"
    for _, file in pairs(net.listdir(base)) do
        if file:match("/$") then
            init_dir(base .. file, prefix .. file)
        else
            local f, err = io.open(base .. file, "r")
            if f then
                local content = f:read("*all")
                f:close()

                local mime = "text/html; charset=utf-8"
                local dynamic = false

                --- if file contains .dyn., it will be ran through template engine in later stage
                --- see process_dynamic for example
                if file:match("%.dyn.*$") then
                    dynamic = true
                end
                if file:match("%.css$") then
                    mime = "text/css"
                elseif file:match("%.jpg$") then
                    mime = "image/jpeg"
                elseif file:match("%.js$") then
                    mime = "application/javascript"
                end

                -- omit .html from files so we get nice URLs
                local endpoint = file:gsub("%.dyn", ""):gsub("%.html", "")

                -- /index.html can be simplified to /
                if endpoint == "index" then
                    endpoint = ""
                end

                create_endpoint(prefix .. endpoint, mime, content, dynamic)
            end
        end
    end
end

init_dir("examples/public_html/")

aio:http_post("/reload", function (self, query, headers, body)
    net.reload()
    self:http_response("200 OK", "text/plain", tostring(WORKERID))
end)