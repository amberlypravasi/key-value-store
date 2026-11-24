// Stage 4: the network server, tested over real TCP connections.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "kvstore/server.h"
#include "kvstore/store.h"

using kvstore::Server;
using kvstore::Store;

namespace {

// Runs a server on a free port for the length of a test.
class RunningServer {
public:
    explicit RunningServer(Store& store) : server_(store, 0), thread_([this] { server_.run(); }) {}
    ~RunningServer() {
        server_.stop();
        thread_.join();
    }

    Server& server() { return server_; }
    std::uint16_t port() const { return server_.port(); }

private:
    Server server_;
    std::thread thread_;
};

// A minimal client that sends raw text and reads responses line by line.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    }
    ~Client() { ::close(fd_); }

    void send_raw(const std::string& text) {
        REQUIRE(::send(fd_, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()));
    }

    // Returns the next line, without its newline, or "<closed>" if the server
    // closed the connection.
    std::string read_line() {
        std::size_t newline;
        while ((newline = buffer_.find('\n')) == std::string::npos) {
            char chunk[4096];
            ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
            if (n <= 0) return "<closed>";
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        return line;
    }

    std::string command(const std::string& line) {
        send_raw(line + "\n");
        return read_line();
    }

private:
    int fd_ = -1;
    std::string buffer_;
};

}  // namespace

TEST_CASE("server handles SET, GET, and DEL") {
    Store store;
    RunningServer running(store);
    Client client(running.port());

    CHECK(client.command("SET name Alisha") == "OK");
    CHECK(client.command("GET name") == "Alisha");
    CHECK(client.command("DEL name") == "1");
    CHECK(client.command("GET name") == "(nil)");
    CHECK(client.command("FLY") == "ERROR: unknown command: FLY");
}

TEST_CASE("clients see each other's writes") {
    Store store;
    RunningServer running(store);
    Client alice(running.port());
    Client bob(running.port());

    CHECK(alice.command("SET greeting hello from alice") == "OK");
    CHECK(bob.command("GET greeting") == "hello from alice");
}

TEST_CASE("handles commands split across packets, several per packet, and \\r\\n") {
    Store store;
    RunningServer running(store);
    Client client(running.port());

    client.send_raw("SE");
    client.send_raw("T a 1\r");
    client.send_raw("\nSET b 2\nGET a\r\nGET b\n");
    CHECK(client.read_line() == "OK");
    CHECK(client.read_line() == "OK");
    CHECK(client.read_line() == "1");
    CHECK(client.read_line() == "2");
}

TEST_CASE("blank lines get no response") {
    Store store;
    RunningServer running(store);
    Client client(running.port());

    client.send_raw("\n\nGET missing\n");
    CHECK(client.read_line() == "(nil)");
}

TEST_CASE("EXIT closes the connection") {
    Store store;
    RunningServer running(store);
    Client client(running.port());

    client.send_raw("EXIT\n");
    CHECK(client.read_line() == "<closed>");
}

TEST_CASE("many clients writing at once") {
    constexpr int kClients = 16;
    constexpr int kWrites = 100;
    Store store;
    RunningServer running(store);

    std::vector<std::thread> threads;
    std::vector<int> ok_counts(kClients, 0);
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            Client client(running.port());
            for (int i = 0; i < kWrites; ++i) {
                std::string key = "c" + std::to_string(c) + "-" + std::to_string(i);
                if (client.command("SET " + key + " " + std::to_string(i)) == "OK") {
                    ++ok_counts[c];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (int c = 0; c < kClients; ++c) {
        CHECK(ok_counts[c] == kWrites);
    }
    CHECK(store.size() == kClients * kWrites);
}

TEST_CASE("a client sending a huge line without a newline is disconnected") {
    Store store;
    RunningServer running(store);
    Client client(running.port());

    // One byte over the 1 MB limit. Sending exactly this much means the
    // server has read everything before it closes the connection.
    std::string chunk(64 * 1024, 'x');
    for (int i = 0; i < 16; ++i) {
        client.send_raw(chunk);
    }
    client.send_raw("x");
    CHECK(client.read_line() == "ERROR: line too long");
    CHECK(client.read_line() == "<closed>");
}

TEST_CASE("stop() disconnects idle clients and returns") {
    Store store;
    std::optional<Client> idle;
    {
        RunningServer running(store);
        idle.emplace(running.port());
        CHECK(idle->command("SET a 1") == "OK");
        // RunningServer's destructor calls stop() here, with the client
        // still connected. The test would hang if stop() waited forever.
    }
    CHECK(idle->read_line() == "<closed>");
}
