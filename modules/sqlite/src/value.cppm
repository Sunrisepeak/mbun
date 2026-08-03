// value.cppm — sqlite scalar values shared by database, statements, and rows.
//
// PORT-SOURCE: bun `src/sql/shared/Data.rs` and `src/sql_jsc/shared/SQLDataCell.rs`.
// The sqlite3 value conversion/FFI layer is intentionally deferred.
export module mbun.sqlite.value;

import std;
import mbun.sqlite.sql;

namespace mbun::sqlite {

export using Blob = std::vector<std::byte>;
export using SqlValue = SQLValue;
export using ValueKind = SQLValueKind;

} // namespace mbun::sqlite
