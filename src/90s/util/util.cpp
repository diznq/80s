#include "util.hpp"
#include <array>
#include <80s/crypto.h>
#include <zlib.h>

namespace s90 {
    namespace util {

        constexpr std::array<unsigned int, 256> create_url_lut() {
            std::array<unsigned int, 256> values;
            for(unsigned int i = 0; i < 256; i++) {
                values[i] = (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z') || (i >= '0' && i <= '9') || i == '~' || i == '-' || i == '.' || i == '_' ? i : 0;
            }
            return values;
        }

        constexpr auto url_lut = create_url_lut();

        std::string url_decode(std::string_view data) {
            char lut[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0, 
                // A   B   C   D   E   F  G  H  I  J  K  L  M  N  O  P  Q  R  S  T  U  V  W  X  Y  Z
                  10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                // [  \  ]  ^  _  `   a   b   c   d   e   f
                   0, 0, 0, 0, 0, 0, 10, 11, 12, 13, 14, 15};
            std::string output;
            output.resize(data.length());
            size_t len = data.length();
            size_t i = 0;
            char c;
            char *ptr = output.data();
            for (i = 0; i < len; i++) {
                c = data[i] & 255;
                if (i <= len - 3) {
                    switch (c) {
                    case '+':
                        *ptr = ' '; ptr++;
                        break;
                    case '%': {
                        if (((data[i + 1] >= '0' && data[i + 1] <= '9') || (data[i + 1] >= 'A' && data[i + 1] <= 'F') || (data[i + 1] >= 'a' && data[i + 1] <= 'f')) && ((data[i + 2] >= '0' && data[i + 2] <= '9') || (data[i + 2] >= 'A' && data[i + 2] <= 'F') || (data[i + 2] >= 'a' && data[i + 2] <= 'f'))) {
                            *ptr = (lut[data[i + 1] - '0'] << 4) | (lut[data[i + 2] - '0']); ptr++;
                            i += 2;
                        } else {
                            *ptr = c; ptr++;
                        }
                        break;
                    }
                    default:
                        *ptr = c; ptr++;
                    }
                } else {
                    *ptr = c == '+' ? ' ' : c; ptr++;
                }
            }
            output.resize(ptr - output.data());
            return output;
        }

        std::string url_encode(std::string_view data) {
            std::string result;
            result.resize(3 * data.size() + 1);
            char chars[] = "0123456789ABCDEF";
            char *ptr = result.data();
            for (char c_ : data) {
                unsigned c = ((unsigned)c_)&255;
                if (url_lut[c]) {
                    *ptr = c;
                    ptr++;
                } else {
                    *ptr = '%'; ptr++;
                    *ptr = chars[(c >> 4) & 15]; ptr++;
                    *ptr = chars[c & 15]; ptr++;
                }
            }
            result.resize(ptr - result.data());
            return result;
        }

        std::string to_hex(std::string_view data) {
            std::string result;
            result.resize(data.size() * 2);
            char *ptr = result.data();
            char chars[] = "0123456789abcdef";
            for (char c_ : data) {
                unsigned c = ((unsigned)c_)&255;
                *ptr = chars[(c >> 4) & 15]; ptr++;
                *ptr = chars[c & 15]; ptr++;
            }
            return result;
        }

        std::string sha1(std::string_view text) {
            unsigned char buff[20];
            crypto_sha1(text.data(), text.length(), buff, sizeof(buff));
            return std::string((char*)buff, (char*)buff + 20);
        }

        std::string sha256(std::string_view text) {
            unsigned char buff[32];
            crypto_sha256(text.data(), text.length(), buff, sizeof(buff));
            return std::string((char*)buff, (char*)buff + 32);
        }

        std::string hmac_sha256(std::string_view text, std::string_view key) {
            unsigned char buff[32];
            crypto_hmac_sha256(text.data(), text.length(), key.data(), key.length(), buff, sizeof(buff));
            return std::string((char*)buff, (char*)buff + 32);
        }

        std::expected<std::string, std::string> from_b64(std::string_view text) {
            char data[200];
            const char *error = NULL;
            dynstr dstr;
            dynstr_init(&dstr, data, sizeof(data));
            int ok = crypto_from64(text.data(), text.length(), &dstr, &error);
            if(ok < 0) {
                dynstr_release(&dstr);
                return std::unexpected(error);
            }
            std::string result(dstr.ptr, dstr.ptr + dstr.length);
            dynstr_release(&dstr);
            return result;
        }

        std::string to_b64(std::string_view text) {
            char data[200];
            dynstr dstr;
            dynstr_init(&dstr, data, sizeof(data));
            int ok = crypto_to64(text.data(), text.length(), &dstr, NULL);
            if(ok < 0) return "";
            std::string result(dstr.ptr, dstr.ptr + dstr.length);
            dynstr_release(&dstr);
            return result;
        }

        std::expected<std::string, std::string> cipher(std::string_view text, std::string_view key, bool encrypt, bool use_iv) {
            const char *error = NULL;
            char buf[200];
            dynstr dstr;
            dynstr_init(&dstr, buf, sizeof(buf));
            int ok = crypto_cipher(text.data(), text.length(), key.data(), key.length(), use_iv, encrypt, &dstr, &error);
            if(ok < 0) {
                dynstr_release(&dstr);
                return std::unexpected(error);
            }
            std::string result(dstr.ptr, dstr.ptr + dstr.length);
            dynstr_release(&dstr);
            return result;
        }

        dict<std::string, std::string> parse_query_string(std::string_view query_string) {
            size_t prev_pos = 0, pos = -1;
            dict<std::string, std::string> qs;
            while(prev_pos < query_string.length()) {
                pos = query_string.find('&', prev_pos);
                std::string_view current;
                if(pos == std::string::npos) {
                    current = query_string.substr(prev_pos);
                    prev_pos = query_string.length();
                } else {
                    current = query_string.substr(prev_pos, pos - prev_pos);
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

        // Compress the input data in-memory
        int compress(std::string& inout) {
            // Initialize zlib stream
            z_stream stream;
            stream.zalloc = Z_NULL;
            stream.zfree = Z_NULL;
            stream.opaque = Z_NULL;
            stream.avail_in = static_cast<uInt>(inout.size());
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(inout.data()));

            // Initialize output buffer
            std::vector<Bytef> compressedBuffer(inout.size() + 4096); // Adjust buffer size as needed

            stream.avail_out = static_cast<uInt>(compressedBuffer.size());
            stream.next_out = compressedBuffer.data();
            
            // Initialize compression with gzip format
            int windowBits = 15; // Default windowBits for zlib
            int GZIP_ENCODING = 16; // Add 16 for gzip format
            if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, windowBits | GZIP_ENCODING, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                return -1;
            }

            // Perform compression
            if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
                deflateEnd(&stream);
                return -1;
            }

            // Finalize compression
            deflateEnd(&stream);

            // Update inout with compressed data
            inout.assign(reinterpret_cast<char*>(compressedBuffer.data()), stream.total_out);
            return 0;
        }

        // Decompress the data in-memory
        int decompress(std::string& inout) {
            // Initialize zlib stream
            z_stream stream;
            stream.zalloc = Z_NULL;
            stream.zfree = Z_NULL;
            stream.opaque = Z_NULL;
            stream.avail_in = static_cast<uInt>(inout.size());
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(inout.data()));

            // Initialize output buffer
            std::vector<Bytef> decompressedBuffer(inout.size() * 2 + 4096); // Initial size, adjust as needed

            stream.avail_out = static_cast<uInt>(decompressedBuffer.size());
            stream.next_out = decompressedBuffer.data();

            // Initialize decompression with gzip format
            int windowBits = 15; // Default windowBits for zlib
            int GZIP_ENCODING = 16; // Add 16 for gzip format
            if (inflateInit2(&stream, windowBits | GZIP_ENCODING) != Z_OK) {
                return -1;
            }

            // Perform decompression
            while (true) {
                int result = inflate(&stream, Z_NO_FLUSH);
                if (result == Z_STREAM_END) {
                    break; // Decompression complete
                } else if (result != Z_OK) {
                    inflateEnd(&stream);
                    return -1;
                }

                // Check if the buffer is full
                if (stream.avail_out == 0) {
                    // Resize the buffer
                    decompressedBuffer.resize(decompressedBuffer.size() + 4096);
                    stream.avail_out = static_cast<uInt>(decompressedBuffer.size()) - stream.total_out;
                    stream.next_out = decompressedBuffer.data() + stream.total_out;
                }
            }

            // Finalize decompression
            inflateEnd(&stream);

            // Update inout with decompressed data
            inout.assign(reinterpret_cast<char*>(decompressedBuffer.data()), stream.total_out);
            return 0;
        }
    }
}