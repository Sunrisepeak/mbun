// test_shell.cpp — T2.10 mbun.shell (bun shell lexer/parser, parse-only).
//
// Vectors are extracted verbatim (semantics preserved; AGENTS.md TDD rules)
// from bun's original shell test suite:
//   source: compat/bun/test/js/bun/shell/lex.test.ts   > describe("lex shell")
//   source: compat/bun/test/js/bun/shell/parse.test.ts > describe("parse shell") / invalid input
//   redirect() helper: compat/bun/test/js/bun/shell/util.ts
//
// The upstream tests assert on `shellInternals.lex`/`.parse`, which serialize
// the token stream / AST to JSON (json_fmt.rs). We compare the same canonical
// JSON. Upstream JSON.parse(...).toEqual(...) is key-order independent; here we
// pin the reference field order on both sides (helpers below mirror json_fmt.rs).
//
// DEFERRED(S1) -> T3.7 (interpreter execution / spawn / real IO); not parsing:
//   - every `TestBuilder.command\`...\`.stdout()/.exitCode()` execution case
//   - JS *string* interpolation refs (\x08__bunstr_N) and quote-escape printing
//   - Unicode/WTF-8 source lexing (upstream ASCII fast path is what we cover)
// Parse/lex-time error cases from those files ARE covered below (decidable here).
//
// JS *object* interpolation (`${uint8array}`) becomes a `\x08__bun_N` ref in the
// source string before lexing (bun's `$` tag). We construct that byte form
// directly for the redirect/pipe-target JSObjRef vectors.

import std;
import mbun.shell;

namespace {

using mbun::shell::lex_errors;
using mbun::shell::lex_json;
using mbun::shell::parse_errors;
using mbun::shell::parse_json;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void check_eq(std::string_view actual, std::string_view expected, std::string_view src) {
    ++gChecks;
    if (actual != expected) {
        ++gFailures;
        if (gFailures <= MAX_FAILURE_PRINTS) {
            std::println("  FAIL [{}]", src);
            std::println("    expected: {}", expected);
            std::println("    actual:   {}", actual);
        }
    }
}

// ── JSON builders (mirror json_fmt.rs field order) ──

std::string join(const std::vector<std::string>& xs) {
    std::string o;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) o += ",";
        o += xs[i];
    }
    return o;
}

std::string rflags(bool si, bool so, bool se, bool ap, bool du) {
    auto b = [](bool x) { return x ? "true" : "false"; };
    return std::format(
        "{{\"stdin\":{},\"stdout\":{},\"stderr\":{},\"append\":{},\"duplicate_out\":{},"
        "\"__unused\":0}}",
        b(si), b(so), b(se), b(ap), b(du));
}
const std::string RD = rflags(false, false, false, false, false);
const std::string RDOUT = rflags(false, true, false, false, false);

// simple-atom inner objects (as they appear inside a compound "atoms" array)
std::string saText(std::string s) { return "{\"Text\":\"" + s + "\"}"; }
std::string saVar(std::string s) { return "{\"Var\":\"" + s + "\"}"; }

// full atoms (wrapped in "simple"/"compound")
std::string simple(std::string inner) { return "{\"simple\":" + inner + "}"; }
std::string T(std::string s) { return simple(saText(s)); }
std::string V(std::string s) { return simple(saVar(s)); }
std::string tilde() { return simple("{\"tilde\":{}}"); }
std::string compound(std::vector<std::string> atoms, bool brace, bool glob) {
    return "{\"compound\":{\"atoms\":[" + join(atoms) +
           "],\"brace_expansion_hint\":" + (brace ? "true" : "false") +
           ",\"glob_hint\":" + (glob ? "true" : "false") + "}}";
}
std::string cmdSubstAtom(std::string script, bool quoted) {
    return simple("{\"cmd_subst\":{\"script\":" + script +
                  ",\"quoted\":" + (quoted ? "true" : "false") + "}}");
}

std::string atomFile(std::string atom) { return "{\"atom\":" + atom + "}"; }
std::string jsbuf(int n) { return std::format("{{\"jsbuf\":{{\"idx\":{}}}}}", n); }

std::string assign(std::string label, std::string valueAtom) {
    return "{\"label\":\"" + label + "\",\"value\":" + valueAtom + "}";
}
std::string cmd(std::vector<std::string> assigns, std::vector<std::string> args, std::string redir,
                std::string file) {
    return "{\"cmd\":{\"assigns\":[" + join(assigns) + "],\"name_and_args\":[" + join(args) +
           "],\"redirect\":" + redir + ",\"redirect_file\":" + file + "}}";
}
std::string exprAssign(std::vector<std::string> assigns) {
    return "{\"assign\":[" + join(assigns) + "]}";
}
std::string binary(std::string op, std::string left, std::string right) {
    return "{\"binary\":{\"op\":\"" + op + "\",\"left\":" + left + ",\"right\":" + right + "}}";
}
std::string pipeline(std::vector<std::string> items) {
    return "{\"pipeline\":{\"items\":[" + join(items) + "]}}";
}
std::string stmt(std::vector<std::string> exprs) { return "{\"exprs\":[" + join(exprs) + "]}"; }
std::string stmtList(std::vector<std::string> stmts) { return "[" + join(stmts) + "]"; }
std::string script(std::vector<std::string> stmts) {
    return "{\"stmts\":" + stmtList(stmts) + "}";
}
std::string ifObj(std::vector<std::string> cond, std::vector<std::string> then_,
                  std::vector<std::string> elseParts) {
    return "{\"cond\":" + stmtList(cond) + ",\"then\":" + stmtList(then_) + ",\"else_parts\":[" +
           join(elseParts) + "]}";
}
std::string exprIf(std::string ifobj) { return "{\"if\":" + ifobj + "}"; }

// ── lex token builders ──
std::string tk(std::string name) { return "{\"" + name + "\":{}}"; }
std::string TX(std::string s) { return "{\"Text\":\"" + s + "\"}"; }
std::string VR(std::string s) { return "{\"Var\":\"" + s + "\"}"; }
std::string SQ(std::string s) { return "{\"SingleQuotedText\":\"" + s + "\"}"; }
std::string DQ(std::string s) { return "{\"DoubleQuotedText\":\"" + s + "\"}"; }
std::string VARGV(int n) { return std::format("{{\"VarArgv\":{}}}", n); }
std::string JSOBJ(int n) { return std::format("{{\"JSObjRef\":{}}}", n); }
std::string REDIR(bool si, bool so, bool se, bool ap, bool du) {
    return "{\"Redirect\":" + rflags(si, so, se, ap, du) + "}";
}
const std::string DL = tk("Delimit");
const std::string EOFT = tk("Eof");
const std::string SEMI = tk("Semicolon");
const std::string BB = tk("BraceBegin");
const std::string BE = tk("BraceEnd");
const std::string CMA = tk("Comma");
const std::string DAMP = tk("DoubleAmpersand");
const std::string DPIPE = tk("DoublePipe");
const std::string PIPE = tk("Pipe");
const std::string AMP = tk("Ampersand");
const std::string CSB = tk("CmdSubstBegin");
const std::string CSE = tk("CmdSubstEnd");
std::string L(std::vector<std::string> toks) { return "[" + join(toks) + "]"; }

void run_lex_tests() {
    // source: lex.test.ts > "basic"
    check_eq(lex_json("next dev"), L({TX("next"), DL, TX("dev"), DL, EOFT}), "lex basic");
    // "var edgecase"
    check_eq(lex_json("$PWD/test.txt"), L({VR("PWD"), TX("/test.txt"), DL, EOFT}),
             "lex var edgecase");
    // "vars"
    check_eq(lex_json("next dev $PORT"), L({TX("next"), DL, TX("dev"), DL, VR("PORT"), EOFT}),
             "lex vars");
    // "quoted_var"
    check_eq(lex_json("next dev \"$PORT\""),
             L({TX("next"), DL, TX("dev"), DL, VR("PORT"), EOFT}), "lex quoted_var");
    // "quoted_edge_case"
    check_eq(lex_json("next dev foo\"$PORT\""),
             L({TX("next"), DL, TX("dev"), DL, TX("foo"), VR("PORT"), EOFT}),
             "lex quoted_edge_case");
    // "quote_multi"
    check_eq(lex_json("echo foo\"$NICE\"good\"NICE\""),
             L({TX("echo"), DL, TX("foo"), VR("NICE"), TX("good"), DQ("NICE"), EOFT}),
             "lex quote_multi");
    // "semicolon"
    check_eq(lex_json("echo foo; bar baz; echo \"NICE;\""),
             L({TX("echo"), DL, TX("foo"), DL, SEMI, TX("bar"), DL, TX("baz"), DL, SEMI,
                TX("echo"), DL, DQ("NICE;"), EOFT}),
             "lex semicolon");
    // "single_quote"
    check_eq(lex_json("next dev 'hello how is it going'"),
             L({TX("next"), DL, TX("dev"), DL, SQ("hello how is it going"), EOFT}),
             "lex single_quote");
    // "env_vars"
    check_eq(lex_json("NAME=zack FULLNAME=\"$NAME radisic\" LOL= ; echo $FULLNAME"),
             L({TX("NAME=zack"), DL, TX("FULLNAME="), VR("NAME"), DQ(" radisic"), DL, TX("LOL="),
                DL, SEMI, TX("echo"), DL, VR("FULLNAME"), EOFT}),
             "lex env_vars");
    // "env_vars2"
    check_eq(lex_json("NAME=zack foo=$bar echo $NAME"),
             L({TX("NAME=zack"), DL, TX("foo="), VR("bar"), DL, TX("echo"), DL, VR("NAME"), EOFT}),
             "lex env_vars2");
    // "env_vars exported"
    check_eq(lex_json("export NAME=zack FOO=bar export NICE=lmao"),
             L({TX("export"), DL, TX("NAME=zack"), DL, TX("FOO=bar"), DL, TX("export"), DL,
                TX("NICE=lmao"), DL, EOFT}),
             "lex env_vars exported");
    // "brace_expansion"
    check_eq(lex_json("echo {ts,tsx,js,jsx}"),
             L({TX("echo"), DL, BB, TX("ts"), CMA, TX("tsx"), CMA, TX("js"), CMA, TX("jsx"), BE,
                EOFT}),
             "lex brace_expansion");
    // operators
    check_eq(lex_json("echo foo && echo bar"),
             L({TX("echo"), DL, TX("foo"), DL, DAMP, TX("echo"), DL, TX("bar"), DL, EOFT}),
             "lex op_and");
    check_eq(lex_json("echo foo || echo bar"),
             L({TX("echo"), DL, TX("foo"), DL, DPIPE, TX("echo"), DL, TX("bar"), DL, EOFT}),
             "lex op_or");
    check_eq(lex_json("echo foo | echo bar"),
             L({TX("echo"), DL, TX("foo"), DL, PIPE, TX("echo"), DL, TX("bar"), DL, EOFT}),
             "lex op_pipe");
    check_eq(lex_json("echo foo & echo bar"),
             L({TX("echo"), DL, TX("foo"), DL, AMP, TX("echo"), DL, TX("bar"), DL, EOFT}),
             "lex op_bg");
    // "op_redirect"
    check_eq(lex_json("echo foo > cat secrets.txt"),
             L({TX("echo"), DL, TX("foo"), DL, REDIR(false, true, false, false, false), TX("cat"),
                DL, TX("secrets.txt"), DL, EOFT}),
             "lex op_redirect >");
    check_eq(lex_json("cmd1 0> file.txt"),
             L({TX("cmd1"), DL, REDIR(true, false, false, false, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect 0>");
    check_eq(lex_json("cmd1 1> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, true, false, false, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect 1>");
    check_eq(lex_json("cmd1 2> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, false, true, false, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect 2>");
    check_eq(lex_json("cmd1 &> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, true, true, false, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect &>");
    check_eq(lex_json("cmd1 1>> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, true, false, true, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect 1>>");
    check_eq(lex_json("cmd1 2>> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, false, true, true, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect 2>>");
    check_eq(lex_json("cmd1 &>> file.txt"),
             L({TX("cmd1"), DL, REDIR(false, true, true, true, false), TX("file.txt"), DL, EOFT}),
             "lex op_redirect &>>");
    // "obj_ref"  (${buffer} -> \x08__bun_N)
    check_eq(lex_json("echo foo > "
                      "\x08"
                      "__bun_0 && echo lmao > "
                      "\x08"
                      "__bun_1",
                      2),
             L({TX("echo"), DL, TX("foo"), DL, REDIR(false, true, false, false, false), JSOBJ(0),
                DAMP, TX("echo"), DL, TX("lmao"), DL, REDIR(false, true, false, false, false),
                JSOBJ(1), EOFT}),
             "lex obj_ref");
    // "cmd_sub_dollar"
    check_eq(lex_json("echo foo $(ls)"),
             L({TX("echo"), DL, TX("foo"), DL, CSB, TX("ls"), DL, CSE, EOFT}),
             "lex cmd_sub_dollar");
    // "cmd_sub_dollar_nested"
    check_eq(lex_json("echo foo $(ls $(ls) $(ls))"),
             L({TX("echo"), DL, TX("foo"), DL, CSB, TX("ls"), DL, CSB, TX("ls"), DL, CSE, DL, CSB,
                TX("ls"), DL, CSE, DL, CSE, EOFT}),
             "lex cmd_sub_dollar_nested");
    // "cmd_sub_edgecase"
    check_eq(lex_json("echo $(FOO=bar $FOO)"),
             L({TX("echo"), DL, CSB, TX("FOO=bar"), DL, VR("FOO"), DL, CSE, EOFT}),
             "lex cmd_sub_edgecase");
    // "cmd_sub_combined_word"
    check_eq(lex_json("echo $(FOO=bar $FOO)NICE"),
             L({TX("echo"), DL, CSB, TX("FOO=bar"), DL, VR("FOO"), DL, CSE, TX("NICE"), DL, EOFT}),
             "lex cmd_sub_combined_word");
    // "cmd_sub_backtick"  (raw template: literal backslash-escaped backticks)
    check_eq(lex_json("echo foo \\`ls\\`"),
             L({TX("echo"), DL, TX("foo"), DL, TX("`ls`"), DL, EOFT}), "lex cmd_sub_backtick");

    // lex-time error cases (from lex.test.ts > describe("errors"))
    check_eq(lex_errors("echo )"), "Unexpected ')'", "lex err lone-close-paren");
    check_eq(lex_errors("echo hi |"), "Unexpected EOF", "lex err unexpected-eof");
    check_eq(lex_errors("echo hi && (echo uh oh"), "Unclosed subshell", "lex err unclosed-subshell");
    check_eq(lex_errors("echo hi && $(echo uh oh"), "Unclosed command substitution",
             "lex err unclosed-cmdsubst");
    check_eq(lex_errors("(((( |||"),
             "Unexpected EOF\nUnclosed subshell\nUnclosed subshell\nUnclosed subshell",
             "lex err multiple-newline-separated");
}

void run_parse_tests() {
    // source: parse.test.ts > "basic"
    check_eq(parse_json("echo foo"),
             script({stmt({cmd({}, {T("echo"), T("foo")}, RD, "null")})}), "parse basic");
    // "basic redirect"
    check_eq(parse_json("echo foo > lmao.txt"),
             script({stmt({cmd({}, {T("echo"), T("foo")}, RDOUT, atomFile(T("lmao.txt")))})}),
             "parse basic redirect");
    // "single atom"
    check_eq(parse_json("ls"), script({stmt({cmd({}, {T("ls")}, RD, "null")})}),
             "parse single atom ls");
    check_eq(parse_json("echo ~"),
             script({stmt({cmd({}, {T("echo"), tilde()}, RD, "null")})}), "parse single atom ~");
    // "compound atom"
    check_eq(parse_json("\"FOO $NICE!\""),
             script({stmt({cmd(
                 {}, {compound({saText("FOO "), saVar("NICE"), saText("!")}, false, false)}, RD,
                 "null")})}),
             "parse compound atom");
    // "pipelines"
    check_eq(
        parse_json("echo > foo.txt | echo hi"),
        script({stmt({pipeline({cmd({}, {T("echo")}, RDOUT, atomFile(T("foo.txt"))),
                                cmd({}, {T("echo"), T("hi")}, RD, "null")})})}),
        "parse pipelines");
    // "binary expressions"
    check_eq(parse_json("echo foo && echo bar || echo lmao"),
             script({stmt({binary(
                 "Or",
                 binary("And", cmd({}, {T("echo"), T("foo")}, RD, "null"),
                        cmd({}, {T("echo"), T("bar")}, RD, "null")),
                 cmd({}, {T("echo"), T("lmao")}, RD, "null"))})}),
             "parse binary expressions");
    // "precedence"
    check_eq(
        parse_json("FOO=bar && echo foo && echo bar | echo lmao | cat > foo.txt"),
        script({stmt({binary(
            "And",
            binary("And", exprAssign({assign("FOO", T("bar"))}),
                   cmd({}, {T("echo"), T("foo")}, RD, "null")),
            pipeline({cmd({}, {T("echo"), T("bar")}, RD, "null"),
                      cmd({}, {T("echo"), T("lmao")}, RD, "null"),
                      cmd({}, {T("cat")}, RDOUT, atomFile(T("foo.txt")))}))})}),
        "parse precedence");
    // "assigns"
    check_eq(parse_json("FOO=bar BAR=baz export LMAO=nice"),
             script({stmt({cmd({assign("FOO", T("bar")), assign("BAR", T("baz"))},
                               {T("export"), T("LMAO=nice")}, RD, "null")})}),
             "parse assigns");
    // "redirect js obj"
    check_eq(parse_json("echo foo > "
                        "\x08"
                        "__bun_0 && echo foo > "
                        "\x08"
                        "__bun_1",
                        2),
             script({stmt({binary(
                 "And", cmd({}, {T("echo"), T("foo")}, RDOUT, jsbuf(0)),
                 cmd({}, {T("echo"), T("foo")}, RDOUT, jsbuf(1)))})}),
             "parse redirect js obj");
    // "cmd subst"
    check_eq(parse_json("echo \"$(echo 1; echo 2)\""),
             script({stmt({cmd(
                 {},
                 {T("echo"),
                  cmdSubstAtom(script({stmt({cmd({}, {T("echo"), T("1")}, RD, "null")}),
                                       stmt({cmd({}, {T("echo"), T("2")}, RD, "null")})}),
                               true)},
                 RD, "null")})}),
             "parse cmd subst");
    // "cmd subst edgecase"
    check_eq(
        parse_json("echo $(ls foo) && echo nice"),
        script({stmt({binary(
            "And",
            cmd({},
                {T("echo"), cmdSubstAtom(script({stmt({cmd({}, {T("ls"), T("foo")}, RD,
                                                           "null")})}),
                                         false)},
                RD, "null"),
            cmd({}, {T("echo"), T("nice")}, RD, "null"))})}),
        "parse cmd subst edgecase");
    // if_clause "basic"
    {
        std::string expected = script({stmt({exprIf(ifObj(
            {stmt({cmd({}, {T("echo"), T("hi")}, RD, "null")})},
            {stmt({cmd({}, {T("echo"), T("lmao")}, RD, "null")})},
            {stmtList({stmt({cmd({}, {T("echo"), T("lol")}, RD, "null")})})}))})});
        check_eq(parse_json("if echo hi; then echo lmao; else echo lol; fi"), expected,
                 "parse if basic (semicolons)");
        check_eq(parse_json("if echo hi\n      then echo lmao\n      else echo lol\n      fi"),
                 expected, "parse if basic (newlines)");
    }
    // if_clause "elif"
    check_eq(parse_json("if a; then b; elif c; then d; else e; fi"),
             script({stmt({exprIf(ifObj(
                 {stmt({cmd({}, {T("a")}, RD, "null")})},
                 {stmt({cmd({}, {T("b")}, RD, "null")})},
                 {stmtList({stmt({cmd({}, {T("c")}, RD, "null")})}),
                  stmtList({stmt({cmd({}, {T("d")}, RD, "null")})}),
                  stmtList({stmt({cmd({}, {T("e")}, RD, "null")})})}))})}),
             "parse if elif");
    // if_clause "precedence > in pipeline"
    check_eq(parse_json("if echo hi; then echo lmao; else echo lol; fi | cat"),
             script({stmt({pipeline({
                 exprIf(ifObj({stmt({cmd({}, {T("echo"), T("hi")}, RD, "null")})},
                              {stmt({cmd({}, {T("echo"), T("lmao")}, RD, "null")})},
                              {stmtList({stmt({cmd({}, {T("echo"), T("lol")}, RD, "null")})})})),
                 cmd({}, {T("cat")}, RD, "null"),
             })})}),
             "parse if in pipeline");
    // bad syntax "cmd subst edgecase"
    check_eq(parse_json("echo $(FOO=bar $FOO)"),
             script({stmt({cmd(
                 {},
                 {T("echo"),
                  cmdSubstAtom(
                      script({stmt({cmd({assign("FOO", T("bar"))}, {V("FOO")}, RD, "null")})}),
                      false)},
                 RD, "null")})}),
             "parse bad-syntax cmd subst edgecase");
    // bad syntax "cmd edgecase"
    check_eq(parse_json("FOO=bar BAR=baz; BUN_DEBUG_QUIET_LOGS=1 echo"),
             script({stmt({exprAssign({assign("FOO", T("bar")), assign("BAR", T("baz"))})}),
                     stmt({cmd({assign("BUN_DEBUG_QUIET_LOGS", T("1"))}, {T("echo")}, RD,
                               "null")})}),
             "parse cmd edgecase");

    // parse-time error cases (parse.test.ts invalid input)
    check_eq(parse_errors("\x08"
                          "__bun_0 | cat",
                          1),
             "expected a command or assignment but got: \"JSObjRef\"", "parse err invalid js obj");
    check_eq(parse_errors("echo (echo foo && echo hi)"), "Unexpected token: `(`",
             "parse err subshell-in-invalid-position");
    check_eq(parse_errors("echo foo >"), "Redirection with no file", "parse err redirect-no-file");
    check_eq(parse_errors("echo hi &"), "Background commands \"&\" are not supported yet.",
             "parse err background");
}

}  // namespace

int main() {
    run_lex_tests();
    run_parse_tests();
    std::println("mbun.shell: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
