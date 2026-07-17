// CommonMark/GFM render tests — exercises the block + inline parser and HTML
// emission (mbun.md.cm_api::render_html) against known-good HTML output.
// Cases follow the CommonMark spec / GFM extensions (bun md/MD4C blueprint).
import std;
import mbun.md;

namespace {

using namespace mbun::md;

int gChecks{0};
int gFailures{0};

void expect_eq(std::string_view md, std::string_view want, Options opts, std::string_view name) {
    ++gChecks;
    std::string got = render_html(md, opts);
    if (got != want) {
        ++gFailures;
        std::println("FAIL [{}]", name);
        std::println("  input:    {:?}", md);
        std::println("  expected: {:?}", std::string_view{want});
        std::println("  got:      {:?}", std::string_view{got});
    }
}

void cm(std::string_view md, std::string_view want, std::string_view name) {
    expect_eq(md, want, Options::commonmark(), name);
}
void gh(std::string_view md, std::string_view want, std::string_view name) {
    expect_eq(md, want, Options::github(), name);
}

}  // namespace

int main() {
    // --- paragraphs ---------------------------------------------------------
    cm("hello world\n", "<p>hello world</p>\n", "para");
    cm("foo\nbar\n", "<p>foo\nbar</p>\n", "para-softbreak");
    cm("foo\n\nbar\n", "<p>foo</p>\n<p>bar</p>\n", "two-paras");

    // --- ATX headings -------------------------------------------------------
    cm("# Heading\n", "<h1>Heading</h1>\n", "atx-h1");
    cm("### Heading ###\n", "<h3>Heading</h3>\n", "atx-closing");
    cm("###### six\n", "<h6>six</h6>\n", "atx-h6");
    cm("####### seven\n", "<p>####### seven</p>\n", "atx-too-many");

    // --- setext headings ----------------------------------------------------
    cm("Title\n=====\n", "<h1>Title</h1>\n", "setext-h1");
    cm("Title\n-----\n", "<h2>Title</h2>\n", "setext-h2");

    // --- thematic break -----------------------------------------------------
    cm("---\n", "<hr />\n", "hr-dash");
    cm("***\n", "<hr />\n", "hr-star");
    cm("- - -\n", "<hr />\n", "hr-spaced");

    // --- blockquote ---------------------------------------------------------
    cm("> quoted\n", "<blockquote>\n<p>quoted</p>\n</blockquote>\n", "quote");
    cm("> a\n> b\n", "<blockquote>\n<p>a\nb</p>\n</blockquote>\n", "quote-multiline");

    // --- fenced code --------------------------------------------------------
    cm("```\ncode\n```\n", "<pre><code>code\n</code></pre>\n", "fenced");
    cm("```js\nlet x = 1 < 2;\n```\n",
       "<pre><code class=\"language-js\">let x = 1 &lt; 2;\n</code></pre>\n", "fenced-lang");
    cm("~~~\na\n~~~\n", "<pre><code>a\n</code></pre>\n", "fenced-tilde");

    // --- indented code ------------------------------------------------------
    cm("    indented\n", "<pre><code>indented\n</code></pre>\n", "indented-code");

    // --- lists --------------------------------------------------------------
    cm("- a\n- b\n", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n", "bullet-tight");
    cm("1. a\n2. b\n", "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n", "ordered-tight");
    cm("- a\n\n- b\n", "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n</li>\n</ul>\n", "bullet-loose");
    cm("3. c\n4. d\n", "<ol start=\"3\">\n<li>c</li>\n<li>d</li>\n</ol>\n", "ordered-start");

    // --- inline emphasis ----------------------------------------------------
    cm("*em*\n", "<p><em>em</em></p>\n", "em");
    cm("_em_\n", "<p><em>em</em></p>\n", "em-underscore");
    cm("**strong**\n", "<p><strong>strong</strong></p>\n", "strong");
    cm("***both***\n", "<p><em><strong>both</strong></em></p>\n", "em-strong");
    cm("a*b*c\n", "<p>a<em>b</em>c</p>\n", "em-intraword-star");
    cm("a_b_c\n", "<p>a_b_c</p>\n", "no-em-intraword-underscore");

    // --- inline code --------------------------------------------------------
    cm("`code`\n", "<p><code>code</code></p>\n", "code-span");
    cm("`` a`b ``\n", "<p><code>a`b</code></p>\n", "code-span-nested-backtick");
    cm("`<html>`\n", "<p><code>&lt;html&gt;</code></p>\n", "code-span-escape");

    // --- escapes & entities -------------------------------------------------
    cm("\\*not emphasis\\*\n", "<p>*not emphasis*</p>\n", "backslash-escape");
    cm("&amp; &copy;\n", "<p>&amp; \xC2\xA9</p>\n", "entities");
    cm("&#65; &#x41;\n", "<p>A A</p>\n", "numeric-entities");
    cm("5 < 6 & 7 > 4\n", "<p>5 &lt; 6 &amp; 7 &gt; 4</p>\n", "raw-lt-amp");

    // --- links & images -----------------------------------------------------
    cm("[text](http://example.com)\n",
       "<p><a href=\"http://example.com\">text</a></p>\n", "link-inline");
    cm("[text](http://example.com \"title\")\n",
       "<p><a href=\"http://example.com\" title=\"title\">text</a></p>\n", "link-title");
    cm("![alt](/img.png)\n",
       "<p><img src=\"/img.png\" alt=\"alt\" /></p>\n", "image");
    cm("[ref][id]\n\n[id]: http://example.com \"t\"\n",
       "<p><a href=\"http://example.com\" title=\"t\">ref</a></p>\n", "link-reference");
    cm("[shortcut]\n\n[shortcut]: /url\n",
       "<p><a href=\"/url\">shortcut</a></p>\n", "link-shortcut");
    cm("<http://example.com>\n",
       "<p><a href=\"http://example.com\">http://example.com</a></p>\n", "autolink-uri");
    cm("<foo@bar.com>\n",
       "<p><a href=\"mailto:foo@bar.com\">foo@bar.com</a></p>\n", "autolink-email");
    cm("**[link](/u)**\n",
       "<p><strong><a href=\"/u\">link</a></strong></p>\n", "emphasis-around-link");

    // --- raw inline html ----------------------------------------------------
    cm("a <b>bold</b> c\n", "<p>a <b>bold</b> c</p>\n", "raw-inline-html");

    // --- html block ---------------------------------------------------------
    cm("<div>\nraw\n</div>\n", "<div>\nraw\n</div>\n", "html-block");

    // --- hard break ---------------------------------------------------------
    cm("foo  \nbar\n", "<p>foo<br />\nbar</p>\n", "hard-break-spaces");
    cm("foo\\\nbar\n", "<p>foo<br />\nbar</p>\n", "hard-break-backslash");

    // --- GFM strikethrough --------------------------------------------------
    gh("~~struck~~\n", "<p><del>struck</del></p>\n", "strikethrough");

    // --- GFM table ----------------------------------------------------------
    gh("| a | b |\n| - | - |\n| 1 | 2 |\n",
       "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n</tr>\n</tbody>\n</table>\n",
       "gfm-table");
    gh("| L | C | R |\n| :- | :-: | -: |\n| 1 | 2 | 3 |\n",
       "<table>\n<thead>\n<tr>\n<th align=\"left\">L</th>\n<th align=\"center\">C</th>\n<th align=\"right\">R</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"center\">2</td>\n<td align=\"right\">3</td>\n</tr>\n</tbody>\n</table>\n",
       "gfm-table-align");

    // --- GFM task list ------------------------------------------------------
    gh("- [ ] todo\n- [x] done\n",
       "<ul>\n<li><input disabled=\"\" type=\"checkbox\"> todo</li>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> done</li>\n</ul>\n",
       "task-list");

    // --- nested / compound structures --------------------------------------
    cm("> # H\n> text\n",
       "<blockquote>\n<h1>H</h1>\n<p>text</p>\n</blockquote>\n", "quote-with-heading");
    cm("- a\n  - b\n",
       "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>\n", "nested-list");
    cm("- a\n\n  b\n",
       "<ul>\n<li>\n<p>a</p>\n<p>b</p>\n</li>\n</ul>\n", "list-item-two-paras");
    cm("1. one\n\n   more\n2. two\n",
       "<ol>\n<li>\n<p>one</p>\n<p>more</p>\n</li>\n<li>\n<p>two</p>\n</li>\n</ol>\n", "ordered-loose-multiline");
    cm("> - x\n> - y\n",
       "<blockquote>\n<ul>\n<li>x</li>\n<li>y</li>\n</ul>\n</blockquote>\n", "quote-containing-list");
    cm("[a](b) and *c*\n",
       "<p><a href=\"b\">a</a> and <em>c</em></p>\n", "link-and-emphasis");
    cm("`code` then **bold**\n",
       "<p><code>code</code> then <strong>bold</strong></p>\n", "code-then-strong");

    std::println("render: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
