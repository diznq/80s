#include "../page.hpp"
#include <ctime> 

class time_renderable : public s90::httpd::page {
public:
    s90::aiopromise<s90::nil> render(s90::httpd::ienvironment& env) const override {
		env.output()->write("<!DOCTYPE html>\n"
		"<html lang=\"en\">\n"
		"<head>\n"
		"    <meta charset=\"UTF-8\">\n"
		"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
		"    <title>Document</title>\n"
		"</head>\n"
		"<body>\n"
		"    <h1>Hello!</h1>\n"
		"    ");
		auto block_output_content_0 = env.output()->append_context();

        std::time_t t = std::time(NULL);
        std::tm *tm = std::localtime(&t);
        const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

        		block_output_content_0->write_formatted(" <h2>Today is: {}</h2>",  days[tm->tm_wday] );
    		env.output()->write("\n"
		"    <br>\n"
		"</body>\n"
		"</html>");

        co_return s90::nil {};
    }
};

#ifndef PAGE_INCLUDE
extern "C" LIBRARY_EXPORT void* load_page() { return new time_renderable; }
#endif