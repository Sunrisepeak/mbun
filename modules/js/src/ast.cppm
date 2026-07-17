// src/ast.cppm — module mbun.ast
//
// AST node model for the JS/TS/JSX parser (T2.4). Performance-first: every node
// is a fixed-size record living in a single flat arena (std::vector), and nodes
// reference each other by 32-bit index rather than by owning pointer. There is
// no per-node heap ownership and no shared_ptr; the arena owns everything and is
// freed in one shot. Variable-arity children (statement lists, call arguments,
// array/object members) live in a second flat pool and a node stores a (offset,
// length) span into it. This keeps a Node compact and tree walks cache-friendly.
//
// This is not a mechanical port of bun's Zig AST: it is a compact self-contained
// model that carries exactly what the T2.4 parser subset needs (expressions,
// statements, literals, and the TS constructs the transpiler tests exercise).
export module mbun.ast;

import std;

export namespace mbun::ast {

using NodeIndex = std::uint32_t;
inline constexpr NodeIndex NONE{0xFFFF'FFFFu};

// A range into the flat child pool (see Arena::children_).
using ListRef = std::uint32_t;  // index of the first child; count stored on Node

enum class NodeKind : std::uint8_t {
    Missing,  // error-recovery placeholder

    // ── expressions ──
    Identifier,
    PrivateName,     // #foo (as a value / brand-check operand)
    NumberLiteral,
    StringLiteral,
    BigIntLiteral,
    BooleanLiteral,
    NullLiteral,
    RegExpLiteral,
    TemplateLiteral,
    TaggedTemplate,
    ThisExpr,
    SuperExpr,
    ImportMeta,
    ImportCall,
    ArrayLiteral,
    ObjectLiteral,
    Property,       // object / class member: a=key b=value c=initializer(opt);
                    // aux=PropertyKind; flags pflags (see below)
    SpreadElement,  // ...expr
    Unary,          // prefix op in aux; operand in a
    Yield,          // yield / yield* ; a=operand(opt), aux bit0 = delegate (`*`)
    Update,         // ++/-- ; aux=op, flags bit0 = prefix
    Binary,         // aux=op token; a=lhs b=rhs
    Logical,        // && || ?? ; aux=op; a=lhs b=rhs
    Assignment,     // aux=op; a=target b=value
    Conditional,    // a=test b=consequent c=alternate
    Sequence,       // comma expression; list = elements
    Call,           // a=callee; list=args; flags ocflags   (e.rs:270 E::Call)
    New,            // a=callee; list=args
    Member,         // a=object; text=property name; flags ocflags  (e.rs:324 E::Dot)
    PrivateMember,  // a=object; text=#name; flags ocflags
    Index,          // a=object; b=index expr; flags ocflags (e.rs:359 E::Index)
    Paren,          // a=inner
    Arrow,          // list=Arg; b=body; flags fnflags        (e.rs E::Arrow)
    FunctionExpr,   // a=name(opt); list=Arg; b=body; flags fnflags  (g.rs G::Fn)
    ClassExpr,      // a=name(opt); b=superclass(opt); list=Property;
                    // aux=close-brace offset                 (g.rs:57 G::Class)

    // ── bindings (destructuring patterns) ──
    // The left-hand side of a declaration is a *binding*, not an expression:
    // `{a}` as a binding declares `a`, while `{a}` as an expression reads it.
    // bun models this as a separate `B` union (src/ast/b.rs:35) reached from
    // `G::Decl.binding` (src/ast/g.rs:20); these kinds are its flat-arena image.
    BindingIdentifier,  // text=name                        (b.rs B::BIdentifier)
    BindingArray,       // list=BindingElement; flags BFlags (b.rs b::Array)
    BindingObject,      // list=BindingProperty; flags BFlags (b.rs b::Object)
    BindingElement,     // a=binding b=default(opt)      (nodes.rs ArrayBinding)
    BindingProperty,    // a=key b=value(binding) c=default(opt); flags BFlags
    BindingMissing,     // array hole: `[, x]`                (b.rs B::BMissing)

    // A single function parameter — image of bun `G::Arg` (src/ast/g.rs:332),
    // which is a STRUCT, not a binding: `{binding, default, ts_decorators,
    // is_typescript_ctor_field, ts_metadata}`. The params list of an Arrow /
    // FunctionExpr / FunctionDecl is a list of these, never of bare Binding*
    // nodes, because bun's `print_fn_args` (lib.rs:2487) reads `arg.binding` and
    // `arg.default` as two separate fields off each element (:2515, :2517).
    //
    // Storing the default on the binding instead would have conflated it with
    // `BindingElement`/`BindingProperty`'s default, which is a DIFFERENT thing:
    // those defaults are part of a destructuring pattern (`[x = 1]` inside the
    // param), and a param can have both (`function f([x = 1] = [])`). Two slots,
    // two meanings — the same reason bun keeps `G::Arg` and `b::Property` apart.
    //
    // The three fields with no slot get none for the pflags reason (see the
    // Property note below): `ts_decorators` are erased into the class lowering
    // plan rather than hung off the node, `is_typescript_ctor_field` is consumed
    // by the parser's own `propNames` path (js_parser.cppm `parse_params_`), and
    // `ts_metadata` is emit-time TS metadata with no printer consumer at all.
    Arg,                // a=binding b=default(opt)              (g.rs:332 G::Arg)

    // ── statements ──
    Program,          // list=statements
    ExpressionStmt,   // a=expression
    Block,            // list=statements
    EmptyStmt,
    VarDecl,          // aux=VarKind; list=declarators
    VarDeclarator,    // a=name b=init(opt)
    FunctionDecl,     // a=name; list=Arg; b=body; flags fnflags   (s.rs S::Function)
    ClassDecl,        // a=name; b=superclass(opt); list=Property;
                      // aux=close-brace offset; flags fnflags::IsExport (S::Class)
    ReturnStmt,       // a=argument(opt)
    IfStmt,           // a=test b=consequent c=alternate(opt)
    ForStmt,          // a=init b=test c=update ; body in list[0]
    ForInStmt,        // a=left b=right ; body in list[0] ; flags bit0 = of
    ThrowStmt,        // a=argument
    WhileStmt,        // a=test b=body                          (s.rs S::While)
    DoWhileStmt,      // a=body b=test                        (s.rs S::DoWhile)
    SwitchStmt,       // a=test; list=SwitchCase                (s.rs S::Switch)
    SwitchCase,       // a=value(opt; NONE => `default:`); list=body (nodes.rs Case)
    TryStmt,          // list=body; b=CatchClause(opt) c=FinallyClause(opt)
    CatchClause,      // a=binding(opt); list=body             (nodes.rs Catch)
    FinallyClause,    // list=stmts                          (nodes.rs Finally)
    BreakStmt,        // text=label (empty => unlabelled)       (s.rs S::Break)
    ContinueStmt,     // text=label (empty => unlabelled)    (s.rs S::Continue)
    LabeledStmt,      // text=name a=stmt                       (s.rs S::Label)
    WithStmt,         // a=value b=body                          (s.rs S::With)

    // ── TypeScript ──
    EnumDecl,         // a=name; list=members
    EnumMember,       // a=key b=init(opt)
    TypeAliasDecl,    // a=name
    InterfaceDecl,    // a=name
    NamespaceDecl,    // a=name; list=body
    TypeParam,        // text=name

    // Image of bun `S::TypeScript` (src/ast/s.rs:72-74, "a stand-in for a
    // TypeScript type declaration"; StmtData::STypeScript, stmt.rs:292). Carries
    // NO payload by design — it exists only to say "a type-only statement was
    // here, emit nothing".
    //
    // Every `declare …` collapses to this. bun's generic declare arm parses the
    // inner statement purely to find its extent and then THROWS IT AWAY —
    // `return Ok(Some(p.s(S::TypeScript {}, loc)))` (parse/parse_stmt.rs:1882),
    // and likewise for `declare global` (:1816) and a declare'd namespace/enum
    // (parse/parse_typescript.rs:352-358). The visit pass then drops it without
    // pushing anything (visit/visit_stmt.rs:76-79), which is why bun's PRINTER
    // has no arm for it — it can never arrive.
    //
    // mbun's parser used to return the INNER node instead (js_parser.cppm:456),
    // which was invisible for as long as only the erasure path existed — that
    // path reads `Arena::edits_` and never looks at a node, so a `declare
    // namespace N{}` left a NamespaceDecl in the arena that nothing consulted.
    // The AST-rebuild printer does consult it, and could not tell that node from
    // a real runtime namespace. Hence this kind: the erasure is now recorded in
    // the TREE as well as in the edit list.
    TypeScriptStmt,

    // ── ESM (module syntax) ──
    //
    // These two used to sit under `── TypeScript ──` above, with no payload
    // documented and none written: for the ERASURE path (the production one) an
    // import needs no model — its text is already in the source and the Edit list
    // does the lowering — so both kinds were pure erasure MARKERS. The AST-rebuild
    // printer needs the real thing, so the structure below is now populated by
    // parse_import_/parse_export_. The erasure path is untouched by that: it reads
    // only `Arena::edits_` (see erase_slice), never a node.
    //
    // Blueprint: s.rs:198 `S::Import`, :48 `ExportClause`, :56 `ExportStar`,
    // :79 `ExportFrom`, :86 `ExportDefault`, :64 `ExportEquals`.
    //
    // ⚠️ NOT modelled here, deliberately: bun's `namespace_ref` /
    // `import_record_index` / `default_name: LocRef` are all symbol-table and
    // ImportRecord handles, and mbun has neither table (no Symbol, no Scope, no
    // Ref). They exist for BUNDLING — renaming and cross-module linking. A
    // transpiler never resolves them: bun's own printer only ever asks
    // `name_for_symbol(ref)` (lib.rs:2472), which without a renamer is just the
    // name the source wrote. So mbun stores the NAMES directly, and the day a
    // symbol table lands these slots are where the Refs hang.
    //
    // ImportDecl — ref s.rs:198. `import <default>, * as <ns> / { items } from <text>`
    //   text = module specifier, RAW source slice INCLUDING quotes ("" if absent)
    //   a    = default binding      → Identifier            (NONE if none)
    //   b    = namespace binding    → Identifier `* as ns`  (NONE if none)
    //   list = ClauseItem*          → the `{ … }` clause    (empty if none/`{}`)
    //   flags= mflags::IsSingleLine / IsTypeOnly
    // An EMPTY clause and NO clause are the same node here, because bun prints
    // them the same: `import {} from "y"` and `import "y"` both emit `import"y";`
    // and `import d, {} from "y"` emits `import d from "y";` (verified, 1.4.0).
    ImportDecl,
    // ExportDecl — the discriminant is `aux` (an ExportForm), NOT the kind.
    // bun has no export-wrapper node at all (`export function f(){}` is an
    // S::Function carrying is_export); mbun's parser has always built a wrapper,
    // and two live call sites already test `kind == ExportDecl`
    // (js_parser.cppm:304 / :2906) to drive the top-level-await hoisting. Splitting
    // it into bun's five S:: kinds would break both for no gain the printer can
    // use, so the form rides in `aux` instead — which was free.
    //   aux  = ExportForm (below)
    //   a    = Decl: the declaration │ DefaultDecl/DefaultExpr: the value
    //          │ Assign: the `export = <expr>` value         (NONE otherwise)
    //   b    = Star: `as ns` alias → Identifier/StringLiteral (NONE if bare `*`)
    //   text = Star/ClauseFrom: module specifier, RAW with quotes
    //   list = Clause/ClauseFrom: ClauseItem*
    //   flags= mflags::IsSingleLine
    ExportDecl,
    // ClauseItem — ref nodes.rs:589. ONE `{ a }` / `{ a as b }` specifier.
    //
    // bun keeps THREE fields (`alias`, `name: LocRef`, `original_name`) because a
    // renamer must tell the external name from the local symbol, and the two sides
    // swap between import and export:
    //   import { L as R }        alias=L (the other module's export), name=R (local)
    //   export { L as R }        name=L  (local),  alias=R (the exported name)
    //   export { L as R } from   original_name=L (the other module's), alias=R
    // So `alias` is always the EXTERNAL name and `name` the LOCAL binding — the
    // doc comment on nodes.rs:591 ("For imports: `import { foo as bar }` - "bar"
    // is the alias") contradicts the printer at lib.rs:3081-3090, which prints
    // `alias` THEN `name` and would emit `bar as foo`; the printer is right and
    // the comment is stale. Either way, in SOURCE ORDER it is always L then R.
    //
    // mbun stores source order and nothing else:
    //   a = L → Identifier │ StringLiteral (`import { "a-b" as c }`)
    //   b = R → same, or NONE when the source wrote no `as`
    // That is lossless for all three of bun's arms *today* because all three
    // reduce to the same rule without a renamer — print L, then ` as R` if the
    // two NAMES DIFFER (lib.rs:3082/:3100/:3119 each compare, and each is an
    // equality test on the very strings mbun holds). See module_syntax.cppm, which
    // keeps the three arms separate anyway so a future renamer has a seam.
    ClauseItem,
};

// ExportForm — `ExportDecl::aux`. Mirrors the parser's own ExForm (which drives
// the erasure path's top-level-await hoisting, js_parser.cppm:141) so the two can
// never drift: the parser aliases ExForm TO this type.
//
// bun spends five S:: kinds on what this enumerates (s.rs :48/:56/:64/:79/:86);
// see the ExportDecl note above for why mbun keeps one kind and a tag.
enum class ExportForm : std::uint32_t {
    None = 0,     // type-only export — fully erased, prints NOTHING
    Assign,       // export = e            (TS export-assignment; s.rs:64)
    DefaultDecl,  // export default function/class …          (s.rs:86)
    DefaultExpr,  // export default <expr>                    (s.rs:86)
    Star,         // export * [as ns] from "m"                (s.rs:56)
    Clause,       // export { … }                             (s.rs:48)
    ClauseFrom,   // export { … } from "m"                    (s.rs:79)
    Decl,         // export <declaration>
};

std::string_view node_kind_name(NodeKind k) {
    switch (k) {
    case NodeKind::Missing: return "Missing";
    case NodeKind::Identifier: return "Identifier";
    case NodeKind::PrivateName: return "PrivateName";
    case NodeKind::NumberLiteral: return "NumberLiteral";
    case NodeKind::StringLiteral: return "StringLiteral";
    case NodeKind::BigIntLiteral: return "BigIntLiteral";
    case NodeKind::BooleanLiteral: return "BooleanLiteral";
    case NodeKind::NullLiteral: return "NullLiteral";
    case NodeKind::RegExpLiteral: return "RegExpLiteral";
    case NodeKind::TemplateLiteral: return "TemplateLiteral";
    case NodeKind::TaggedTemplate: return "TaggedTemplate";
    case NodeKind::ThisExpr: return "ThisExpr";
    case NodeKind::SuperExpr: return "SuperExpr";
    case NodeKind::ImportMeta: return "ImportMeta";
    case NodeKind::ImportCall: return "ImportCall";
    case NodeKind::ArrayLiteral: return "ArrayLiteral";
    case NodeKind::ObjectLiteral: return "ObjectLiteral";
    case NodeKind::Property: return "Property";
    case NodeKind::SpreadElement: return "SpreadElement";
    case NodeKind::Unary: return "Unary";
    case NodeKind::Yield: return "Yield";
    case NodeKind::Update: return "Update";
    case NodeKind::Binary: return "Binary";
    case NodeKind::Logical: return "Logical";
    case NodeKind::Assignment: return "Assignment";
    case NodeKind::Conditional: return "Conditional";
    case NodeKind::Sequence: return "Sequence";
    case NodeKind::Call: return "Call";
    case NodeKind::New: return "New";
    case NodeKind::Member: return "Member";
    case NodeKind::PrivateMember: return "PrivateMember";
    case NodeKind::Index: return "Index";
    case NodeKind::Paren: return "Paren";
    case NodeKind::Arrow: return "Arrow";
    case NodeKind::FunctionExpr: return "FunctionExpr";
    case NodeKind::ClassExpr: return "ClassExpr";
    case NodeKind::BindingIdentifier: return "BindingIdentifier";
    case NodeKind::BindingArray: return "BindingArray";
    case NodeKind::BindingObject: return "BindingObject";
    case NodeKind::BindingElement: return "BindingElement";
    case NodeKind::BindingProperty: return "BindingProperty";
    case NodeKind::BindingMissing: return "BindingMissing";
    case NodeKind::Arg: return "Arg";
    case NodeKind::Program: return "Program";
    case NodeKind::ExpressionStmt: return "ExpressionStmt";
    case NodeKind::Block: return "Block";
    case NodeKind::EmptyStmt: return "EmptyStmt";
    case NodeKind::VarDecl: return "VarDecl";
    case NodeKind::VarDeclarator: return "VarDeclarator";
    case NodeKind::FunctionDecl: return "FunctionDecl";
    case NodeKind::ClassDecl: return "ClassDecl";
    case NodeKind::ReturnStmt: return "ReturnStmt";
    case NodeKind::IfStmt: return "IfStmt";
    case NodeKind::ForStmt: return "ForStmt";
    case NodeKind::ForInStmt: return "ForInStmt";
    case NodeKind::ThrowStmt: return "ThrowStmt";
    case NodeKind::WhileStmt: return "WhileStmt";
    case NodeKind::DoWhileStmt: return "DoWhileStmt";
    case NodeKind::SwitchStmt: return "SwitchStmt";
    case NodeKind::SwitchCase: return "SwitchCase";
    case NodeKind::TryStmt: return "TryStmt";
    case NodeKind::CatchClause: return "CatchClause";
    case NodeKind::FinallyClause: return "FinallyClause";
    case NodeKind::BreakStmt: return "BreakStmt";
    case NodeKind::ContinueStmt: return "ContinueStmt";
    case NodeKind::LabeledStmt: return "LabeledStmt";
    case NodeKind::WithStmt: return "WithStmt";
    case NodeKind::EnumDecl: return "EnumDecl";
    case NodeKind::EnumMember: return "EnumMember";
    case NodeKind::TypeAliasDecl: return "TypeAliasDecl";
    case NodeKind::InterfaceDecl: return "InterfaceDecl";
    case NodeKind::NamespaceDecl: return "NamespaceDecl";
    case NodeKind::TypeScriptStmt: return "TypeScriptStmt";
    case NodeKind::ImportDecl: return "ImportDecl";
    case NodeKind::ExportDecl: return "ExportDecl";
    case NodeKind::ClauseItem: return "ClauseItem";
    case NodeKind::TypeParam: return "TypeParam";
    }
    return "?";
}

// VarDecl kinds (Node::aux for a VarDecl).
enum class VarKind : std::uint32_t { Var = 0, Let = 1, Const = 2 };

// Node::flags bits for the Binding* kinds. The array-rest / object-rest split
// mirrors bun rather than unifying them: bun keeps `has_spread` on `b::Array`
// (src/ast/b.rs:84) and asks `has_spread && i == len-1` at use
// (src/ast/binding.rs:230), but tags object rest per-property via
// `flags::Property::IsSpread` (src/ast/binding.rs:258). Same asymmetry here so
// the two models stay 1:1 and neither side needs a translation table.
namespace bflags {
// BindingArray
inline constexpr std::uint8_t HasSpread{1u << 0};  // last element is `...rest`
// BindingArray / BindingObject
inline constexpr std::uint8_t IsSingleLine{1u << 1};
// BindingProperty
inline constexpr std::uint8_t IsComputed{1u << 2};    // `{[k]: v}`
inline constexpr std::uint8_t IsSpread{1u << 3};      // `{...rest}`
inline constexpr std::uint8_t WasShorthand{1u << 4};  // `{a}` not `{a: a}`
}  // namespace bflags

// ─────────────────────────────────────────────────────────────────────────────
// Node::flags bits for Arrow / FunctionExpr / FunctionDecl. ref bun
// `flags::Function` (src/ast/lib.rs:3292), an `EnumSet`, so — exactly as pflags
// documents — what is ported is the SET of flags and their meanings, not the
// enumset crate's numeric encoding. The bit order is bun's declaration order
// anyway, so the two can be diffed line for line.
//
// Only the first three plus IsExport have a printer consumer; the rest are
// parser/analysis state bun carries on the same set. They are declared so the
// SET stays 1:1 with bun's (a flag that exists is a flag nobody re-invents under
// a second name), and each is marked with whether mbun's parser can set it.
// ─────────────────────────────────────────────────────────────────────────────
namespace fnflags {
inline constexpr std::uint8_t IsAsync{1u << 0};      // `async function` (lib.rs:5322)
inline constexpr std::uint8_t IsGenerator{1u << 1};  // `function*`      (lib.rs:5326)
inline constexpr std::uint8_t HasRestArg{1u << 2};   // `(...a)`         (lib.rs:2510)
// Never set by mbun's parser — no consumer. bun uses it to decide whether a
// function body's scope needs an extra `if` scope for TS/`arguments` lowering.
inline constexpr std::uint8_t HasIfScope{1u << 3};
// Never set: mbun's parser DELETES a TS overload signature / `abstract` method
// outright (js_parser.cppm `parse_class_member_`, the "No body" arm returns NONE
// after `add_edit`) rather than building a node and flagging it, so no node that
// survives can carry this. bun's `IsForwardDeclaration` is the same decision
// expressed as a flag (parse_property.rs:123-127 "Skip this property entirely").
inline constexpr std::uint8_t IsForwardDeclaration{1u << 4};
// Never set — "true if the function is a method" (lib.rs:3300). Strict-mode
// duplicate-parameter checking, which mbun's parser does not do.
inline constexpr std::uint8_t IsUniqueFormalParameters{1u << 5};
// "Only applicable to function statements" (lib.rs:3303) — `export function f`.
// Read by lib.rs:5318 (function) and, as `SClass::is_export`, by :5372.
inline constexpr std::uint8_t IsExport{1u << 6};
// Never set — bun scans `// eslint-disable react-hooks/…` comments for the React
// Compiler (lib.rs:3305-3306). mbun's lexer discards comments.
inline constexpr std::uint8_t HasReactHooksSuppression{1u << 7};
}  // namespace fnflags

// Node::flags bits for ObjectLiteral / ArrayLiteral. bun spells these as plain
// bool FIELDS on `E::Object` (src/ast/e.rs:1233 `is_single_line`) and `E::Array`
// (e.rs:45), not as an EnumSet — there is no `flags::Expr` to be 1:1 with. They
// are bits here because mbun's Node is a fixed-size record with one flags byte
// and no room for per-kind fields; the MEANING is bun's.
//
// `is_single_line` is set by the parser when the literal's source had no newline
// inside it, and it is what picks the whole layout at lib.rs:3941-3971: single
// line gets `{ a, b }` (print_space between), multi-line gets one property per
// indented line. It is NOT cosmetic drift — bun preserves the author's choice.
// bflags::IsSingleLine (above) is the same rule on the BINDING side, and the
// parser already records it there (`{a}` vs `{\n a\n}`), which is the precedent
// this follows.
namespace eflags {
inline constexpr std::uint8_t IsSingleLine{1u << 0};
}  // namespace eflags

// ─────────────────────────────────────────────────────────────────────────────
// Property — an object-literal member or a class member. ref bun `G::Property`
// (src/ast/g.rs:143).
//
// ⚠️ NOT `b::Property` (b.rs:68), which BindingProperty above images. The two
// share a name and nothing else: `b::Property` is the LHS of a destructuring
// pattern (`{a} = o` BINDS a), `G::Property` is a member of an object literal or
// class body (`{a: 1}` CREATES a). bun keeps them as two types; so does this
// arena. `print_binding` prints the first (lib.rs:5052), `print_property` the
// second (lib.rs:4743).
//
// bun's G::Property is {initializer, kind, flags, key, value, class_static_block,
// ts_decorators, ts_metadata}. The mapping onto the flat node:
//   key         -> a     (NONE for a spread — bun's key is an `Option` and
//                         g.rs:158 says "Key is optional for spread")
//   value       -> b     (NONE for a class field — g.rs:161 "omitted for class
//                         fields")
//   initializer -> c     (`= <expr>`: the default in a pattern (`{a = 1} = {}`)
//                         and the class-field initialiser — g.rs:143-151)
//   kind        -> aux   (PropertyKind)
//   flags       -> flags (pflags)
//
// DEFERRED — `class_static_block` and `ts_decorators` get no slot, because no
// Property this parser builds can reach them: a `static {}` block is returned as
// a plain Block (js_parser.cppm:2315, never a Property) and decorators are erased
// into the class lowering plan (DecMember::decs) rather than hung off the node.
// A slot would be an invented model ahead of its consumer. `ts_metadata` is
// emit-time TS metadata with no printer consumer at all.
// ─────────────────────────────────────────────────────────────────────────────

// Property kinds (Node::aux for a Property). ref bun `G::PropertyKind`, g.rs:245.
// Order is bun's. Nothing casts between the two enums, so the numbering is not
// load-bearing — it is identical so the two can be diffed line for line.
enum class PropertyKind : std::uint32_t {
    Normal = 0,
    Get = 1,               // `{get a(){}}`    — printer emits `get `  (lib.rs:4816)
    Set = 2,               // `{set a(v){}}`   — printer emits `set `  (lib.rs:4822)
    Spread = 3,            // `{...rest}`      — printer emits `...`   (lib.rs:4759)
    Declare = 4,           // TS `declare` class member
    Abstract = 5,          // TS `abstract` class member
    ClassStaticBlock = 6,  // `class C { static {} }`
    AutoAccessor = 7,      // `accessor x = 1` — printer emits `accessor `(lib.rs:4828)
};

// Node::flags bits for a Property. ref bun `flags::Property` (src/ast/lib.rs:3283).
// bun stores it as an `EnumSet`, so the numeric encoding there is the enumset
// crate's business; what is ported is the SET of flags and their meanings.
namespace pflags {
inline constexpr std::uint8_t IsComputed{1u << 0};    // `{[k]: v}`   (lib.rs:4869)
inline constexpr std::uint8_t IsMethod{1u << 1};      // `{m(){}}` not `{m: f}`
inline constexpr std::uint8_t IsStatic{1u << 2};      // `class C { static m(){} }`
inline constexpr std::uint8_t WasShorthand{1u << 3};  // `{a}` not `{a: a}`
// ⚠️ Object-literal spread does NOT set this — bun's `{...x}` is
// `G::Property{kind: Spread, value, ..Default::default()}` with flags EMPTY
// (parse_prefix.rs:820-829), and print_property switches on `kind` (:4759), never
// on this flag. It is the BINDING side that tags rest with a flag instead of a
// kind (`b::Property{flags: IsSpread}`, parse/mod.rs:1167 -> binding.rs:258),
// which is the same array/object asymmetry bflags documents above. Kept so the
// flag SET stays 1:1 with bun's; a Property built by this parser never sets it.
inline constexpr std::uint8_t IsSpread{1u << 4};
// mbun-only — bun has NO such flag, and the asymmetry is deliberate, not an
// oversight: in bun a private member is expressed by its KEY being an
// `E::PrivateIdentifier`, which print_property matches on (lib.rs:4993), because
// bun's key is a full expression. mbun's class-member parser builds no key node
// for `#x` — it parks the name in `Node::text` (js_parser.cppm:2432) — so "is
// private" has nowhere else to live and needs a bit of its own.
inline constexpr std::uint8_t IsPrivate{1u << 5};  // `class C { #x }`
}  // namespace pflags

// Node::flags bits for Member / PrivateMember / Index / Call — optional chaining.
//
// ref bun `Option<OptionalChain>` on `E::Dot` (e.rs:324), `E::Index` (e.rs:359)
// and `E::Call` (e.rs:270); the enum itself is `nodes.rs:705`. bun's three states
// are None / Start / Continuation; the two bits below encode the same tri-state
// (neither bit set == bun's `None`). Setting both is meaningless and never built.
//
//   a?.b     -> `.b` is Start          (nodes.rs:707)
//   a?.b.c   -> `.c` is Continuation   (nodes.rs:709-710)
//   (a?.b).c -> `.c` is None           (nodes.rs:710) — the parens END the chain
//
// Why the tri-state and not one bool: Continuation is what makes the WHOLE chain
// short-circuit. `a?.b.c` must yield `undefined` (not throw) when `a` is null, so
// the printer has to know `.c` belongs to the chain that `?.b` started. A bool
// ("is this link written with `?.`") cannot tell `a?.b.c` from `(a?.b).c`, and
// those two differ at runtime — the second one THROWS. See the printer's
// `HasNonOptionalChainParent` handling (js_printer/expr.cppm) for the other half.
//
// IsOptionalStart is deliberately bit 1: the legacy printer already reads
// `flags & 2u` on Member/PrivateMember to emit `?.` (js_printer/legacy.cppm:243,
// :248) and the parser already wrote it there, so keeping the bit value pins that
// existing behaviour instead of silently re-numbering it.
namespace ocflags {
inline constexpr std::uint8_t IsOptionalStart{1u << 1};         // `?.` written here
inline constexpr std::uint8_t IsOptionalContinuation{1u << 2};  // inside a `?.` chain
inline constexpr std::uint8_t IsOptionalAny{IsOptionalStart | IsOptionalContinuation};
}  // namespace ocflags

// Node::flags bits for ImportDecl / ExportDecl (the ESM kinds above).
namespace mflags {
// ref s.rs:207 (`Import::is_single_line`), :50 (`ExportClause`), :83
// (`ExportFrom`) — a plain bool field on each in bun, a bit here for the same
// reason eflags::IsSingleLine is (one flags byte, no per-kind fields).
//
// Drives the clause layout exactly as it does for object literals: set gets
// `import { a, b } from "y"`, clear gets one specifier per indented line.
// VERIFIED against real bun 1.4.0 (Bun.Transpiler.transformSync) — and the rule
// is narrower than "the statement is one line": it is ONLY newlines INSIDE the
// braces that count.
//     import {a\n} from 'y'   =>  import {\n  a\n} from "y";   // multi
//     import\n{a} from 'y'    =>  import { a } from "y";       // single
//     import d\nfrom 'y'      =>  import d from "y";           // single
inline constexpr std::uint8_t IsSingleLine{1u << 0};
// `import type {T} from 'y'` / `export type {T} from 'y'` — TypeScript's
// type-only forms, which bun ERASES whole (both transform to ""). The parser has
// already recorded that erasure as an Edit for the erasure path; the bit is what
// tells the printer the same thing, since a type-only import is otherwise shaped
// exactly like a value one. (An INLINE `{type T, a}` specifier needs no bit — it
// is simply not collected into the clause.)
inline constexpr std::uint8_t IsTypeOnly{1u << 1};
// `import X = require("m")` / `import X = A.B` — TypeScript's import-equals, which
// is not an import at all: bun lowers the first to `const X = require("m");` and
// ELIDES the second when unreferenced (both verified, 1.4.0). The erasure path
// does the same by rewriting the `import` keyword to `const` in place
// (js_parser.cppm:1144), which leaves the node describing a statement that is no
// longer an import — `text`/`a`/`b`/`list` are all empty, and without this bit the
// printer would read that as a bare `import "";`.
//
// ⚠️ DEFERRED, and this bit is the marker: the printer prints NOTHING for it,
// which matches today's behaviour (print_stmt's default arm already dropped the
// whole kind) but NOT bun's. Modelling it properly means giving the node a
// VarDecl — an AST change with no ESM content, so it is not this slice's.
inline constexpr std::uint8_t IsTsImportEquals{1u << 2};
}  // namespace mflags

// A source-erasure edit: replace the source byte-range [start, end) with `text`
// (empty `text` == a plain deletion). The transpiler-by-erasure path uses these
// to strip/lower TypeScript constructs from the ORIGINAL source while leaving the
// surrounding JavaScript byte-for-byte intact (types are erased, formatting is
// not normalised — see mbun.js_printer erase_slice). This is what keeps the JSC
// module loader's script-mode eval from choking on TS syntax that the AST-rebuild
// printer subset does not reconstruct.
struct Edit {
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::string text;  // replacement; empty == deletion
};

// Fixed-size arena node. `a`/`b`/`c` are child NodeIndex slots (NONE if unused);
// `aux` is a small integer payload (operator token, var kind, …); `text` points
// into the source buffer. Variable-arity children are the pool span
// [listStart, listStart+listCount).
struct Node {
    NodeKind kind{NodeKind::Missing};
    std::uint8_t flags{0};
    std::uint16_t pad_{0};
    std::uint32_t start{0};
    std::uint32_t end{0};
    NodeIndex a{NONE};
    NodeIndex b{NONE};
    NodeIndex c{NONE};
    std::uint32_t aux{0};
    ListRef listStart{0};
    std::uint32_t listCount{0};
    std::string_view text;
};

// Owns all nodes and the flat child pool. Freed in one shot.
class Arena {
public:
    Arena() {
        // Reserve index 0 as the canonical "missing" node.
        nodes_.push_back(Node{});
    }

    NodeIndex make(NodeKind kind, std::size_t start, std::size_t end) {
        Node n{};
        n.kind = kind;
        n.start = static_cast<std::uint32_t>(start);
        n.end = static_cast<std::uint32_t>(end);
        NodeIndex idx = static_cast<NodeIndex>(nodes_.size());
        nodes_.push_back(n);
        return idx;
    }

    Node& at(NodeIndex i) { return nodes_[i]; }
    const Node& at(NodeIndex i) const { return nodes_[i]; }
    std::size_t size() const { return nodes_.size(); }

    // Append a flat list of children and return its start offset; the count is
    // stored on the owning Node by the caller.
    ListRef commit_list(std::span<const NodeIndex> items) {
        ListRef start = static_cast<ListRef>(children_.size());
        children_.insert(children_.end(), items.begin(), items.end());
        return start;
    }

    std::span<const NodeIndex> list_of(const Node& n) const {
        return std::span<const NodeIndex>{children_.data() + n.listStart, n.listCount};
    }

    // Mutable view of the span list_of() returns — for in-place COMPACTION of an
    // existing list, and nothing else.
    //
    // ref js_parser/visit/visit_stmt.rs:214 `items.swap(end, i); end += 1;`, which
    // is how bun drops dead `export {…}` specifiers: survivors are packed into a
    // prefix and the count is lowered. Rewriting the list through commit_list()
    // instead would APPEND to `children_`, which reallocates and dangles every
    // live `list_of()` span in the caller's stack — so the mutation has to happen
    // where the list already lives. Callers may reorder/overwrite within the span
    // and then lower `Node::listCount`; growing a list is still commit_list()'s
    // job.
    std::span<NodeIndex> list_of_mut(const Node& n) {
        return std::span<NodeIndex>{children_.data() + n.listStart, n.listCount};
    }

    // ── decoded literal values ───────────────────────────────────────────────
    // A string literal's VALUE is not the source bytes between its quotes:
    // `'\x41'` is the ONE-character string `A`, and re-quoting the raw bytes
    // yields the four-character string `\x41` instead — the escape gets doubled,
    // silently changing what the program means. The lexer decodes escapes while
    // scanning (js_lexer.cppm:1280); this pool is where the parser parks the
    // result so consumers never have to re-derive it (they cannot: the raw bytes
    // are ambiguous once re-escaped).
    //
    // UTF-16, not UTF-8, because a JS string is a sequence of 16-bit code units
    // and may hold a LONE SURROGATE (`'\uD800'`) that UTF-8 cannot encode. bun
    // draws the same distinction, keeping E::String as utf8-or-utf16 (e.rs).
    //
    // Referenced by `Node::aux`, 1-biased so a default-constructed node (aux==0)
    // reads as "no decoded value" without needing a second field.
    std::uint32_t add_string(std::u16string s) {
        strings_.push_back(std::move(s));
        return static_cast<std::uint32_t>(strings_.size());  // 1-biased
    }

    const std::u16string* string_value(const Node& n) const {
        if (n.aux == 0 || n.aux > strings_.size()) {
            return nullptr;
        }
        return &strings_[n.aux - 1];
    }

    // ── source-erasure edits ─────────────────────────────────────────────────
    // Record a TypeScript-erasure edit. Deletions pass an empty `text`; lowering
    // (enum/namespace) passes generated JS. The parser records these as it walks;
    // speculative backtracking rolls them back via truncate_edits().
    void add_edit(std::uint32_t start, std::uint32_t end, std::string text = {}) {
        if (start <= end) {
            edits_.push_back(Edit{start, end, std::move(text)});
        }
    }
    std::size_t edit_count() const { return edits_.size(); }
    void truncate_edits(std::size_t n) {
        if (n < edits_.size()) {
            edits_.resize(n);
        }
    }
    const std::vector<Edit>& edits() const { return edits_; }

    // Turn the edit covering exactly [start,end) into a pure deletion, i.e. drop
    // its replacement text. Returns false when no such edit exists.
    //
    // This exists for ONE caller: the unused-import trimmer (js_parser/
    // trim_imports.cppm). In CJS mode the import statement's span is already
    // spoken for — it was replaced by the `require()` lowering during the parse —
    // and erase_slice drops a second edit over the same span (the overlap rule),
    // so the trimmer cannot delete the statement by recording another edit. It
    // has to retract the lowering that is already there.
    //
    // Addressed BY SPAN rather than by a stored edit index on purpose: the parser
    // rolls edits back on speculative backtracking (truncate_edits), so an index
    // captured during the parse can silently come to name a different edit. A span
    // resolved at apply() time — after the whole program is parsed — cannot.
    bool blank_edit(std::uint32_t start, std::uint32_t end) {
        for (Edit& e : edits_) {
            if (e.start == start && e.end == end) {
                e.text.clear();
                return true;
            }
        }
        return false;
    }

    // Return src[start, end) with every recorded edit that falls inside that range
    // applied (deletions removed, replacements substituted). Edits are applied in
    // source order; edits that overlap an already-applied edit are skipped so the
    // output is always well formed even if recording produced a rare overlap.
    std::string erase_slice(std::uint32_t start, std::uint32_t end, std::string_view src) const {
        std::vector<const Edit*> hits;
        for (const Edit& e : edits_) {
            // Deletions/replacements span [start,end); a zero-width insertion
            // (start==end with text) injects code at a point (used by ESM→CJS
            // lowering to append `exports.x = x` after a declaration).
            const bool span = e.start < e.end;
            const bool insertion = e.start == e.end && !e.text.empty();
            if (e.start >= start && e.end <= end && (span || insertion)) {
                hits.push_back(&e);
            }
        }
        // Sort by start, then by end so a zero-width insertion at position p is
        // applied before a deletion that also begins at p (text lands first).
        // Stable so two insertions at the same point keep recording order (e.g.
        // the decorator-lowering postlude before a CJS `exports.x = x` append).
        std::stable_sort(hits.begin(), hits.end(), [](const Edit* a, const Edit* b) {
            return a->start != b->start ? a->start < b->start : a->end < b->end;
        });
        std::string out;
        std::uint32_t cur = start;
        for (const Edit* e : hits) {
            if (e->start < cur) {
                continue;  // overlaps an already-applied edit
            }
            out.append(src.substr(cur, e->start - cur));
            out.append(e->text);
            cur = e->end;
        }
        if (cur < end) {
            out.append(src.substr(cur, end - cur));
        }
        return out;
    }

private:
    std::vector<Node> nodes_;
    std::vector<NodeIndex> children_;
    std::vector<Edit> edits_;
    std::vector<std::u16string> strings_;
};

}  // namespace mbun::ast
