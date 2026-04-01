#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

    volatile sig_atomic_t stop_requested = 0;

    void handle_signal(int) {
        stop_requested = 1;
    }

/* ===================== ARGS ===================== */

    struct Args {
        std::string host;
        std::uint16_t port = 0;
        std::size_t size = 0;
        std::uint32_t duration = 0;
        std::uint32_t pps = 0;
        bool connect_socket = false;
        std::uint16_t report_interval = 1;
    };

    [[noreturn]] void usage(const char* prog) {
        std::cerr
                << "Usage:\n"
                << "  " << prog
                << " --host <host> --port <port> --size <bytes> --duration <seconds> --pps <rate> [--connect]\n\n"
                << "Options:\n"
                << "  --host <host>               Destination host\n"
                << "  --port <port>               Destination port\n"
                << "  --size <bytes>              Payload size in bytes\n"
                << "  --duration <seconds>        Send duration in seconds\n"
                << "  --pps <rate>                Packets per second\n"
                << "  --connect                   Use connected UDP socket\n"
                << "  --help                      Show this help\n";
        std::exit(1);
    }

    bool is_flag(const char* a, const char* b) {
        return std::strcmp(a, b) == 0;
    }

    std::string require_value(int& i, int argc, char* argv[]) {
        if (i + 1 >= argc) usage(argv[0]);
        return argv[++i];
    }

    std::uint32_t parse_u32(const std::string& s) {
        char* end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(s.c_str(), &end, 10);
        if (errno || *end != '\0') throw std::runtime_error("bad number");
        return static_cast<std::uint32_t>(v);
    }

    Args parse_args(int argc, char* argv[]) {
        if (argc == 1) usage(argv[0]);

        Args a;

        for (int i = 1; i < argc; ++i) {
            if (is_flag(argv[i], "--host")) a.host = require_value(i, argc, argv);
            else if (is_flag(argv[i], "--port")) a.port = parse_u32(require_value(i, argc, argv));
            else if (is_flag(argv[i], "--size")) a.size = parse_u32(require_value(i, argc, argv));
            else if (is_flag(argv[i], "--duration")) a.duration = parse_u32(require_value(i, argc, argv));
            else if (is_flag(argv[i], "--pps")) a.pps = parse_u32(require_value(i, argc, argv));
            else if (is_flag(argv[i], "--report-interval")) a.report_interval = parse_u32(require_value(i, argc, argv));
            else if (is_flag(argv[i], "--connect")) a.connect_socket = true;
            else usage(argv[0]);
        }

        if (a.host.empty() || a.port == 0 || a.size == 0 || a.duration == 0 || a.pps == 0) {
            throw std::runtime_error("invalid args");
        }

        return a;
    }

/* ===================== RESOLVE ===================== */

    struct Endpoint {
        sockaddr_storage addr{};
        socklen_t len;
        int family;
    };

    Endpoint resolve(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* res;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res))
            throw std::runtime_error("resolve failed");

        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> holder(res, freeaddrinfo);

        Endpoint ep;
        ep.family = res->ai_family;
        ep.len = res->ai_addrlen;
        std::memcpy(&ep.addr, res->ai_addr, res->ai_addrlen);

        return ep;
    }

/* ===================== RUN ===================== */

    using clock = std::chrono::steady_clock;

    using clock = std::chrono::steady_clock;

    void run_connected(int sock, const std::vector<char>& payload, const Args& a) {
        const auto start = clock::now();
        const auto end = start + std::chrono::seconds(a.duration);

        const auto tick = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(a.pps))
        );

        auto estimated_now = start;
        auto next_send = start;
        auto next_report = start + std::chrono::seconds(a.report_interval);

        std::uint64_t pkts = 0;
        std::uint64_t bytes = 0;

        std::uint32_t interpolation_counter = 0;
        constexpr std::uint32_t interpolation_threshold = 1024;

        while (!stop_requested) {
            bool refreshed_now = false;
            clock::time_point now{};

            if (interpolation_counter >= interpolation_threshold) {
                now = clock::now();
                estimated_now = now;
                interpolation_counter = 0;
                refreshed_now = true;

                if (now >= end) {
                    break;
                }
            }

            if (estimated_now < next_send) {
                if (!refreshed_now) {
                    now = clock::now();
                    estimated_now = now;
                    refreshed_now = true;
                }

                if (now >= end) {
                    break;
                }

                if (now < next_send) {
                    std::this_thread::sleep_for(next_send - now);
                    now = clock::now();
                    estimated_now = now;
                }

                interpolation_counter = 0;
                continue;
            }

            const ssize_t n = ::send(sock, payload.data(), payload.size(), 0);
            if (n > 0) {
                ++pkts;
                bytes += static_cast<std::uint64_t>(n);
            }

            next_send += tick;
            estimated_now += tick;
            ++interpolation_counter;

            if (estimated_now >= next_report) {
                if (!refreshed_now) {
                    now = clock::now();
                    estimated_now = now;
                    refreshed_now = true;
                    interpolation_counter = 0;
                }

                const double elapsed = std::chrono::duration<double>(now - start).count();
                std::cout << "[TX] t=" << elapsed
                          << " pps=" << (elapsed > 0.0 ? pkts / elapsed : 0.0)
                          << " Mb/s=" << (elapsed > 0.0 ? (bytes * 8e-6) / elapsed : 0.0)
                          << "\n";

                next_report += std::chrono::seconds(a.report_interval);
            }
        }

        const auto final_now = clock::now();
        const double elapsed = std::chrono::duration<double>(final_now - start).count();

        std::cout << "\n=== FINAL TX STATS ===\n";
        std::cout << "elapsed_seconds : " << std::fixed << std::setprecision(6) << elapsed << "\n";
        std::cout << "packets         : " << pkts << "\n";
        std::cout << "bytes           : " << bytes << "\n";
        std::cout << "packets/sec     : " << std::setprecision(0)
                  << (elapsed > 0.0 ? pkts / elapsed : 0.0) << "\n";
        std::cout << "Mb/s            : " << std::setprecision(3)
                  << (elapsed > 0.0 ? (bytes * 8.0 / elapsed) / 1e6 : 0.0) << "\n";
    }

    void run_unconnected(int sock, const std::vector<char>& payload, const Endpoint& ep, const Args& a) {
        const auto start = clock::now();
        auto next_send = start;
        auto next_report = start + std::chrono::seconds(a.report_interval);
        const auto end = start + std::chrono::seconds(a.duration);

        std::uint64_t pkts = 0, bytes = 0;
        const auto tick = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(a.pps))
        );

        while (!stop_requested) {
            auto now = clock::now();
            if (now >= end) break;

            if (now < next_send) {
                std::this_thread::sleep_for(next_send - now);
                continue;
            }

            ssize_t n = ::sendto(sock, payload.data(), payload.size(), 0,
                                 reinterpret_cast<const sockaddr*>(&ep.addr), ep.len);

            if (n > 0) {
                ++pkts;
                bytes += n;
            }

            next_send += tick;

            if (now >= next_report) {
                double elapsed = std::chrono::duration<double>(now - start).count();
                std::cout << "[TX] t=" << elapsed
                          << " pps=" << pkts / elapsed
                          << " Mb/s=" << (bytes * 8e-6) / elapsed << "\n";
                next_report += std::chrono::seconds(a.report_interval);
            }
        }
    }

} // namespace

/* ===================== MAIN ===================== */

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_signal);

    try {
        Args args = parse_args(argc, argv);
        Endpoint ep = resolve(args.host, args.port);

        int sock = socket(ep.family, SOCK_DGRAM, 0);
        if (sock < 0) throw std::runtime_error("socket");

        if (args.connect_socket) {
            if (connect(sock, (sockaddr*)&ep.addr, ep.len) != 0)
                throw std::runtime_error("connect");
        }

        std::vector<char> payload(args.size, 'x');

        if (args.connect_socket)
            run_connected(sock, payload, args);
        else
            run_unconnected(sock, payload, ep, args);

        close(sock);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}