// html_rewriter.cppm — mbun.html_rewriter: dependency-free HTMLRewriter engine.
//
// Blueprint: Cloudflare lol-html's rewriter surface (bun drives lol-html for
// its HTMLRewriter). This is a from-scratch C++26 implementation of the same
// observable behaviour, not a port: the tokenizer walks the input once, keeps
// an open-element stack for selector matching, and streams bytes to the output
// as it goes. Anything the handlers do not touch is emitted as the ORIGINAL
// input bytes, so a transform with no mutations is byte-identical.
//
// Covered: element/comment/text/doctype/document-end handlers; the mutation
// set (before/after/prepend/append/replace/setInnerContent/remove/
// removeAndKeepContent, attribute + tag-name rewrites, end-tag handlers);
// void elements; raw-text elements (script/style/title/textarea/…); foreign
// (svg/math) self-closing; the selector subset in mbun.html_rewriter.selector.
//
// DEFERRED (honest scope): chunked/streaming input (transform is single-shot;
// callers buffer), entity decoding inside attribute values and text, and the
// HTML5 tree-construction fix-ups (implied end tags, adoption agency). Text
// nodes are delivered as one chunk with lastInTextNode=true rather than
// lol-html's chunk-plus-empty-tail sequence.
export module mbun.html_rewriter;

export import mbun.html_rewriter.selector;

import std;

export namespace mbun::html_rewriter {

enum class Directive { CONTINUE, STOP };

class Element;
class TextChunk;
class Comment;
class Doctype;
class DocumentEnd;
class EndTag;

}  // export namespace mbun::html_rewriter

namespace mbun::html_rewriter::detail {

inline constexpr std::string_view HTML_NS_URI { "http://www.w3.org/1999/xhtml" };
inline constexpr std::string_view SVG_NS_URI { "http://www.w3.org/2000/svg" };
inline constexpr std::string_view MATHML_NS_URI { "http://www.w3.org/1998/Math/MathML" };

enum class Ns { HTML, SVG, MATHML };

inline bool ascii_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline bool ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline std::string lower_copy(std::string_view s) {
    std::string r { s };
    for (char& c : r) c = ascii_lower(c);
    return r;
}

inline bool is_void_element(std::string_view tag) {
    static constexpr std::array<std::string_view, 14> VOID_TAGS {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    return std::ranges::find(VOID_TAGS, tag) != VOID_TAGS.end();
}

// Raw-text (script/style/…) and escapable raw-text (title/textarea) elements:
// their content is tokenized as text only, terminated by the matching end tag.
inline bool is_raw_text_element(std::string_view tag) {
    static constexpr std::array<std::string_view, 8> RAW_TAGS {
        "script", "style", "title", "textarea", "xmp", "iframe", "noembed", "noframes",
    };
    return std::ranges::find(RAW_TAGS, tag) != RAW_TAGS.end();
}

// Content inserted with {html:false} is escaped so it reads back as text.
inline std::string escape_text(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '&') r += "&amp;";
        else if (c == '<') r += "&lt;";
        else if (c == '>') r += "&gt;";
        else r += c;
    }
    return r;
}

inline std::string escape_attribute(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '&') r += "&amp;";
        else if (c == '"') r += "&quot;";
        else r += c;
    }
    return r;
}

inline std::string render_content(std::string_view content, bool isHtml) {
    return isHtml ? std::string { content } : escape_text(content);
}

inline std::expected<void, std::string> validate_attribute_name(std::string_view name) {
    if (name.empty()) return std::unexpected("Attribute name can't be empty.");
    for (char c : name) {
        if (ascii_ws(c) || c == '"' || c == '\'' || c == '>' || c == '/' || c == '=' ||
            static_cast<unsigned char>(c) < 0x20) {
            return std::unexpected(
                std::format("'{}' character is forbidden in the attribute name", c));
        }
    }
    return {};
}

inline std::expected<void, std::string> validate_tag_name(std::string_view name) {
    if (name.empty()) return std::unexpected("Tag name can't be empty.");
    if (!ascii_alpha(name.front())) {
        return std::unexpected("First character of the tag name should be an ASCII alphabetical character.");
    }
    for (char c : name) {
        if (ascii_ws(c) || c == '/' || c == '>' || static_cast<unsigned char>(c) < 0x20) {
            return std::unexpected(std::format("'{}' character is forbidden in the tag name", c));
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Per-token mutable state the public handles operate on. Instances live on the
// transformer's stack (elements) or its C++ call frame (text/comment/…), so a
// handle is valid only while its handler runs — the JS binding enforces that.
// ---------------------------------------------------------------------------

struct ElementRecord {
    std::string tagName;          // current (rename target), ASCII-lowercase
    std::string originalTagName;  // as parsed; selector matching + end-tag pairing
    std::string rawStart;         // original start-tag bytes
    std::vector<Attribute> attributes;
    std::uint64_t attrGeneration { 0 };
    bool startDirty { false };
    bool selfClosingSlash { false };
    bool isVoid { false };
    Ns ns { Ns::HTML };
    int nthChild { 0 };
    int nthOfType { 0 };
    int childElements { 0 };
    std::map<std::string, int, std::less<>> childTypeCounts;
    std::vector<std::string> beforeParts, prependParts, appendParts, afterParts;
    std::optional<std::string> innerHtml;
    std::optional<std::string> replacementHtml;
    bool removed { false };
    bool keepContent { false };
    std::vector<std::function<Directive(EndTag&)>> endTagHandlers;
    std::vector<std::size_t> matchedHandlers;  // indices into the rewriter's entries
    bool suppressesContent { false };
};

struct TextRecord {
    std::string_view content;
    bool lastInTextNode { true };
    std::vector<std::string> beforeParts, afterParts;
    std::optional<std::string> replacementHtml;
    bool removed { false };
};

struct CommentRecord {
    std::string_view raw;  // full original spelling, e.g. "<!--x-->"
    std::string text;
    bool textChanged { false };
    std::vector<std::string> beforeParts, afterParts;
    std::optional<std::string> replacementHtml;
    bool removed { false };
};

struct DoctypeRecord {
    std::string_view raw;
    std::optional<std::string> name, publicId, systemId;
    bool removed { false };
};

struct DocumentEndRecord {
    std::string* out { nullptr };
};

struct EndTagRecord {
    std::string name;          // current (rename target)
    std::string originalName;  // element's original tag name
    std::string_view raw;      // original end-tag bytes ("" for synthesized ends)
    std::vector<std::string> beforeParts, afterParts;
    bool removed { false };
};

}  // namespace mbun::html_rewriter::detail

export namespace mbun::html_rewriter {

// ---------------------------------------------------------------------------
// Handles passed to content handlers. Only valid during the handler call.
// ---------------------------------------------------------------------------

class EndTag {
private:
    detail::EndTagRecord* record_;

public:
    explicit EndTag(detail::EndTagRecord* record) : record_ { record } {}

    std::string_view name() const { return record_->name; }
    std::expected<void, std::string> set_name(std::string_view name) {
        auto valid { detail::validate_tag_name(name) };
        if (!valid) return valid;
        record_->name = detail::lower_copy(name);
        return {};
    }
    void before(std::string_view content, bool isHtml) {
        record_->beforeParts.push_back(detail::render_content(content, isHtml));
    }
    void after(std::string_view content, bool isHtml) {
        record_->afterParts.insert(record_->afterParts.begin(),
                                   detail::render_content(content, isHtml));
    }
    void remove() { record_->removed = true; }
};

class Element {
private:
    detail::ElementRecord* record_;

public:
    explicit Element(detail::ElementRecord* record) : record_ { record } {}

    std::string_view tag_name() const { return record_->tagName; }
    std::expected<void, std::string> set_tag_name(std::string_view name) {
        auto valid { detail::validate_tag_name(name) };
        if (!valid) return valid;
        record_->tagName = detail::lower_copy(name);
        record_->startDirty = true;
        return {};
    }

    bool self_closing() const { return record_->selfClosingSlash; }
    bool can_have_content() const {
        return !record_->isVoid && !(record_->selfClosingSlash && record_->ns != detail::Ns::HTML);
    }
    std::string_view namespace_uri() const {
        switch (record_->ns) {
        case detail::Ns::SVG: return detail::SVG_NS_URI;
        case detail::Ns::MATHML: return detail::MATHML_NS_URI;
        default: return detail::HTML_NS_URI;
        }
    }
    bool removed() const { return record_->removed; }

    const std::vector<Attribute>& attributes() const { return record_->attributes; }
    // Bumped by every attribute mutation; the JS binding's attribute iterators
    // detach when it changes.
    std::uint64_t attribute_generation() const { return record_->attrGeneration; }

    std::optional<std::string_view> get_attribute(std::string_view name) const {
        std::string query { detail::lower_copy(name) };
        for (const auto& a : record_->attributes) {
            if (a.name == query) return std::string_view { a.value };
        }
        return std::nullopt;
    }
    bool has_attribute(std::string_view name) const { return get_attribute(name).has_value(); }
    std::expected<void, std::string> set_attribute(std::string_view name, std::string_view value) {
        auto valid { detail::validate_attribute_name(name) };
        if (!valid) return valid;
        std::string query { detail::lower_copy(name) };
        record_->startDirty = true;
        ++record_->attrGeneration;
        for (auto& a : record_->attributes) {
            if (a.name == query) {
                a.value.assign(value);
                a.hasValue = true;
                return {};
            }
        }
        record_->attributes.push_back(Attribute { std::move(query), std::string { value }, true });
        return {};
    }
    void remove_attribute(std::string_view name) {
        std::string query { detail::lower_copy(name) };
        std::erase_if(record_->attributes, [&](const Attribute& a) { return a.name == query; });
        record_->startDirty = true;
        ++record_->attrGeneration;
    }

    void before(std::string_view content, bool isHtml) {
        record_->beforeParts.push_back(detail::render_content(content, isHtml));
    }
    void after(std::string_view content, bool isHtml) {
        record_->afterParts.insert(record_->afterParts.begin(),
                                   detail::render_content(content, isHtml));
    }
    void prepend(std::string_view content, bool isHtml) {
        record_->prependParts.insert(record_->prependParts.begin(),
                                     detail::render_content(content, isHtml));
    }
    void append(std::string_view content, bool isHtml) {
        record_->appendParts.push_back(detail::render_content(content, isHtml));
    }
    void replace(std::string_view content, bool isHtml) {
        record_->replacementHtml = detail::render_content(content, isHtml);
        record_->removed = true;
        record_->keepContent = false;
    }
    void set_inner_content(std::string_view content, bool isHtml) {
        record_->innerHtml = detail::render_content(content, isHtml);
    }
    void remove() {
        record_->removed = true;
        record_->keepContent = false;
        record_->replacementHtml.reset();
    }
    void remove_and_keep_content() {
        record_->removed = true;
        record_->keepContent = true;
        record_->replacementHtml.reset();
    }

    void on_end_tag(std::function<Directive(EndTag&)> handler) {
        record_->endTagHandlers.push_back(std::move(handler));
    }
};

class TextChunk {
private:
    detail::TextRecord* record_;

public:
    explicit TextChunk(detail::TextRecord* record) : record_ { record } {}

    std::string_view text() const { return record_->content; }
    bool last_in_text_node() const { return record_->lastInTextNode; }
    bool removed() const { return record_->removed; }
    void before(std::string_view content, bool isHtml) {
        record_->beforeParts.push_back(detail::render_content(content, isHtml));
    }
    void after(std::string_view content, bool isHtml) {
        record_->afterParts.insert(record_->afterParts.begin(),
                                   detail::render_content(content, isHtml));
    }
    void replace(std::string_view content, bool isHtml) {
        record_->replacementHtml = detail::render_content(content, isHtml);
        record_->removed = true;
    }
    void remove() {
        record_->removed = true;
        record_->replacementHtml.reset();
    }
};

class Comment {
private:
    detail::CommentRecord* record_;

public:
    explicit Comment(detail::CommentRecord* record) : record_ { record } {}

    std::string_view text() const { return record_->text; }
    std::expected<void, std::string> set_text(std::string_view text) {
        if (text.find("-->") != std::string_view::npos) {
            return std::unexpected("Comment text shouldn't contain comment closing sequence (`-->`).");
        }
        record_->text.assign(text);
        record_->textChanged = true;
        return {};
    }
    bool removed() const { return record_->removed; }
    void before(std::string_view content, bool isHtml) {
        record_->beforeParts.push_back(detail::render_content(content, isHtml));
    }
    void after(std::string_view content, bool isHtml) {
        record_->afterParts.insert(record_->afterParts.begin(),
                                   detail::render_content(content, isHtml));
    }
    void replace(std::string_view content, bool isHtml) {
        record_->replacementHtml = detail::render_content(content, isHtml);
        record_->removed = true;
    }
    void remove() {
        record_->removed = true;
        record_->replacementHtml.reset();
    }
};

class Doctype {
private:
    detail::DoctypeRecord* record_;

public:
    explicit Doctype(detail::DoctypeRecord* record) : record_ { record } {}

    // Distinguish absent (nullopt) from present-but-empty ("").
    std::optional<std::string_view> name() const {
        if (!record_->name) return std::nullopt;
        return std::string_view { *record_->name };
    }
    std::optional<std::string_view> public_id() const {
        if (!record_->publicId) return std::nullopt;
        return std::string_view { *record_->publicId };
    }
    std::optional<std::string_view> system_id() const {
        if (!record_->systemId) return std::nullopt;
        return std::string_view { *record_->systemId };
    }
    bool removed() const { return record_->removed; }
    void remove() { record_->removed = true; }
};

class DocumentEnd {
private:
    detail::DocumentEndRecord* record_;

public:
    explicit DocumentEnd(detail::DocumentEndRecord* record) : record_ { record } {}

    void append(std::string_view content, bool isHtml) {
        record_->out->append(detail::render_content(content, isHtml));
    }
};

// ---------------------------------------------------------------------------
// Handler registration surface.
// ---------------------------------------------------------------------------

struct ElementContentHandlers {
    std::function<Directive(Element&)> element;
    std::function<Directive(Comment&)> comments;
    std::function<Directive(TextChunk&)> text;
};

struct DocumentContentHandlers {
    std::function<Directive(Doctype&)> doctype;
    std::function<Directive(Comment&)> comments;
    std::function<Directive(TextChunk&)> text;
    std::function<Directive(DocumentEnd&)> end;
};

struct TransformResult {
    std::string output;
    bool stopped { false };  // a handler returned Directive::STOP; output is truncated
};

}  // export namespace mbun::html_rewriter

namespace mbun::html_rewriter::detail {

struct HandlerEntry {
    std::optional<Selector> selector;  // nullopt => document handlers
    ElementContentHandlers element;
    DocumentContentHandlers document;
};

// One transform() run. Owns the open-element stack and the output buffer.
class Transformer {
private:
    std::span<const HandlerEntry> entries_;
    std::string_view input_;
    std::string output_;
    std::vector<std::unique_ptr<ElementRecord>> stack_;  // [0] is a synthetic root
    std::vector<bool> activeScratch_;
    std::string rawTextTag_;
    int suppressDepth_ { 0 };
    bool stopped_ { false };

public:
    Transformer(std::span<const HandlerEntry> entries, std::string_view input)
        : entries_ { entries }, input_ { input } {
        output_.reserve(input.size());
        stack_.push_back(std::make_unique<ElementRecord>());
        activeScratch_.assign(entries_.size(), false);
    }

    TransformResult run() {
        std::size_t i { 0 };
        const std::size_t n { input_.size() };
        while (i < n && !stopped_) {
            if (!rawTextTag_.empty()) {
                std::size_t p { find_end_tag_ci_(i, rawTextTag_) };
                rawTextTag_.clear();
                if (p == std::string_view::npos) {
                    handle_text_(input_.substr(i));
                    i = n;
                } else {
                    if (p > i) handle_text_(input_.substr(i, p - i));
                    i = p;
                }
                continue;
            }
            if (input_[i] != '<') {
                std::size_t j { input_.find('<', i) };
                if (j == std::string_view::npos) j = n;
                handle_text_(input_.substr(i, j - i));
                i = j;
                continue;
            }
            if (i + 1 >= n) {
                emit_(input_.substr(i));
                break;
            }
            char c { input_[i + 1] };
            if (c == '!') {
                i = handle_bang_(i);
            } else if (c == '/') {
                i = handle_end_tag_(i);
            } else if (c == '?') {
                i = handle_bogus_comment_(i, i + 1);
            } else if (ascii_alpha(c)) {
                i = handle_start_tag_(i);
            } else {
                std::size_t j { input_.find('<', i + 1) };
                if (j == std::string_view::npos) j = n;
                handle_text_(input_.substr(i, j - i));
                i = j;
            }
        }
        if (!stopped_) {
            while (stack_.size() > 1) pop_silently_();
            run_document_end_();
        }
        return TransformResult { std::move(output_), stopped_ };
    }

private:
    void emit_(std::string_view s) {
        if (suppressDepth_ == 0) output_.append(s);
    }
    void emit_parts_(const std::vector<std::string>& parts) {
        if (suppressDepth_ != 0) return;
        for (const auto& p : parts) output_.append(p);
    }

    // ---- handler dispatch ---------------------------------------------------

    template <typename Handle, typename Record>
    void call_handler_(const std::function<Directive(Handle&)>& fn, Record* record) {
        if (!fn || stopped_) return;
        Handle handle { record };
        if (fn(handle) == Directive::STOP) stopped_ = true;
    }

    // Handler indices "active" for content tokens: every document entry plus
    // every element entry matched by an open element, fired in registration
    // order and at most once per token.
    template <typename Handle, typename Record, typename SelectFn>
    void dispatch_content_(SelectFn selectFn, Record* record) {
        std::ranges::fill(activeScratch_, false);
        for (std::size_t idx { 0 }; idx < entries_.size(); ++idx) {
            if (!entries_[idx].selector.has_value()) activeScratch_[idx] = true;
        }
        for (std::size_t s { 1 }; s < stack_.size(); ++s) {
            for (std::size_t idx : stack_[s]->matchedHandlers) activeScratch_[idx] = true;
        }
        for (std::size_t idx { 0 }; idx < entries_.size() && !stopped_; ++idx) {
            if (!activeScratch_[idx]) continue;
            const std::function<Directive(Handle&)>& fn { selectFn(entries_[idx]) };
            call_handler_<Handle>(fn, record);
        }
    }

    // ---- text / comment / doctype tokens -------------------------------------

    void handle_text_(std::string_view content) {
        TextRecord record;
        record.content = content;
        dispatch_content_<TextChunk>(
            [](const HandlerEntry& e) -> const std::function<Directive(TextChunk&)>& {
                return e.selector.has_value() ? e.element.text : e.document.text;
            },
            &record);
        if (stopped_) return;
        emit_parts_(record.beforeParts);
        if (record.removed) {
            if (record.replacementHtml) emit_(*record.replacementHtml);
        } else {
            emit_(content);
        }
        emit_parts_(record.afterParts);
    }

    void handle_comment_(std::string_view raw, std::string_view text) {
        CommentRecord record;
        record.raw = raw;
        record.text.assign(text);
        dispatch_content_<Comment>(
            [](const HandlerEntry& e) -> const std::function<Directive(Comment&)>& {
                return e.selector.has_value() ? e.element.comments : e.document.comments;
            },
            &record);
        if (stopped_) return;
        emit_parts_(record.beforeParts);
        if (record.removed) {
            if (record.replacementHtml) emit_(*record.replacementHtml);
        } else if (record.textChanged) {
            emit_("<!--");
            emit_(record.text);
            emit_("-->");
        } else {
            emit_(raw);
        }
        emit_parts_(record.afterParts);
    }

    // ---- token parsers --------------------------------------------------------

    std::size_t handle_bang_(std::size_t i) {
        const std::size_t n { input_.size() };
        if (input_.compare(i, 4, "<!--") == 0) return handle_comment_token_(i);
        static constexpr std::string_view DOCTYPE_KW { "doctype" };
        if (i + 2 + DOCTYPE_KW.size() <= n) {
            bool isDoctype { true };
            for (std::size_t k { 0 }; k < DOCTYPE_KW.size(); ++k) {
                if (ascii_lower(input_[i + 2 + k]) != DOCTYPE_KW[k]) {
                    isDoctype = false;
                    break;
                }
            }
            if (isDoctype) return handle_doctype_(i);
        }
        return handle_bogus_comment_(i, i + 2);
    }

    std::size_t handle_comment_token_(std::size_t i) {
        const std::size_t n { input_.size() };
        // Abrupt closings the spec treats as empty comments: <!--> and <!--->.
        if (input_.compare(i, 5, "<!-->") == 0) {
            handle_comment_(input_.substr(i, 5), {});
            return i + 5;
        }
        if (input_.compare(i, 6, "<!--->") == 0) {
            handle_comment_(input_.substr(i, 6), {});
            return i + 6;
        }
        std::size_t close { input_.find("-->", i + 4) };
        if (close == std::string_view::npos) {
            handle_comment_(input_.substr(i), input_.substr(i + 4));
            return n;
        }
        handle_comment_(input_.substr(i, close + 3 - i), input_.substr(i + 4, close - (i + 4)));
        return close + 3;
    }

    std::size_t handle_bogus_comment_(std::size_t i, std::size_t dataStart) {
        const std::size_t n { input_.size() };
        std::size_t gt { input_.find('>', dataStart) };
        if (gt == std::string_view::npos) {
            handle_comment_(input_.substr(i), input_.substr(dataStart));
            return n;
        }
        handle_comment_(input_.substr(i, gt + 1 - i), input_.substr(dataStart, gt - dataStart));
        return gt + 1;
    }

    std::size_t handle_doctype_(std::size_t i) {
        const std::size_t n { input_.size() };
        std::size_t gt { input_.find('>', i + 2) };
        if (gt == std::string_view::npos) gt = n;  // unterminated doctype at EOF
        std::string_view raw { input_.substr(i, std::min(gt + 1, n) - i) };

        DoctypeRecord record;
        record.raw = raw;
        std::size_t k { i + 9 };  // past "<!doctype"
        while (k < gt && ascii_ws(input_[k])) ++k;
        if (k < gt) {
            std::size_t b { k };
            while (k < gt && !ascii_ws(input_[k])) ++k;
            record.name = lower_copy(input_.substr(b, k - b));
        }
        while (k < gt && ascii_ws(input_[k])) ++k;
        auto keyword_is { [&](std::string_view kw) {
            if (k + kw.size() > gt) return false;
            for (std::size_t j { 0 }; j < kw.size(); ++j) {
                if (ascii_lower(input_[k + j]) != kw[j]) return false;
            }
            return true;
        } };
        auto read_quoted { [&]() -> std::optional<std::string> {
            while (k < gt && ascii_ws(input_[k])) ++k;
            if (k >= gt || (input_[k] != '"' && input_[k] != '\'')) return std::nullopt;
            char quote { input_[k++] };
            std::size_t b { k };
            while (k < gt && input_[k] != quote) ++k;
            std::string value { input_.substr(b, k - b) };
            if (k < gt) ++k;
            return value;
        } };
        if (keyword_is("public")) {
            k += 6;
            record.publicId = read_quoted();
            record.systemId = read_quoted();
        } else if (keyword_is("system")) {
            k += 6;
            record.systemId = read_quoted();
        }

        for (std::size_t idx { 0 }; idx < entries_.size() && !stopped_; ++idx) {
            if (entries_[idx].selector.has_value()) continue;
            call_handler_<Doctype>(entries_[idx].document.doctype, &record);
        }
        if (stopped_) return n;
        if (!record.removed) emit_(raw);
        return std::min(gt + 1, n);
    }

    std::size_t handle_start_tag_(std::size_t i) {
        const std::size_t n { input_.size() };
        std::size_t k { i + 1 };
        std::size_t nameBegin { k };
        while (k < n && !ascii_ws(input_[k]) && input_[k] != '/' && input_[k] != '>') ++k;
        std::string tagName { lower_copy(input_.substr(nameBegin, k - nameBegin)) };

        auto record { std::make_unique<ElementRecord>() };
        bool selfClosingSlash { false };
        std::size_t gt { std::string_view::npos };
        while (k < n) {
            while (k < n && ascii_ws(input_[k])) ++k;
            if (k >= n) break;
            char c { input_[k] };
            if (c == '>') {
                gt = k;
                break;
            }
            if (c == '/') {
                if (k + 1 < n && input_[k + 1] == '>') {
                    selfClosingSlash = true;
                    gt = k + 1;
                    break;
                }
                ++k;  // stray slash between attributes: ignored (HTML rule)
                continue;
            }
            std::size_t ab { k };
            while (k < n && !ascii_ws(input_[k]) && input_[k] != '=' && input_[k] != '/' &&
                   input_[k] != '>') {
                ++k;
            }
            if (ab == k) {  // unexpected byte; skip it so the scan always advances
                ++k;
                continue;
            }
            Attribute attr;
            attr.name = lower_copy(input_.substr(ab, k - ab));
            attr.hasValue = false;
            while (k < n && ascii_ws(input_[k])) ++k;
            if (k < n && input_[k] == '=') {
                ++k;
                while (k < n && ascii_ws(input_[k])) ++k;
                if (k < n && (input_[k] == '"' || input_[k] == '\'')) {
                    char quote { input_[k++] };
                    std::size_t vb { k };
                    while (k < n && input_[k] != quote) ++k;
                    if (k >= n) break;  // unterminated quote: incomplete tag
                    attr.value.assign(input_.substr(vb, k - vb));
                    ++k;
                } else {
                    std::size_t vb { k };
                    while (k < n && !ascii_ws(input_[k]) && input_[k] != '>') ++k;
                    attr.value.assign(input_.substr(vb, k - vb));
                }
                attr.hasValue = true;
            }
            record->attributes.push_back(std::move(attr));
        }
        if (gt == std::string_view::npos) {
            // EOF inside the tag: pass the fragment through untouched.
            emit_(input_.substr(i));
            return n;
        }

        ElementRecord& parent { *stack_.back() };
        record->tagName = tagName;
        record->originalTagName = tagName;
        record->rawStart = std::string { input_.substr(i, gt + 1 - i) };
        record->selfClosingSlash = selfClosingSlash;
        record->ns = tagName == "svg"    ? Ns::SVG
                     : tagName == "math" ? Ns::MATHML
                                         : parent.ns;
        record->isVoid = record->ns == Ns::HTML && is_void_element(tagName);
        record->nthChild = ++parent.childElements;
        record->nthOfType = ++parent.childTypeCounts[tagName];
        const bool closesImmediately { record->isVoid ||
                                       (selfClosingSlash && record->ns != Ns::HTML) };

        // Selector matching runs against the pre-handler snapshot, then the
        // element handlers fire in registration order.
        std::vector<MatchContext> ancestry;
        ancestry.reserve(stack_.size());
        for (std::size_t s { 1 }; s < stack_.size(); ++s) {
            ancestry.push_back(MatchContext { stack_[s]->originalTagName, &stack_[s]->attributes,
                                              stack_[s]->nthChild, stack_[s]->nthOfType });
        }
        ancestry.push_back(MatchContext { record->originalTagName, &record->attributes,
                                          record->nthChild, record->nthOfType });
        for (std::size_t idx { 0 }; idx < entries_.size(); ++idx) {
            if (entries_[idx].selector.has_value() && entries_[idx].selector->matches(ancestry)) {
                record->matchedHandlers.push_back(idx);
            }
        }
        for (std::size_t idx : record->matchedHandlers) {
            if (stopped_) break;
            call_handler_<Element>(entries_[idx].element.element, record.get());
        }
        if (stopped_) return n;

        emit_parts_(record->beforeParts);
        if (record->removed && !record->keepContent) {
            if (record->replacementHtml) emit_(*record->replacementHtml);
            if (closesImmediately) {
                emit_parts_(record->afterParts);
                return gt + 1;
            }
            record->suppressesContent = true;
        } else {
            if (!record->removed) emit_start_tag_(*record);
            if (closesImmediately) {
                emit_parts_(record->afterParts);
                return gt + 1;
            }
            if (record->innerHtml) {
                emit_(*record->innerHtml);
                record->suppressesContent = true;
            } else {
                emit_parts_(record->prependParts);
            }
        }

        if (record->ns == Ns::HTML && is_raw_text_element(tagName)) rawTextTag_ = tagName;
        if (record->suppressesContent) ++suppressDepth_;
        stack_.push_back(std::move(record));
        return gt + 1;
    }

    void emit_start_tag_(const ElementRecord& record) {
        if (!record.startDirty) {
            emit_(record.rawStart);
            return;
        }
        if (suppressDepth_ != 0) return;
        output_ += '<';
        output_ += record.tagName;
        for (const auto& a : record.attributes) {
            output_ += ' ';
            output_ += a.name;
            if (a.hasValue) {
                output_ += "=\"";
                output_ += escape_attribute(a.value);
                output_ += '"';
            }
        }
        output_ += record.selfClosingSlash ? "/>" : ">";
    }

    std::size_t handle_end_tag_(std::size_t i) {
        const std::size_t n { input_.size() };
        std::size_t k { i + 2 };
        if (k >= n) {
            emit_(input_.substr(i));
            return n;
        }
        if (input_[k] == '>') {  // "</>" is dropped by the spec; keep the bytes
            emit_(input_.substr(i, 3));
            return i + 3;
        }
        if (!ascii_alpha(input_[k])) return handle_bogus_comment_(i, k);
        std::size_t nameBegin { k };
        while (k < n && !ascii_ws(input_[k]) && input_[k] != '/' && input_[k] != '>') ++k;
        std::string name { lower_copy(input_.substr(nameBegin, k - nameBegin)) };
        std::size_t gt { input_.find('>', k) };
        if (gt == std::string_view::npos) {
            emit_(input_.substr(i));
            return n;
        }
        std::string_view raw { input_.substr(i, gt + 1 - i) };

        std::size_t matchIndex { 0 };  // 0 = no match (index 0 is the synthetic root)
        for (std::size_t s { stack_.size() }; s > 1; --s) {
            if (stack_[s - 1]->originalTagName == name) {
                matchIndex = s - 1;
                break;
            }
        }
        if (matchIndex == 0) {
            emit_(raw);  // stray end tag: passthrough
            return gt + 1;
        }
        // Elements the input never closed: popped without output; their
        // pending append/after content is dropped (they have no end point).
        while (stack_.size() - 1 > matchIndex) pop_silently_();
        close_top_(raw);
        return gt + 1;
    }

    void close_top_(std::string_view rawEndTag) {
        std::unique_ptr<ElementRecord> record { std::move(stack_.back()) };
        stack_.pop_back();

        EndTagRecord endTag;
        endTag.name = record->tagName;
        endTag.originalName = record->originalTagName;
        endTag.raw = rawEndTag;
        for (const auto& handler : record->endTagHandlers) {
            if (stopped_) return;
            if (handler) {
                EndTag handle { &endTag };
                if (handler(handle) == Directive::STOP) {
                    stopped_ = true;
                    return;
                }
            }
        }

        if (record->suppressesContent) --suppressDepth_;
        if (record->removed && !record->keepContent) {
            emit_parts_(record->afterParts);
            return;
        }
        if (!record->removed) emit_parts_(record->appendParts);
        else if (record->keepContent) emit_parts_(record->appendParts);
        if (!record->removed) {
            emit_parts_(endTag.beforeParts);
            if (!endTag.removed) {
                if (endTag.name == endTag.originalName) {
                    emit_(rawEndTag);
                } else if (suppressDepth_ == 0) {
                    output_ += "</";
                    output_ += endTag.name;
                    output_ += '>';
                }
            }
            emit_parts_(endTag.afterParts);
        }
        emit_parts_(record->afterParts);
    }

    void pop_silently_() {
        std::unique_ptr<ElementRecord> record { std::move(stack_.back()) };
        stack_.pop_back();
        if (record->suppressesContent) --suppressDepth_;
    }

    void run_document_end_() {
        DocumentEndRecord record { &output_ };
        for (std::size_t idx { 0 }; idx < entries_.size() && !stopped_; ++idx) {
            if (entries_[idx].selector.has_value()) continue;
            call_handler_<DocumentEnd>(entries_[idx].document.end, &record);
        }
    }

    // Case-insensitive search for "</tag" followed by a tag-boundary byte.
    std::size_t find_end_tag_ci_(std::size_t from, std::string_view tag) const {
        const std::size_t n { input_.size() };
        for (std::size_t i { from }; i + 2 + tag.size() <= n; ++i) {
            if (input_[i] != '<' || input_[i + 1] != '/') continue;
            bool match { true };
            for (std::size_t j { 0 }; j < tag.size(); ++j) {
                if (ascii_lower(input_[i + 2 + j]) != tag[j]) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            std::size_t after { i + 2 + tag.size() };
            if (after >= n || ascii_ws(input_[after]) || input_[after] == '/' ||
                input_[after] == '>') {
                return i;
            }
        }
        return std::string_view::npos;
    }
};

}  // namespace mbun::html_rewriter::detail

export namespace mbun::html_rewriter {

class Rewriter {
private:
    std::vector<detail::HandlerEntry> entries_;

public:  // Big Five — handler entries hold move-only selectors.
    Rewriter() = default;
    Rewriter(const Rewriter&) = delete;
    Rewriter& operator=(const Rewriter&) = delete;
    Rewriter(Rewriter&&) = default;
    Rewriter& operator=(Rewriter&&) = default;
    ~Rewriter() = default;

public:
    void on(Selector selector, ElementContentHandlers handlers) {
        entries_.push_back(detail::HandlerEntry { std::move(selector), std::move(handlers), {} });
    }
    void on_document(DocumentContentHandlers handlers) {
        entries_.push_back(detail::HandlerEntry { std::nullopt, {}, std::move(handlers) });
    }

    // Single-shot: parses and rewrites the whole document in one call.
    TransformResult transform(std::string_view html) const {
        detail::Transformer transformer { entries_, html };
        return transformer.run();
    }
};

}  // export namespace mbun::html_rewriter
