---@type aio
local aio = loadfile("aio/aio.lua")()

aio:start()

--- Process a dynamic output template
--- Syntax for dynamic content is:
--- <?lu ... ?> for synchronous functions that don't have to use done() at the end
--- <?lua ... ?> for asynchronous functions that must signalize they are done by making done() call at end
--- During function executon, following variables and functions are available within context:
--- - endpoint: request URL
--- - query: key, value table of query parameters
--- - mime: request mime type
--- - write(text, unsafe?): write text to response, if unsafe is true, no HTML escaping is performed
--- - done(): called to signalize dynamic content is complete
---
---@param res aiosocket
---@param endpoint string
---@param query {[string]: string}
---@param mime string
---@param content string
local function process_dynamic(res, endpoint, query, mime, content)
    local parts = {}
    local new = content:gsub("<%?lu(a?)(.-)%?>", function (async, match)
        local code = load("return function(endpoint, query, write, done)" .. match .. "end")()
        local data = ""
        table.insert(parts, 
        function(done)
            code(endpoint, query, function(text, unsafe)
                if not unsafe then
                    text = text:gsub("%&", "&amp;"):gsub("%\"", "&quot;"):gsub("%<", "&lt;"):gsub("%>", "&gt;") 
                end
                data = data .. text 
            end, function ()
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
        res:http_response("200 OK", mime, response)
    end)
end

local function create_endpoint(endpoint, mime, content, dynamic)
    aio:http_get(endpoint, function (self, query, headers, body)
        if dynamic then
            query = aio:parse_query(query)
            process_dynamic(self, endpoint, query, mime, content)
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

                local mime = "text/html"
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