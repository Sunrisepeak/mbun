// test_md.cpp — mbun.md skeleton smoke checks.
//
// The block/inline parser and renderer emission are DEFERRED(S1); these checks
// exercise the pure pieces that are present today: the OutputBuffer OOM-sticky
// sink, the Options presets (commonmark/github/terminal), and construction of
// the HTML/ANSI renderer adapters plus the Parser state object.
// ref: bun src/md/{output,root,html_renderer,ansi_renderer,parser}.rs
import std;
import mbun.md;

namespace {

using namespace mbun::md;

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("FAIL: {}", what);
    }
}

}  // namespace

int main() {
    // OutputBuffer accumulates bytes and take() empties it.
    {
        OutputBuffer buf;
        buf.write("hello");
        buf.write_byte(' ');
        buf.write("world");
        check(!buf.oom(), "output buffer not OOM after normal writes");
        check(buf.take() == "hello world", "output buffer accumulates then takes");
        check(buf.take().empty(), "output buffer is empty after take");
    }

    // Options presets differ as bun configures them.
    {
        Options gh = Options::github();
        check(gh.permissiveAutolinks && gh.permissiveWwwAutolinks && gh.tagFilter,
              "github preset enables permissive autolinks + tag filter");
        Options term = Options::terminal();
        check(term.wikiLinks && term.underline && term.latexMath,
              "terminal preset enables wiki links, underline, latex math");
        Options cm = Options::commonmark();
        check(!cm.tagFilter && !cm.headingIds && !cm.autolinkHeadings,
              "commonmark preset disables render extensions");
    }

    // Renderer adapters construct and expose an (initially empty) output sink.
    {
        HtmlRenderer html;
        check(html.take_output().empty(), "html renderer starts with empty output");
        AnsiRenderer ansi;
        check(ansi.take_output().empty(), "ansi renderer starts with empty output");
    }

    // Parser holds source + options + renderer; process_document is a S1 seam.
    {
        RendererAdapter sink;
        Parser parser{"# heading\n", Options::commonmark(), sink};
        auto r = parser.process_document();
        check(r.has_value(), "parser process_document seam returns ok");
    }

    std::println("md: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
