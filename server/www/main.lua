local mysql = require("server.mysql")
local orm = require("server.orm")

SQL = mysql:new()
--- @type ormrepo
Posts = orm:create(SQL, {
    source = "posts",
    --- @type ormentity
    entity = {
        id = { field = "id", type = orm.t.int },
        author = { field = "author", type = orm.t.text },
        text = { field = "text", type = orm.t.text },
    },
    findById = true,
    findBy = true
})

local user, password, db = os.getenv("DB_USER") or "80s", os.getenv("DB_PASSWORD") or "password", os.getenv("DB_NAME") or "db80"
SQL:connect(user, password, db)(function (ok, err)
    if not ok then
        print("Failed to connect to SQL: ", err)
    end
end)
