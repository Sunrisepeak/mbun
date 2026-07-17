import std;
import mbun.windows_sys;

namespace {
int checks{0};
int failures{0};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL: {}", message); }
}
void test_handle_contract() {
    using namespace mbun::windows_sys;
    Handle handle{42, HandleKind::File};
    check(handle.valid(), "constructed handle is valid");
    check(handle.kind() == HandleKind::File, "handle preserves its kind");
    const auto raw{handle.release()};
    check(raw == 42 && !handle.valid(), "release transfers and invalidates the raw handle");
    handle.reset(7, HandleKind::Process);
    check(handle.valid() && handle.kind() == HandleKind::Process, "reset installs a new handle");
}
void test_error_mapping() {
    using namespace mbun::windows_sys;
    check(classify_error(2) == ErrorKind::NotFound, "ERROR_FILE_NOT_FOUND maps to not found");
    check(classify_error(5) == ErrorKind::AccessDenied, "ERROR_ACCESS_DENIED maps to access denied");
    check(classify_error(87) == ErrorKind::InvalidArgument, "ERROR_INVALID_PARAMETER maps to invalid argument");
    check(classify_error(9999) == ErrorKind::Unknown, "unknown Win32 errors stay unknown");
}
void test_path_contract() {
    using namespace mbun::windows_sys;
    check(is_absolute_path(u"C:\\tmp"), "drive-root path is absolute");
    check(is_absolute_path(u"\\\\server\\share"), "UNC path is absolute");
    check(!is_absolute_path(u"relative\\file"), "relative path is not absolute");
    check(has_extended_prefix(u"\\\\?\\C:\\long"), "extended path prefix is recognized");
    check(preferred_separator() == u'\\', "Windows separator is backslash");
}
void test_process_is_deferred() {
    using namespace mbun::windows_sys;
    const std::array<std::u16string_view, 4> arguments{u"cmd.exe", u"/c", u"echo", u"x"};
    const ProcessRequest request{u"cmd.exe", arguments, {}};
    const auto result{launch_deferred(request)};
    check(!result.has_value(), "process launch is deferred");
    check(result.error().kind == ErrorKind::NotSupported, "deferred launch reports not supported");
}
} // namespace
int main() {
    test_handle_contract(); test_error_mapping(); test_path_contract(); test_process_is_deferred();
    std::println("windows_sys: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
