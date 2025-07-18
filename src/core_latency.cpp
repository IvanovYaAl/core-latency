#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

using clock_type = std::chrono::steady_clock;

struct Stats {
    double median_ns;
    double p90_ns;
    double p95_ns;
};

static void pin_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::cerr << "pthread_setaffinity_np(" << cpu << ") failed: "
                  << std::strerror(errno) << "\n";
        std::exit(EXIT_FAILURE);
    }
}

static Stats measure_pair(int src, int dst, int lines, std::size_t iters) {
    alignas(64) std::atomic<uint64_t> line1{0};
    alignas(64) std::atomic<uint64_t> line2{0};

    std::vector<uint64_t> samples;
    samples.reserve(iters);

    std::thread listener([&] {
        pin_thread(dst);
        uint64_t expect = 1;
        for (std::size_t i = 0; i < iters; ++i, ++expect) {
            while (line1.load(std::memory_order_acquire) != expect) {
                /* spin */
            }
            if (lines == 2) {
                while (line2.load(std::memory_order_acquire) != expect) {
                    /* spin */
                }
            }
            line1.store(0, std::memory_order_release);
        }
    });

    pin_thread(src);
    for (uint64_t seq = 1; seq <= iters; ++seq) {
        auto t0 = clock_type::now();

        line1.store(seq, std::memory_order_release);
        if (lines == 2) line2.store(seq, std::memory_order_release);

        while (line1.load(std::memory_order_acquire) != 0) {
            /* spin */
        }

        auto t1 = clock_type::now();
        uint64_t rtt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(rtt_ns / 2); // half RTT = one‑way latency
    }

    listener.join();

    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(p * samples.size());
        idx = std::min(idx, samples.size() - 1);
        return static_cast<double>(samples[idx]);
    };

    return { pct(0.5), pct(0.90), pct(0.95) };
}

int main(int argc, char** argv) {
    std::size_t iters = 1'000'000;
    if (argc > 1) iters = std::stoull(argv[1]);

    int n_cpu = ::sysconf(_SC_NPROCESSORS_ONLN);

    std::cout << "src,dst,lines,median_ns,p90_ns,p95_ns\n";
    
    for (int src = 0; src < n_cpu; ++src) {
        for (int dst = 0; dst < n_cpu; ++dst) {
            if (src == dst) continue;
            for (int lines : {1, 2}) {
                Stats s = measure_pair(src, dst, lines, iters);
                std::cout << src << ',' << dst << ',' << lines << ','
                          << std::fixed << std::setprecision(1)
                          << s.median_ns << ',' << s.p90_ns << ',' << s.p95_ns << "\n";
            }
        }
    }
    return 0;
}

