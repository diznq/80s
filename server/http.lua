require("aio.aio")
local templates = require("server.templates")

MASTER_KEY = os.getenv("MASTER_KEY") or nil
NO_STATIC = (os.getenv("NO_STATIC") or "false") == "true"

aio:set_master_key(MASTER_KEY)

local function create_endpoint(base, method, endpoint, mime, content, dynamic)
    local ctx = nil
    --- @type table|nil
    local resp_headers = nil
    if dynamic then
        ctx = templates:prepare(content, base, method .. " " .. endpoint)
    else
        resp_headers = {}
        resp_headers["content-type"] = mime
        if endpoint:match("/static/") or endpoint:match("%.ico$") then
            resp_headers["cache-control"] = "public, max-age=604800, immutable"
        end
        if endpoint:match("%.gz$") then
            endpoint = endpoint:gsub("%.gz$", "")
            resp_headers["content-encoding"] = "gzip"
        end
    end
    aio:http_any(method, endpoint, function (self, query, headers, body)
        --for i, v in pairs(headers) do print(i, v) end
        if dynamic then
            -- process as dynamic template
            local session = {}
            local parsed_query = aio:parse_query(query, aio.master_key and endpoint or nil)
            if type(on_before_http) == "function" then
                pcall(on_before_http, method, endpoint, session)
            end
            templates:render(self, session, headers, body, method, endpoint, parsed_query, mime, ctx)(function (result)
                local code = result.status:sub(1, 3)
                -- in case of redirects, prevent any kind of output
                if code == "301" or code == "302" then
                    result.content = ""
                end
                if type(on_after_http) == "function" then
                    pcall(on_after_http, method, endpoint, session)
                end    
                self:http_response(result.status, result.headers, result.content)
            end)
        else
            self:http_response("200 OK", resp_headers or mime, content)
        end
    end)
end

--- Initialize directory
---@param root string root directory
---@param base string current base directory
---@param prefix string api endpoint prefix
---@return string[]
local function init_dir(root, base, prefix)
    prefix = prefix or "/"
    local dirs = {base}
    for _, file in pairs(net.listdir(base)) do
        if file:match("/$") then
            -- all folders that begin with i. will be ignored, i.e. they might contain large files
            local ignore = file:match("i%.") or file:match("^%.") or (NO_STATIC and file == "static/")
            if not ignore then
                -- treat files in public_html/ as in root /
                local found_dirs = {}
                if prefix == "/" and file == "public_html/" then
                    found_dirs = init_dir(root, base .. file, prefix)
                else
                    found_dirs = init_dir(root, base .. file, prefix .. file)
                end
                for _, dir in ipairs(found_dirs) do
                    table.insert(dirs, dir)
                end
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
            local content = aio:read_file_sync(base .. file, "r")
            if content then

                local mime = "text/plain; charset=utf-8"
                local dynamic = false
                local method = "GET"

                --- if file contains .dyn., it will be ran through template engine in later stage
                --- see process_dynamic for example
                if file:match("%.dyn%..*$") or (#content < 1000000 and (content:match("<%?lua?.+%?>") or content:match("<%?include.+%?>"))) then
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
                elseif file_sanitized:match("%.png$") or file_sanitized:match("%.ico$") then
                    mime = "image/png"
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
    return dirs
end

local root = os.getenv("PUBLIC_HTML") or "server/www/"
local dirs = init_dir(root, root, "/")

if (os.getenv("RELOAD") or "false") == "true" then
    LAST_LIVE_RELOAD = LAST_LIVE_RELOAD or 0
    if not LIVE_RELOAD_ACTIVE then
        for _, dir in ipairs(dirs) do
            print("Watching ", dir)
        end
        LIVE_RELOAD_ACTIVE = aio:watch(ELFD, dirs, function (events)
            if #events > 0 then
                local now = events[1].clock
                if now - LAST_LIVE_RELOAD > 0.2 then
                    print("Reloading server, time delta: ", now - LAST_LIVE_RELOAD)
                    aio:reload()
                    LAST_LIVE_RELOAD = now
                end
                for _, evt in ipairs(events) do
                    if evt.create and evt.dir and LIVE_RELOAD_ACTIVE ~= nil then
                        net.inotify_add(ELFD, LIVE_RELOAD_ACTIVE.fd, evt.name)
                        print("Added ", evt.name, " to watch list")
                    end
                end
            end
        end)
    end
end