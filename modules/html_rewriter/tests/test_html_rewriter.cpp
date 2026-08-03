// Engine-level tests for mbun.html_rewriter, off the JS engine. The expected
// strings mirror bun's HTMLRewriter suite
// (compat/bun/test/js/workerd/html-rewriter.test.js) where they translate 1:1.
import std;
import mbun.html_rewriter;

namespace {

using namespace mbun::html_rewriter;

int checks { 0 };
int failures { 0 };

void check(bool value, std::string_view label) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL: {}", label);
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view label) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   {}", label, expected, actual);
    }
}

Selector sel(std::string_view text) {
    auto parsed { Selector::parse(text) };
    if (!parsed) {
        std::println("FAIL: selector '{}' did not parse: {}", text, parsed.error());
        std::exit(1);
    }
    return std::move(*parsed);
}

std::string rewrite(std::string_view selector, std::function<Directive(Element&)> fn,
                    std::string_view input) {
    Rewriter rewriter;
    rewriter.on(sel(selector), ElementContentHandlers { .element = std::move(fn) });
    return rewriter.transform(input).output;
}

std::string set_inner(std::string_view selector, std::string_view input) {
    return rewrite(selector, [](Element& e) {
        e.set_inner_content("new", false);
        return Directive::CONTINUE;
    }, input);
}

void test_passthrough() {
    const std::array<std::string_view, 8> docs {
        "<a>y</a>",
        "<div first second=\"alrihgt\" third=\"123\" fourth=5 fifth=helloooo>hello</div>",
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\" /></head><body><br>x</body></html>",
        "<p>Lorem ipsum!<br></p><div />",
        "<script>if (a < b) { x(\"</div>\"); }</script><style>a>b{}</style>",
        "<!-- c --><?php echo ?><![CDATA[j]]><p></p></nope>",
        "text <5 & unclosed <",
        "<svg><circle cx=\"5\" /></svg><textarea><div></textarea>",
    };
    for (auto doc : docs) {
        Rewriter noHandlers;
        check_eq(noHandlers.transform(doc).output, doc, "passthrough (no handlers)");
        Rewriter reading;
        reading.on(sel("*"), ElementContentHandlers {
            .element = [](Element& e) { (void)e.tag_name(); return Directive::CONTINUE; },
            .comments = [](Comment&) { return Directive::CONTINUE; },
            .text = [](TextChunk&) { return Directive::CONTINUE; },
        });
        reading.on_document(DocumentContentHandlers {
            .doctype = [](Doctype&) { return Directive::CONTINUE; },
            .comments = [](Comment&) { return Directive::CONTINUE; },
            .text = [](TextChunk&) { return Directive::CONTINUE; },
        });
        check_eq(reading.transform(doc).output, doc, "passthrough (read-only handlers)");
    }
}

void test_selectors() {
    check_eq(set_inner("*", "<h1>1</h1><p>2</p>"), "<h1>new</h1><p>new</p>", "selector *");
    check_eq(set_inner("p", "<h1>1</h1><p>2</p>"), "<h1>1</h1><p>new</p>", "selector p");
    check_eq(set_inner("p:nth-child(2)", "<div><p>1</p><p>2</p><p>3</p></div>"),
             "<div><p>1</p><p>new</p><p>3</p></div>", "selector :nth-child");
    check_eq(set_inner("p:first-child", "<div><p>1</p><p>2</p><p>3</p></div>"),
             "<div><p>new</p><p>2</p><p>3</p></div>", "selector :first-child");
    check_eq(set_inner("p:nth-of-type(2)",
                       "<div><p>1</p><h1>2</h1><p>3</p><h1>4</h1><p>5</p></div>"),
             "<div><p>1</p><h1>2</h1><p>new</p><h1>4</h1><p>5</p></div>", "selector :nth-of-type");
    check_eq(set_inner("p:first-of-type", "<div><h1>1</h1><p>2</p><p>3</p></div>"),
             "<div><h1>1</h1><p>new</p><p>3</p></div>", "selector :first-of-type");
    check_eq(set_inner("p:not(:first-child)", "<div><p>1</p><p>2</p><p>3</p></div>"),
             "<div><p>1</p><p>new</p><p>new</p></div>", "selector :not(:first-child)");
    check_eq(set_inner("p.red", "<p class=\"red\">1</p><p>2</p>"),
             "<p class=\"red\">new</p><p>2</p>", "selector .class");
    check_eq(set_inner("h1#header", "<h1 id=\"header\">1</h1><h1>2</h1>"),
             "<h1 id=\"header\">new</h1><h1>2</h1>", "selector #id");
    check_eq(set_inner("p[data-test]", "<p data-test>1</p><p>2</p>"),
             "<p data-test>new</p><p>2</p>", "selector [attr]");
    check_eq(set_inner("p[data-test=\"one\"]",
                       "<p data-test=\"one\">1</p><p data-test=\"two\">2</p>"),
             "<p data-test=\"one\">new</p><p data-test=\"two\">2</p>", "selector [attr=value]");
    check_eq(set_inner("p[data-test=\"one\" i]",
                       "<p data-test=\"one\">1</p><p data-test=\"OnE\">2</p><p data-test=\"two\">3</p>"),
             "<p data-test=\"one\">new</p><p data-test=\"OnE\">new</p><p data-test=\"two\">3</p>",
             "selector [attr=value i]");
    check_eq(set_inner("p[data-test=\"one\" s]",
                       "<p data-test=\"one\">1</p><p data-test=\"OnE\">2</p><p data-test=\"two\">3</p>"),
             "<p data-test=\"one\">new</p><p data-test=\"OnE\">2</p><p data-test=\"two\">3</p>",
             "selector [attr=value s]");
    check_eq(set_inner("p[data-test~=\"two\"]",
                       "<p data-test=\"one two three\">1</p><p data-test=\"one two\">2</p><p data-test=\"one\">3</p>"),
             "<p data-test=\"one two three\">new</p><p data-test=\"one two\">new</p><p data-test=\"one\">3</p>",
             "selector [attr~=value]");
    check_eq(set_inner("p[data-test^=\"a\"]",
                       "<p data-test=\"a1\">1</p><p data-test=\"a2\">2</p><p data-test=\"b1\">3</p>"),
             "<p data-test=\"a1\">new</p><p data-test=\"a2\">new</p><p data-test=\"b1\">3</p>",
             "selector [attr^=value]");
    check_eq(set_inner("p[data-test$=\"1\"]",
                       "<p data-test=\"a1\">1</p><p data-test=\"a2\">2</p><p data-test=\"b1\">3</p>"),
             "<p data-test=\"a1\">new</p><p data-test=\"a2\">2</p><p data-test=\"b1\">new</p>",
             "selector [attr$=value]");
    check_eq(set_inner("p[data-test*=\"b\"]",
                       "<p data-test=\"abc\">1</p><p data-test=\"ab\">2</p><p data-test=\"a\">3</p>"),
             "<p data-test=\"abc\">new</p><p data-test=\"ab\">new</p><p data-test=\"a\">3</p>",
             "selector [attr*=value]");
    check_eq(set_inner("p[data-test|=\"a\"]",
                       "<p data-test=\"a\">1</p><p data-test=\"a-1\">2</p><p data-test=\"a2\">3</p>"),
             "<p data-test=\"a\">new</p><p data-test=\"a-1\">new</p><p data-test=\"a2\">3</p>",
             "selector [attr|=value]");
    check_eq(set_inner("div span", "<div><h1><span>1</span></h1><span>2</span><b>3</b></div>"),
             "<div><h1><span>new</span></h1><span>new</span><b>3</b></div>", "descendant combinator");
    check_eq(set_inner("div > span", "<div><h1><span>1</span></h1><span>2</span><b>3</b></div>"),
             "<div><h1><span>1</span></h1><span>new</span><b>3</b></div>", "child combinator");
    check_eq(set_inner("h1, p", "<h1>1</h1><p>2</p><b>3</b>"), "<h1>new</h1><p>new</p><b>3</b>",
             "selector list");

    check(!Selector::parse("").has_value(), "empty selector rejected");
    check(!Selector::parse("p:hover").has_value(), "unsupported pseudo rejected");
    check(!Selector::parse("p[").has_value(), "unclosed attribute rejected");
}

void test_element_mutations() {
    // prepend/append escape vs html — order mirrors lol-html.
    {
        auto out { rewrite("p", [](Element& e) {
            e.prepend("<span>prepend</span>", false);
            e.prepend("<span>prepend html</span>", true);
            e.append("<span>append</span>", false);
            e.append("<span>append html</span>", true);
            return Directive::CONTINUE;
        }, "<p>test</p>") };
        check_eq(out,
                 "<p><span>prepend html</span>&lt;span&gt;prepend&lt;/span&gt;test"
                 "&lt;span&gt;append&lt;/span&gt;<span>append html</span></p>",
                 "prepend/append ordering + escaping");
    }
    {
        auto out { rewrite("p", [](Element& e) {
            e.set_inner_content("<span>replace</span>", false);
            return Directive::CONTINUE;
        }, "<p>test</p>") };
        check_eq(out, "<p>&lt;span&gt;replace&lt;/span&gt;</p>", "setInnerContent escaped");
    }
    {
        auto out { rewrite("p", [](Element& e) {
            e.set_inner_content("<span>replace</span>", true);
            return Directive::CONTINUE;
        }, "<p>test</p>") };
        check_eq(out, "<p><span>replace</span></p>", "setInnerContent html");
    }
    {
        auto out { rewrite("div", [](Element& e) {
            e.set_inner_content("", true);
            return Directive::CONTINUE;
        }, "<div><span>content</span></div>") };
        check_eq(out, "<div></div>", "setInnerContent empty deletes children");
    }
    {
        auto out { rewrite("p", [](Element& e) {
            e.remove_and_keep_content();
            return Directive::CONTINUE;
        }, "<p>test</p>") };
        check_eq(out, "test", "removeAndKeepContent");
    }
    {
        auto out { rewrite("div", [](Element& e) {
            e.remove();
            return Directive::CONTINUE;
        }, "a<div><b>x</b></div>z") };
        check_eq(out, "az", "remove drops subtree");
    }
    {
        auto out { rewrite("div", [](Element& e) {
            e.replace("<section>done</section>", true);
            return Directive::CONTINUE;
        }, "a<div><b>x</b></div>z") };
        check_eq(out, "a<section>done</section>z", "replace");
    }
    {
        auto out { rewrite("p", [](Element& e) {
            auto rc { e.set_tag_name("section") };
            check(rc.has_value(), "set_tag_name ok");
            return Directive::CONTINUE;
        }, "<p>hi</p>") };
        check_eq(out, "<section>hi</section>", "tagName renames start and end tag");
    }
}

void test_attributes() {
    // getAttribute distinguishes empty from absent; setAttribute round-trips.
    {
        std::optional<std::string> explicitEmpty, boolean, valued, absent;
        bool hasBoolean {}, hasAbsent {};
        rewrite("div", [&](Element& e) {
            auto opt { [&](std::optional<std::string_view> v) -> std::optional<std::string> {
                if (!v) return std::nullopt;
                return std::string { *v };
            } };
            explicitEmpty = opt(e.get_attribute("a"));
            boolean = opt(e.get_attribute("b"));
            valued = opt(e.get_attribute("c"));
            absent = opt(e.get_attribute("zzz"));
            hasBoolean = e.has_attribute("b");
            hasAbsent = e.has_attribute("zzz");
            return Directive::CONTINUE;
        }, "<div a=\"\" b c=\"v\">t</div>");
        check(explicitEmpty.has_value() && explicitEmpty->empty(), "explicit empty attr is ''");
        check(boolean.has_value() && boolean->empty(), "boolean attr is ''");
        check(valued == "v", "valued attr");
        check(!absent.has_value(), "absent attr is nullopt");
        check(hasBoolean && !hasAbsent, "has_attribute");
    }
    {
        auto out { rewrite("div", [](Element& e) {
            check(!e.set_attribute("a b", "1").has_value(), "forbidden attr name rejected");
            check(e.set_attribute("", "1").error() == "Attribute name can't be empty.",
                  "empty attr name message");
            return Directive::CONTINUE;
        }, "<div x=\"1\">t</div>") };
        check_eq(out, "<div x=\"1\">t</div>", "failed setAttribute leaves element untouched");
    }
    {
        auto out { rewrite("div", [](Element& e) {
            check(e.set_attribute("x", "9").has_value(), "setAttribute ok");
            e.remove_attribute("a");
            return Directive::CONTINUE;
        }, "<div a=\"1\" b=\"2\">t</div>") };
        check_eq(out, "<div b=\"2\" x=\"9\">t</div>", "setAttribute/removeAttribute rewrite tag");
    }
    {
        std::uint64_t genBefore {}, genAfter {};
        rewrite("div", [&](Element& e) {
            genBefore = e.attribute_generation();
            (void)e.set_attribute("x", "9");
            genAfter = e.attribute_generation();
            return Directive::CONTINUE;
        }, "<div>t</div>");
        check(genAfter == genBefore + 1, "attribute generation bumps on mutation");
    }
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        rewrite("p", [&](Element& e) {
            for (const auto& a : e.attributes()) pairs.emplace_back(a.name, a.value);
            return Directive::CONTINUE;
        }, "<p šž=\"Õäöü\" ab=\"Õäöü\" šž=\"Õäöü\" šž=\"dc\">x</p>");
        check(pairs.size() == 4 && pairs[0].first == "šž" && pairs[3].second == "dc",
              "duplicate/unicode attributes preserved in order");
    }
}

void test_comments_and_text() {
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .comments = [](Comment& c) {
            c.before("<span>before</span>", false);
            c.before("<span>before html</span>", true);
            c.after("<span>after</span>", false);
            c.after("<span>after html</span>", true);
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p><!--test--></p>").output,
                 "<p>&lt;span&gt;before&lt;/span&gt;<span>before html</span><!--test-->"
                 "<span>after html</span>&lt;span&gt;after&lt;/span&gt;</p>",
                 "comment before/after ordering");
    }
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .comments = [](Comment& c) {
            check(!c.removed(), "comment not removed initially");
            check(c.text() == "test", "comment text");
            check(c.set_text("new").has_value(), "comment set_text");
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p><!--test--></p>").output, "<p><!--new--></p>",
                 "comment text setter");
    }
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .comments = [](Comment& c) {
            c.remove();
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p><!--test--></p>").output, "<p></p>", "comment remove");
    }
    {
        Rewriter rewriter;
        int calls { 0 };
        rewriter.on(sel("h1"), ElementContentHandlers { .text = [&](TextChunk& t) {
            ++calls;
            check(t.last_in_text_node(), "lastInTextNode");
            return Directive::CONTINUE;
        } });
        // Subtree semantics: the b-nested text also belongs to the h1 handler.
        check_eq(rewriter.transform("<div>skip</div><h1>a<b>c</b></h1>").output,
                 "<div>skip</div><h1>a<b>c</b></h1>", "text handler passthrough");
        check(calls == 2, "text handler fires for subtree text only");
    }
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .text = [](TextChunk& t) {
            if (!t.text().empty()) t.replace("X", false);
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p>one</p><b>two</b>").output, "<p>X</p><b>two</b>",
                 "text replace");
    }
}

void test_document_handlers() {
    {
        std::optional<std::string> name, publicId, systemId;
        Rewriter rewriter;
        rewriter.on_document(DocumentContentHandlers { .doctype = [&](Doctype& d) {
            auto opt { [](std::optional<std::string_view> v) -> std::optional<std::string> {
                if (!v) return std::nullopt;
                return std::string { *v };
            } };
            name = opt(d.name());
            publicId = opt(d.public_id());
            systemId = opt(d.system_id());
            return Directive::CONTINUE;
        } });
        rewriter.transform("<!DOCTYPE html><div></div>");
        check(name == "html" && !publicId && !systemId, "doctype absent ids are nullopt");

        name.reset();
        rewriter.transform("<!DOCTYPE html PUBLIC \"\" \"\"><div></div>");
        check(name == "html" && publicId == "" && systemId == "", "doctype empty ids are ''");

        rewriter.transform(
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">");
        check(publicId == "-//W3C//DTD HTML 4.01//EN" &&
                  systemId == "http://www.w3.org/TR/html4/strict.dtd",
              "doctype with ids");
    }
    {
        Rewriter rewriter;
        int endCalls { 0 };
        rewriter.on_document(DocumentContentHandlers { .end = [&](DocumentEnd& end) {
            ++endCalls;
            end.append("<!--bye-->", true);
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p>x</p>").output, "<p>x</p><!--bye-->", "document end append");
        check(endCalls == 1, "document end fires once");
    }
}

void test_end_tags() {
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .element = [](Element& e) {
            e.on_end_tag([](EndTag& end) {
                check(end.set_name("div").has_value(), "endTag set_name");
                return Directive::CONTINUE;
            });
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p>hi</p>").output, "<p>hi</div>",
                 "endTag rename affects only the closing tag");
    }
    {
        Rewriter rewriter;
        rewriter.on(sel("p"), ElementContentHandlers { .element = [](Element& e) {
            e.on_end_tag([](EndTag& end) {
                end.before("B", false);
                end.after("A", false);
                end.remove();
                return Directive::CONTINUE;
            });
            return Directive::CONTINUE;
        } });
        check_eq(rewriter.transform("<p>hi</p>").output, "<p>hiBA", "endTag before/after/remove");
    }
}

void test_structure() {
    // selfClosing reflects the raw "/>" spelling; canHaveContent is semantic.
    std::map<std::string, std::pair<bool, bool>> seen;
    Rewriter rewriter;
    rewriter.on(sel("*"), ElementContentHandlers { .element = [&](Element& e) {
        seen[std::string { e.tag_name() }] = { e.self_closing(), e.can_have_content() };
        return Directive::CONTINUE;
    } });
    rewriter.transform("<p>Lorem ipsum!<br></p><div /><svg><circle /></svg>");
    check(seen.at("p") == std::pair { false, true }, "p: not self-closing, content ok");
    check(seen.at("br") == std::pair { false, false }, "br: void");
    check(seen.at("div") == std::pair { true, true }, "div/: slash kept, still has content");
    check(seen.at("svg") == std::pair { false, true }, "svg container");
    check(seen.at("circle") == std::pair { true, false }, "foreign self-closing circle");

    // Raw-text content never surfaces elements.
    Rewriter rawText;
    bool sawDiv { false };
    rawText.on(sel("div"), ElementContentHandlers { .element = [&](Element&) {
        sawDiv = true;
        return Directive::CONTINUE;
    } });
    rawText.transform("<script>var a = \"<div>\";</script>");
    check(!sawDiv, "no element handlers inside script raw text");

    // STOP truncates.
    Rewriter stopper;
    stopper.on(sel("b"), ElementContentHandlers { .element = [](Element&) {
        return Directive::STOP;
    } });
    auto result { stopper.transform("<a>1</a><b>2</b><c>3</c>") };
    check(result.stopped, "STOP reported");
}

}  // namespace

int main() {
    test_passthrough();
    test_selectors();
    test_element_mutations();
    test_attributes();
    test_comments_and_text();
    test_document_handlers();
    test_end_tags();
    test_structure();
    std::println("test_html_rewriter: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
