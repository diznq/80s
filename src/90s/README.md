# 90s (modern 80s)

90s is an additional layer on top of the 80s project, that wraps 80s primitives into C++ classes and also provides a templating syntax for rendering server side content.

## Abstract

90s is made of three main components: web server, template compiler, and webpages.

Web server provides basic HTTP serving functionality and also automatically locates & loads webpages.

Template compiler takes `.html` files found in `modern/httpd/pages/` and compiles them into C++ source codes.

Webpages are compiler versions (`.so` on Linux, `.dll` on Windows) of those C++ source codes and are dynamically loaded when server starts. They can also be live-reloaded when reload is requested via 80s API.

## Compiling

To compile the web server and template compiler, simply run `./90s.sh`

To compile web pages, run `./90s.sh pages`

## Running

To run, same syntax as for `80s` web server can be used: `bin/90s -c 4 -p 8080` to run web server with 4 workers on port 8080.

Alternatively `WEB_ROOT` environment variable can be used to specify where to look for webpages, i.e. `WEB_ROOT=modern/httpd/pages/ bin/90s`.

## Template syntax

Template compiler supports syntax similar to that of PHP, that is source code is located between `<?cpp` and `?>`.

There are several syntax features:
- `<?include relative_path ?>` can be used to include other files, path is always relative to the file being compiled, i.e. `<?include header.html ?>`
- `<?hpp ... code ... ?>` can be used to declare anything that belongs to beginning of file, such as `#include`s etc.
- `<?cpp ... code ... ?>` can be used to include custom C++ code into the template, to write either use `env.output()->write()` or `| ...` syntax
- `| Formatted text #[[argument1]] #[[argument2]] ...` is a syntax sugar for `env.output()->write(...)` or `env.output()->write_formatted(...)`
- `#!` at the beginning files defines the endpoint path, i.e. `#! GET /time`

Inside `<?cpp ... ?>` blocks on code `ienvironment& env` is always available and can be used to declare output headers, content type as such, for all methods see `environment.hpp`.

When files are included, `#!` is stripped from within of them, so `#!` is always applies only to the currently parsed file

## Webpages (pagelets)

Webpages are dynamic linked libraries that are automatically loaded by the server on load and reloaded on refresh.

Webpages can also serve as initializer to create a global context reused by other webpages, in usual scenario only one webpage can do this, i.e. `main.so`.

### Default build strategy

Default build strategy is as follows:

1. Locate all `.html` files in `$WEB_ROOT` (defaults to `modern/httpd/pages/`)
2. Compile all `.html` files to `.html.cpp` and compile them into standalone dynamically linked libraries
3. Locate all `.cpp` files in `$WEB_ROOT` that aren't `.html.cpp`
4. Compile all `.cpp` files into single `main.so` (`main.dll` on Windows)

This allows to easily create simple separate pagelets but also create a main library composed of many C++ files.

### Creating a global context

To create a global context, webpage has to expose `extern "C" void* initialize();` and `extern "C" void release(void*)` procedures.

The context is later available when webpage is rendered using `env.context()` to retrieve `void *const` or `env.context<T>()` to retrieve `T *const` context.

Here is an example:

```cpp
#include <httpd/page.hpp>

struct my_context {
    sql_connection *sql;
};

class renderable : public page {
public:
    const char *name() const override {
        return "GET /test";
    }
    s90::aiopromise<s90::nil> render(s90::httpd::ienvironment& env) const override {
        // retrieve the context back from environment
        auto ctx = env.context<my_context>();
        auto result = ctx->sql->select(...);
    }
};

extern "C" {
    // provide pagelet initializer
    LIBRARY_EXPORT void* load_page() { return new renderable; }
    LIBRARY_EXPORT void unload_page(renderable *entity) { delete entity; }

    // provide context initializer
    LIBRARY_EXPORT my_context* initialize(my_context *previous_context) { return new my_context; }
    LIBRARY_EXPORT my_context* release(my_context* current_context) { delete current_context; }
}
```

Note that this could be also split into two separate modules, where one provides `load_page` & `unload_page` and the other proides `initialize` & `release`. In ideal case `initialize` & `release` should be provided by `main.so` (as described in previous section) and `load_page` & `unload_page` by webpage pagelets.

### Example template

```html
#! GET /time
<?hpp 
#include <ctime> 
?><!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Document</title>
</head>
<body>
    <?cpp
        auto name = env.query("name");
        if(name) {
            | <h1>Hi, #[[name.value()]]</h1>
        } else {
            | <h1>Hello!</h1>
        }
        std::time_t t = std::time(NULL);
        std::tm *tm = std::localtime(&t);
        const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

        | <h2>Today is: #[[ days[tm->tm_wday] ]]</h2>
    ?>
    <br>
</body>
</html>
```

The compiled C++ source code would look like this:

```cpp
// autogenerated by template_compiler
#include <httpd/page.hpp>
#include <ctime> 

class renderable : public s90::httpd::page {
public:
    const char *name() const override {
        return "GET /time";
    }
    s90::aiopromise<s90::nil> render(s90::httpd::ienvironment& env) const override {
        env.content_type("text/html");
		env.output()->write("<!DOCTYPE html>\n"
		"<html lang=\"en\">\n"
		"<head>\n"
		"    <meta charset=\"UTF-8\">\n"
		"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
		"    <title>Document</title>\n"
		"</head>\n"
		"<body>\n"
		"    ");

        auto name = env.query("name");
        if(name) {
            		env.output()->write_formatted(" <h1>Hi, {}</h1>", name.value());
        } else {
            		env.output()->write(" <h1>Hello!</h1>");
        }
        std::time_t t = std::time(NULL);
        std::tm *tm = std::localtime(&t);
        const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

        		env.output()->write_formatted(" <h2>Today is: {}</h2>",  days[tm->tm_wday] );
    		env.output()->write("\n"
		"    <br>\n"
		"</body>\n"
		"</html>");

        co_return s90::nil {};
    }
};

#ifndef PAGE_INCLUDE
extern "C" LIBRARY_EXPORT void* load_page() { return new renderable; }
extern "C" LIBRARY_EXPORT void unload_page(renderable *entity) { delete entity; }
#endif
```