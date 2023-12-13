#include "util.hpp"

namespace s90 {
    namespace util {
        std::string url_decode(std::string_view data) {
            char lut[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0, 
                // A   B   C   D   E   F  G  H  I  J  K  L  M  N  O  P  Q  R  S  T  U  V  W  X  Y  Z
                  10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                // [  \  ]  ^  _  `   a   b   c   d   e   f
                   0, 0, 0, 0, 0, 0, 10, 11, 12, 13, 14, 15};
            std::string output;
            output.reserve(data.length());
            size_t len = data.length();
            size_t i = 0;
            char c;
            for (i = 0; i < len; i++) {
                c = data[i] & 255;
                if (i <= len - 3) {
                    switch (c) {
                    case '+':
                        output += ' ';
                        break;
                    case '%': {
                        if (((data[i + 1] >= '0' && data[i + 1] <= '9') || (data[i + 1] >= 'A' && data[i + 1] <= 'F') || (data[i + 1] >= 'a' && data[i + 1] <= 'f')) && ((data[i + 2] >= '0' && data[i + 2] <= '9') || (data[i + 2] >= 'A' && data[i + 2] <= 'F') || (data[i + 2] >= 'a' && data[i + 2] <= 'f'))) {
                            output += (lut[data[i + 1] - '0'] << 4) | (lut[data[i + 2] - '0']);
                            i += 2;
                        } else {
                            output += c;
                        }
                        break;
                    }
                    default:
                        output += c;
                    }
                } else {
                    output += c == '+' ? ' ' : c;
                }
            }
            return output;
        }

        std::map<std::string, std::string> parse_query_string(std::string_view query_string) {
            size_t prev_pos = 0, pos = -1;
            std::map<std::string, std::string> qs;
            while(prev_pos < query_string.length()) {
                pos = query_string.find('&', prev_pos);
                std::string_view current;
                if(pos == std::string::npos) {
                    current = query_string.substr(prev_pos);
                    prev_pos = query_string.length();
                } else {
                    current = query_string.substr(prev_pos, pos);
                    prev_pos = pos + 1;
                }
                if(current.length() == 0) break;
                auto mid = current.find('=');
                if(mid == std::string::npos) {
                    qs[url_decode(current)] = "";
                } else {
                    qs[url_decode(current.substr(0, mid))] = url_decode(current.substr(mid + 1));
                }
            }
            return qs;
        }

        aiopromise<nil> aiolock::lock() {
            aiopromise<nil> result;
            if(sem == 1 && waiters.size() == 0) {
                sem = 0;
                result.resolve({});
            } else if(sem == 1) {
                sem = 0;
                auto first = waiters.front();
                waiters.pop();
                waiters.push(result);
                first.resolve({});
            } else {
                waiters.push(result);
            }
            return result;
        }

        void aiolock::unlock() {
            if(waiters.size() > 0) {
                sem = 0;
                auto first = waiters.front();
                waiters.pop();
                first.resolve({});
            } else {
                sem = 1;
            }
        }
    }
}