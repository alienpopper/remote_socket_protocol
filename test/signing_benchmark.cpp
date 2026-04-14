#include "common/message_queue/mq_signing.hpp"
#include "common/keypair.hpp"
#include "common/service_message.hpp"
#include "resource_service/bsd_sockets/bsd_sockets.pb.h"

#include <chrono>
#include <cstring>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace {

rsp::proto::NodeId makeProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoId.set_value(value);
    return protoId;
}

rsp::proto::RSPMessage makePingMessage(const rsp::NodeID& source, const rsp::NodeID& dest) {
    rsp::proto::RSPMessage msg;
    *msg.mutable_source() = makeProtoNodeId(source);
    *msg.mutable_destination() = makeProtoNodeId(dest);
    msg.mutable_ping_request()->mutable_nonce()->set_value(std::string(16, '\x42'));
    msg.mutable_ping_request()->set_sequence(1);
    return msg;
}

rsp::proto::RSPMessage makeServiceMessage(const rsp::NodeID& source, const rsp::NodeID& dest) {
    rsp::proto::RSPMessage msg;
    *msg.mutable_source() = makeProtoNodeId(source);
    *msg.mutable_destination() = makeProtoNodeId(dest);
    msg.mutable_nonce()->set_value(std::string(16, '\xAB'));

    rsp::proto::ConnectTCPRequest req;
    req.set_host_port("127.0.0.1:8080");
    req.mutable_socket_number()->set_value(std::string(16, '\xCD'));
    req.set_timeout_ms(5000);
    rsp::packServiceMessage(msg, req);
    return msg;
}

struct BenchmarkResult {
    const char* name;
    int iterations;
    double totalSeconds;

    double opsPerSecond() const { return iterations / totalSeconds; }
    double usPerOp() const { return (totalSeconds * 1e6) / iterations; }
};

void printResult(const BenchmarkResult& r) {
    std::cout << std::left << std::setw(45) << r.name
              << std::right << std::setw(10) << r.iterations << " ops  "
              << std::fixed << std::setprecision(1)
              << std::setw(10) << r.opsPerSecond() << " ops/s  "
              << std::setw(8) << r.usPerOp() << " us/op"
              << std::endl;
}

// Run a benchmark for at least minSeconds, return result.
template <typename Func>
BenchmarkResult benchmark(const char* name, double minSeconds, Func&& func) {
    // Warmup
    for (int i = 0; i < 10; ++i) func();

    int iterations = 0;
    const auto start = std::chrono::steady_clock::now();
    double elapsed = 0.0;

    while (elapsed < minSeconds) {
        func();
        ++iterations;
        const auto now = std::chrono::steady_clock::now();
        elapsed = std::chrono::duration<double>(now - start).count();
    }

    return {name, iterations, elapsed};
}

}  // namespace

int main() {
    const auto signerKey = rsp::KeyPair::generateP256();
    const auto verifierKey = rsp::KeyPair::fromPublicKey(signerKey.publicKey());
    const auto destKey = rsp::KeyPair::generateP256();

    const auto pingMsg = makePingMessage(signerKey.nodeID(), destKey.nodeID());
    const auto serviceMsg = makeServiceMessage(signerKey.nodeID(), destKey.nodeID());

    // Pre-compute hashes and signatures for verify benchmarks
    const auto pingHash = rsp::computeMessageHash(pingMsg);
    const auto serviceHash = rsp::computeMessageHash(serviceMsg);
    const rsp::Buffer pingHashBuf(pingHash.data(), static_cast<uint32_t>(pingHash.size()));
    const rsp::Buffer serviceHashBuf(serviceHash.data(), static_cast<uint32_t>(serviceHash.size()));
    const auto pingSignature = signerKey.sign(pingHashBuf);
    const auto serviceSignature = signerKey.sign(serviceHashBuf);

    constexpr double MIN_SECONDS = 2.0;

    std::cout << "RSP Signing Benchmark" << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    // --- Key generation ---
    printResult(benchmark("P256 key generation", MIN_SECONDS, []() {
        auto k = rsp::KeyPair::generateP256();
        (void)k;
    }));

    // --- Hashing ---
    printResult(benchmark("hash ping message", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(pingMsg);
        (void)h;
    }));

    printResult(benchmark("hash service message (Any reflection)", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(serviceMsg);
        (void)h;
    }));

    // --- Signing ---
    printResult(benchmark("sign ping hash", MIN_SECONDS, [&]() {
        auto sig = signerKey.sign(pingHashBuf);
        (void)sig;
    }));

    printResult(benchmark("sign service message hash", MIN_SECONDS, [&]() {
        auto sig = signerKey.sign(serviceHashBuf);
        (void)sig;
    }));

    // --- Verification ---
    printResult(benchmark("verify ping signature", MIN_SECONDS, [&]() {
        bool ok = verifierKey.verify(pingHashBuf, pingSignature);
        (void)ok;
    }));

    printResult(benchmark("verify service message signature", MIN_SECONDS, [&]() {
        bool ok = verifierKey.verify(serviceHashBuf, serviceSignature);
        (void)ok;
    }));

    // --- Combined: hash + sign (what the sender does) ---
    printResult(benchmark("hash + sign ping (sender path)", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(pingMsg);
        rsp::Buffer hb(h.data(), static_cast<uint32_t>(h.size()));
        auto sig = signerKey.sign(hb);
        (void)sig;
    }));

    printResult(benchmark("hash + sign service msg (sender path)", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(serviceMsg);
        rsp::Buffer hb(h.data(), static_cast<uint32_t>(h.size()));
        auto sig = signerKey.sign(hb);
        (void)sig;
    }));

    // --- Combined: hash + verify (what the RM does) ---
    printResult(benchmark("hash + verify ping (RM path)", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(pingMsg);
        rsp::Buffer hb(h.data(), static_cast<uint32_t>(h.size()));
        bool ok = verifierKey.verify(hb, pingSignature);
        (void)ok;
    }));

    printResult(benchmark("hash + verify service msg (RM path)", MIN_SECONDS, [&]() {
        auto h = rsp::computeMessageHash(serviceMsg);
        rsp::Buffer hb(h.data(), static_cast<uint32_t>(h.size()));
        bool ok = verifierKey.verify(hb, serviceSignature);
        (void)ok;
    }));

    std::cout << std::string(85, '-') << std::endl;

    // =====================================================================
    // Multi-core scaling test
    // =====================================================================
    const unsigned int hwThreads = std::thread::hardware_concurrency();
    const std::vector<unsigned int> threadCounts = {1, 2, 4, 8, hwThreads};

    // Deduplicate thread counts and skip those > hwThreads
    std::vector<unsigned int> uniqueThreadCounts;
    for (auto tc : threadCounts) {
        if (tc == 0 || tc > hwThreads) continue;
        bool dup = false;
        for (auto u : uniqueThreadCounts) { if (u == tc) { dup = true; break; } }
        if (!dup) uniqueThreadCounts.push_back(tc);
    }

    std::cout << std::endl;
    std::cout << "Multi-core Scaling (" << hwThreads << " hardware threads)" << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    // Each thread gets its own key pair (no shared mutable state).
    // We measure aggregate ops/s across all threads.
    auto multiCoreBenchmark = [&](const char* label, unsigned int numThreads,
                                  double seconds) {
        // Prepare per-thread key pairs
        std::vector<rsp::KeyPair> keys;
        std::vector<rsp::KeyPair> verifyKeys;
        std::vector<rsp::Buffer> signatures;
        for (unsigned int i = 0; i < numThreads; ++i) {
            auto k = rsp::KeyPair::generateP256();
            auto vk = rsp::KeyPair::fromPublicKey(k.publicKey());
            auto sig = k.sign(pingHashBuf);
            keys.push_back(std::move(k));
            verifyKeys.push_back(std::move(vk));
            signatures.push_back(std::move(sig));
        }

        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::vector<int64_t> counts(numThreads, 0);
        std::vector<std::thread> threads;

        for (unsigned int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) {}  // spin-wait for start
                int64_t localCount = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    auto h = rsp::computeMessageHash(pingMsg);
                    rsp::Buffer hb(h.data(), static_cast<uint32_t>(h.size()));
                    bool ok = verifyKeys[t].verify(hb, signatures[t]);
                    (void)ok;
                    ++localCount;
                }
                counts[t] = localCount;
            });
        }

        // Start all threads
        const auto start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);

        // Let them run
        auto deadline = start + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);

        for (auto& th : threads) th.join();
        const auto end = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(end - start).count();

        int64_t totalOps = 0;
        for (auto c : counts) totalOps += c;

        double opsPerSec = totalOps / elapsed;
        std::cout << std::left << std::setw(45) << label
                  << std::right << std::setw(3) << numThreads << " thr  "
                  << std::fixed << std::setprecision(1)
                  << std::setw(10) << opsPerSec << " ops/s"
                  << std::endl;
        return opsPerSec;
    };

    double baseline = 0.0;
    for (auto tc : uniqueThreadCounts) {
        std::string label = "hash + verify (RM path)";
        double ops = multiCoreBenchmark(label.c_str(), tc, MIN_SECONDS);
        if (tc == 1) baseline = ops;
    }

    std::cout << std::endl;
    std::cout << "Scaling efficiency:" << std::endl;
    for (auto tc : uniqueThreadCounts) {
        if (tc == 1) continue;
        std::string label = "hash + verify (RM path)";
        double ops = multiCoreBenchmark(label.c_str(), tc, MIN_SECONDS);
        double speedup = ops / baseline;
        double efficiency = speedup / tc * 100.0;
        std::cout << "  " << tc << " threads: "
                  << std::fixed << std::setprecision(2) << speedup << "x speedup, "
                  << std::setprecision(1) << efficiency << "% efficiency"
                  << std::endl;
    }

    std::cout << std::string(85, '-') << std::endl;
    return 0;
}
