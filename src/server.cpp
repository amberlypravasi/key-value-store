#include "kvstore/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>
#include <thread>

#include "kvstore/command.h"
#include "kvstore/handler.h"

namespace kvstore {

namespace {

// A client that sends this much without a newline is disconnected, so one
// client can't make the server use unlimited memory.
constexpr std::size_t kMaxLineBytes = 1 << 20;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// Sends all of `bytes`. Returns false if the client has disconnected.
bool send_all(int fd, const std::string& bytes) {
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        ssize_t sent = ::send(fd, data, left, kSendFlags);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += sent;
        left -= static_cast<std::size_t>(sent);
    }
    return true;
}

// Writing to a socket whose client has disconnected normally sends SIGPIPE,
// which kills the whole server. Turn that into an ordinary error instead.
void disable_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;  // MSG_NOSIGNAL in send_all handles it instead.
#endif
}

}  // namespace

Server::Server(Store& store, std::uint16_t port) : store_(store) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw_errno("could not create socket");
    }
    ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);

    // Lets the server restart right away on the same port.
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        int saved = errno;
        ::close(listen_fd_);
        errno = saved;
        throw_errno("could not listen on port " + std::to_string(port));
    }
    if (::listen(listen_fd_, SOMAXCONN) != 0) {
        int saved = errno;
        ::close(listen_fd_);
        errno = saved;
        throw_errno("could not listen");
    }

    socklen_t len = sizeof addr;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    if (::pipe(wake_pipe_) != 0) {
        int saved = errno;
        ::close(listen_fd_);
        errno = saved;
        throw_errno("could not create pipe");
    }
}

Server::~Server() {
    stop();
    ::close(listen_fd_);
    ::close(wake_pipe_[0]);
    ::close(wake_pipe_[1]);
}

void Server::run() {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    while (true) {
        // Wait until a client connects or stop() is called.
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            throw_errno("poll failed");
        }
        if (fds[1].revents != 0) {
            return;
        }

        int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            continue;  // The client may have given up already. Keep serving others.
        }
        ::fcntl(client, F_SETFD, FD_CLOEXEC);
        disable_sigpipe(client);

        std::lock_guard lock(mutex_);
        if (stopping_) {
            ::close(client);
            return;
        }
        clients_.insert(client);
        // stop() waits until clients_ is empty, so this thread never outlives
        // the server even though it's detached.
        std::thread(&Server::serve_client, this, client).detach();
    }
}

void Server::stop() {
    std::unique_lock lock(mutex_);
    if (!stopping_) {
        stopping_ = true;
        (void)::write(wake_pipe_[1], "x", 1);
        // Shutting down a socket wakes its thread from recv(), which then
        // sees the connection closed and exits.
        for (int client : clients_) {
            ::shutdown(client, SHUT_RDWR);
        }
    }
    clients_done_.wait(lock, [this] { return clients_.empty(); });
}

void Server::serve_client(int fd) {
    std::string buffer;
    char chunk[4096];
    bool open = true;

    while (open) {
        ssize_t received = ::recv(fd, chunk, sizeof chunk, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;  // Disconnected, or the server is stopping.
        buffer.append(chunk, static_cast<std::size_t>(received));

        // Handle every complete line received so far. A command can arrive
        // split across several recv() calls, so keep any partial line.
        std::size_t start = 0;
        std::size_t newline;
        while (open && (newline = buffer.find('\n', start)) != std::string::npos) {
            std::string line = buffer.substr(start, newline - start);
            start = newline + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();  // Clients like telnet end lines with \r\n.
            }

            Command cmd = parse_command(line);
            if (cmd.type == CommandType::Exit) {
                open = false;
            } else if (!(cmd.type == CommandType::Invalid && line.empty())) {
                open = send_all(fd, handle_command(store_, cmd) + "\n");
            }
        }
        buffer.erase(0, start);

        if (buffer.size() > kMaxLineBytes) {
            send_all(fd, "ERROR: line too long\n");
            open = false;
        }
    }

    std::lock_guard lock(mutex_);
    ::close(fd);
    clients_.erase(fd);
    clients_done_.notify_all();
}

}  // namespace kvstore
