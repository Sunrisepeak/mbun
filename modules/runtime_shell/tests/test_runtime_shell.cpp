import std;
import mbun.runtime_shell;

namespace {
int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_command_model() {
    mbun::runtime_shell::Command command { .program = "echo", .arguments = { "hello", "world" } };
    const auto argv = command.argv();
    check(command.valid(), "a program makes a command valid");
    check(argv.size() == 3 && argv[0] == "echo" && argv[2] == "world", "argv preserves order");
}

void test_injected_backend_and_output() {
    mbun::runtime_shell::ShellBinding binding { mbun::runtime_shell::Backend {
        [](const mbun::runtime_shell::Command& command)
            -> std::expected<mbun::runtime_shell::CommandOutput, mbun::runtime_shell::ShellError> {
            return mbun::runtime_shell::CommandOutput {
                .stdout_text = command.program + " output", .stderr_text = {}, .exit_code = 0
            };
        }
    } };
    const auto result = binding.run({ .program = "echo" });
    check(result.has_value() && result->succeeded(), "backend result reports success");
    check(result->stdout_text == "echo output", "backend receives the command");
}

void test_errors_and_deferred_backend() {
    mbun::runtime_shell::ShellBinding deferred { mbun::runtime_shell::deferred_backend() };
    const auto invalid = deferred.run({});
    check(!invalid.has_value() && invalid.error().kind == mbun::runtime_shell::ErrorKind::InvalidCommand,
          "empty command is rejected before backend dispatch");
    const auto unavailable = deferred.run({ .program = "echo" });
    check(!unavailable.has_value()
              && unavailable.error().kind == mbun::runtime_shell::ErrorKind::BackendUnavailable,
          "native backend remains an explicit deferred error");
}
} // namespace

int main() {
    test_command_model();
    test_injected_backend_and_output();
    test_errors_and_deferred_backend();
    std::println("runtime_shell: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
