export module mbun.crash_handler.events;

import std;

namespace mbun::crash_handler {

export enum class CrashKind {
    panic,
    unreachable,
    segmentation_fault,
    illegal_instruction,
    bus_error,
    floating_point_error,
    datatype_misalignment,
    stack_overflow,
    error,
    out_of_memory,
};

export enum class CrashSignal {
    none,
    abort,
    segmentation_fault,
    illegal_instruction,
    bus_error,
    floating_point_error,
    trap,
};

export struct CrashEvent {
    CrashKind kind { CrashKind::panic };
    std::string message {};
    std::uintptr_t address { 0 };
    CrashSignal signal { CrashSignal::none };

    static CrashEvent panic(std::string_view message) {
        return { CrashKind::panic, std::string { message }, 0, CrashSignal::abort };
    }

    static CrashEvent segmentation_fault(std::uintptr_t address) {
        return { CrashKind::segmentation_fault, {}, address, CrashSignal::segmentation_fault };
    }

    static CrashEvent out_of_memory() {
        return { CrashKind::out_of_memory, {}, 0, CrashSignal::abort };
    }
};

export enum class TraceSeedKind { none, begin_address, fault, error_return };

export struct TraceSeed {
    TraceSeedKind kind { TraceSeedKind::none };
    std::uintptr_t address { 0 };
    std::uintptr_t frame_pointer { 0 };

    static TraceSeed begin_address(std::uintptr_t address) {
        return { TraceSeedKind::begin_address, address, 0 };
    }

    static TraceSeed fault(std::uintptr_t programCounter, std::uintptr_t framePointer) {
        return { TraceSeedKind::fault, programCounter, framePointer };
    }
};

export inline std::string format_reason(const CrashEvent& event) {
    switch (event.kind) {
    case CrashKind::panic:
        return event.message;
    case CrashKind::unreachable:
        return "reached unreachable code";
    case CrashKind::segmentation_fault:
        return std::format("Segmentation fault at address 0x{:X}", event.address);
    case CrashKind::illegal_instruction:
        return std::format("Illegal instruction at address 0x{:X}", event.address);
    case CrashKind::bus_error:
        return std::format("Bus error at address 0x{:X}", event.address);
    case CrashKind::floating_point_error:
        return std::format("Floating point error at address 0x{:X}", event.address);
    case CrashKind::datatype_misalignment:
        return "Unaligned memory access";
    case CrashKind::stack_overflow:
        return "Stack overflow";
    case CrashKind::error:
        return event.message.empty() ? "unknown error" : event.message;
    case CrashKind::out_of_memory:
        return "Bun ran out of memory";
    }
    return "unknown crash reason";
}

} // namespace mbun::crash_handler
