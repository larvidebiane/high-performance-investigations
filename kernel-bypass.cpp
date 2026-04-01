//
// Created by Larvi DEBIANE on 31/03/2026
//
// UDP Receive Benchmark
// Simple benchmark receiver used to measure RX throughput in a controlled TX/RX setup.
// This implementation is intentionally simple and is meant for bottleneck isolation,
// not as a fully optimized production receiver.
//
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    constexpr int SELECT_TIMEOUT_MS = 10;

    struct Config {
        int port;
        int duration_seconds;
        std::size_t buffer_size;   // recvfrom() user buffer
        std::size_t payload_size;  // expected UDP payload size (sender)
    };

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
        double elapsed_seconds = 0.0;
    };

    [[noreturn]] void usage(const char* prog) {
        std::cerr
                << "Usage:\n  " << prog
                << " --port N --duration N --buffer N --payload N\n\n"
                << "Rules:\n"
                << "  buffer >= payload   (otherwise UDP datagrams are truncated and stats are invalid)\n";
        std::exit(1);
    }

    Config parse_args(int argc, char** argv) {
        if (argc == 1) throw std::runtime_error("No arguments provided");

        Config cfg{};
        bool port_set = false, duration_set = false, buffer_set = false, payload_set = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            auto require_value = [&](const std::string& name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + name);
                return std::string(argv[++i]);
            };

            if (arg == "--port") {
                cfg.port = std::stoi(require_value("--port"));
                port_set = true;
            } else if (arg == "--duration") {
                cfg.duration_seconds = std::stoi(require_value("--duration"));
                duration_set = true;
            } else if (arg == "--buffer") {
                cfg.buffer_size = static_cast<std::size_t>(std::stoul(require_value("--buffer")));
                buffer_set = true;
            } else if (arg == "--payload") {
                cfg.payload_size = static_cast<std::size_t>(std::stoul(require_value("--payload")));
                payload_set = true;
            } else if (arg == "--help") {
                usage(argv[0]);
            } else {
                throw std::runtime_error("Unknown argument: " + arg);
            }
        }

        if (!port_set || !duration_set || !buffer_set || !payload_set)
            throw std::runtime_error("All parameters required: --port --duration --buffer --payload");

        if (cfg.port <= 0 || cfg.port > 65535) throw std::runtime_error("Invalid port");
        if (cfg.duration_seconds <= 0) throw std::runtime_error("duration must be > 0");
        if (cfg.buffer_size == 0) throw std::runtime_error("buffer must be > 0");
        if (cfg.payload_size == 0) throw std::runtime_error("payload must be > 0");

        // 🔥 TOP PRIORITY GUARD
        if (cfg.buffer_size < cfg.payload_size) {
            throw std::runtime_error(
                    "FATAL CONFIG: buffer < payload. "
                    "UDP recvfrom() will truncate datagrams and stats become invalid. "
                    "Fix: set --buffer >= --payload."
            );
        }

        return cfg;
    }

    int create_udp_socket(int port) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            std::perror("socket");
            return -1;
        }

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::perror("bind");
            ::close(fd);
            return -1;
        }

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            std::perror("fcntl(O_NONBLOCK)");
            ::close(fd);
            return -1;
        }

        return fd;
    }

    bool wait_for_readable(int fd, int timeout_ms) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int rc = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) return false;
            std::perror("select");
            return false;
        }
        return rc > 0 && FD_ISSET(fd, &readfds);
    }

    void print_running(const Stats& stats) {
        double pps = stats.elapsed_seconds > 0.0 ? stats.packets / stats.elapsed_seconds : 0.0;
        double mbps = stats.elapsed_seconds > 0.0 ? (stats.bytes * 8.0) / stats.elapsed_seconds / 1e6 : 0.0;

        std::cout << "[RX-running] "
                  << "elapsed=" << std::fixed << std::setprecision(3) << stats.elapsed_seconds << " s, "
                  << "packets=" << stats.packets << ", "
                  << "bytes=" << stats.bytes << ", "
                  << "pps=" << static_cast<std::uint64_t>(pps) << ", "
                  << "Mb/s=" << std::setprecision(3) << mbps
                  << std::endl;
    }

    Stats run_benchmark(int fd, int duration_seconds, std::size_t buffer_size) {
        Stats stats{};
        std::vector<char> buffer(buffer_size);

        auto start = std::chrono::steady_clock::now();
        auto next_report = start + std::chrono::seconds(1);
        auto end_time = start + std::chrono::seconds(duration_seconds);

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time) break;

            if (wait_for_readable(fd, SELECT_TIMEOUT_MS)) {
                while (true) {
                    ssize_t n = ::recvfrom(fd, buffer.data(), buffer.size(), 0, nullptr, nullptr);

                    if (n > 0) {
                        stats.packets++;
                        stats.bytes += static_cast<std::uint64_t>(n);
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        std::perror("recvfrom");
                        return stats;
                    }
                }
            }

            auto current = std::chrono::steady_clock::now();
            if (current >= next_report) {
                stats.elapsed_seconds = std::chrono::duration<double>(current - start).count();
                print_running(stats);
                next_report += std::chrono::seconds(1);
            }
        }

        auto finish = std::chrono::steady_clock::now();
        stats.elapsed_seconds = std::chrono::duration<double>(finish - start).count();
        return stats;
    }

    void print_final(const Stats& stats) {
        double pps = stats.elapsed_seconds > 0.0 ? stats.packets / stats.elapsed_seconds : 0.0;
        double mbps = stats.elapsed_seconds > 0.0 ? (stats.bytes * 8.0) / stats.elapsed_seconds / 1e6 : 0.0;

        std::cout << "\n=== FINAL RX STATS ===" << std::endl;
        std::cout << "elapsed_seconds : " << std::fixed << std::setprecision(6) << stats.elapsed_seconds <<  std::endl;
        std::cout << "packets         : " << stats.packets <<  std::endl;
        std::cout << "bytes           : " << stats.bytes <<  std::endl;
        std::cout << "packets/sec     : " << static_cast<std::uint64_t>(pps) <<  std::endl;
        std::cout << "Mb/s            : " << std::setprecision(3) << mbps <<  std::endl;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        Config cfg = parse_args(argc, argv);

        std::cout << "=== Effective Configuration ===\n";
        std::cout << "port            : " << cfg.port << "\n";
        std::cout << "duration_seconds: " << cfg.duration_seconds << "\n";
        std::cout << "buffer_size     : " << cfg.buffer_size << "\n";
        std::cout << "payload_size    : " << cfg.payload_size << "\n";
        std::cout << "================================\n\n";

        int fd = create_udp_socket(cfg.port);
        if (fd < 0) return 1;

        // READY is a synchronization signal for the shell script.
        // Send it to stderr so stdout can remain dedicated to live benchmark stats.
        std::cerr << "RX READY port=" << cfg.port << std::endl;

        Stats stats = run_benchmark(fd, cfg.duration_seconds, cfg.buffer_size);

        ::close(fd);
        print_final(stats);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n======================================\n";
        std::cerr << "FATAL: " << e.what() << "\n";
        std::cerr << "======================================\n";
        std::cerr << "Usage: " << argv[0] << " --port N --duration N --buffer N --payload N\n";
        return 1;
    }
}