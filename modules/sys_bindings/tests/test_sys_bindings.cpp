import std;
import mbun.sys_bindings;

namespace {

int checks {};
int failures {};

template <typename Actual, typename Expected>
void eq(const Actual& actual, const Expected& expected, std::string_view label) {
    ++checks;
    if (!(actual == expected)) {
        ++failures;
        std::println("FAIL: {}", label);
    }
}

void test_stable_libuv_errors() {
    using mbun::libuv_sys::Error;
    eq(std::to_underlying(Error::again), -4088, "UV_EAGAIN is stable");
    eq(std::to_underlying(Error::canceled), -4081, "UV_ECANCELED is stable");
    eq(std::to_underlying(Error::invalid_argument), -4071, "UV_EINVAL is stable");
    eq(std::to_underlying(Error::no_entry), -4058, "UV_ENOENT is stable");
    eq(std::to_underlying(Error::permission_denied), -4092, "UV_EACCES is stable");
    eq(std::to_underlying(Error::timed_out), -4039, "UV_ETIMEDOUT is stable");
    eq(std::to_underlying(Error::unknown), -4094, "UV_UNKNOWN is stable");
    eq(std::to_underlying(Error::eof), -4095, "UV_EOF is stable");
}

void test_errno_categories_remain_distinct() {
    using namespace mbun::sys_bindings::errno_;
    const auto canceled { classify(std::errc::operation_canceled) };
    eq(canceled, Category::canceled, "operation_canceled has its own category");
    eq(classify(std::errc::resource_unavailable_try_again), Category::again,
       "resource_unavailable_try_again maps to again");
}

void test_jsc_numeric_boundaries() {
    using namespace mbun::sys_jsc;
    const auto signal { parse_signal_number(std::numeric_limits<double>::quiet_NaN()) };
    eq(signal.has_value(), true, "NaN selects the default signal");
    eq(signal ? std::to_underlying(*signal) : std::uint8_t {}, std::uint8_t { 15 },
       "default signal is SIGTERM 15");

    constexpr auto maxFd { std::numeric_limits<std::int32_t>::max() };
    eq(FdJsc::from_js_integer(maxFd).value_or(FdJsc::INVALID), maxFd,
       "FdJsc accepts int32 max");
    eq(FdJsc::from_js_integer(static_cast<std::int64_t>(maxFd) + 1), std::nullopt,
       "FdJsc rejects input above int32 max");
    eq(FdJsc::to_js(static_cast<std::int64_t>(maxFd) + 1), FdJsc::INVALID,
       "FdJsc rejects output above int32 max");
}

} // namespace

int main() {
    test_stable_libuv_errors();
    test_errno_categories_remain_distinct();
    test_jsc_numeric_boundaries();
    std::println("sys_bindings: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
