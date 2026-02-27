#include "scheduler.h"
#include "socket.h"
#include "async_mutex.h"
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

struct RedisDB {

    std::map<std::string, std::string> kv_store;
    AsyncMutex mutex;

    RedisDB(Scheduler& sched) : mutex(sched) {}
};

std::vector<std::string> parse_resp(const std::string& data) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t rn = data.find("\r\n", pos);
        if (rn == std::string::npos) break;
        std::string line = data.substr(pos, rn - pos);
        pos = rn + 2;

        if (line.empty()) continue;
        if (line[0] == '*' || line[0] == '$') continue;

        tokens.push_back(line);
    }
    return tokens;
}

Task handle_client(AsyncSocket client, RedisDB& db) {
    char buf[4096];

    while (true) {
        ssize_t n = co_await client.read(buf, sizeof(buf));
        if (n <= 0) {
            std::cout << "[Client Disconnected] fd: " << client.fd() << "\n";
            co_return;
        }

        std::string req_data(buf, n);
        auto args = parse_resp(req_data);
        if (args.empty()) continue;

        std::string cmd = args[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "PING") {
            co_await client.write("+PONG\r\n");

        } else if (cmd == "SET" && args.size() >= 3) {
            const std::string& key = args[1];
            const std::string& val = args[2];

            {
                auto guard = co_await db.mutex.lock();
                db.kv_store[key] = val; // std::map 支持相同的下标操作
            }

            co_await client.write("+OK\r\n");

        } else if (cmd == "GET" && args.size() >= 2) {
            const std::string& key = args[1];
            std::string response;

            {
                auto guard = co_await db.mutex.lock();
                auto it = db.kv_store.find(key); // std::map 支持相同的 find 操作
                if (it != db.kv_store.end()) {
                    response = "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
                } else {
                    response = "$-1\r\n";
                }
            }
            co_await client.write(response);

        } else if (cmd == "DEL" && args.size() >= 2) {
            const std::string& key = args[1];
            int count = 0;

            {
                auto guard = co_await db.mutex.lock();
                count = db.kv_store.erase(key); // std::map 支持相同的 erase 操作
            }
            co_await client.write(":" + std::to_string(count) + "\r\n");

        } else if (cmd == "QUIT") {
            co_await client.write("+OK\r\n");
            co_return;

        } else {
            co_await client.write("-ERR unknown command\r\n");
        }
    }
}

Task start_redis_server(Scheduler& sched, int port) {
    TcpListener listener(sched.reactor());
    if (listener.bind("0.0.0.0", port) < 0) {
        std::cerr << "Miniredis bind failed on port " << port << "!\n";
        co_return;
    }

    std::cout << "=> Miniredis is running on 0.0.0.0:" << port << "\n";
    std::cout << "=> Using std::map (Red-Black Tree) for KV storage.\n";

    RedisDB db(sched);

    while (true) {
        AsyncSocket client = co_await listener.accept();
        sched.spawn(handle_client(std::move(client), db));
    }
}

int main() {

    Scheduler sched(4);
    sched.spawn(start_redis_server(sched, 6379));

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}