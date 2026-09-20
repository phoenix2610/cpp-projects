// One thread, thousands of connections: an edge-triggered epoll loop with timers.
//
//   g++ -std=c++23 -O2 loop.cpp -o loop && ./loop --demo
//   ./loop --port 8080          # an echo server you can telnet into
//
// Blocking sockets need a thread each (8MB of stack, a context switch per message).
// epoll flips it: ask the kernel which of N sockets are ready, handle exactly those.
// The details that matter: edge-triggered mode reports a fd once per state change,
// so you MUST drain until EAGAIN or you will hang forever holding unread bytes;
// every fd must be non-blocking or one slow client stalls all of them; and partial
// writes need an output buffer plus EPOLLOUT, since the kernel can accept 3 of your
// 4KB and leave the rest.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Connection {
    int fd = -1;
    std::string outbox;          // bytes the kernel would not take yet
    std::uint64_t read_bytes = 0, written_bytes = 0;
    bool want_write = false;
};

class EventLoop {
public:
    EventLoop() : epoll_fd_(::epoll_create1(0)) {}
    ~EventLoop() { if (epoll_fd_ >= 0) ::close(epoll_fd_); }

    void add(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback) {
        handlers_[fd] = std::move(callback);
        epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
    }
    void modify(int fd, std::uint32_t events) {
        epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
    }
    void remove(int fd) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
        ::close(fd);
    }

    int add_timer(int interval_ms, std::function<void()> callback) {
        int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        itimerspec spec{};
        spec.it_interval.tv_sec = interval_ms / 1000;
        spec.it_interval.tv_nsec = (interval_ms % 1000) * 1'000'000L;
        spec.it_value = spec.it_interval;
        ::timerfd_settime(fd, 0, &spec, nullptr);
        add(fd, EPOLLIN, [fd, callback](std::uint32_t) {
            std::uint64_t expirations = 0;
            while (::read(fd, &expirations, sizeof(expirations)) > 0) {}
            callback();
        });
        return fd;
    }

    void run(int milliseconds = -1) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds < 0 ? 0 : milliseconds);
        std::vector<epoll_event> events(256);
        stop_ = false;
        while (!stop_) {
            int timeout = -1;
            if (milliseconds >= 0) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) break;
                timeout = int(left);
            }
            int ready = ::epoll_wait(epoll_fd_, events.data(), int(events.size()), timeout);
            ++wakeups_;
            if (ready < 0) { if (errno == EINTR) continue; break; }
            events_seen_ += ready;
            for (int i = 0; i < ready; ++i) {
                auto found = handlers_.find(events[i].data.fd);
                if (found != handlers_.end()) found->second(events[i].events);
            }
            if (ready == int(events.size())) events.resize(events.size() * 2);   // grow if we keep filling it
        }
    }

    void stop() { stop_ = true; }
    std::uint64_t wakeups() const { return wakeups_; }
    std::uint64_t events_seen() const { return events_seen_; }
    std::size_t watched() const { return handlers_.size(); }

private:
    int epoll_fd_;
    bool stop_ = false;
    std::uint64_t wakeups_ = 0, events_seen_ = 0;
    std::unordered_map<int, std::function<void(std::uint32_t)>> handlers_;
};

class EchoServer {
public:
    EchoServer(EventLoop& loop, int port) : loop_(loop) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(std::uint16_t(port));
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listen_fd_, 512);
        set_nonblocking(listen_fd_);
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        loop_.add(listen_fd_, EPOLLIN | EPOLLET, [this](std::uint32_t) { accept_all(); });
    }

    int port() const { return port_; }
    std::size_t connections() const { return connections_.size(); }
    std::uint64_t echoed() const { return echoed_; }
    std::uint64_t partial_writes() const { return partial_writes_; }
    std::uint64_t accepted() const { return accepted_; }

private:
    void accept_all() {
        // edge-triggered: one readiness notification may cover many pending connections
        for (;;) {
            int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            connections_[fd] = Connection{fd};
            ++accepted_;
            loop_.add(fd, EPOLLIN | EPOLLET, [this, fd](std::uint32_t events) { on_ready(fd, events); });
        }
    }

    void on_ready(int fd, std::uint32_t events) {
        auto found = connections_.find(fd);
        if (found == connections_.end()) return;
        Connection& conn = found->second;

        if (events & (EPOLLHUP | EPOLLERR)) { close(conn); return; }

        if (events & EPOLLOUT) flush(conn);

        if (events & EPOLLIN) {
            char buffer[16384];
            for (;;) {                                  // drain until EAGAIN, or lose the tail
                ssize_t got = ::read(fd, buffer, sizeof(buffer));
                if (got > 0) {
                    conn.read_bytes += std::uint64_t(got);
                    conn.outbox.append(buffer, std::size_t(got));
                    continue;
                }
                if (got == 0) { close(conn); return; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close(conn);
                return;
            }
            flush(conn);
        }
    }

    void flush(Connection& conn) {
        while (!conn.outbox.empty()) {
            ssize_t written = ::write(conn.fd, conn.outbox.data(), conn.outbox.size());
            if (written > 0) {
                conn.written_bytes += std::uint64_t(written);
                echoed_ += std::uint64_t(written);
                conn.outbox.erase(0, std::size_t(written));
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ++partial_writes_;
                if (!conn.want_write) {                 // ask to be told when it drains
                    conn.want_write = true;
                    loop_.modify(conn.fd, EPOLLIN | EPOLLOUT | EPOLLET);
                }
                return;
            }
            close(conn);
            return;
        }
        if (conn.want_write) {
            conn.want_write = false;
            loop_.modify(conn.fd, EPOLLIN | EPOLLET);
        }
    }

    void close(Connection& conn) {
        int fd = conn.fd;
        connections_.erase(fd);
        loop_.remove(fd);
    }

    EventLoop& loop_;
    int listen_fd_ = -1, port_ = 0;
    std::unordered_map<int, Connection> connections_;
    std::uint64_t echoed_ = 0, partial_writes_ = 0, accepted_ = 0;
};

int main(int argc, char** argv) {
    int port = 0;
    bool serve = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) { port = std::atoi(argv[++i]); serve = true; }
    }

    EventLoop loop;
    EchoServer server(loop, port);

    if (serve) {
        std::printf("echo server on 127.0.0.1:%d — one thread, edge-triggered epoll\n", server.port());
        loop.add_timer(5000, [&server] {
            std::printf("  %zu open connections, %llu bytes echoed\n",
                        server.connections(), (unsigned long long)server.echoed());
        });
        loop.run();
        return 0;
    }

    std::printf("echo server listening on 127.0.0.1:%d\n\n", server.port());

    constexpr int kClients = 500;
    constexpr int kMessages = 20;
    std::atomic<int> completed{0}, mismatches{0};
    std::atomic<bool> clients_done{false};
    std::uint64_t sent_total = 0;

    std::thread clients([&] {
        std::vector<int> sockets;
        for (int i = 0; i < kClients; ++i) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(std::uint16_t(server.port()));
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) sockets.push_back(fd);
            else ::close(fd);
        }
        std::string message(512, 'x');
        message.replace(0, 6, "hello ");
        for (int round = 0; round < kMessages; ++round) {
            for (int fd : sockets) ::send(fd, message.data(), message.size(), MSG_NOSIGNAL);
            for (int fd : sockets) {
                std::string received;
                received.resize(message.size());
                std::size_t have = 0;
                while (have < message.size()) {
                    ssize_t got = ::recv(fd, received.data() + have, message.size() - have, 0);
                    if (got <= 0) break;
                    have += std::size_t(got);
                }
                if (received != message) ++mismatches;
                else ++completed;
            }
        }
        for (int fd : sockets) ::close(fd);
        clients_done.store(true);
    });

    // stop as soon as the clients finish, so the timing measures work and not the budget
    loop.add_timer(20, [&] { if (clients_done.load()) loop.stop(); });
    auto start = std::chrono::steady_clock::now();
    loop.run(15000);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    clients.join();
    loop.run(200);
    sent_total = std::uint64_t(completed.load()) * 512;

    std::printf("  %d clients x %d messages on ONE thread\n", kClients, kMessages);
    std::printf("  accepted %llu connections, echoed %llu bytes in %.0fms\n",
                (unsigned long long)server.accepted(), (unsigned long long)server.echoed(), ms);
    std::printf("  round trips completed %d, corrupted %d\n", completed.load(), mismatches.load());
    std::printf("  throughput %.1f MB/s, %.0f round trips/second\n",
                double(sent_total) / ms / 1048.576, completed.load() / ms * 1000);
    std::printf("  epoll_wait woke %llu times for %llu events (%.1f events per wakeup — "
                "that ratio is the whole reason this scales)\n",
                (unsigned long long)loop.wakeups(), (unsigned long long)loop.events_seen(),
                double(loop.events_seen()) / double(loop.wakeups()));
    std::printf("  partial writes needing EPOLLOUT: %llu\n", (unsigned long long)server.partial_writes());
    std::printf("\n  a thread-per-connection server would have needed %d threads (~%.0f MB of stacks)\n",
                kClients, kClients * 8.0);
    return 0;
}
