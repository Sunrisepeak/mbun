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
      execute(text, binds, flags) {
        const d = sqlObj.__driver();
        if (!d) return Promise.reject(pgErr("Connection closed", PG_CODE.CLOSED));
        return d.execute(text, binds, flags);
      },
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
      execute(text, binds, flags) {
        const d = sqlObj.__driver();
        if (!d) return Promise.reject(myErr("Connection closed", MY_CODE.CLOSED));
        return d.execute(text, binds, flags);
      },
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
      // bun-ref shared.ts: `prepare` defaults to true — parameterized queries go
      // out as a named prepare exchange followed by Bind/Execute.
      o.prepare = opts.prepare === undefined ? true : !!opts.prepare;
      // `tls` selects the encrypted transport (mysql negotiates it in the
      // handshake, postgres via SSLRequest); sslMode stays the postgres-only knob.
      if (opts.tls !== undefined) o.tls = opts.tls;
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
      INVALID_MESSAGE: "ERR_POSTGRES_INVALID_MESSAGE",
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
      let closed = false, closeFired = false;

      // ---- request queue --------------------------------------------------
      // PORT-SOURCE: bun-ref src/sql_jsc/postgres/PostgresSQLConnection.rs —
      // backend responses are attributed to `requests.peek_item(0)`, i.e. in
      // strict enqueue order, so a query may only put frames on the wire once it
      // is the head of the queue. mbun runs one exchange at a time (no
      // pipelining), which makes attribution independent of how the inbound
      // stream happens to be segmented (see
      // compat/bun/test/js/sql/postgres-split-prepare-reorder.test.ts).
      const queue = [];        // enqueued requests, FIFO
      let inflight = null;     // the request whose frames are on the wire
      let phase = "idle";      // idle | preparing | executing | draining
      const statements = new Map();  // query text -> { name, prepared, fields }
      let stmtSeq = 0;

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
      // Reject every request that can no longer be answered (socket down /
      // forced close / fatal protocol error).
      const failAllRequests = (jsErr) => {
        const q = inflight;
        inflight = null;
        phase = "idle";
        const pending = queue.splice(0, queue.length);
        for (const r of pending) { if (r !== q) { try { r.reject(jsErr); } catch (e) {} } }
        if (q) { try { q.reject(jsErr); } catch (e) {} }
      };

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
        } else {
          failAllRequests(pgErr("Connection closed", PG_CODE.CLOSED));
        }
        if (wasConnected) fireClose(pgErr("Connection closed", PG_CODE.CLOSED));
        connectPromise = null;
      };

      const failFatal = (jsErr) => {
        // protocol / server error: reject the current phase, tear the socket down.
        if (status === "connected") failAllRequests(jsErr);
        else settleDial(dialReject, jsErr);
        downOnce = true;  // suppress the trailing close (bun: failed conn is not resurrected)
        status = "closed";
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        connectPromise = null;
      };

      const pushRow = (m) => {
        const q = inflight, cols = m.columns || [], fields = q.fields || [];
        const wantRaw = q.flags && (q.flags.values || q.flags.raw);
        const decode = (i) => {
          const f = fields[i];
          return cols[i] == null ? null : PGN.decodeValue(f ? f.typeOid : 0, cols[i], f ? f.formatCode : 0);
        };
        if (wantRaw) {
          const row = new Array(cols.length);
          for (let i = 0; i < cols.length; i++) row[i] = decode(i);
          q.rows.push(row);
        } else {
          const obj = {};
          // A DataRow may carry fewer cells than the RowDescription declared;
          // the columns it omits read back as null (bun-ref DataCell fill).
          const n = Math.max(cols.length, fields.length);
          for (let i = 0; i < n; i++) {
            const f = fields[i];
            obj[f ? f.name : i] = i < cols.length ? decode(i) : null;
          }
          q.rows.push(obj);
        }
      };

      // One CommandComplete closes one result set. A simple query may carry
      // several statements ("copy ...; select ..."), and bun then resolves with
      // an array of result sets instead of a flat row array.
      const endResultSet = () => {
        inflight.results.push(inflight.rows);
        inflight.rows = [];
      };

      const settleQuery = (err) => {
        const q = inflight;
        inflight = null;
        if (queue[0] === q) queue.shift();
        if (err) { q.reject(err); return; }
        const out = q.results.length === 0 ? q.rows
                  : q.results.length === 1 ? q.results[0]
                  : q.results;
        Object.defineProperty(out, "count", { value: out.length, enumerable: false, configurable: true });
        Object.defineProperty(out, "command", { value: q.command, enumerable: false, configurable: true });
        q.resolve(out);
      };

      // Drop the head request when its frames could not be written at all.
      const abortHead = (jsErr) => {
        const q = inflight;
        inflight = null;
        if (queue[0] === q) queue.shift();
        if (q) q.reject(jsErr);
      };

      const writeFrames = (frame) => {
        if (!sock) return false;
        try { sock.write(frame); return true; } catch (e) { return false; }
      };

      const bindBytesOf = (binds) => binds.map((v) => (v == null ? null : pgEnc.encode(String(v))));

      // Bind + Execute + Sync for a prepared statement (named) or the unnamed
      // one-shot portal (which also needs Describe('P') for its RowDescription).
      const writeBindExecute = (q, stmtName) => {
        let frame = PGN.encodeBind("", stmtName, bindBytesOf(q.binds));
        if (!stmtName) frame = catBytes(frame, PGN.encodeDescribe("P", ""));
        frame = catBytes(frame, PGN.encodeExecute("", 0));
        frame = catBytes(frame, PGN.encodeSync());
        return writeFrames(frame);
      };

      const pumpQueue = () => {
        if (inflight || phase === "draining") return;
        if (status !== "connected" || !sock) return;
        const q = queue[0];
        if (!q) return;
        inflight = q;
        q.rows = []; q.results = []; q.command = ""; q.fields = null; q.error = null;
        const extended = !q.flags.simple && q.binds && q.binds.length > 0;
        if (extended && options.prepare !== false) {
          // Named prepare is its own exchange (Parse + Describe('S') + Sync),
          // answered by ParseComplete/ParameterDescription/RowDescription/
          // ReadyForQuery before any Bind goes out.
          let st = statements.get(q.text);
          if (!st) { st = { name: "mbun_s" + (++stmtSeq), prepared: false, fields: null }; statements.set(q.text, st); }
          q.stmt = st;
          if (!st.prepared) {
            phase = "preparing";
            let frame = PGN.encodeParse(st.name, q.text, []);
            frame = catBytes(frame, PGN.encodeDescribe("S", st.name));
            frame = catBytes(frame, PGN.encodeSync());
            if (!writeFrames(frame)) abortHead(pgErr("Connection closed", PG_CODE.CLOSED));
            return;
          }
          phase = "executing";
          q.fields = st.fields;
          if (!writeBindExecute(q, st.name)) abortHead(pgErr("Connection closed", PG_CODE.CLOSED));
          return;
        }
        phase = "executing";
        let wrote;
        if (!extended) wrote = writeFrames(PGN.encodeQuery(q.text));  // simple query protocol ('Q')
        else wrote = writeFrames(catBytes(PGN.encodeParse("", q.text, []), (() => {
          let f = PGN.encodeBind("", "", bindBytesOf(q.binds));
          f = catBytes(f, PGN.encodeDescribe("P", ""));
          f = catBytes(f, PGN.encodeExecute("", 0));
          return catBytes(f, PGN.encodeSync());
        })()));
        if (!wrote) abortHead(pgErr("Connection closed", PG_CODE.CLOSED));
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
          case "invalidBody":
            // The body ran past its own (well-formed) length header — libpq's
            // "insufficient data left in message". Distinct from a bad length.
            failFatal(pgErr("Invalid message", PG_CODE.INVALID_MESSAGE));
            return;
          case "R": handleAuth(m, bs); return;
          case "E": {
            const err = pgErr(m.message || "server error", PG_CODE.SERVER, m.code);
            if (status !== "connected" || !inflight) { failFatal(err); return; }
            // A statement error does NOT close the connection: reject just this
            // exchange, discard whatever else it still emits, and become idle
            // again at its ReadyForQuery.
            if (phase === "preparing") statements.delete(inflight.text);
            const q = inflight;
            inflight = null;
            if (queue[0] === q) queue.shift();
            phase = "draining";
            q.reject(err);
            return;
          }
          case "Z":  // ReadyForQuery
            if (status === "connecting" || status === "sentStartup" || status === "sslNegotiate") {
              status = "connected";
              settleDial(dialResolve, undefined);
              pumpQueue();
            } else if (inflight && phase === "preparing") {
              const st = inflight.stmt;
              st.prepared = true;
              st.fields = inflight.fields;
              phase = "executing";
              if (!writeBindExecute(inflight, st.name)) abortHead(pgErr("Connection closed", PG_CODE.CLOSED));
            } else if (inflight) {
              settleQuery(inflight.error);
              phase = "idle";
              pumpQueue();
            } else {
              phase = "idle";
              pumpQueue();
            }
            return;
          case "T": if (inflight) inflight.fields = m.fields; return;
          case "D":
            if (inflight && phase === "executing") {
              // decodeValue throws a PostgresError on malformed binary payloads;
              // hold it and reject at ReadyForQuery so the exchange still ends.
              try { pushRow(m); } catch (e) { inflight.error = e; }
            }
            return;
          case "C":
            if (inflight && phase === "executing") { inflight.command = m.command || ""; endResultSet(); }
            return;
          default: return;  // ParameterStatus / BackendKeyData / Notice / Notification / COPY
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

      // Enqueue first, connect second: a query issued before the handshake
      // completes keeps its place in the enqueue order.
      const execute = (text, binds, flags) => new Promise((resolve, reject) => {
        const q = { text, binds: binds || [], flags: flags || {}, resolve, reject,
                    rows: [], results: [], fields: null, command: "", error: null, stmt: null };
        queue.push(q);
        ensureConnected().then(pumpQueue, (e) => {
          const i = queue.indexOf(q);
          if (i >= 0) { queue.splice(i, 1); reject(e); }
          else if (inflight !== q) reject(e);
        });
      });

      const close = () => {
        closed = true;
        if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
        if (retryWake) { const w = retryWake; retryWake = null; w(); }
        settleDial(dialReject, pgErr("Connection closed", PG_CODE.CLOSED));
        failAllRequests(pgErr("Connection closed", PG_CODE.CLOSED));
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        status = "closed";
        connectPromise = null;
        return Promise.resolve();
      };

      return { connect: () => ensureConnected(), execute, close };
    }

    // ----- live MySQL transport driver (Bun.connect + a JS wire codec) -------
    // PORT-SOURCE: bun-ref src/sql_jsc/mysql/JSMySQLConnection.rs (connect-error
    // taxonomy: on_connect_error -> ConnectionRefused; do_close /
    // consume_on_connect -> ConnectionFailed while handshaking, ConnectionClosed
    // afterwards) + src/sql/mysql packet set, aligned to the MySQL client/server
    // protocol reference (page_protocol_basic_packets.html and siblings).
    //
    // MySQL is server-speaks-first: the client sends nothing on open and waits
    // for HandshakeV10, answers with HandshakeResponse41, and then runs every
    // query as COM_STMT_PREPARE + COM_STMT_EXECUTE (bun's default
    // `prepare: true`); `.simple()` / `prepare: false` use COM_QUERY. A real
    // STARTTLS upgrade of the pool socket is still DEFERRED(S-net).
    const MY_CODE = {
      REFUSED: "ERR_MYSQL_CONNECTION_REFUSED",
      FAILED: "ERR_MYSQL_CONNECTION_FAILED",
      CLOSED: "ERR_MYSQL_CONNECTION_CLOSED",
      SERVER: "ERR_MYSQL_SERVER_ERROR",
      UNEXPECTED_PACKET: "ERR_MYSQL_UNEXPECTED_PACKET",
      MISSING_AUTH_DATA: "ERR_MYSQL_MISSING_AUTH_DATA",
      INVALID_AUTH_SWITCH: "ERR_MYSQL_INVALID_AUTH_SWITCH_REQUEST",
      INVALID_PREPARE_OK: "ERR_MYSQL_INVALID_PREPARE_OK_PACKET",
      INVALID_RESULT_ROW: "ERR_MYSQL_INVALID_RESULT_ROW",
      UNSUPPORTED_AUTH_PLUGIN: "ERR_MYSQL_UNSUPPORTED_AUTH_PLUGIN",
      UNSUPPORTED_PROTOCOL: "ERR_MYSQL_UNSUPPORTED_PROTOCOL_VERSION",
      PUBLIC_KEY_RETRIEVAL: "ERR_MYSQL_PUBLIC_KEY_RETRIEVAL_NOT_ALLOWED",
      OVERFLOW: "ERR_MYSQL_OVERFLOW",
      LOCAL_INFILE: "ERR_MYSQL_LOCAL_INFILE_NOT_SUPPORTED",
    };
    const myErr = (msg, code) => {
      const e = new Error(msg);
      e.name = "MySQLError";   // bun-ref ErrorCode.ts: all ERR_MYSQL_* are MySQLError
      e.code = code;
      return e;
    };

    // Capability flags — page_protocol_basic_capability_flags.html (subset used).
    const MY_CAP = {
      LONG_PASSWORD: 1, FOUND_ROWS: 2, LONG_FLAG: 4, CONNECT_WITH_DB: 8,
      LOCAL_FILES: 128, PROTOCOL_41: 1 << 9, SSL: 1 << 11, TRANSACTIONS: 1 << 13,
      SECURE_CONNECTION: 1 << 15, MULTI_RESULTS: 1 << 17, PS_MULTI_RESULTS: 1 << 18,
      PLUGIN_AUTH: 1 << 19, PLUGIN_AUTH_LENENC_CLIENT_DATA: 1 << 21, DEPRECATE_EOF: 1 << 24,
    };
    const MY_CMD = { QUIT: 0x01, QUERY: 0x03, STMT_PREPARE: 0x16, STMT_EXECUTE: 0x17 };
    const myEnc = new TextEncoder();
    const myDec = new TextDecoder();

    // Packet framing: Int<3>(payload_length) Int<1>(sequence_id) payload.
    const myPacket = (seqId, payload) => {
      const out = new Uint8Array(4 + payload.length);
      out[0] = payload.length & 0xff;
      out[1] = (payload.length >> 8) & 0xff;
      out[2] = (payload.length >> 16) & 0xff;
      out[3] = seqId & 0xff;
      out.set(payload, 4);
      return out;
    };
    // length-encoded integer — page_protocol_basic_dt_integers.html. Returns null
    // when the buffer is too short; `.value` is null for the 0xfb NULL marker and
    // NaN when the encoded value does not fit a JS safe integer.
    const myLenenc = (b, o) => {
      if (o >= b.length) return null;
      const f = b[o];
      if (f < 0xfb) return { value: f, width: 1 };
      if (f === 0xfb) return { value: null, width: 1 };
      if (f === 0xfc) return o + 3 > b.length ? null : { value: b[o + 1] | (b[o + 2] << 8), width: 3 };
      if (f === 0xfd) return o + 4 > b.length ? null : { value: b[o + 1] | (b[o + 2] << 8) | (b[o + 3] << 16), width: 4 };
      if (o + 9 > b.length) return null;
      let v = 0;
      for (let i = 7; i >= 0; i--) v = v * 256 + b[o + 1 + i];
      return { value: Number.isSafeInteger(v) ? v : NaN, width: 9 };
    };
    const myPutLenenc = (out, n) => {
      if (n < 0xfb) out.push(n);
      else if (n < 0x10000) out.push(0xfc, n & 0xff, (n >> 8) & 0xff);
      else out.push(0xfd, n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff);
    };
    const myStr = (b, from, to) => myDec.decode(b.subarray(from, Math.min(to, b.length)));
    const myCstrEnd = (b, from) => { let i = from; while (i < b.length && b[i] !== 0) i++; return i; };
    const myDigest = (algo, bytes) => {
      const h = new G.Bun.CryptoHasher(algo);
      h.update(bytes);
      return new Uint8Array(h.digest());
    };
    const myXor = (a, b) => {
      const o = new Uint8Array(a.length);
      for (let i = 0; i < a.length; i++) o[i] = a[i] ^ b[i % b.length];
      return o;
    };
    // mysql_native_password: SHA1(pw) XOR SHA1(nonce + SHA1(SHA1(pw)))
    const myNativeScramble = (pw, nonce) => {
      if (!pw) return new Uint8Array(0);
      const s1 = myDigest("sha1", myEnc.encode(String(pw)));
      return myXor(s1, myDigest("sha1", catBytes(nonce, myDigest("sha1", s1))));
    };
    // caching_sha2_password: SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) + nonce)
    const mySha2Scramble = (pw, nonce) => {
      if (!pw) return new Uint8Array(0);
      const d1 = myDigest("sha256", myEnc.encode(String(pw)));
      const d2 = myDigest("sha256", d1);
      return myXor(d1, myDigest("sha256", catBytes(d2, nonce)));
    };
    const myAuthResponse = (plugin, pw, nonce) =>
      plugin === "caching_sha2_password" ? mySha2Scramble(pw, nonce)
      : plugin === "mysql_native_password" ? myNativeScramble(pw, nonce)
      : null;

    // ColumnDefinition41 — page_protocol_com_query_response_text_resultset_column_definition.html
    const myParseColumnDef = (p) => {
      let o = 0, name = "", type = 0xfd, flags = 0;
      for (let i = 0; i < 6; i++) {
        const le = myLenenc(p, o);
        if (!le) return { name: String(i), type, flags };
        o += le.width;
        const end = o + (le.value || 0);
        if (i === 4) name = myStr(p, o, end);
        o = end;
      }
      const fixed = myLenenc(p, o);
      if (fixed) {
        o += fixed.width;
        if (o + 10 <= p.length) { type = p[o + 6]; flags = p[o + 7] | (p[o + 8] << 8); }
      }
      return { name, type, flags };
    };
    const myParseOk = (p) => {
      let o = 1;
      const a = myLenenc(p, o); o += a ? a.width : 1;
      const l = myLenenc(p, o);
      return { affectedRows: a && a.value != null ? a.value : 0,
               lastInsertId: l && l.value != null ? l.value : 0 };
    };
    // ERR_Packet: Int<1>(0xff) Int<2>(code) ['#' + 5-byte SQL state] String<EOF>(msg)
    const myParseErr = (p) => {
      const code = p.length >= 3 ? (p[1] | (p[2] << 8)) : 0;
      const o = (p.length > 3 && p[3] === 0x23) ? 9 : 3;
      return myErr(myStr(p, o, p.length) || ("MySQL error " + code), MY_CODE.SERVER);
    };
    const MY_NUMERIC_TYPES = [0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0d];
    const myCoerceText = (s, type) => (MY_NUMERIC_TYPES.indexOf(type) >= 0 ? Number(s) : s);

    function makeMysqlDriver(options) {
      let sock = null;
      // idle | connecting | handshake | auth | connected | closed
      let status = "idle";
      let readBuf = new Uint8Array(0);
      let dialResolve = null, dialReject = null, dialSettled = true;
      let downOnce = false, opened = false;
      let connectPromise = null;
      let retryTimer = null, retryWake = null;
      let connectStartedAt = 0, connectAttempts = 0;
      let closed = false, closeFired = false;

      // Same one-exchange-at-a-time discipline as the postgres driver: MySQL has
      // no request id on the wire, so a reply belongs to the head of the queue.
      const queue = [];
      let inflight = null;
      let step = "";   // prepareOk | prepareParams | prepareColumns | resultHeader | columns | rows
      const statements = new Map();  // query text -> { id, columns }

      const fireClose = (err) => {
        if (closeFired) return;
        closeFired = true;
        if (typeof options.onclose === "function") { try { options.onclose(err); } catch (e) {} }
      };
      const settleDial = (fn, arg) => { if (dialSettled) return; dialSettled = true; dialResolve = null; dialReject = null; fn(arg); };

      const failAllRequests = (jsErr) => {
        const q = inflight;
        inflight = null;
        step = "";
        const pending = queue.splice(0, queue.length);
        for (const r of pending) { if (r !== q) { try { r.reject(jsErr); } catch (e) {} } }
        if (q) { try { q.reject(jsErr); } catch (e) {} }
      };

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
        } else {
          failAllRequests(myErr("Connection closed", MY_CODE.CLOSED));
        }
        if (wasConnected) fireClose(myErr("Connection closed", MY_CODE.CLOSED));
        connectPromise = null;
      };

      const failFatal = (jsErr) => {
        if (status === "connected") failAllRequests(jsErr);
        else { failAllRequests(jsErr); settleDial(dialReject, jsErr); }
        downOnce = true;
        status = "closed";
        const s = sock; sock = null;
        if (s) { try { s.end(); } catch (e) {} }
        connectPromise = null;
      };

      const writePacket = (seqId, payload) => {
        if (!sock) return false;
        try { sock.write(myPacket(seqId, payload)); return true; } catch (e) { return false; }
      };

      const settleQuery = (err) => {
        const q = inflight;
        inflight = null;
        step = "";
        if (queue[0] === q) queue.shift();
        if (!q) return;
        if (err) { q.reject(err); return; }
        const out = q.rows;
        Object.defineProperty(out, "count", { value: out.length, enumerable: false, configurable: true });
        Object.defineProperty(out, "lastInsertRowid", { value: q.lastInsertId || 0, enumerable: false, configurable: true });
        Object.defineProperty(out, "affectedRows", { value: q.affectedRows || 0, enumerable: false, configurable: true });
        q.resolve(out);
      };
      const abortHead = (jsErr) => {
        const q = inflight;
        inflight = null;
        step = "";
        if (queue[0] === q) queue.shift();
        if (q) q.reject(jsErr);
      };

      const buildHandshakeResponse = (caps, authResp, plugin) => {
        const body = [];
        const user = myEnc.encode(String(options.username || "root"));
        const db = options.database ? myEnc.encode(String(options.database)) : null;
        let flags = MY_CAP.PROTOCOL_41 | MY_CAP.SECURE_CONNECTION | MY_CAP.PLUGIN_AUTH |
                    MY_CAP.PLUGIN_AUTH_LENENC_CLIENT_DATA | MY_CAP.LONG_PASSWORD |
                    MY_CAP.LONG_FLAG | MY_CAP.TRANSACTIONS | MY_CAP.MULTI_RESULTS |
                    MY_CAP.PS_MULTI_RESULTS;
        if (db) flags |= MY_CAP.CONNECT_WITH_DB;
        if (caps & MY_CAP.DEPRECATE_EOF) flags |= MY_CAP.DEPRECATE_EOF;
        flags = flags >>> 0;
        body.push(flags & 0xff, (flags >> 8) & 0xff, (flags >> 16) & 0xff, (flags >>> 24) & 0xff);
        body.push(0, 0, 0, 1);   // max packet size (16 MiB)
        body.push(0x2d);         // utf8mb4_general_ci
        for (let i = 0; i < 23; i++) body.push(0);
        for (const b of user) body.push(b);
        body.push(0);
        myPutLenenc(body, authResp.length);
        for (const b of authResp) body.push(b);
        if (db) { for (const b of db) body.push(b); body.push(0); }
        for (const b of myEnc.encode(plugin)) body.push(b);
        body.push(0);
        return new Uint8Array(body);
      };

      // Protocol::HandshakeV10 — page_protocol_connection_phase_packets_protocol_handshake_v10.html
      const onHandshakePacket = (seqId, p) => {
        if (p.length === 0) { failFatal(myErr("Empty handshake packet", MY_CODE.UNEXPECTED_PACKET)); return; }
        if (p[0] === 0xff) { failFatal(myParseErr(p)); return; }
        if (p[0] !== 10) { failFatal(myErr("Unsupported protocol version " + p[0], MY_CODE.UNSUPPORTED_PROTOCOL)); return; }
        let o = myCstrEnd(p, 1) + 1;   // server_version
        o += 4;                        // thread_id
        const auth1 = p.subarray(o, o + 8); o += 8;
        o += 1;                        // filler
        let caps = p[o] | (p[o + 1] << 8); o += 2;
        let auth2 = new Uint8Array(0);
        let plugin = "mysql_native_password";
        if (o < p.length) {
          o += 1;                      // character_set
          o += 2;                      // status_flags
          caps = (caps | ((p[o] | (p[o + 1] << 8)) << 16)) >>> 0; o += 2;
          const authLen = p[o]; o += 1;
          o += 10;                     // reserved
          const n = Math.max(13, authLen - 8);
          auth2 = p.subarray(o, o + n); o += n;
          const end = myCstrEnd(p, o);
          if (end > o) plugin = myStr(p, o, end);
        }
        // The nonce every plugin scrambles against is part 1 + part 2 minus the
        // trailing NUL filler that part 2 carries.
        let nonce = catBytes(auth1, auth2);
        if (nonce.length > 0 && nonce[nonce.length - 1] === 0) nonce = nonce.subarray(0, nonce.length - 1);

        if (options.tls && (caps & MY_CAP.SSL)) {
          // Once the handshake decides to upgrade, everything after the greeting
          // must arrive over the encrypted channel: bytes already buffered in
          // plaintext are an injection attempt and must never reach the auth or
          // command handlers.
          if (readBuf.length > 0) {
            failFatal(myErr("Unexpected plaintext packet buffered behind the server greeting",
                            MY_CODE.UNEXPECTED_PACKET));
            return;
          }
          failFatal(myErr("MySQL TLS upgrade is not yet implemented in mbun (DEFERRED: needs STARTTLS on the pool socket)",
                          MY_CODE.FAILED));
          return;
        }
        const resp = myAuthResponse(plugin, options.password, nonce);
        if (resp === null) { failFatal(myErr("Unsupported authentication plugin " + plugin, MY_CODE.UNSUPPORTED_AUTH_PLUGIN)); return; }
        status = "auth";
        if (!writePacket(seqId + 1, buildHandshakeResponse(caps, resp, plugin))) onSocketDown("closed");
      };

      const onAuthPacket = (seqId, p) => {
        // A packet whose declared length is 0 cannot carry an auth status byte.
        if (p.length === 0) { failFatal(myErr("Invalid AuthSwitchRequest", MY_CODE.INVALID_AUTH_SWITCH)); return; }
        const h = p[0];
        if (h === 0x00) {   // OK_Packet: authenticated
          status = "connected";
          settleDial(dialResolve, undefined);
          pumpQueue();
          return;
        }
        if (h === 0xff) { failFatal(myParseErr(p)); return; }
        if (h === 0x01) {   // AuthMoreData
          if (p.length > 1 && p[1] === 0x03) return;   // caching_sha2 fast_auth_success; OK follows
          failFatal(myErr("caching_sha2_password full authentication requires TLS or the server public key",
                          MY_CODE.PUBLIC_KEY_RETRIEVAL));
          return;
        }
        if (h === 0xfe) {   // AuthSwitchRequest
          const end = myCstrEnd(p, 1);
          if (end >= p.length) { failFatal(myErr("Invalid AuthSwitchRequest", MY_CODE.INVALID_AUTH_SWITCH)); return; }
          const plugin = myStr(p, 1, end);
          let data = p.subarray(end + 1);
          if (data.length > 0 && data[data.length - 1] === 0) data = data.subarray(0, data.length - 1);
          // scramble() slices nonce[0..8] and nonce[8..20]; a server-controlled
          // plugin_data shorter than that must be rejected, not read past.
          if (data.length < 20) { failFatal(myErr("Missing auth data", MY_CODE.MISSING_AUTH_DATA)); return; }
          const resp = myAuthResponse(plugin, options.password, data);
          if (resp === null) { failFatal(myErr("Unsupported authentication plugin " + plugin, MY_CODE.UNSUPPORTED_AUTH_PLUGIN)); return; }
          if (!writePacket(seqId + 1, resp)) onSocketDown("closed");
          return;
        }
        failFatal(myErr("Unexpected packet during authentication", MY_CODE.UNEXPECTED_PACKET));
      };

      const sendExecute = (q) => {
        const st = q.stmt;
        step = "resultHeader";
        q.columns = st.columns;
        const binds = q.binds || [];
        const body = [MY_CMD.STMT_EXECUTE,
                      st.id & 0xff, (st.id >> 8) & 0xff, (st.id >> 16) & 0xff, (st.id >>> 24) & 0xff,
                      0,             // flags: CURSOR_TYPE_NO_CURSOR
                      1, 0, 0, 0];   // iteration_count
        if (binds.length > 0) {
          const nullBytes = (binds.length + 7) >> 3;
          const nullMap = new Array(nullBytes).fill(0);
          for (let i = 0; i < binds.length; i++) if (binds[i] == null) nullMap[i >> 3] |= 1 << (i & 7);
          for (const b of nullMap) body.push(b);
          body.push(1);   // new_params_bound_flag
          for (let i = 0; i < binds.length; i++) body.push(0xfe, 0);   // MYSQL_TYPE_STRING
          for (let i = 0; i < binds.length; i++) {
            if (binds[i] == null) continue;
            const v = myEnc.encode(String(binds[i]));
            myPutLenenc(body, v.length);
            for (const b of v) body.push(b);
          }
        }
        if (!writePacket(0, new Uint8Array(body))) abortHead(myErr("Connection closed", MY_CODE.CLOSED));
      };

      const pumpQueue = () => {
        if (inflight || status !== "connected" || !sock) return;
        const q = queue[0];
        if (!q) return;
        inflight = q;
        q.rows = []; q.columns = null; q.error = null; q.affectedRows = 0; q.lastInsertId = 0;
        if (q.flags.simple || options.prepare === false) {
          q.viaText = true;
          step = "resultHeader";
          const body = [MY_CMD.QUERY];
          for (const b of myEnc.encode(q.text)) body.push(b);
          if (!writePacket(0, new Uint8Array(body))) abortHead(myErr("Connection closed", MY_CODE.CLOSED));
          return;
        }
        q.viaText = false;
        const cached = statements.get(q.text);
        if (cached) { q.stmt = cached; sendExecute(q); return; }
        step = "prepareOk";
        const body = [MY_CMD.STMT_PREPARE];
        for (const b of myEnc.encode(q.text)) body.push(b);
        if (!writePacket(0, new Uint8Array(body))) abortHead(myErr("Connection closed", MY_CODE.CLOSED));
      };

      const parseTextRow = (p, q) => {
        const cols = q.columns || [];
        const wantRaw = q.flags && (q.flags.values || q.flags.raw);
        const out = wantRaw ? new Array(cols.length) : {};
        let o = 0;
        for (let i = 0; i < cols.length; i++) {
          let v = null;
          if (p[o] === 0xfb) { o += 1; }
          else {
            const le = myLenenc(p, o);
            if (!le || le.value == null || !Number.isSafeInteger(le.value)) throw new Error("short row");
            o += le.width;
            v = myCoerceText(myStr(p, o, o + le.value), cols[i].type);
            o += le.value;
          }
          if (wantRaw) out[i] = v; else out[cols[i].name] = v;
        }
        return out;
      };

      // Binary protocol row — page_protocol_binary_resultset_row.html: Int<1>(0x00),
      // NULL bitmap of (n + 7 + 2) / 8 bytes (offset 2), then the packed values.
      const parseBinaryRow = (p, q) => {
        const cols = q.columns || [];
        const wantRaw = q.flags && (q.flags.values || q.flags.raw);
        const out = wantRaw ? new Array(cols.length) : {};
        const nullBytes = (cols.length + 9) >> 3;
        let o = 1 + nullBytes;
        const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
        for (let i = 0; i < cols.length; i++) {
          const isNull = (p[1 + ((i + 2) >> 3)] & (1 << ((i + 2) & 7))) !== 0;
          let v = null;
          if (!isNull) {
            const t = cols[i].type;
            if (t === 0x01) { v = dv.getInt8(o); o += 1; }
            else if (t === 0x02 || t === 0x0d) { v = dv.getInt16(o, true); o += 2; }
            else if (t === 0x03 || t === 0x09) { v = dv.getInt32(o, true); o += 4; }
            else if (t === 0x08) { v = Number(dv.getBigInt64(o, true)); o += 8; }
            else if (t === 0x04) { v = dv.getFloat32(o, true); o += 4; }
            else if (t === 0x05) { v = dv.getFloat64(o, true); o += 8; }
            else {
              const le = myLenenc(p, o);
              if (!le || le.value == null || !Number.isSafeInteger(le.value)) throw new Error("short row");
              o += le.width;
              v = myStr(p, o, o + le.value);
              o += le.value;
            }
          }
          if (wantRaw) out[i] = v; else out[cols[i].name] = v;
        }
        return out;
      };

      const onQueryPacket = (seqId, p) => {
        const q = inflight;
        if (!q) return;   // stale bytes for an already-settled exchange
        if (p.length === 0) { settleQuery(myErr("Empty packet", MY_CODE.UNEXPECTED_PACKET)); pumpQueue(); return; }
        if (p[0] === 0xff) { settleQuery(myParseErr(p)); pumpQueue(); return; }
        const isEof = (p[0] === 0xfe && p.length < 9);
        switch (step) {
          case "prepareOk": {
            // COM_STMT_PREPARE_OK — page_protocol_com_stmt_prepare.html:
            // Int<1>(0x00) Int<4>(statement_id) Int<2>(num_columns) Int<2>(num_params)
            if (p[0] !== 0x00 || p.length < 12) {
              failFatal(myErr("Invalid COM_STMT_PREPARE_OK packet", MY_CODE.INVALID_PREPARE_OK));
              return;
            }
            const id = (p[1] | (p[2] << 8) | (p[3] << 16) | (p[4] << 24)) >>> 0;
            const numColumns = p[5] | (p[6] << 8);
            const numParams = p[7] | (p[8] << 8);
            // statement_id 0 is reserved: a prepare-OK carrying it is a protocol
            // error and must never reach COM_STMT_EXECUTE.
            if (id === 0) {
              failFatal(myErr("Invalid COM_STMT_PREPARE_OK packet: statement id 0", MY_CODE.INVALID_PREPARE_OK));
              return;
            }
            q.stmt = { id, columns: [] };
            q.pendingParams = numParams;
            q.pendingColumns = numColumns;
            if (numParams > 0) { step = "prepareParams"; return; }
            if (numColumns > 0) { step = "prepareColumns"; return; }
            statements.set(q.text, q.stmt);
            sendExecute(q);
            return;
          }
          case "prepareParams": {
            if (isEof) return;   // EOF terminator when CLIENT_DEPRECATE_EOF is off
            if (--q.pendingParams > 0) return;
            if (q.pendingColumns > 0) { step = "prepareColumns"; return; }
            statements.set(q.text, q.stmt);
            sendExecute(q);
            return;
          }
          case "prepareColumns": {
            if (isEof) return;
            q.stmt.columns.push(myParseColumnDef(p));
            if (--q.pendingColumns > 0) return;
            statements.set(q.text, q.stmt);
            sendExecute(q);
            return;
          }
          case "resultHeader": {
            if (p[0] === 0x00 || isEof) {   // OK_Packet: no result set
              const ok = myParseOk(p);
              q.affectedRows = ok.affectedRows;
              q.lastInsertId = ok.lastInsertId;
              settleQuery(null);
              pumpQueue();
              return;
            }
            if (p[0] === 0xfb) { settleQuery(myErr("LOCAL INFILE is not supported", MY_CODE.LOCAL_INFILE)); pumpQueue(); return; }
            const le = myLenenc(p, 0);
            // A server-declared field count the client cannot represent must be
            // rejected outright — never used to size an allocation.
            if (!le || le.value == null || !Number.isSafeInteger(le.value) || le.value > 0xffff) {
              failFatal(myErr("Invalid result set column count", MY_CODE.OVERFLOW));
              return;
            }
            q.pendingColumns = le.value;
            q.columns = [];
            step = q.pendingColumns > 0 ? "columns" : "rows";
            return;
          }
          case "columns": {
            if (isEof) { step = "rows"; return; }
            q.columns.push(myParseColumnDef(p));
            if (--q.pendingColumns <= 0) step = "rows";
            return;
          }
          case "rows": {
            if (isEof) { settleQuery(q.error); pumpQueue(); return; }
            try { q.rows.push(q.viaText ? parseTextRow(p, q) : parseBinaryRow(p, q)); }
            catch (e) { q.error = myErr("Invalid result row", MY_CODE.INVALID_RESULT_ROW); }
            return;
          }
          default:
            return;
        }
      };

      const dialOnce = () => new Promise((resolve, reject) => {
        status = "connecting";
        readBuf = new Uint8Array(0);
        downOnce = false;
        opened = false;
        dialSettled = false;
        dialResolve = resolve;
        dialReject = reject;
        const handlers = {
          open(bs) {
            opened = true;
            if (status === "closed" || closed) { try { bs.end(); } catch (e) {} return; }
            status = "handshake";  // MySQL server speaks first; wait for HandshakeV10.
            sock = bs;
          },
          data(bs, chunk) {
            readBuf = catBytes(readBuf, chunk);
            for (;;) {
              if (readBuf.length < 4) break;
              const len = readBuf[0] | (readBuf[1] << 8) | (readBuf[2] << 16);
              if (readBuf.length < 4 + len) break;
              const seqId = readBuf[3];
              const payload = readBuf.slice(4, 4 + len);
              readBuf = readBuf.slice(4 + len);
              if (status === "handshake") onHandshakePacket(seqId, payload);
              else if (status === "auth") onAuthPacket(seqId, payload);
              else if (status === "connected") onQueryPacket(seqId, payload);
              if (status === "closed") break;
            }
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

      const execute = (text, binds, flags) => new Promise((resolve, reject) => {
        const q = { text, binds: binds || [], flags: flags || {}, resolve, reject,
                    rows: [], columns: null, error: null, stmt: null, viaText: false,
                    affectedRows: 0, lastInsertId: 0 };
        queue.push(q);
        ensureConnected().then(pumpQueue, (e) => {
          const i = queue.indexOf(q);
          if (i >= 0) { queue.splice(i, 1); reject(e); }
          else if (inflight !== q) reject(e);
        });
      });

      const close = () => {
        closed = true;
        if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
        if (retryWake) { const w = retryWake; retryWake = null; w(); }
        settleDial(dialReject, myErr("Connection closed", MY_CODE.CLOSED));
        failAllRequests(myErr("Connection closed", MY_CODE.CLOSED));
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

      // ---- connection pool -------------------------------------------------
      // PORT-SOURCE: bun-ref src/js/internal/sql/shared.ts — the pool array is
      // `new Array(max)` and is filled one slot at a time when the pool starts.
      // A function-valued `password` is resolved per slot, synchronously, inside
      // that fill loop, so pool methods re-entered from it must tolerate slots
      // that have not been assigned yet (bun issue #32198).
      const poolSize = options.adapter === "sqlite" ? 0 : Math.max(1, Number(options.max) || 1);
      const drivers = new Array(poolSize).fill(null);
      let filling = false, nextSlot = 0;
      const slotOptions = () => {
        if (typeof options.password !== "function") return options;
        const o = Object.assign({}, options);
        try { o.password = options.password(); } catch (e) { o.password = ""; }
        return o;
      };
      const makeDriver = () => (options.adapter === "mysql" ? makeMysqlDriver(slotOptions())
                                                            : makePgDriver(slotOptions()));
      const startPool = () => {
        if (filling) return;   // re-entered from password(): slots are still being filled
        filling = true;
        try { for (let i = 0; i < poolSize; i++) if (drivers[i] == null) drivers[i] = makeDriver(); }
        finally { filling = false; }
      };
      const liveDrivers = () => drivers.filter((d) => d != null);
      // Queries go to a live slot; with the common max:1 that is the single
      // connection, so the pooled path is a no-op there.
      sqlObj.__driver = () => {
        startPool();
        const live = liveDrivers();
        if (live.length === 0) return null;
        const d = live[nextSlot % live.length];
        nextSlot++;
        return d;
      };

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
        startPool();
        const live = liveDrivers();
        // Re-entered from the pool-start fill loop: no slot is assigned yet.
        if (live.length === 0) return Promise.resolve(sqlObj);
        return Promise.all(live.map((d) => d.connect())).then(() => sqlObj);
      };
      sqlObj.close = (_options) => {
        if (sqliteHandle != null && SQN) { try { SQN.close(sqliteHandle); } catch {} sqliteHandle = null; }
        for (const d of liveDrivers()) { try { d.close(); } catch (e) {} }
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
