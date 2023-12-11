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

There are 4 syntax features in total:
- `<?hpp ... code ... ?>` can be used to declare anything that belongs to beginning of file, such as `#include`s etc.
- `<?cpp ... code ... ?>` can be used to include custom C++ code into the template, to write either use `env.output()->write()` or `| ...` syntax
- `| Formatted text #[[argument1]] #[[argument2]] ...` is a syntax sugar for `env.output()->write(...)` or `env.output()->write_formatted(...)`
- `#!` at the beginning files defines the endpoint path, i.e. `#! GET /time`

Inside `<?cpp ... ?>` blocks on code `ienvironment& env` is always available and can be used to declare output headers, content type as such, for all methods see `environment.hpp`.

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
    <h1>Hello!</h1>
    <?cpp
        std::time_t t = std::time(NULL);
        std::tm *tm = std::localtime(&t);
        const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

        | <h2>Today is: #[[ days[tm->tm_wday] ]]</h2>
    ?>
    <br>
</body>
</html>
```