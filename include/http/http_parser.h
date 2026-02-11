#pragma once
#include <string_view>
#include <vector>
#include "../picohttpparser/picohttpparser.h"

struct HttpRequest {
    std::string_view method;
    std::string_view path;
    int minor_version;

    struct Header {
        std::string_view name;
        std::string_view value;
    };
    std::vector<Header> headers;

    // Helper method: quickly get Header value
    std::string_view get_header(std::string_view name) const {
        for (const auto& h : headers) {
            // Simple implementation; it is recommended to handle case insensitivity in production environments
            if (h.name.size() == name.size()) {
                bool match = true;
                for (size_t i = 0; i < name.size(); ++i) {
                    if (std::tolower(h.name[i]) != std::tolower(name[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) return h.value;
            }
        }
        return "";
    }
};

class HttpParser {
public:
    /**
      * Parse request
      * @return:
      * > 0: Parsing succeeded, returns the total length (bytes) of the request header
      * -1: Parsing error
      * -2: Incomplete data (need to read more)
      */
    static int parse_request(const char* buf, size_t len, HttpRequest& req) {
        const char *method_ptr, *path_ptr;
        size_t method_len, path_len;
        int minor_version;
        struct phr_header headers[32]; // Supports up to 32 Headers
        size_t num_headers = 32;

        int ret = phr_parse_request(buf, len, &method_ptr, &method_len, &path_ptr, &path_len,
                                    &minor_version, headers, &num_headers, 0);

        if (ret > 0) {
            req.method = {method_ptr, method_len};
            req.path = {path_ptr, path_len};
            req.minor_version = minor_version;
            req.headers.clear();
            req.headers.reserve(num_headers);
            for (size_t i = 0; i < num_headers; ++i) {
                req.headers.push_back({
                    {headers[i].name, headers[i].name_len},
                    {headers[i].value, headers[i].value_len}
                });
            }
        }
        return ret;
    }
};