// mbun.core.io positioned descriptor I/O benchmark driver.
// Usage: bench-core-io [minimum-round-ms] [rounds]. Output is one JSON line.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

import std;
import mbun.core.io;

namespace io = mbun::core::io;

namespace {

constexpr std::array<std::size_t, 2> SIZES{4 * 1024, 1024 * 1024};
constexpr int CASE_COUNT{16};

class AlignedBuffer {
private:
    std::byte* data_{nullptr};
    std::size_t size_{0};
    std::size_t capacity_{0};
public:
    explicit AlignedBuffer(std::size_t size = 0, std::size_t alignment = 4096,
                           bool adviseHuge = false)
        : size_{size} {
        capacity_ = size == 0 ? 0 : ((size + alignment - 1) / alignment) * alignment;
        data_ =
            size == 0 ? nullptr : static_cast<std::byte*>(std::aligned_alloc(alignment, capacity_));
        if (size != 0 && data_ == nullptr) {
            throw std::bad_alloc{};
        }
#if defined(MADV_HUGEPAGE)
        if (adviseHuge && capacity_ >= 2 * 1024 * 1024) {
            (void)::madvise(data_, capacity_, MADV_HUGEPAGE);
        }
#endif
    }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_{std::exchange(other.data_, nullptr)},
          size_{std::exchange(other.size_, 0)},
          capacity_{std::exchange(other.capacity_, 0)} {}
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            std::free(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }
    ~AlignedBuffer() {
        std::free(data_);
    }
    [[nodiscard]] std::byte* data() noexcept {
        return data_;
    }
    [[nodiscard]] const std::byte* data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::span<std::byte> span() noexcept {
        return {data_, size_};
    }
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {data_, size_};
    }
    std::byte& operator[](std::size_t index) noexcept {
        return data_[index];
    }
};

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct SizeState {
    AlignedBuffer payload;
    AlignedBuffer buffer;
    AlignedBuffer pagePayload;
    AlignedBuffer pageBuffer;
    std::vector<std::byte> vectorPayload;
    std::vector<std::byte> vectorBuffer;
    io::File reader;
    io::File writer;
    int rawReader{-1};
    int rawWriter{-1};

    SizeState() = default;
    SizeState(const SizeState&) = delete;
    SizeState& operator=(const SizeState&) = delete;
    SizeState(SizeState&& other) noexcept
        : payload{std::move(other.payload)},
          buffer{std::move(other.buffer)},
          pagePayload{std::move(other.pagePayload)},
          pageBuffer{std::move(other.pageBuffer)},
          vectorPayload{std::move(other.vectorPayload)},
          vectorBuffer{std::move(other.vectorBuffer)},
          reader{std::move(other.reader)},
          writer{std::move(other.writer)},
          rawReader{std::exchange(other.rawReader, -1)},
          rawWriter{std::exchange(other.rawWriter, -1)} {}

    ~SizeState() {
        if (rawReader >= 0) {
            (void)::close(rawReader);
        }
        if (rawWriter >= 0) {
            (void)::close(rawWriter);
        }
    }
};

struct CaseResult {
    std::string_view name;
    std::uint64_t iterations{0};
    std::vector<std::int64_t> roundNs;
    std::uint64_t checksum{0};
};

[[noreturn]] void fail(std::string_view operation) {
    std::println(stderr, "bench-core-io: {} failed (errno={})", operation, errno);
    std::exit(2);
}

SizeState make_state(const std::filesystem::path& directory, std::size_t size) {
    SizeState state;
    const std::size_t hugeAlignment{size >= 1024 * 1024 ? std::size_t{2 * 1024 * 1024}
                                                        : std::size_t{4096}};
    state.payload = AlignedBuffer{size, hugeAlignment, true};
    state.buffer = AlignedBuffer{size, hugeAlignment, true};
    state.pagePayload = AlignedBuffer{size, 4096, false};
    state.pageBuffer = AlignedBuffer{size, 4096, false};
    state.vectorPayload.resize(size);
    state.vectorBuffer.resize(size);
    for (std::size_t index{0}; index < size; ++index) {
        state.payload[index] = static_cast<std::byte>((index * 131U + 17U) & 0xffU);
        state.vectorPayload[index] = state.payload[index];
        state.pagePayload[index] = state.payload[index];
    }
    const auto input{directory / std::format("input-{}.bin", size)};
    const auto output{directory / std::format("output-{}.bin", size)};
    if (!io::write_file(input, state.payload.span())) {
        fail("seed input");
    }
    auto reader{io::File::open(input)};
    auto writer{io::File::open(output, io::OpenOptions{.access = io::Access::ReadWrite,
                                                       .create = true,
                                                       .truncate = true})};
    if (!reader || !writer) {
        fail("open mbun descriptors");
    }
    state.reader = std::move(*reader);
    state.writer = std::move(*writer);
    state.rawReader = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    state.rawWriter = ::open(output.c_str(), O_RDWR | O_CLOEXEC);
    if (state.rawReader < 0 || state.rawWriter < 0) {
        fail("open raw descriptors");
    }
    return state;
}

std::uint64_t run_case_once(int caseIndex, SizeState& state, std::uint64_t iterations) {
    const bool pageAligned{caseIndex >= 4 && caseIndex < 8};
    const bool unaligned{caseIndex >= 8 && caseIndex < 12};
    const bool raw{caseIndex >= 12};
    const bool write{caseIndex % 4 >= 2};
    const std::span<const std::byte> payload{unaligned
                                                 ? std::span<const std::byte>{state.vectorPayload}
                                             : pageAligned ? state.pagePayload.span()
                                                           : state.payload.span()};
    const std::span<std::byte> buffer{unaligned     ? std::span<std::byte>{state.vectorBuffer}
                                      : pageAligned ? state.pageBuffer.span()
                                                    : state.buffer.span()};
    std::uint64_t checksum{0};
    for (std::uint64_t iteration{0}; iteration < iterations; ++iteration) {
        if (write) {
            if (raw) {
                const ssize_t amount{::pwrite(state.rawWriter, payload.data(), payload.size(), 0)};
                if (amount != static_cast<ssize_t>(payload.size())) {
                    fail("raw pwrite");
                }
                checksum += static_cast<std::uint64_t>(amount);
            } else {
                auto amount{state.writer.write_at(0, payload)};
                if (!amount || *amount != payload.size()) {
                    fail("mbun write_at");
                }
                checksum += *amount;
            }
        } else {
            std::size_t amount{0};
            if (raw) {
                const ssize_t rawAmount{::pread(state.rawReader, buffer.data(), buffer.size(), 0)};
                if (rawAmount != static_cast<ssize_t>(buffer.size())) {
                    fail("raw pread");
                }
                amount = static_cast<std::size_t>(rawAmount);
            } else {
                auto mbunAmount{state.reader.read_at(0, buffer)};
                if (!mbunAmount || *mbunAmount != buffer.size()) {
                    fail("mbun read_at");
                }
                amount = *mbunAmount;
            }
            checksum += amount;
            checksum += std::to_integer<unsigned char>(buffer.front());
            checksum += std::to_integer<unsigned char>(buffer.back());
        }
    }
    return checksum;
}

std::int64_t measure(int caseIndex, SizeState& state, std::uint64_t iterations,
                     std::uint64_t& checksum) {
    const auto start{now_ns()};
    checksum += run_case_once(caseIndex, state, iterations);
    return now_ns() - start;
}

std::uint64_t calibrate(int caseIndex, SizeState& state, std::int64_t minimumNs,
                        std::uint64_t& checksum) {
    std::uint64_t iterations{caseIndex % 2 == 0 ? 4096ULL : 32ULL};
    for (;;) {
        const std::int64_t elapsed{measure(caseIndex, state, iterations, checksum)};
        if (elapsed >= minimumNs) {
            return iterations;
        }
        const double scale{std::clamp(static_cast<double>(minimumNs) /
                                          static_cast<double>(std::max<std::int64_t>(elapsed, 1)),
                                      1.25, 8.0)};
        const auto next{static_cast<std::uint64_t>(static_cast<double>(iterations) * scale)};
        iterations = std::max(iterations + 1, next);
    }
}

void print_result(const std::array<CaseResult, CASE_COUNT>& results,
                  const std::array<SizeState, 2>& states, std::int64_t minimumNs, int rounds) {
    auto modulo = [](const void* pointer, std::uintptr_t alignment) {
        return reinterpret_cast<std::uintptr_t>(pointer) % alignment;
    };
    std::print(
        R"({{"impl":"mbun 0.1.0","profile":"release-o3","minimum_round_ns":{},"rounds":{},"alignment":{{"huge_buffer_4k_mod_2m":{},"huge_buffer_1m_mod_2m":{},"huge_payload_1m_mod_2m":{},"page_buffer_4k_mod_4k":{},"page_buffer_1m_mod_4k":{},"page_payload_1m_mod_4k":{},"vector_buffer_4k_mod_4k":{},"vector_buffer_1m_mod_4k":{},"vector_payload_1m_mod_4k":{}}},"cases":{{)",
        minimumNs, rounds, modulo(states[0].buffer.data(), 2 * 1024 * 1024),
        modulo(states[1].buffer.data(), 2 * 1024 * 1024),
        modulo(states[1].payload.data(), 2 * 1024 * 1024),
        modulo(states[0].pageBuffer.data(), 4096), modulo(states[1].pageBuffer.data(), 4096),
        modulo(states[1].pagePayload.data(), 4096), modulo(states[0].vectorBuffer.data(), 4096),
        modulo(states[1].vectorBuffer.data(), 4096), modulo(states[1].vectorPayload.data(), 4096));
    for (std::size_t index{0}; index < results.size(); ++index) {
        const auto& result{results[index]};
        if (index != 0) {
            std::print(",");
        }
        std::print(R"("{}":{{"iterations":{},"round_ns":[)", result.name, result.iterations);
        for (std::size_t round{0}; round < result.roundNs.size(); ++round) {
            std::print("{}{}", round == 0 ? "" : ",", result.roundNs[round]);
        }
        std::print(R"(],"checksum":{}}})", result.checksum);
    }
    std::println("}}}}");
}

}  // namespace

int main(int argc, char* argv[]) {
    const int minimumMs{argc > 1 ? std::max(250, std::atoi(argv[1])) : 250};
    const int rounds{argc > 2 ? std::max(10, std::atoi(argv[2])) : 10};
    const std::int64_t minimumNs{static_cast<std::int64_t>(minimumMs) * 1'000'000};
    std::error_code error;
    const auto nonce{std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto directory{std::filesystem::temp_directory_path(error) /
                         std::format("mbun-io-bench-{}", nonce)};
    if (error || !std::filesystem::create_directory(directory, error)) {
        fail("create temporary directory");
    }

    std::array<SizeState, 2> states{make_state(directory, SIZES[0]),
                                    make_state(directory, SIZES[1])};
    constexpr std::array<std::string_view, CASE_COUNT>
        NAMES{"mbun_huge_read_4k",   "mbun_huge_read_1m",    "mbun_huge_write_4k",
              "mbun_huge_write_1m",  "mbun_page_read_4k",    "mbun_page_read_1m",
              "mbun_page_write_4k",  "mbun_page_write_1m",   "mbun_vector_read_4k",
              "mbun_vector_read_1m", "mbun_vector_write_4k", "mbun_vector_write_1m",
              "raw_huge_read_4k",    "raw_huge_read_1m",     "raw_huge_write_4k",
              "raw_huge_write_1m"};
    std::array<CaseResult, CASE_COUNT> results{};
    for (int index{0}; index < CASE_COUNT; ++index) {
        results[index].name = NAMES[index];
        results[index].roundNs.reserve(rounds);
        results[index].iterations =
            calibrate(index, states[index % 2], minimumNs, results[index].checksum);
    }

    std::array<int, CASE_COUNT> order{};
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 random{0x6d62756eU};
    for (int round{0}; round < rounds; ++round) {
        std::shuffle(order.begin(), order.end(), random);
        for (const int index : order) {
            auto& result{results[index]};
            result.roundNs.push_back(
                measure(index, states[index % 2], result.iterations, result.checksum));
        }
    }
    print_result(results, states, minimumNs, rounds);
    std::filesystem::remove_all(directory, error);
    return 0;
}
