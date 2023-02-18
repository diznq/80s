require("aio.aio")
local mysql = require("server.mysql")
local templates = require("server.templates")
local http_client = require("server.http_client")

--- @class mysql
SQL = nil
HTTP = http_client

local function create_endpoint(method, endpoint, mime, content, dynamic)
    local ctx = nil
    if dynamic then
        ctx = templates:prepare(content, mime)
    end
    aio:http_any(method, endpoint, function (self, query, headers, body)
        --for i, v in pairs(headers) do print(i, v) end
        if dynamic then
            -- process as dynamic template
            local session = {}
            query = aio:parse_query(query)
            templates:render(session, headers, body, endpoint, query, mime, ctx)(function (result)
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

                local mime = "text/plain; charset=utf-8"
                local dynamic = false
                local method = "GET"

                --- if file contains .dyn., it will be ran through template engine in later stage
                --- see process_dynamic for example
                if file:match("%.dyn%..*$") then
                    dynamic = true
                end
                if file:match("^post%..*$") then
                    method = "POST"
                end
                if file:match("^put%..*$") then
                    method = "PUT"
                end
                if file:match("^delete%..*$") then
                    method = "DELETE"
                end
                
                if file:match("%.css$") then
                    mime = "text/css"
                elseif file:match("%.html?$") then
                    mime = "text/html"
                elseif file:match("%.jpg$") then
                    mime = "image/jpeg"
                elseif file:match("%.js$") then
                    mime = "application/javascript; charset=utf-8"
                elseif file:match("%.json$") then
                    mime = "application/json; charset=utf-8"
                end

                -- omit .html from files so we get nice URLs
                local endpoint = file
                    :gsub("%.dyn", "")
                    :gsub("^post%.", ""):gsub("^put%.", ""):gsub("^delete%.", "")
                    :gsub("%.html", "")

                -- /index.html can be simplified to /
                if endpoint == "index" then
                    endpoint = ""
                end

                create_endpoint(method, prefix .. endpoint, mime, content, dynamic)
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

aio:http_post("/gc", function (self, query, headers, body)
    local before = collectgarbage("count")
    local col = collectgarbage("collect")
    self:http_response("200 OK", "text/plain", tostring(before - collectgarbage("count")))
end)

aio:http_get("/fds", function (self, query, headers, body)
    local n = 0
    for i, v in pairs(aio.fds) do
        n = n + 1
    end
    self:http_response("200 OK", "text/plain", tostring(n) .. ", " .. aio.cors)
end)