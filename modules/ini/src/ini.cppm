// ini.cppm — mbun.ini: core INI / .npmrc parser + serializer (pure logic).
//
// Aggregator over the submodules. Behavior is a mechanical port of bun's
// src/ini/lib.rs core (sections, key=value, comments, quotes -> JSON, `key[]=`
// arrays, escapes, `${VAR}`/`${VAR?}` env expansion) onto a self-contained
// `ini::Value` tree with a JS-style `to_string`.
//
// The npmrc semantic layer (load_npmrc / ConfigIterator / ScopeIterator, auth
// keys, scoped registries, the option matrix) is ported in `mbun.ini.npmrc` on
// top of this core, with a self-contained registry-URL parser. Still DEFERRED
// there: filesystem npmrc path discovery/precedence and the pnpm regex matcher
// (`hoist-pattern`) — both couple to bun install infra.
export module mbun.ini;

export import mbun.ini.value;
export import mbun.ini.json;
export import mbun.ini.parser;
export import mbun.ini.npmrc;
