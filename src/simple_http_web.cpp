#include "scheduler.h"
#include "socket.h"
#include <string_view>

static constexpr std::string_view RAW_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";


Task handle_client(AsyncSocket socket) {

    char buf[1024];

    while (true) {
        ssize_t n = co_await socket.read(buf, sizeof(buf));

        if (n <= 0) break;

        ssize_t ret = co_await socket.write(RAW_RESPONSE.data(), RAW_RESPONSE.size());

        if (ret <= 0) break;
    }

}

Task start_server(Scheduler& sched, int port) {
    TcpListener listener(sched.reactor());

    if (listener.bind("0.0.0.0", port) < 0) {
        co_return;
    }

    while (true) {
        AsyncSocket client = co_await listener.accept();

        sched.spawn(handle_client(std::move(client)));
    }
}

int main() {

    Scheduler sched;

    sched.spawn(start_server(sched, 8080));

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}