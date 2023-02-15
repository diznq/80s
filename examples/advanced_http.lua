---@type aio
local aio = loadfile("aio/aio.lua")()

aio:start()

local function create_endpoint(endpoint,  mime,content, dynamic)
    aio:http_get(endpoint, function (self, query, headers, body)
        if dynamic then
            query = aio:parse_query(query)
            local dyncontent = content:gsub("%{(.-)%}", function(match)
                return query[match] or ("query." .. match)
            end)
            self:http_response("200 OK", mime, dyncontent)
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