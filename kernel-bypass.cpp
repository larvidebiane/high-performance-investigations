//
// Created Larvi DEBIANE on 31/03/2026.
// Investigation on Kernel Bypass
//

#include <arpa/inet.h>
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
#include <string>
#include <vector>

namespace {

// Paramètres par défaut
    constexpr int DEFAULT_PORT = 9000;
    constexpr int DEFAULT_DURATION_SECONDS = 10;
    constexpr std::size_t DEFAULT_BUFFER_SIZE = 2048;
    constexpr int SELECT_TIMEOUT_MS = 200;

// Résultats du benchmark
    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
        double elapsed_seconds = 0.0;
    };

// Parse minimal des arguments
    void parse_args(int argc, char** argv,
                    int& port,
                    int& duration_seconds,
                    std::size_t& buffer_size) {
        if (argc >= 2) {
            port = std::stoi(argv[1]);
        }
        if (argc >= 3) {
            duration_seconds = std::stoi(argv[2]);
        }
        if (argc >= 4) {
            buffer_size = static_cast<std::size_t>(std::stoul(argv[3]));
        }

        if (port <= 0 || port > 65535) {
            throw std::runtime_error("invalid port");
        }
        if (duration_seconds <= 0) {
            throw std::runtime_error("duration must be > 0");
        }
        if (buffer_size == 0) {
            throw std::runtime_error("buffer_size must be > 0");
        }
    }

// Création du socket UDP + bind
    int create_udp_socket(int port) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            std::perror("socket");
            return -1;
        }

        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            std::perror("setsockopt(SO_REUSEADDR)");
            ::close(fd);
            return -1;
        }

        // Tente d'augmenter le buffer noyau de réception
        int rcvbuf = 4 * 1024 * 1024;
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0) {
            // non fatal
            std::perror("setsockopt(SO_RCVBUF)");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::perror("bind");
            ::close(fd);
            return -1;
        }

        return fd;
    }

// Attend qu'il y ait des données lisibles sur le socket
// Retourne :
//   1  -> socket lisible
//   0  -> timeout
//  -1  -> erreur
    int wait_for_readable_fd(int fd, int timeout_ms) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        const int rc = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);

        if (rc < 0) {
            if (errno == EINTR) {
                return 0; // on traite EINTR comme un mini-timeout
            }
            std::perror("select");
            return -1;
        }

        if (rc == 0) {
            return 0;
        }

        return FD_ISSET(fd, &readfds) ? 1 : 0;
    }

// Affichage d'un point intermédiaire
    void print_running_stats(const Stats& stats) {
        const double pps =
                stats.elapsed_seconds > 0.0 ? stats.packets / stats.elapsed_seconds : 0.0;
        const double mbps =
                stats.elapsed_seconds > 0.0 ? (stats.bytes * 8.0) / stats.elapsed_seconds / 1e6 : 0.0;

        std::cout << "[running] "
                  << "elapsed=" << std::fixed << std::setprecision(3) << stats.elapsed_seconds << " s, "
                  << "packets=" << stats.packets << ", "
                  << "bytes=" << stats.bytes << ", "
                  << "pps=" << static_cast<std::uint64_t>(pps) << ", "
                  << "Mb/s=" << std::setprecision(3) << mbps
                  << '\n';
    }

// Benchmark principal
    Stats run_udp_receive_benchmark(int fd,
                                    int duration_seconds,
                                    std::size_t buffer_size) {
        Stats stats{};
        std::vector<char> buffer(buffer_size);

        const auto start = std::chrono::steady_clock::now();
        auto next_report = start + std::chrono::seconds(1);
        const auto end_time = start + std::chrono::seconds(duration_seconds);

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= end_time) {
                break;
            }

            const int wait_rc = wait_for_readable_fd(fd, SELECT_TIMEOUT_MS);
            if (wait_rc < 0) {
                break;
            }
            if (wait_rc == 0) {
                // timeout, rien à lire pour l’instant
            } else {
                const ssize_t n = ::recvfrom(fd,
                                             buffer.data(),
                                             buffer.size(),
                                             0,
                                             nullptr,
                                             nullptr);

                if (n < 0) {
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::perror("recvfrom");
                        break;
                    }
                } else {
                    stats.packets += 1;
                    stats.bytes += static_cast<std::uint64_t>(n);
                }
            }

            const auto current = std::chrono::steady_clock::now();
            if (current >= next_report) {
                stats.elapsed_seconds =
                        std::chrono::duration<double>(current - start).count();
                print_running_stats(stats);
                next_report += std::chrono::seconds(1);
            }
        }

        const auto finish = std::chrono::steady_clock::now();
        stats.elapsed_seconds =
                std::chrono::duration<double>(finish - start).count();

        return stats;
    }

// Affichage final
    void print_final_stats(const Stats& stats) {
        const double pps =
                stats.elapsed_seconds > 0.0 ? stats.packets / stats.elapsed_seconds : 0.0;
        const double mbps =
                stats.elapsed_seconds > 0.0 ? (stats.bytes * 8.0) / stats.elapsed_seconds / 1e6 : 0.0;

        std::cout << "\n=== FINAL STATS ===\n";
        std::cout << "elapsed_seconds : " << std::fixed << std::setprecision(6)
                  << stats.elapsed_seconds << '\n';
        std::cout << "packets         : " << stats.packets << '\n';
        std::cout << "bytes           : " << stats.bytes << '\n';
        std::cout << "packets/sec     : " << static_cast<std::uint64_t>(pps) << '\n';
        std::cout << "Mb/s            : " << std::setprecision(3) << mbps << '\n';
    }

} // namespace

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    int duration_seconds = DEFAULT_DURATION_SECONDS;
    std::size_t buffer_size = DEFAULT_BUFFER_SIZE;

    try {
        parse_args(argc, argv, port, duration_seconds, buffer_size);
    } catch (const std::exception& e) {
        std::cerr << "Invalid arguments: " << e.what() << '\n';
        std::cerr << "Usage: " << argv[0] << " [port] [duration_seconds] [buffer_size]\n";
        return 1;
    }

    std::cout << "Starting UDP receive benchmark\n";
    std::cout << "port=" << port
              << ", duration_seconds=" << duration_seconds
              << ", buffer_size=" << buffer_size
              << '\n';

    const int fd = create_udp_socket(port);
    if (fd < 0) {
        return 1;
    }

    const Stats stats = run_udp_receive_benchmark(fd, duration_seconds, buffer_size);

    ::close(fd);

    print_final_stats(stats);
    return 0;
}