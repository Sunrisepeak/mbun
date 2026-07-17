// Bun.sql / Bun.SQL (postgres + sqlite adapters) JS API shape. Kept as a
// dedicated payload partition so the tagged-template + query-normalization
// machinery does not grow bootstrap. The pure-logic postgres wire codec is
// bridged natively via __mbunPostgresNative (see runtime/sql.inc); sqlite
// executes synchronously through __mbunSqliteNative. The live postgres socket
// transport now runs over Bun.connect (makePgDriver below): connect + the
// connect-error taxonomy, AuthenticationOk/Cleartext, and simple/extended query
// execution are wired. MD5/SASL auth, MySQL transport, connection pooling, and
// TLS remain DEFERRED(S-net). MySQL query/connect still reject.
//
// Blueprint: bun-ref src/js/bun/sql.ts (the `sql` tagged function + SQL
// factory) and src/js/internal/sql/{shared,postgres}.ts (SQLHelper,
// normalizeQuery, detectCommand, adapter escape/placeholder/bindParam).
export module mbun.jsc.js_builtins:sql;

import std;

export namespace mbun::jsc::builtins::detail {

// NOTE: this partition is appended AFTER the master builtins IIFE (opened in
// bootstrap, closed by image_closure) has already run, so it is wrapped in its
// own self-contained IIFE that re-binds G = globalThis. Everything it needs is
// on the global object by the time this evaluates.
inline constexpr std::string_view kSqlJS = R"JS(
(function () {
  const G = globalThis;
  // ---- Bun.sql / Bun.SQL (postgres + sqlite; pg execution DEFERRED) ----
  if (G.Bun && typeof G.Bun.sql === "undefined") {
    const PGN = G.__mbunPostgresNative;   // wire codec bridge (offline)
    const SQN = G.__mbunSqliteNative;     // sqlite3 backend bridge
    const env = (G.process && G.process.env) || {};

    // ----- shared query-normalization machinery (port of internal/sql/shared.ts) -----
    class SQLHelper {
      constructor(value, keys) {
        if (keys !== undefined && keys.length === 0 && (value[0] !== null && typeof value[0] === "object")) {
          keys = Object.keys(value[0]);
        }
        if (keys !== undefined) {
          for (let key of keys) {
            if (typeof key === "string") {
              const asNumber = Number(key);
              if (Number.isNaN(asNumber)) continue;
              key = asNumber;
            }
            if (typeof key !== "string") {
              if (Number.isSafeInteger(key) && key >= 0 && key <= 64 * 1024) continue;
              throw new Error("Keys must be strings or numbers: " + String(key));
            }
          }
        }
        this.value = value;
        this.columns = keys ?? [];
      }
    }

    const SQLCommand = { insert: 0, update: 1, updateSet: 2, where: 3, in: 4, none: -1 };
    const commandToString = (c) =>
      c === SQLCommand.insert ? "INSERT"
      : (c === SQLCommand.updateSet || c === SQLCommand.update) ? "UPDATE"
      : (c === SQLCommand.in || c === SQLCommand.where) ? "WHERE" : "";

    const detectCommand = (query, anyAndAllMeanIn) => {
      const text = query.toLowerCase().trim();
      const n = text.length;
      let token = "", command = SQLCommand.none, quoted = false;
      for (let i = n - 1; i >= 0; i--) {
        const ch = text[i];
        if (ch === " " || ch === "\n" || ch === "\t" || ch === "\r" || ch === "\f" || ch === "\v") {
          switch (token) {
            case "insert": return SQLCommand.insert;
            case "update": return SQLCommand.update;
            case "where": return SQLCommand.where;
            case "set": return SQLCommand.updateSet;
            case "in": return SQLCommand.in;
            default: token = ""; continue;
          }
        } else {
          if (ch === '"') { quoted = !quoted; continue; }
          if (!quoted) token = ch + token;
        }
      }
      if (token) {
        switch (token) {
          case "insert": return SQLCommand.insert;
          case "update": return SQLCommand.update;
          case "where": return SQLCommand.where;
          case "set": return SQLCommand.updateSet;
          case "in": return SQLCommand.in;
          case "any": case "all": return anyAndAllMeanIn ? SQLCommand.in : SQLCommand.none;
          default: return SQLCommand.none;
        }
      }
      return command;
    };

    const getHelperCommandFromDetect = (query, anyAndAllMeanIn) => {
      const command = detectCommand(query, anyAndAllMeanIn);
      if (command === SQLCommand.none || command === SQLCommand.where) {
        throw new SyntaxError("Helpers are only allowed for INSERT, UPDATE and IN commands");
      }
      return command;
    };

    const buildDefinedColumnsAndQuery = (columns, items, escapeIdentifier) => {
      const definedColumns = [];
      let columnsSql = "(";
      const columnCount = columns.length;
      if (Array.isArray(items)) {
        for (let j = 0; j < items.length; j++) {
          if (items[j] == null) throw new SyntaxError("Cannot use null or undefined as an item in INSERT helper");
        }
      }
      for (let k = 0; k < columnCount; k++) {
        const column = columns[k];
        let hasDefinedValue = false;
        if (Array.isArray(items)) {
          for (let j = 0; j < items.length; j++) {
            if (typeof items[j][column] !== "undefined") { hasDefinedValue = true; break; }
          }
        } else {
          hasDefinedValue = typeof items[column] !== "undefined";
        }
        if (hasDefinedValue) {
          if (definedColumns.length > 0) columnsSql += ", ";
          columnsSql += escapeIdentifier(column);
          definedColumns.push(column);
        }
      }
      columnsSql += ") VALUES";
      return { definedColumns, columnsSql };
    };

    const pushBindParam = (adapter, value, binding_values, index) => {
      binding_values.push(typeof value === "undefined" ? null : value);
      return adapter.placeholder(index) + " ";
    };

    const normalizeQuery = (adapter, strings, values, binding_idx) => {
      binding_idx = binding_idx || 1;
      if (typeof strings === "string") return [strings, values || []];
      if (!Array.isArray(strings)) throw new SyntaxError("Invalid query: SQL Fragment cannot be executed or was misused");
      const str_len = strings.length;
      if (str_len === 0) return ["", []];
      const binding_values = [];
      let query = "";
      for (let i = 0; i < str_len; i++) {
        const string = strings[i];
        if (typeof string !== "string") throw new SyntaxError("Invalid query: SQL Fragment cannot be executed or was misused");
        query += string;
        if (values.length > i) {
          const value = values[i];
          if (value instanceof Query) {
            const [sub_query, sub_values] = normalizeQuery(adapter, value.__strings, value.__values, binding_idx);
            query += sub_query;
            for (let j = 0; j < sub_values.length; j++) binding_values.push(sub_values[j]);
            binding_idx += sub_values.length;
          } else if (value instanceof SQLHelper) {
            const command = adapter.getHelperCommand(query);
            const columns = value.columns, items = value.value;
            const columnCount = columns.length;
            if (columnCount === 0 && command !== SQLCommand.in) {
              throw new SyntaxError("Cannot " + commandToString(command) + " with no columns");
            }
            const lastColumnIndex = columns.length - 1;
            if (command === SQLCommand.insert) {
              const { definedColumns, columnsSql } = buildDefinedColumnsAndQuery(columns, items, (c) => adapter.escapeIdentifier(c));
              const definedColumnCount = definedColumns.length;
              if (definedColumnCount === 0) throw new SyntaxError("Insert needs to have at least one column with a defined value");
              const lastDefinedColumnIndex = definedColumnCount - 1;
              query += columnsSql;
              if (Array.isArray(items)) {
                const itemsCount = items.length, lastItemIndex = itemsCount - 1;
                for (let j = 0; j < itemsCount; j++) {
                  query += "(";
                  const item = items[j];
                  for (let k = 0; k < definedColumnCount; k++) {
                    const columnValue = item[definedColumns[k]];
                    query += adapter.placeholder(binding_idx++) + (k < lastDefinedColumnIndex ? ", " : "");
                    binding_values.push(typeof columnValue === "undefined" ? null : columnValue);
                  }
                  query += j < lastItemIndex ? ")," : ") ";
                }
              } else {
                query += "(";
                for (let j = 0; j < definedColumnCount; j++) {
                  const columnValue = items[definedColumns[j]];
                  query += adapter.placeholder(binding_idx++) + (j < lastDefinedColumnIndex ? ", " : "");
                  binding_values.push(columnValue);
                }
                query += ") ";
              }
            } else if (command === SQLCommand.in) {
              if (!Array.isArray(items)) throw new SyntaxError("An array of values is required for WHERE IN helper");
              const itemsCount = items.length, lastItemIndex = itemsCount - 1;
              query += "(";
              for (let j = 0; j < itemsCount; j++) {
                query += adapter.placeholder(binding_idx++) + (j < lastItemIndex ? ", " : "");
                if (columnCount > 0) {
                  if (columnCount > 1) throw new SyntaxError("Cannot use WHERE IN helper with multiple columns");
                  const value2 = items[j];
                  if (typeof value2 === "undefined") binding_values.push(null);
                  else if (value2 === null) throw new SyntaxError("Cannot use null as an item in WHERE IN helper with a column");
                  else {
                    const vk = value2[columns[0]];
                    binding_values.push(typeof vk === "undefined" ? null : vk);
                  }
                } else {
                  const value2 = items[j];
                  binding_values.push(typeof value2 === "undefined" ? null : value2);
                }
              }
              query += ") ";
            } else {
              let item;
              if (Array.isArray(items)) {
                if (items.length > 1) throw new SyntaxError("Cannot use array of objects for UPDATE");
                item = items[0];
              } else item = items;
              if (item == null) throw new SyntaxError("Cannot use null or undefined as an item in UPDATE helper");
              if (command === SQLCommand.update && !adapter.isUpsertUpdate(query)) query += " SET ";
              let hasValues = false;
              for (let k = 0; k < columnCount; k++) {
                const column = columns[k];
                const columnValue = item[column];
                if (typeof columnValue === "undefined") continue;
                hasValues = true;
                query += adapter.escapeIdentifier(column) + " = " + adapter.placeholder(binding_idx++) + (k < lastColumnIndex ? ", " : "");
                binding_values.push(columnValue);
              }
              if (query.endsWith(", ")) query = query.substring(0, query.length - 2);
              adapter.throwIfUpdateEmpty(query, hasValues);
              query += " ";
            }
          } else {
            query += adapter.bindParam(value, binding_values, binding_idx++);
          }
        }
      }
      return [query, binding_values];
    };

    // $ERR_INVALID_ARG_VALUE — TypeError with a Node-style code/message.
    const invalidArgValue = (name, value, reason) => {
      const e = new TypeError("The argument '" + name + "' must " + reason + ". Received " +
        (typeof value === "string" ? JSON.stringify(value) : String(value)));
      e.code = "ERR_INVALID_ARG_VALUE";
      return e;
    };

    const validateDistributedName = (name) => {
      if (typeof name !== "string") return { valid: false, error: "Distributed transaction name must be a string." };
      if (name.indexOf("'") !== -1) return { valid: false, error: "Distributed transaction name cannot contain single quotes." };
      return { valid: true };
    };

    // ----- adapters -----
    const baseThrowIfUpdateEmpty = (_q, hasValues) => {
      if (!hasValues) throw new SyntaxError("Update needs to have at least one column");
    };

    const makePostgresAdapter = (sqlObj) => ({
      kind: "postgres",
      escapeIdentifier(str) {
        if (str.includes("\0")) throw invalidArgValue("name", str, "not contain null bytes");
        return '"' + str.replaceAll('"', '""').replaceAll(".", '"."') + '"';
      },
      placeholder(index) { return "$" + index; },
      getHelperCommand(query) { return getHelperCommandFromDetect(query, false); },
      isUpsertUpdate(_q) { return false; },
      throwIfUpdateEmpty: baseThrowIfUpdateEmpty,
      bindParam(value, binding_values, index) { return pushBindParam(this, value, binding_values, index); },
      normalizeQuery(strings, values, idx) { return normalizeQuery(this, strings, values, idx); },
      // live execution over Bun.connect + __mbunPostgresNative (see makePgDriver).
      execute(text, binds, flags) { return sqlObj.__pgDriver.execute(text, binds, flags); },
    });

    const makeMysqlAdapter = (sqlObj) => ({
      kind: "mysql",
      escapeIdentifier(str) {
        if (str.includes("\0")) throw invalidArgValue("name", str, "not contain null bytes");
        return "`" + str.replaceAll("`", "``").replaceAll(".", "`.`") + "`";
      },
      placeholder(_index) { return "?"; },
      getHelperCommand(query) { return getHelperCommandFromDetect(query, true); },
      isUpsertUpdate(_q) { return false; },
      throwIfUpdateEmpty: baseThrowIfUpdateEmpty,
      bindParam(value, binding_values, index) { return pushBindParam(this, value, binding_values, index); },
      normalizeQuery(strings, values, idx) { return normalizeQuery(this, strings, values, idx); },
      execute(text, binds, flags) { return sqlObj.__myDriver.execute(text, binds, flags); },
    });

    const makeSqliteAdapter = (sqlObj) => ({
      kind: "sqlite",
      escapeIdentifier(str) {
        if (str.includes("\0")) throw invalidArgValue("name", str, "not contain null bytes");
        return '"' + str.replaceAll('"', '""') + '"';
      },
      placeholder(_index) { return "?"; },
      getHelperCommand(query) { return getHelperCommandFromDetect(query, false); },
      isUpsertUpdate(_q) { return false; },
      throwIfUpdateEmpty: baseThrowIfUpdateEmpty,
      bindParam(value, binding_values, index) { return pushBindParam(this, value, binding_values, index); },
      normalizeQuery(strings, values, idx) { return normalizeQuery(this, strings, values, idx); },
      execute(text, binds, flags) {
        if (!SQN) return Promise.reject(new Error("sqlite backend unavailable"));
        try {
          const handle = sqlObj.__handle();
          const res = SQN.run(handle, text, binds);
          return Promise.resolve(shapeSqliteRows(res, flags));
        } catch (e) { return Promise.reject(e); }
      },
    });

    // sqlite result { columns, values(row-arrays), changes, lastInsertRowid }
    // → array of row objects (default) / row-arrays (.values()/.raw()).
    const shapeSqliteRows = (res, flags) => {
      const cols = res.columns || [], rows = res.values || [];
      let out;
      if (flags && (flags.values || flags.raw)) {
        out = rows.slice();
      } else {
        out = new Array(rows.length);
        for (let i = 0; i < rows.length; i++) {
          const row = rows[i], obj = {};
          for (let c = 0; c < cols.length; c++) obj[cols[c]] = row[c];
          out[i] = obj;
        }
      }
      Object.defineProperty(out, "count", { value: res.changes || 0, enumerable: false, configurable: true });
      Object.defineProperty(out, "lastInsertRowid", { value: res.lastInsertRowid, enumerable: false, configurable: true });
      return out;
    };

    // ----- lazy Query (thenable; normalization + execution run on await) -----
    class Query {
      constructor(strings, values, sqlObj, isIdentifier) {
        this.__strings = strings;
        this.__values = values;
        this.__sql = sqlObj;
        this.__isIdentifier = !!isIdentifier;
        this.__flags = { values: false, raw: false, simple: false };
        this.__promise = null;
      }
      __run() {
        if (this.__promise) return this.__promise;
        const adapter = this.__sql.__adapter;
        try {
          if (this.__isIdentifier) {
            // sql("ident") — escape as a dynamic identifier fragment (throws on NUL).
            this.__promise = Promise.resolve(adapter.escapeIdentifier(this.__strings));
          } else {
            const [text, binds] = adapter.normalizeQuery(this.__strings, this.__values, 1);
            this.__promise = adapter.execute(text, binds, this.__flags);
          }
        } catch (e) {
          this.__promise = Promise.reject(e);
        }
        return this.__promise;
      }
      then(onF, onR) { return this.__run().then(onF, onR); }
      catch(onR) { return this.__run().catch(onR); }
      finally(onF) { return this.__run().finally(onF); }
      values() { this.__flags.values = true; return this; }
      raw() { this.__flags.raw = true; return this; }
      simple() { this.__flags.simple = true; return this; }
      execute() { this.__run(); return this; }
      cancel() { return this; }
      get [Symbol.toStringTag]() { return "Query"; }
    }

    // ----- connection option resolution -----
    const schemeAdapter = (sch) => sch == null ? null
      : (["postgres", "postgresql", "pg"].includes(sch) ? "postgres"
      : (["mysql", "mysql2", "mariadb"].includes(sch) ? "mysql"
      : (["sqlite", "file"].includes(sch) ? "sqlite" : null)));

    // PORT-SOURCE: bun-ref src/js/internal/sql/shared.ts:1394-1410 —
    // SQLITE_MEMORY_VARIANTS is consulted by parseDefinitelySqliteUrl BEFORE any
    // scheme sniffing, because ":memory:" has no parseable scheme (it starts with
    // ':'), so the scheme regex below can never classify it. shared.ts:1652-1659
    // then promotes a non-null parseDefinitelySqliteUrl result to adapter
    // "sqlite". Without this, `new SQL(":memory:")` fell through to the
    // DEFAULT_PROTOCOL ("postgres") branch and rejected on the deferred pg
    // transport. The other variants ("sqlite://:memory:", "sqlite:memory") are
    // already covered by schemeAdapter, but are listed here to keep the set
    // 1:1 with the blueprint.
    const SQLITE_MEMORY_VARIANTS = [":memory:", "sqlite://:memory:", "sqlite:memory"];

    // bun-ref src/js/internal/sql/shared.ts:113 SSLMode enum + normalizeSSLMode.
    const normalizeSSLMode = (v) => {
      if (v == null || v === "") return 0;
      switch (String(v).toLowerCase()) {
        case "disable": return 0;
        case "prefer": return 1;
        case "require": case "required": return 2;
        case "verify-ca": case "verify_ca": return 3;
        case "verify-full": case "verify_full": return 4;
        default: return 0;
      }
    };

    const parsePgUrl = (raw) => {
      let s = String(raw), scheme = null;
      const m = s.match(/^([a-zA-Z][a-zA-Z0-9+.-]*):\/\//);
      if (m) { scheme = m[1].toLowerCase(); s = s.slice(m[0].length); }
      if (scheme === "unix") return { scheme, path: "/" + s.replace(/^\/+/, "") };
      const out = { scheme };
      const at = s.lastIndexOf("@");
      if (at >= 0) {
        const cred = s.slice(0, at); s = s.slice(at + 1);
        const c = cred.indexOf(":");
        if (c >= 0) { out.username = decodeURIComponent(cred.slice(0, c)); out.password = decodeURIComponent(cred.slice(c + 1)); }
        else if (cred) out.username = decodeURIComponent(cred);
      }
      const q = s.indexOf("?");
      if (q >= 0) {
        // PORT-SOURCE: bun-ref shared.ts:1801 — sslmode comes from the URL query.
        const params = new URLSearchParams(s.slice(q + 1));
        const sm = params.get("sslmode");
        if (sm != null) out.sslMode = normalizeSSLMode(sm);
        s = s.slice(0, q);
      }
      const slash = s.indexOf("/");
      if (slash >= 0) { const db = s.slice(slash + 1); if (db) out.database = decodeURIComponent(db); s = s.slice(0, slash); }
      const colon = s.lastIndexOf(":");
      if (colon >= 0) { const p = parseInt(s.slice(colon + 1), 10); if (!isNaN(p)) out.port = p; s = s.slice(0, colon); }
      if (s) out.hostname = s;
      return out;
    };

    // sqlite: strip scheme, split ?query params (mode → readonly/create), keep
    // the raw filename; empty → :memory:; file:// via fileURLToPath when valid.
    const parseSqliteUrl = (raw) => {
      let s = String(raw), scheme = null, isFileSlashes = false;
      if (/^sqlite:\/\//i.test(s)) { scheme = "sqlite"; s = s.slice(9); }
      else if (/^sqlite:/i.test(s)) { scheme = "sqlite"; s = s.slice(7); }
      else if (/^file:\/\//i.test(s)) { scheme = "file"; isFileSlashes = true; }
      else if (/^file:/i.test(s)) { scheme = "file"; s = s.slice(5); }
      let readonly, create;
      const applyQuery = (qs) => {
        const params = new URLSearchParams(qs);
        const mode = params.get("mode");
        if (mode === "ro") { readonly = true; }
        else if (mode === "rw") { readonly = false; }
        else if (mode === "rwc") { readonly = false; create = true; }
      };
      let filename;
      if (isFileSlashes) {
        // file:// — try fileURLToPath (percent-decodes, validates host), else strip.
        let noQuery = raw, qIdx = raw.indexOf("?");
        if (qIdx >= 0) { applyQuery(raw.slice(qIdx + 1)); noQuery = raw.slice(0, qIdx); }
        try { filename = G.Bun.fileURLToPath(noQuery); }
        catch { filename = noQuery.slice(7); }
      } else {
        const qIdx = s.indexOf("?");
        if (qIdx >= 0) { applyQuery(s.slice(qIdx + 1)); s = s.slice(0, qIdx); }
        filename = s;
      }
      if (filename === "" || filename == null) filename = ":memory:";
      return { scheme, filename, readonly, create };
    };

    const pick = (...vals) => { for (const v of vals) if (v !== undefined && v !== null && v !== "") return v; return undefined; };

    const resolveOptions = (a, b) => {
      let url = null, opts = {};
      if (typeof a === "string" || (a instanceof URL)) { url = String(a); opts = b || {}; }
      else if (a && typeof a === "object") { opts = a; if (typeof opts.url === "string") url = opts.url; else if (opts.url instanceof URL) url = String(opts.url); }
      else { opts = b || {}; }  // a is null/undefined: options come from the 2nd arg
      let adapter = opts.adapter || null;
      let sslMode = opts.sslMode;
      if (url == null && !("hostname" in opts) && !("host" in opts) && !("filename" in opts) && !("path" in opts)) {
        const SOURCES = [
          ["POSTGRES_URL", "postgres"], ["PGURL", "postgres"], ["PG_URL", "postgres"],
          ["MYSQL_URL", "mysql"], ["MYSQLURL", "mysql"], ["MARIADB_URL", "mysql"], ["MARIADBURL", "mysql"],
          ["SQLITE_URL", "sqlite"], ["SQLITEURL", "sqlite"],
          ["TLS_POSTGRES_DATABASE_URL", "postgres", 2], ["TLS_MYSQL_DATABASE_URL", "mysql", 2], ["TLS_MARIADB_DATABASE_URL", "mysql", 2],
          ["TLS_DATABASE_URL", null, 2], ["DATABASE_URL", null], ["DATABASEURL", null],
        ];
        for (const [name, impliedAdapter, ssl] of SOURCES) {
          if (!env[name]) continue;
          if (adapter && impliedAdapter && impliedAdapter !== adapter) continue;
          url = env[name];
          if (!adapter) adapter = impliedAdapter;
          if (ssl !== undefined && sslMode === undefined) sslMode = ssl;
          break;
        }
      }
      // detect adapter from URL scheme if still unknown. ref: bun-ref
      // src/js/internal/sql/shared.ts:1652-1659 — the "definitely sqlite" probe
      // runs first, so a bare ":memory:" is classified before the scheme regex
      // (which cannot match it) hands the URL to the postgres default.
      if (!adapter && url != null && SQLITE_MEMORY_VARIANTS.includes(String(url))) adapter = "sqlite";
      if (!adapter && url != null) {
        const m = String(url).match(/^([a-zA-Z][a-zA-Z0-9+.-]*):/);
        adapter = schemeAdapter(m ? m[1].toLowerCase() : null);
      }
      if (!adapter && (opts.filename !== undefined)) adapter = "sqlite";
      if (!adapter) adapter = "postgres";

      if (adapter === "sqlite") {
        let o = { adapter: "sqlite", filename: ":memory:", readonly: undefined, create: undefined };
        if (url != null) {
          const u = parseSqliteUrl(url);
          o.filename = u.filename;
          o.readonly = u.readonly;
          o.create = u.create;
        }
        if (opts.filename !== undefined) o.filename = opts.filename;
        if (opts.readonly !== undefined) o.readonly = opts.readonly;
        if (opts.create !== undefined) o.create = opts.create;
        if (o.filename === "" || o.filename == null) o.filename = ":memory:";
        return o;
      }

      const u = url != null ? parsePgUrl(url) : {};
      if (sslMode === undefined && u.sslMode !== undefined) sslMode = u.sslMode;
      const isPg = adapter === "postgres", isMy = adapter === "mysql";
      const o = { adapter };
      if (u.scheme === "unix" || ("path" in opts)) {
        o.path = pick(opts.path, u.path);
        o.username = pick(opts.username, opts.user, u.username, env.USER);
        o.password = pick(opts.password, opts.pass, u.password, "");
        o.database = pick(opts.database, opts.db, u.database);
      } else {
        o.hostname = pick(opts.hostname, opts.host, u.hostname, isPg ? env.PGHOST : undefined, isMy ? env.MYSQL_HOST : undefined, "localhost");
        o.port = Number(pick(opts.port, u.port, isPg ? env.PGPORT : undefined, isMy ? env.MYSQL_PORT : undefined, isMy ? 3306 : 5432));
        o.username = pick(opts.username, opts.user, u.username, isPg ? env.PGUSER : undefined, isMy ? env.MYSQL_USER : undefined, env.USER, env.USERNAME, isMy ? "root" : "postgres");
        o.password = pick(opts.password, opts.pass, u.password, isPg ? env.PGPASSWORD : undefined, isMy ? env.MYSQL_PASSWORD : undefined, "");
        o.database = pick(opts.database, opts.db, u.database, isPg ? env.PGDATABASE : undefined, isMy ? env.MYSQL_DATABASE : undefined, isMy ? "mysql" : o.username);
      }
      if (opts.max !== undefined) o.max = opts.max;
      // connectionTimeout is in SECONDS at the API (bun-ref shared.ts:2004 *=1000);
      // the driver multiplies by 1000. 0 disables the connect timer / retry budget.
      o.connectionTimeout = pick(opts.connectionTimeout, opts.connection_timeout,
                                 opts.connectTimeout, opts.connect_timeout);
      o.connectionTimeout = o.connectionTimeout === undefined ? 30 : Number(o.connectionTimeout);
      if (typeof opts.onclose === "function") o.onclose = opts.onclose;
      if (typeof opts.onconnect === "function") o.onconnect = opts.onconnect;
      o.sslMode = sslMode === undefined ? 0 : sslMode;
      return o;
    };

    // ----- live Postgres transport driver (Bun.connect + __mbunPostgresNative) -----
    // PORT-SOURCE: bun-ref src/sql_jsc/postgres/PostgresSQLConnection.rs (on_open /
    // on_close / on_connect_error / on_data fail taxonomy) + src/js/internal/sql/
    // shared.ts (BasePooledConnection retry-with-backoff). The Rust connection is a
    // native socket state machine; here the equivalent state machine runs in JS
    // driving Bun.connect, with the wire codec bridged natively.
    const PG_CODE = {
      REFUSED: "ERR_POSTGRES_CONNECTION_REFUSED",
      FAILED: "ERR_POSTGRES_CONNECTION_FAILED",
      CLOSED: "ERR_POSTGRES_CONNECTION_CLOSED",
      SERVER: "ERR_POSTGRES_SERVER_ERROR",
      INVALID_LEN: "ERR_POSTGRES_INVALID_MESSAGE_LENGTH",
      UNSUPPORTED_AUTH: "ERR_POSTGRES_UNSUPPORTED_AUTHENTICATION_METHOD",
      TLS_NOT_AVAILABLE: "ERR_POSTGRES_TLS_NOT_AVAILABLE",
      TLS_UPGRADE_FAILED: "ERR_POSTGRES_TLS_UPGRADE_FAILED",
    };
    const pgErr = (msg, code, errno) => {
      const e = new Error(msg);
      e.name = "PostgresError";   // bun-ref ErrorCode.ts: all ERR_POSTGRES_* are PostgresError
      e.code = code;
      if (errno !== undefined) e.errno = errno;
      return e;
    };
    const catBytes = (a, b) => { const o = new Uint8Array(a.length + b.length); o.set(a, 0); o.set(b, a.length); return o; };
    const pgEnc = new TextEncoder();

    function makePgDriver(options) {
      const PGN = G.__mbunPostgresNative;
      let sock = null;
      let status = "idle";  // idle | connecting | sentStartup | connected | closed
      let readBuf = new Uint8Array(0);
      let dialResolve = null, dialReject = null, dialSettled = true;
      let downOnce = false, opened = false, awaitingSSL = false;
      let connectPromise = null;
      let retryTimer = null, retryWake = null;
      let connectStartedAt = 0, connectAttempts = 0;
      let currentQuery = null;
      let closed = false, closeFired = false;

      // onclose fires once per closed connection (not per retry attempt) —
      // bun-ref shared.ts handleClose / #finishClose.
      const fireClose = (err) => {
        if (closeFired) return;
        closeFired = true;
        if (typeof options.onclose === "function") { try { options.onclose(err); } catch (e) {} }
      };

      const pgPasswordMessage = (pw) => {
        const body = pgEnc.encode(pw == null ? "" : String(pw));
        const len = 4 + body.length + 1;
        const buf = new Uint8Array(1 + len);
        buf[0] = 0x70;  // 'p'
        new DataView(buf.buffer).setInt32(1, len, false);
        buf.set(body, 5);
        buf[5 + body.length] = 0;
        return buf;
      };

      // SSLRequest: Int32(8) Int32(80877103) — bun-ref src/sql_jsc/postgres SSL
      // negotiation. The server answers with a single byte 'S' (willing) / 'N'.
      const pgSSLRequest = () => {
        const buf = new Uint8Array(8);
        const dv = new DataView(buf.buffer);
        dv.setInt32(0, 8, false);
        dv.setInt32(4, 80877103, false);
        return buf;
      };
      const sendStartup = (bs) => {
        const startup = { user: options.username || "postgres" };
        if (options.database) startup.database = options.database;
        try { bs.write(PGN.encodeStartup(startup)); } catch (e) {}
      };

      const settleDial = (fn, arg) => { if (dialSettled) return; dialSettled = true; dialResolve = null; dialReject = null; fn(arg); };

      // A socket-level down event. bun-ref splits this into on_connect_error
      // (never established -> REFUSED) and on_close (established then closed ->
      // FAILED while handshaking / CLOSED afterwards). The mbun Bun.connect shim
      // fires a bare "error" alongside connectError, so a not-yet-opened "error"
      // is ignored and connectError/close decides the outcome via `opened`.
      const onSocketDown = (evt) => {
        if (downOnce) return;
        if (!opened && evt === "error") return;  // let connectError/close win
        const wasConnected = (status === "connected");
        downOnce = true;
        status = "closed";
        sock = null;
        if (!opened) {
          settleDial(dialReject, pgErr("Failed to connect", PG_CODE.REFUSED));
        } else if (!wasConnected) {
          settleDial(dialReject, pgErr("Connection closed before the connection was established", PG_CODE.FAILED));
        } else if (currentQuery) {
          const q = currentQuery; currentQuery = null;
          q.reject(pgErr("Connection closed", PG_CODE.CLOSED));
        }
        if (wasConnected) fireClose(pgErr("Connection closed", PG_CODE.CLOSED));
        connectPromise = null;
      };

      const failFatal = (jsErr) => {
        // protocol / server error: reject the current phase, tear the socket down.
        if (status === "connected" && currentQuery) {
          const q = currentQuery; currentQuery = null;
          q.reject(jsErr);
        } else {
          settleDial(dialReject, jsErr);
        }
        downOnce = true;  // suppress the trailing close (bun: failed conn is not resurrected)
        status = "closed";
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        connectPromise = null;
      };

      const pushRow = (m) => {
        const q = currentQuery, cols = m.columns || [], fields = q.fields || [];
        const wantRaw = q.flags && (q.flags.values || q.flags.raw);
        if (wantRaw) {
          const row = new Array(cols.length);
          for (let i = 0; i < cols.length; i++) {
            const f = fields[i];
            row[i] = cols[i] == null ? null : PGN.decodeValue(f ? f.typeOid : 0, cols[i], f ? f.formatCode : 0);
          }
          q.rows.push(row);
        } else {
          const obj = {};
          for (let i = 0; i < cols.length; i++) {
            const f = fields[i];
            obj[f ? f.name : i] = cols[i] == null ? null : PGN.decodeValue(f ? f.typeOid : 0, cols[i], f ? f.formatCode : 0);
          }
          q.rows.push(obj);
        }
      };

      const finishQuery = () => {
        const q = currentQuery; currentQuery = null;
        const out = q.rows;
        Object.defineProperty(out, "count", { value: q.rows.length, enumerable: false, configurable: true });
        Object.defineProperty(out, "command", { value: q.command, enumerable: false, configurable: true });
        q.resolve(out);
      };

      const handleAuth = (m, bs) => {
        const kind = m.kind | 0;
        if (kind === 0) return;                         // AuthenticationOk
        if (kind === 3) { bs.write(pgPasswordMessage(options.password)); return; }  // Cleartext
        // MD5 (5) / SASL (10) not yet driven over the live socket in mbun.
        failFatal(pgErr("Unsupported authentication method", PG_CODE.UNSUPPORTED_AUTH));
      };

      const handleMessage = (m, bs) => {
        switch (m.tag) {
          case "invalid":
            failFatal(pgErr("Invalid message length", PG_CODE.INVALID_LEN));
            return;
          case "R": handleAuth(m, bs); return;
          case "E": failFatal(pgErr(m.message || "server error", PG_CODE.SERVER, m.code)); return;
          case "Z":  // ReadyForQuery
            if (status === "connecting" || status === "sentStartup" || status === "sslNegotiate") {
              status = "connected";
              settleDial(dialResolve, undefined);
            } else if (currentQuery) {
              finishQuery();
            }
            return;
          case "T": if (currentQuery) currentQuery.fields = m.fields; return;
          case "D": if (currentQuery) pushRow(m); return;
          case "C": if (currentQuery) currentQuery.command = m.command || ""; return;
          default: return;  // ParameterStatus / BackendKeyData / Notice / Notification
        }
      };

      const pump = (bs) => {
        const res = PGN.decode(readBuf);
        const consumed = res.consumed | 0;
        if (consumed > 0) readBuf = readBuf.slice(consumed);
        const msgs = res.messages || [];
        for (let i = 0; i < msgs.length; i++) {
          handleMessage(msgs[i], bs);
          if (status === "closed") break;
        }
      };

      const dialOnce = () => new Promise((resolve, reject) => {
        status = "connecting";
        readBuf = new Uint8Array(0);
        downOnce = false;
        opened = false;
        awaitingSSL = false;
        dialSettled = false;
        dialResolve = resolve;
        dialReject = reject;
        const handlers = {
          open(bs) {
            opened = true;
            if (status === "closed" || closed) { try { bs.end(); } catch (e) {} return; }
            sock = bs;
            if ((Number(options.sslMode) || 0) > 0) {
              status = "sslNegotiate";
              awaitingSSL = true;
              try { bs.write(pgSSLRequest()); } catch (e) {}
            } else {
              status = "sentStartup";
              sendStartup(bs);
            }
          },
          data(bs, chunk) {
            readBuf = catBytes(readBuf, chunk);
            if (awaitingSSL) {
              if (readBuf.length < 1) return;
              const resp = readBuf[0];
              readBuf = readBuf.slice(1);   // consume the 1-byte SSL response
              awaitingSSL = false;
              if (resp === 0x53 /* 'S' */) {
                // Server willing: a real STARTTLS upgrade over the pool socket is
                // not yet wired in mbun; fail honestly rather than go unencrypted.
                failFatal(pgErr("TLS upgrade failed: STARTTLS is not yet implemented in mbun", PG_CODE.TLS_UPGRADE_FAILED));
                return;
              }
              // 'N' — server refuses SSL: require/verify fail; prefer falls back
              // to a plaintext startup. ref: bun-ref postgres.ts sslmode handling.
              if ((Number(options.sslMode) || 0) >= 2) {
                failFatal(pgErr("The server does not support SSL connections", PG_CODE.TLS_NOT_AVAILABLE));
                return;
              }
              status = "sentStartup";
              sendStartup(bs);
              // fall through: pump any startup replies already buffered
            }
            pump(bs);
          },
          close(bs) { onSocketDown("closed"); },
          error(bs, e) { onSocketDown("error"); },
          connectError(bs, e) { onSocketDown("refused"); },
        };
        let p;
        try {
          p = G.Bun.connect({ hostname: options.hostname || "localhost", port: options.port || 5432, socket: handlers });
        } catch (e) { onSocketDown("refused"); return; }
        if (p && typeof p.catch === "function") p.catch(() => {});  // connectError already handles it
      });

      const budgetMs = () => (Number(options.connectionTimeout) || 0) * 1000;
      const canRetry = () => { const b = budgetMs(); return b > 0 && connectStartedAt !== 0 && (Date.now() - connectStartedAt) < b; };
      const sleep = (ms) => new Promise((r) => { retryWake = r; retryTimer = setTimeout(() => { retryTimer = null; retryWake = null; r(); }, ms); });

      const connectLoop = async () => {
        connectStartedAt = Date.now();
        connectAttempts = 0;
        closeFired = false;
        for (;;) {
          try {
            await dialOnce();
            if (typeof options.onconnect === "function") { try { options.onconnect(null); } catch (e) {} }
            return;
          }
          catch (e) {
            // ConnectionFailed (pre-handshake close) is retried with backoff while
            // the connect budget remains — bun-ref shared.ts #shouldRetryConnecting.
            if (e && e.code === PG_CODE.FAILED && !closed && canRetry()) {
              connectAttempts++;
              await sleep(Math.min(20 * 2 ** connectAttempts, 1000));
              if (closed) { fireClose(e); throw pgErr("Connection closed", PG_CODE.CLOSED); }
              continue;
            }
            fireClose(e);
            throw e;
          }
        }
      };

      const ensureConnected = () => {
        if (status === "connected") return Promise.resolve();
        if (!connectPromise) {
          connectPromise = connectLoop().catch((e) => { connectPromise = null; throw e; });
        }
        return connectPromise;
      };

      const execute = (text, binds, flags) => ensureConnected().then(() => new Promise((resolve, reject) => {
        if (status !== "connected" || !sock) { reject(pgErr("Connection closed", PG_CODE.CLOSED)); return; }
        currentQuery = { resolve, reject, fields: null, rows: [], command: "", flags: flags || {} };
        try {
          if ((flags && flags.simple) || !binds || binds.length === 0) {
            sock.write(PGN.encodeQuery(text));   // simple query protocol ('Q')
          } else {
            const bindBytes = binds.map((v) => (v == null ? null : pgEnc.encode(String(v))));
            let frame = PGN.encodeParse("", text, []);
            frame = catBytes(frame, PGN.encodeBind("", "", bindBytes));
            frame = catBytes(frame, PGN.encodeDescribe("P", ""));
            frame = catBytes(frame, PGN.encodeExecute("", 0));
            frame = catBytes(frame, PGN.encodeSync());
            sock.write(frame);
          }
        } catch (e) { currentQuery = null; reject(pgErr("Connection closed", PG_CODE.CLOSED)); }
      }));

      const close = () => {
        closed = true;
        if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
        if (retryWake) { const w = retryWake; retryWake = null; w(); }
        settleDial(dialReject, pgErr("Connection closed", PG_CODE.CLOSED));
        if (currentQuery) { const q = currentQuery; currentQuery = null; q.reject(pgErr("Connection closed", PG_CODE.CLOSED)); }
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        status = "closed";
        connectPromise = null;
        return Promise.resolve();
      };

      return { connect: () => ensureConnected(), execute, close };
    }

    // ----- live MySQL transport driver (Bun.connect; connect-error subset) -----
    // PORT-SOURCE: bun-ref src/sql_jsc/mysql/JSMySQLConnection.rs — on_connect_error
    // ("Failed to connect" / ConnectionRefused), do_close + consume_on_connect
    // (pre-handshake close -> "Connection closed before the connection was
    // established" / ConnectionFailed; else "Connection closed" /
    // ConnectionClosed) — plus src/js/internal/sql/shared.ts retry-with-backoff.
    // MySQL is server-speaks-first: the client sends nothing on open and waits
    // for HandshakeV10. The handshake/auth + query codec are DEFERRED (no MySQL
    // wire bridge yet), so a server that actually answers is honestly rejected;
    // the connect-error / retry / forced-close paths (mock TCP servers that
    // refuse / drop / never answer) are fully wired.
    const MY_CODE = {
      REFUSED: "ERR_MYSQL_CONNECTION_REFUSED",
      FAILED: "ERR_MYSQL_CONNECTION_FAILED",
      CLOSED: "ERR_MYSQL_CONNECTION_CLOSED",
    };
    const myErr = (msg, code) => {
      const e = new Error(msg);
      e.name = "MySQLError";   // bun-ref ErrorCode.ts: all ERR_MYSQL_* are MySQLError
      e.code = code;
      return e;
    };

    function makeMysqlDriver(options) {
      let sock = null;
      let status = "idle";  // idle | connecting | handshaking | closed
      let dialResolve = null, dialReject = null, dialSettled = true;
      let downOnce = false, opened = false;
      let connectPromise = null;
      let retryTimer = null, retryWake = null;
      let connectStartedAt = 0, connectAttempts = 0;
      let currentQuery = null;
      let closed = false, closeFired = false;

      const fireClose = (err) => {
        if (closeFired) return;
        closeFired = true;
        if (typeof options.onclose === "function") { try { options.onclose(err); } catch (e) {} }
      };
      const settleDial = (fn, arg) => { if (dialSettled) return; dialSettled = true; dialResolve = null; dialReject = null; fn(arg); };

      const onSocketDown = (evt) => {
        if (downOnce) return;
        if (!opened && evt === "error") return;  // let connectError/close win
        const wasConnected = (status === "connected");
        downOnce = true;
        status = "closed";
        sock = null;
        if (!opened) {
          settleDial(dialReject, myErr("Failed to connect", MY_CODE.REFUSED));
        } else if (!wasConnected) {
          settleDial(dialReject, myErr("Connection closed before the connection was established", MY_CODE.FAILED));
        } else if (currentQuery) {
          const q = currentQuery; currentQuery = null;
          q.reject(myErr("Connection closed", MY_CODE.CLOSED));
        }
        if (wasConnected) fireClose(myErr("Connection closed", MY_CODE.CLOSED));
        connectPromise = null;
      };

      const dialOnce = () => new Promise((resolve, reject) => {
        status = "connecting";
        downOnce = false;
        opened = false;
        dialSettled = false;
        dialResolve = resolve;
        dialReject = reject;
        const handlers = {
          open(bs) {
            opened = true;
            if (status === "closed" || closed) { try { bs.end(); } catch (e) {} return; }
            status = "handshaking";  // MySQL server speaks first; wait for HandshakeV10.
            sock = bs;
          },
          data(bs, chunk) {
            // A real server answered: the handshake/auth codec is not yet wired.
            settleDial(dialReject, myErr(
              "MySQL handshake over a live socket is not yet implemented in mbun (DEFERRED: needs the wire codec)",
              MY_CODE.CLOSED));
            downOnce = true; status = "closed"; const s = sock; sock = null;
            if (s) { try { s.end(); } catch (e) {} }
            connectPromise = null;
          },
          close(bs) { onSocketDown("closed"); },
          error(bs, e) { onSocketDown("error"); },
          connectError(bs, e) { onSocketDown("refused"); },
        };
        let p;
        try {
          p = G.Bun.connect({ hostname: options.hostname || "localhost", port: options.port || 3306, socket: handlers });
        } catch (e) { onSocketDown("refused"); return; }
        if (p && typeof p.catch === "function") p.catch(() => {});
      });

      const budgetMs = () => (Number(options.connectionTimeout) || 0) * 1000;
      const canRetry = () => { const b = budgetMs(); return b > 0 && connectStartedAt !== 0 && (Date.now() - connectStartedAt) < b; };
      const sleep = (ms) => new Promise((r) => { retryWake = r; retryTimer = setTimeout(() => { retryTimer = null; retryWake = null; r(); }, ms); });

      const connectLoop = async () => {
        connectStartedAt = Date.now();
        connectAttempts = 0;
        closeFired = false;
        for (;;) {
          try {
            await dialOnce();
            if (typeof options.onconnect === "function") { try { options.onconnect(null); } catch (e) {} }
            return;
          }
          catch (e) {
            if (e && e.code === MY_CODE.FAILED && !closed && canRetry()) {
              connectAttempts++;
              await sleep(Math.min(20 * 2 ** connectAttempts, 1000));
              if (closed) { fireClose(e); throw myErr("Connection closed", MY_CODE.CLOSED); }
              continue;
            }
            fireClose(e);
            throw e;
          }
        }
      };

      const ensureConnected = () => {
        if (status === "connected") return Promise.resolve();
        if (!connectPromise) connectPromise = connectLoop().catch((e) => { connectPromise = null; throw e; });
        return connectPromise;
      };

      // Query execution needs the (deferred) MySQL wire codec; ensureConnected
      // rejects first on the mock/error paths, so this only surfaces for a real
      // server that completes the TCP connect.
      const execute = (text, binds, flags) => ensureConnected().then(() => Promise.reject(myErr(
        "MySQL query execution over a live socket is not yet implemented in mbun (DEFERRED: needs the wire codec)",
        MY_CODE.CLOSED)));

      const close = () => {
        closed = true;
        if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
        if (retryWake) { const w = retryWake; retryWake = null; w(); }
        settleDial(dialReject, myErr("Connection closed", MY_CODE.CLOSED));
        if (currentQuery) { const q = currentQuery; currentQuery = null; q.reject(myErr("Connection closed", MY_CODE.CLOSED)); }
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        status = "closed";
        connectPromise = null;
        return Promise.resolve();
      };

      return { connect: () => ensureConnected(), execute, close };
    }

    // ----- SQL factory (returns a callable tagged `sql` bound to one config) -----
    function makeSQL(a, b) {
      const options = resolveOptions(a, b);
      const sqlObj = function sql(first, ...values) {
        if (Array.isArray(first)) {
          if (!Array.isArray(first.raw)) return new SQLHelper(first, values);
          return new Query(first, values, sqlObj, false);
        } else if (typeof first === "object" && !(first instanceof Query) && !(first instanceof SQLHelper)) {
          // typeof null === "object" too: null becomes a SQLHelper([null]) so the
          // UPDATE/INSERT/IN helper validation reports the proper SyntaxError.
          return new SQLHelper([first], values);
        } else if (typeof first === "string") {
          // dynamic identifier: sql("column")
          return new Query(first, values, sqlObj, true);
        }
        return new Query(first, values, sqlObj, false);
      };

      sqlObj.__adapter =
        options.adapter === "sqlite" ? makeSqliteAdapter(sqlObj)
        : options.adapter === "mysql" ? makeMysqlAdapter(sqlObj)
        : makePostgresAdapter(sqlObj);
      sqlObj.options = options;
      if (options.adapter === "postgres") sqlObj.__pgDriver = makePgDriver(options);
      if (options.adapter === "mysql") sqlObj.__myDriver = makeMysqlDriver(options);

      // sqlite: lazily open the sqlite3 handle on first execution.
      let sqliteHandle = null;
      sqlObj.__handle = () => {
        if (sqliteHandle == null) {
          sqliteHandle = SQN.open(options.filename, !!options.readonly, options.create === undefined ? true : !!options.create);
        }
        return sqliteHandle;
      };

      sqlObj.unsafe = (text, args = []) => new Query([text], args, sqlObj, false);
      sqlObj.file = (path, args = []) => G.Bun.file(path).text().then((text) => new Query([text], args, sqlObj, false));

      sqlObj.connect = () => {
        if (options.adapter === "sqlite") { try { sqlObj.__handle(); return Promise.resolve(sqlObj); } catch (e) { return Promise.reject(e); } }
        if (options.adapter === "postgres") return sqlObj.__pgDriver.connect().then(() => sqlObj);
        if (options.adapter === "mysql") return sqlObj.__myDriver.connect().then(() => sqlObj);
        return Promise.reject(new Error("SQL connect over a live socket is not yet implemented in mbun"));
      };
      sqlObj.close = (_options) => {
        if (sqliteHandle != null && SQN) { try { SQN.close(sqliteHandle); } catch {} sqliteHandle = null; }
        if (sqlObj.__pgDriver) { try { sqlObj.__pgDriver.close(); } catch {} }
        if (sqlObj.__myDriver) { try { sqlObj.__myDriver.close(); } catch {} }
        return Promise.resolve();
      };
      sqlObj.end = sqlObj.close;
      sqlObj.flush = () => {};
      sqlObj[Symbol.asyncDispose] = () => sqlObj.close();

      sqlObj.commitDistributed = async (name) => {
        const v = validateDistributedName(name);
        if (!v.valid) throw new Error(v.error);
        return sqlObj.unsafe("COMMIT PREPARED '" + name + "'");
      };
      sqlObj.rollbackDistributed = async (name) => {
        const v = validateDistributedName(name);
        if (!v.valid) throw new Error(v.error);
        return sqlObj.unsafe("ROLLBACK PREPARED '" + name + "'");
      };
      const notImpl = (what) => () => Promise.reject(new Error(
        "SQL " + what + " is not yet implemented in mbun (DEFERRED: needs the event loop)"));
      sqlObj.reserve = notImpl("reserve");
      sqlObj.begin = notImpl("begin");
      sqlObj.transaction = sqlObj.begin;
      sqlObj.beginDistributed = notImpl("beginDistributed");
      sqlObj.distributed = sqlObj.beginDistributed;
      sqlObj.array = (values) => values;

      return sqlObj;
    }

    // SQL: usable with or without `new` (returns the callable sql tagged fn).
    function SQL(a, b) { return makeSQL(a, b); }
    G.Bun.SQL = SQL;

    // Bun.sql: a lazily-initialized default instance that is itself callable.
    let defaultSql = null;
    const ensureDefault = () => { if (!defaultSql) defaultSql = makeSQL(undefined); return defaultSql; };
    const bunSql = function sql(first, ...values) { return ensureDefault()(first, ...values); };
    for (const k of ["unsafe", "file", "connect", "close", "end", "begin", "transaction",
                     "reserve", "commitDistributed", "rollbackDistributed", "beginDistributed", "array"]) {
      bunSql[k] = (...args) => ensureDefault()[k](...args);
    }
    Object.defineProperty(bunSql, "options", { get() { return ensureDefault().options; }, configurable: true });
    G.Bun.sql = bunSql;

    // Expose the offline wire codec for protocol-layer tests (injected bytes).
    if (PGN) G.Bun.__postgresWire = PGN;
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
