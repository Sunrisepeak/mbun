export module mbun.options.validation;

import std;
import mbun.options.cli;
import mbun.options.install;

namespace mbun::options {

export enum class ValidationError : std::uint8_t {
    None, TimeoutIsZero, ShardOutOfRange, CoverageThresholdOutOfRange,
    OfflineInstallConflict,
};

export struct ValidationResult {
    ValidationError error{ValidationError::None};
    constexpr bool ok() const noexcept { return error == ValidationError::None; }
};

export constexpr ValidationResult validate(const TestOptions& options) noexcept {
    if (options.timeout_ms == 0) return {ValidationError::TimeoutIsZero};
    if (options.shard_count != 0 &&
        (options.shard_index == 0 || options.shard_index > options.shard_count)) {
        return {ValidationError::ShardOutOfRange};
    }
    const auto in_range = [](double value) { return value >= 0.0 && value <= 1.0; };
    if (!in_range(options.coverage.minimum_functions) ||
        !in_range(options.coverage.minimum_lines) ||
        !in_range(options.coverage.minimum_statements)) {
        return {ValidationError::CoverageThresholdOutOfRange};
    }
    return {};
}

export constexpr ValidationResult validate(const InstallOptions& options) noexcept {
    if (options.offline_mode == OfflineMode::Offline &&
        global_cache_can_install(options.global_cache)) {
        return {ValidationError::OfflineInstallConflict};
    }
    return {};
}

} // namespace mbun::options
