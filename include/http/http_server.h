#pragma once
#include "../socket.h"
#include "http_parser.h"
#include <fstream>
#include <string>
#include <algorithm>

class HttpServer {
    AsyncSocket& socket_;

public:
    explicit HttpServer(AsyncSocket& s) : socket_(s) {}

    /**
     * Send response
     * Automatically construct the HTTP message header and send the Body
     */
    Task send_response(int code, std::string_view content_type, std::string_view body) {
        std::string res;
        res.reserve(256 + body.size());

        res += "HTTP/1.1 " + std::to_string(code);
        res += (code == 200 ? " OK\r\n" : " Error\r\n");
        res += "Server: tiny_coro/1.0\r\n";
        res += "Content-Type: " + std::string(content_type) + "\r\n";
        res += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        res += "Connection: keep-alive\r\n\r\n";

        // Send Header first
        co_await socket_.write(res);
        // Then send the Body
        if (!body.empty()) {
            co_await socket_.write(body.data(), body.size());
        }
    }

    /**
     * Stream file upload
     * Advantage: Regardless of file size, only a fixed buf memory (8KB) is occupied, and data is pumped directly to disk via coroutines
     */
    Task receive_to_file(std::string_view save_path, size_t content_length, std::string_view initial_data = "") {
        std::ofstream file(std::string(save_path).c_str(), std::ios::binary);
        if (!file.is_open()) {
            co_return;
        }

        size_t total_received = 0;

        // 1. Process the Body data that has been read into the buffer during the first read
        if (!initial_data.empty()) {
            size_t to_write = std::min(initial_data.size(), content_length);
            file.write(initial_data.data(), to_write);
            total_received += to_write;
        }

        // 2. Loop to read the remaining data
        char buf[8192];
        while (total_received < content_length) {
            size_t to_read = std::min(sizeof(buf), content_length - total_received);
            ssize_t n = co_await socket_.read(buf, to_read);

            if (n <= 0) break; // Connection interrupted

            file.write(buf, n);
            total_received += n;
        }

        file.close();
    }
};