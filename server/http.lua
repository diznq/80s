require("aio.aio")
local mysql = require("server.mysql")
local templates = require("server.templates")

--- @class mysql
SQL = nil

local function create_endpoint(endpoint, mime, content, dynamic)
    aio:http_get(endpoint, function (self, query, headers, body)
        if dynamic then
            -- process as dynamic template
            local session = {}
            query = aio:parse_query(query)
            templates:render(session, headers, body, endpoint, query, mime, content)(function (result)
                self:http_response(result.status, result.headers, result.content)
            end)
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

init_dir("server/public_html/")

function aio:on_init()
    SQL = mysql:new()
    SQL:connect("80s", "password", "db80")(function (ok, err)
        if not ok then
            print("Failed to connect to SQL: ", err)
        end
    end)
end

aio:http_post("/reload", function (self, query, headers, body)
    net.reload()
    self:http_response("200 OK", "text/plain", tostring(WORKERID))
end)