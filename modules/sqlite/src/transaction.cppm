// transaction.cppm — sqlite transaction state machine and nesting seam.
//
// PORT-SOURCE: bun `src/js/bun/sqlite.ts` getController/wrapTransaction and
// `src/js/internal/sql/sqlite.ts`. Nested transactions use savepoints, matching
// better-sqlite3's behavior used by Bun.
export module mbun.sqlite.transaction;

import std;
import mbun.sqlite.statement;

namespace mbun::sqlite {

export enum class TransactionMode : std::uint8_t { default_mode, deferred, immediate, exclusive };
export enum class TransactionState : std::uint8_t { idle, active };

export struct TransactionBackend {
    std::function<std::expected<void, SqlError>(TransactionMode)> begin;
    std::function<std::expected<void, SqlError>()> commit;
    std::function<std::expected<void, SqlError>()> rollback;
    std::function<std::expected<void, SqlError>(std::string_view)> savepoint;
    std::function<std::expected<void, SqlError>(std::string_view)> release;
    std::function<std::expected<void, SqlError>(std::string_view)> rollback_to;
    std::function<bool()> in_transaction;
};

export class Transaction {
public:
    Transaction() = default;
    explicit Transaction(TransactionBackend backend) : backend_{std::move(backend)} {}

    TransactionState state() const noexcept { return state_; }
    bool in_transaction() const noexcept { return state_ == TransactionState::active; }

    std::expected<void, SqlError> run(TransactionMode mode,
                                      const std::function<std::expected<void, SqlError>()>& body) {
        const bool nested = backend_.in_transaction && backend_.in_transaction();
        const std::string_view savepoint_name{"mbun_sqlite_tx"};
        auto before = nested ? (backend_.savepoint ? backend_.savepoint(savepoint_name)
                                                  : std::expected<void, SqlError>{})
                             : (backend_.begin ? backend_.begin(mode)
                                                : std::expected<void, SqlError>{});
        if (!before) return before;
        state_ = TransactionState::active;
        auto result = body ? body() : std::expected<void, SqlError>{};
        if (result) {
            result = nested ? (backend_.release ? backend_.release(savepoint_name)
                                                : std::expected<void, SqlError>{})
                            : (backend_.commit ? backend_.commit() : std::expected<void, SqlError>{});
        } else if (nested) {
            if (backend_.rollback_to) (void)backend_.rollback_to(savepoint_name);
            if (backend_.release) (void)backend_.release(savepoint_name);
        } else if (backend_.rollback) {
            (void)backend_.rollback();
        }
        state_ = TransactionState::idle;
        return result;
    }

private:
    TransactionBackend backend_;
    TransactionState state_{TransactionState::idle};
};

export struct TransactionCommands {
    std::string begin;
    std::string commit{"COMMIT"};
    std::string rollback{"ROLLBACK"};
    std::string savepoint{"SAVEPOINT"};
    std::string release_savepoint{"RELEASE SAVEPOINT"};
    std::string rollback_to_savepoint{"ROLLBACK TO SAVEPOINT"};
};

export struct TransactionValidation { bool valid{true}; std::string error; };

export TransactionValidation validate_transaction_options(std::string_view options) {
    if (options.empty()) return {};
    std::string upper{options};
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (upper == "READONLY" || upper == "READ") {
        return {false, "SQLite does not support readonly transactions; use DEFERRED, IMMEDIATE, or EXCLUSIVE."};
    }
    if (!std::ranges::all_of(options, [](unsigned char c) {
        return std::isalpha(c) || c == ' ' || c == ',';
    })) {
        return {false, "Transaction options can only contain letters, spaces, and commas."};
    }
    return {};
}

export TransactionCommands transaction_commands(std::string_view options = {}) {
    auto validation{validate_transaction_options(options)};
    if (!validation.valid) return {"BEGIN " + std::string{options}};
    std::string upper{options};
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (upper.empty()) upper = "BEGIN";
    else if (upper == "DEFERRED" || upper == "IMMEDIATE" || upper == "EXCLUSIVE") {
        upper = "BEGIN " + upper;
    } else {
        upper = "BEGIN " + std::string{options};
    }
    return {std::move(upper)};
}

} // namespace mbun::sqlite
