require("aio.aio")
local mysql = require("server.mysql")
local templates = require("server.templates")
local http_client = require("server.http_client")

HTTP = http_client

local function create_endpoint(base, method, endpoint, mime, content, dynamic)
    local ctx = nil
    --- @type table|nil
    local resp_headers = nil
    if dynamic then
        ctx = templates:prepare(content, base)
    else
        resp_headers = {}
        resp_headers["Content-type"] = mime
        if endpoint:match("/static/") then
            resp_headers["Cache-Control"] = "public, max-age=604800, immutable"
        end
        if endpoint:match("%.gz$") then
            endpoint = endpoint:gsub("%.gz$", "")
            resp_headers["Content-encoding"] = "gzip"
        end
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
            self:http_response("200 OK", resp_headers or mime, content)
        end
    end)
end

local function init_dir(root, base, prefix)
    prefix = prefix or "/"
    for _, file in pairs(net.listdir(base)) do
        if file:match("/$") then
            -- treat files in public_html/ as in root /
            if prefix == "/" and file == "public_html/" then
                init_dir(root, base .. file, prefix)
            else
                init_dir(root, base .. file, prefix .. file)
            end
        elseif prefix == "/" and file == "main.lua" then
            -- Only main.lua in root folder will be loaded
            local main, err = loadfile(base .. file)
            if not main then
                print("http.init_dir: failed to load main.lua, error: " .. err)
            else
                main()
            end
        elseif not file:match("%.lua$") and base:sub(1, #root + 12) == root .. "public_html/" then
            -- only parse files that are at least in public_html and are not .lua
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
                -- if file is inside /static/ folder, it should never be rendered as dynamic file
                if base:match("/?static/") then
                    dynamic = false
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

                local file_sanitized = file:gsub("%.gz$", "")
                
                if file_sanitized:match("%.css$") then
                    mime = "text/css"
                elseif file_sanitized:match("%.html?$") then
                    mime = "text/html"
                elseif file_sanitized:match("%.jpg$") then
                    mime = "image/jpeg"
                elseif file_sanitized:match("%.js$") then
                    mime = "application/javascript; charset=utf-8"
                elseif file_sanitized:match("%.json$") then
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

                -- exclude private files
                if not file:match("%.priv%.") then
                    create_endpoint(base, method, prefix .. endpoint, mime, content, dynamic)
                end
            else
                print("http.init_dir: failed to load file " .. base .. file .. ", error: " .. err)
            end
        end
    end
end

local root = os.getenv("PUBLIC_HTML") or "server/www/"
init_dir(root, root)

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