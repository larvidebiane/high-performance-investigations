//
// Created by Larvi DEBIANE on 31/03/2026
//
//

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
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

    struct Args {
        std::string host;
        int port = 0;
        std::size_t size = 512;
        double duration = 10.0;
        std::uint64_t count = 0;
        double pps = 0.0;
        bool connect_socket = false;
        double report_interval = 1.0;
    };

    [[noreturn]] void usage(const char* prog) {
        std::cerr
                << "Usage:\n"
                << "  " << prog << " --host <host> --port <port> [options]\n\n"
                << "Options:\n"
                << "  --host <host>               Destination host\n"
                << "  --port <port>               Destination port\n"
                << "  --size <bytes>              Payload size in bytes (default: 512)\n"
                << "  --duration <seconds>        Send duration in seconds (default: 10)\n"
                << "  --count <packets>           Number of packets (overrides duration)\n"
                << "  --pps <rate>                Packets per second, 0 = unlimited (default: 0)\n"
                << "  --connect                   Use connected UDP socket\n"
                << "  --report-interval <sec>     Reporting interval in seconds (default: 1)\n"
                << "  --help                      Show this help\n";
        std::exit(1);
    }

    bool is_flag(const char* arg, const char* name) {
        return std::strcmp(arg, name) == 0;
    }

    std::string require_value(int& i, int argc, char* argv[]) {
        if (i + 1 >= argc) {
            throw std::runtime_error(std::string("missing value for ") + argv[i]);
        }
        return argv[++i];
    }

    int parse_int(const std::string& s, const std::string& field) {
        char* end = nullptr;
        errno = 0;
        long v = std::strtol(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str() || *end != '\0') {
            throw std::runtime_error("invalid integer for " + field + ": " + s);
        }
        if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
            throw std::runtime_error("integer out of range for " + field + ": " + s);
        }
        return static_cast<int>(v);
    }

    std::uint64_t parse_u64(const std::string& s, const std::string& field) {
        char* end = nullptr;
        errno = 0;
        unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str() || *end != '\0') {
            throw std::runtime_error("invalid unsigned integer for " + field + ": " + s);
        }
        return static_cast<std::uint64_t>(v);
    }

    double parse_double(const std::string& s, const std::string& field) {
        char* end = nullptr;
        errno = 0;
        double v = std::strtod(s.c_str(), &end);
        if (errno != 0 || end == s.c_str() || *end != '\0' || !std::isfinite(v)) {
            throw std::runtime_error("invalid floating-point value for " + field + ": " + s);
        }
        return v;
    }

    Args parse_args(int argc, char* argv[]) {
        if (argc == 1) {
            usage(argv[0]);
        }

        Args args;

        for (int i = 1; i < argc; ++i) {
            if (is_flag(argv[i], "--help")) {
                usage(argv[0]);
            } else if (is_flag(argv[i], "--host")) {
                args.host = require_value(i, argc, argv);
            } else if (is_flag(argv[i], "--port")) {
                args.port = parse_int(require_value(i, argc, argv), "port");
            } else if (is_flag(argv[i], "--size")) {
                auto v = parse_u64(require_value(i, argc, argv), "size");
                args.size = static_cast<std::size_t>(v);
            } else if (is_flag(argv[i], "--duration")) {
                args.duration = parse_double(require_value(i, argc, argv), "duration");
            } else if (is_flag(argv[i], "--count")) {
                args.count = parse_u64(require_value(i, argc, argv), "count");
            } else if (is_flag(argv[i], "--pps")) {
                args.pps = parse_double(require_value(i, argc, argv), "pps");
            } else if (is_flag(argv[i], "--connect")) {
                args.connect_socket = true;
            } else if (is_flag(argv[i], "--report-interval")) {
                args.report_interval = parse_double(require_value(i, argc, argv), "report-interval");
            } else {
                throw std::runtime_error(std::string("unknown argument: ") + argv[i]);
            }
        }

        if (args.host.empty()) {
            throw std::runtime_error("--host is required");
        }

        if (!(1 <= args.port && args.port <= 65535)) {
            throw std::runtime_error("port must be in [1, 65535]");
        }

        if (args.size == 0) {
            throw std::runtime_error("size must be > 0");
        }

        if (args.duration <= 0.0 && args.count == 0) {
            throw std::runtime_error("duration must be > 0 unless count > 0");
        }

        if (args.pps < 0.0) {
            throw std::runtime_error("pps must be >= 0");
        }

        if (args.report_interval <= 0.0) {
            throw std::runtime_error("report-interval must be > 0");
        }

        return args;
    }

    struct AddrInfoDeleter {
        void operator()(addrinfo* p) const {
            if (p) {
                freeaddrinfo(p);
            }
        }
    };

    struct ResolvedEndpoint {
        int family = AF_UNSPEC;
        int socktype = SOCK_DGRAM;
        int protocol = 0;
        sockaddr_storage addr{};
        socklen_t addrlen = 0;
        std::string numeric_host;
        std::string numeric_port;
    };

    ResolvedEndpoint resolve_udp_endpoint(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* raw = nullptr;
        const std::string port_str = std::to_string(port);

        int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &raw);
        if (rc != 0) {
            throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
        }

        std::unique_ptr<addrinfo, AddrInfoDeleter> result(raw);

        for (addrinfo* p = result.get(); p != nullptr; p = p->ai_next) {
            if (p->ai_socktype != SOCK_DGRAM) {
                continue;
            }

            ResolvedEndpoint ep;
            ep.family = p->ai_family;
            ep.socktype = p->ai_socktype;
            ep.protocol = p->ai_protocol;
            ep.addrlen = static_cast<socklen_t>(p->ai_addrlen);
            std::memcpy(&ep.addr, p->ai_addr, p->ai_addrlen);

            char hostbuf[NI_MAXHOST];
            char servbuf[NI_MAXSERV];
            int ni = getnameinfo(
                    p->ai_addr,
                    p->ai_addrlen,
                    hostbuf,
                    sizeof(hostbuf),
                    servbuf,
                    sizeof(servbuf),
                    NI_NUMERICHOST | NI_NUMERICSERV
            );
            if (ni == 0) {
                ep.numeric_host = hostbuf;
                ep.numeric_port = servbuf;
            } else {
                ep.numeric_host = host;
                ep.numeric_port = port_str;
            }

            return ep;
        }

        throw std::runtime_error("no suitable UDP address found");
    }

    int open_udp_socket(const ResolvedEndpoint& ep) {
        int fd = ::socket(ep.family, ep.socktype, ep.protocol);
        if (fd < 0) {
            throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
        }

        int sndbuf = 4 * 1024 * 1024;
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0) {
            // Non-fatal
        }

        return fd;
    }

    void print_config(const Args& args, const ResolvedEndpoint& ep) {
        std::cout << "=== Effective Configuration ===\n";
        std::cout << "host            : " << args.host << "\n";
        std::cout << "resolved_host   : " << ep.numeric_host << "\n";
        std::cout << "port            : " << args.port << "\n";
        std::cout << "payload_size    : " << args.size << "\n";
        std::cout << "duration        : " << args.duration << "\n";
        std::cout << "count           : " << args.count << "\n";
        std::cout << "pps             : " << args.pps << "\n";
        std::cout << "connect         : " << (args.connect_socket ? "true" : "false") << "\n";
        std::cout << "================================\n\n";
    }

}  // namespace

int main(int argc, char* argv[]) {
    int sock = -1;

    try {
        ::signal(SIGINT, handle_signal);
        ::signal(SIGTERM, handle_signal);

        const Args args = parse_args(argc, argv);
        const ResolvedEndpoint ep = resolve_udp_endpoint(args.host, args.port);

        print_config(args, ep);

        const std::vector<char> payload(args.size, 'x');

        sock = open_udp_socket(ep);
        if (args.connect_socket) {
            if (::connect(sock, reinterpret_cast<const sockaddr*>(&ep.addr), ep.addrlen) != 0) {
                int saved_errno = errno;
                ::close(sock);
                sock = -1;
                throw std::runtime_error(std::string("connect failed: ") + std::strerror(saved_errno));
            }
        }

        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        auto next_report = start + std::chrono::duration<double>(args.report_interval);
        auto next_send_time = start;

        std::uint64_t sent_packets = 0;
        std::uint64_t sent_bytes = 0;

        std::uint64_t send_failures = 0;
        std::uint64_t conn_refused = 0;
        std::uint64_t enobufs = 0;
        std::uint64_t other_errors = 0;

        const bool use_count = (args.count > 0);
        const auto end_time = start + std::chrono::duration<double>(args.duration);

        while (!stop_requested) {
            const auto now = clock::now();

            if (use_count) {
                if (sent_packets >= args.count) {
                    break;
                }
            } else {
                if (now >= end_time) {
                    break;
                }
            }

            if (args.pps > 0.0 && now < next_send_time) {
                auto sleep_dur = next_send_time - now;
                const auto max_sleep = std::chrono::milliseconds(1);
                if (sleep_dur > max_sleep) {
                    sleep_dur = max_sleep;
                }
                std::this_thread::sleep_for(sleep_dur);
                continue;
            }

            ssize_t n = -1;
            if (args.connect_socket) {
                n = ::send(sock, payload.data(), payload.size(), 0);
            } else {
                n = ::sendto(
                        sock,
                        payload.data(),
                        payload.size(),
                        0,
                        reinterpret_cast<const sockaddr*>(&ep.addr),
                        ep.addrlen
                );
            }

            if (n < 0) {
                ++send_failures;

                const int e = errno;
                if (e == ECONNREFUSED) {
                    ++conn_refused;
                } else if (e == ENOBUFS) {
                    ++enobufs;
                } else {
                    ++other_errors;
                }

                static int printed = 0;
                if (printed < 16) {
                    std::cerr << "[TX-error] errno=" << e
                              << " message=" << std::strerror(e) << "\n";
                    ++printed;
                }

                if (args.pps > 0.0) {
                    next_send_time += std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double>(1.0 / args.pps)
                    );

                    if (next_send_time < now - std::chrono::seconds(1)) {
                        next_send_time = now;
                    }
                }

                continue;
            }

            ++sent_packets;
            sent_bytes += static_cast<std::uint64_t>(n);

            if (args.pps > 0.0) {
                next_send_time += std::chrono::duration_cast<clock::duration>(
                        std::chrono::duration<double>(1.0 / args.pps)
                );

                if (next_send_time < now - std::chrono::seconds(1)) {
                    next_send_time = now;
                }
            }

            if (now >= next_report) {
                const double elapsed =
                        std::chrono::duration<double>(now - start).count();

                std::cout
                        << "[TX-running] elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s "
                        << "packets=" << sent_packets << " "
                        << "bytes=" << sent_bytes << " "
                        << "pps=" << std::setprecision(0) << (elapsed > 0.0 ? sent_packets / elapsed : 0.0) << " "
                        << "Mb/s=" << std::setprecision(3) << (elapsed > 0.0 ? (sent_bytes * 8.0 / elapsed) / 1e6 : 0.0) << " "
                        << "send_failures=" << send_failures << " "
                        << "conn_refused=" << conn_refused << " "
                        << "enobufs=" << enobufs << " "
                        << "other_errors=" << other_errors
                        << "\n";

                next_report += std::chrono::duration<double>(args.report_interval);
            }
        }

        const double elapsed =
                std::chrono::duration<double>(clock::now() - start).count();

        std::cout << "\n=== FINAL TX STATS ===\n";
        std::cout << "elapsed_seconds : " << std::fixed << std::setprecision(6) << elapsed << "\n";
        std::cout << "packets         : " << sent_packets << "\n";
        std::cout << "bytes           : " << sent_bytes << "\n";
        std::cout << "packets/sec     : " << std::setprecision(0)
                  << (elapsed > 0.0 ? sent_packets / elapsed : 0.0) << "\n";
        std::cout << "Mb/s            : " << std::setprecision(3)
                  << (elapsed > 0.0 ? (sent_bytes * 8.0 / elapsed) / 1e6 : 0.0) << "\n";
        std::cout << "send_failures   : " << send_failures << "\n";
        std::cout << "conn_refused    : " << conn_refused << "\n";
        std::cout << "enobufs         : " << enobufs << "\n";
        std::cout << "other_errors    : " << other_errors << "\n";

        ::close(sock);
        return 0;
    } catch (const std::exception& e) {
        if (sock >= 0) {
            ::close(sock);
        }
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}