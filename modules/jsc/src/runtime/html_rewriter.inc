// ── __mbunHTMLRewriterNative: Bun's HTMLRewriter over mbun.html_rewriter ────
// Blueprint: bun's HTMLRewriter binding (bun-ref src/runtime/api/html_rewriter.rs,
// which drives lol-html). The engine here is mbun.html_rewriter — the
// dependency-free C++26 rewriter in modules/html_rewriter — imported by
// runtime.cppm; this file only translates between JS and that engine.
//
// Model: transform() is synchronous. Engine handlers are std::functions that
// call the user's JS handler directly, passing a wrapper object whose methods
// operate on the live engine handle. The handle is only valid for the duration
// of the handler call, so the wrapper's private slot is nulled right after
// (use-after-return throws; getters return undefined/null like bun).
//
// Ownership: everything is scoped to the transform() C++ frame — the
// hr::Rewriter, its selectors and handler closures are destroyed on return.
// The only manual pieces are the GC protections for element.onEndTag callbacks
// (released after the run on every path) and the attribute-iterator state
// (released by the iterator's JSC finalizer; a shared epoch counter detaches
// iterators when their element wrapper is invalidated, so they never touch a
// dangling engine pointer).
//
// Surface (JS layer in builtins/html_rewriter.cppm):
//   new HTMLRewriter().on(sel, {element,comments,text})
//                     .onDocument({doctype,comments,text,end})
//                     .transform(string|ArrayBuffer|TypedArray|Response|Blob)
// Native entry: __mbunHTMLRewriterNative.transform(handlerSpec, html) -> string.
//
// DEFERRED (honest scope): async (Promise-returning) handlers are rejected on
// this synchronous path (bun pauses the stream instead); chunked writes (the
// JS layer buffers the body first). NOTE: callbacks below stick to the plain
// JSC C API — see the DropAllLocks warning on set_fn in common.inc.

namespace mbun_htmlrewriter {

namespace hr = mbun::html_rewriter;

// ---- per-transform shared state -------------------------------------------
struct TransformCtx {
    JSContextRef ctx{nullptr};
    JSValueRef exception{nullptr};   // first JS exception thrown by a handler
    // Bumped whenever a wrapper is invalidated; attribute iterators snapshot it
    // and report {done:true} once it moves on. shared_ptr so an iterator leaked
    // to JS can still read it safely after this TransformCtx is gone.
    std::shared_ptr<std::uint64_t> epoch{std::make_shared<std::uint64_t>(0)};
    // element.onEndTag(fn) callbacks: protected while the engine may still call
    // them, unprotected after the run (leak-tested in bun's suite).
    std::vector<JSObjectRef> endTagFns;
};

// The currently-running transform, so wrapper methods with no ctx handle of
// their own (onEndTag registration, attribute-iterator creation) can reach it.
// transform() runs synchronously on one thread, so a single global suffices.
TransformCtx* g_current_tc{nullptr};

JSClassRef g_element_class{nullptr};
JSClassRef g_text_class{nullptr};
JSClassRef g_comment_class{nullptr};
JSClassRef g_doctype_class{nullptr};
JSClassRef g_docend_class{nullptr};
JSClassRef g_endtag_class{nullptr};
JSClassRef g_attriter_class{nullptr};

template <typename T>
inline T* live(JSObjectRef obj) {
    return static_cast<T*>(JSObjectGetPrivate(obj));
}

inline JSValueRef throw_used_after(JSContextRef ctx, JSValueRef* exc) {
    if (exc) *exc = make_string(ctx, "This content handle is no longer valid (used after the handler returned)");
    return JSValueMakeUndefined(ctx);
}

// Real Error / TypeError objects (so JS `instanceof Error/TypeError` and
// .message hold, matching bun — a bare thrown string would not).
inline JSValueRef make_error(JSContextRef ctx, std::string_view msg) {
    JSValueRef m{make_string(ctx, msg)};
    return JSObjectMakeError(ctx, 1, &m, nullptr);
}
inline JSValueRef make_type_error(JSContextRef ctx, std::string_view msg) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSValueRef ctorV{get_prop(ctx, global, "TypeError")};
    if (JSValueIsObject(ctx, ctorV)) {
        JSObjectRef ctor{JSValueToObject(ctx, ctorV, nullptr)};
        JSValueRef m{make_string(ctx, msg)};
        if (JSValueRef e{JSObjectCallAsConstructor(ctx, ctor, 1, &m, nullptr)}) return e;
    }
    return make_error(ctx, msg);
}

// Engine mutations report failures as std::expected errors; surface them as
// thrown JS Errors (setAttribute("a b", …) must throw, not return a code).
inline bool check_result(JSContextRef ctx, const std::expected<void, std::string>& result,
                         JSValueRef* exc) {
    if (result.has_value()) return true;
    if (exc) *exc = make_error(ctx, result.error());
    return false;
}

// content arg (args[0]) + optional {html:bool} (args[1]) → (string, is_html).
inline std::string content_arg(JSContextRef ctx, std::size_t argc, const JSValueRef args[], bool& is_html) {
    is_html = false;
    if (argc > 1 && JSValueIsObject(ctx, args[1])) {
        JSObjectRef opt{JSValueToObject(ctx, args[1], nullptr)};
        is_html = JSValueToBoolean(ctx, get_prop(ctx, opt, "html"));
    }
    return arg_string(ctx, argc, args, 0);
}

// Is `v` a thenable (Promise-like)? Async handlers can't be awaited on the
// synchronous transform path, so returning one is treated as an error (bun
// throws too). Detected structurally (v.then is callable).
inline bool is_thenable(JSContextRef ctx, JSValueRef v) {
    if (!v || !JSValueIsObject(ctx, v)) return false;
    JSObjectRef o{JSValueToObject(ctx, v, nullptr)};
    JSValueRef thenV{get_prop(ctx, o, "then")};
    return JSValueIsObject(ctx, thenV) &&
           JSObjectIsFunction(ctx, JSValueToObject(ctx, thenV, nullptr));
}

inline JSValueRef optional_string(JSContextRef ctx, std::optional<std::string_view> s) {
    if (!s.has_value()) return JSValueMakeNull(ctx);
    return make_string(ctx, *s);
}

// ---- attribute iterator -----------------------------------------------------
// element.attributes returns a fresh live iterator. It detaches (reports done)
// when the element wrapper is invalidated (epoch moved) or the attribute list
// is mutated (generation moved) — never dereferencing a stale engine pointer.
struct AttrIterState {
    std::shared_ptr<std::uint64_t> epoch;
    std::uint64_t epochAt{0};
    std::optional<hr::Element> element;
    std::uint64_t genAt{0};
    std::size_t index{0};

    bool valid() const {
        return element.has_value() && epoch && *epoch == epochAt &&
               element->attribute_generation() == genAt;
    }
};

void attriter_finalize(JSObjectRef obj) {
    delete static_cast<AttrIterState*>(JSObjectGetPrivate(obj));
}

JSValueRef attriter_next(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                         std::size_t, const JSValueRef[], JSValueRef*) {
    auto* st{live<AttrIterState>(self)};
    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    bool done{true};
    JSValueRef value{JSValueMakeUndefined(ctx)};
    if (st && st->valid()) {
        const auto& attrs{st->element->attributes()};
        if (st->index < attrs.size()) {
            done = false;
            const auto& a{attrs[st->index++]};
            JSValueRef kv[2]{make_string(ctx, a.name), make_string(ctx, a.value)};
            value = JSObjectMakeArray(ctx, 2, kv, nullptr);
        }
    }
    set_prop(ctx, result, "done", JSValueMakeBoolean(ctx, done));
    set_prop(ctx, result, "value", value);
    return result;
}

static const JSStaticFunction kAttrIterFns[]{
    {"next", attriter_next, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};

// it[Symbol.iterator] = function(){ return this; } — the only step that needs
// JS (the C API cannot address well-known symbols). The helper function is
// created once per context by install_htmlrewriter_native.
inline void mark_iterable(JSContextRef ctx, JSObjectRef it) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSValueRef nsV{get_prop(ctx, global, "__mbunHTMLRewriterNative")};
    if (!JSValueIsObject(ctx, nsV)) return;
    JSValueRef mkV{get_prop(ctx, JSValueToObject(ctx, nsV, nullptr), "_mkIter")};
    if (!JSValueIsObject(ctx, mkV)) return;
    JSObjectRef mk{JSValueToObject(ctx, mkV, nullptr)};
    if (!JSObjectIsFunction(ctx, mk)) return;
    JSValueRef arg{it};
    JSObjectCallAsFunction(ctx, mk, nullptr, 1, &arg, nullptr);
}

JSObjectRef make_attributes_iterator(JSContextRef ctx, hr::Element* element) {
    auto* st{new AttrIterState};
    if (element != nullptr && g_current_tc != nullptr) {
        st->epoch = g_current_tc->epoch;
        st->epochAt = *st->epoch;
        st->element = *element;
        st->genAt = element->attribute_generation();
    }  // else: st->valid() is false → a permanently-done iterator
    JSObjectRef it{JSObjectMake(ctx, g_attriter_class, st)};
    mark_iterable(ctx, it);
    return it;
}

// ===== Element methods ======================================================
JSValueRef el_getAttribute(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                           std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    std::string name{arg_string(ctx, argc, args, 0)};
    return optional_string(ctx, e->get_attribute(name));
}
JSValueRef el_hasAttribute(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                           std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    std::string name{arg_string(ctx, argc, args, 0)};
    return JSValueMakeBoolean(ctx, e->has_attribute(name));
}
JSValueRef el_setAttribute(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                           std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    std::string name{arg_string(ctx, argc, args, 0)};
    std::string val{arg_string(ctx, argc, args, 1)};
    if (!check_result(ctx, e->set_attribute(name, val), exc)) return JSValueMakeUndefined(ctx);
    return self;
}
JSValueRef el_removeAttribute(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                              std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    std::string name{arg_string(ctx, argc, args, 0)};
    e->remove_attribute(name);
    return self;
}
#define EL_INSERT(FNAME, METHOD)                                                    \
    JSValueRef FNAME(JSContextRef ctx, JSObjectRef, JSObjectRef self,               \
                     std::size_t argc, const JSValueRef args[], JSValueRef* exc) {  \
        auto* e{live<hr::Element>(self)};                                           \
        if (!e) return throw_used_after(ctx, exc);                                  \
        bool html;                                                                  \
        std::string c{content_arg(ctx, argc, args, html)};                          \
        e->METHOD(c, html);                                                         \
        return self;                                                                \
    }
EL_INSERT(el_before, before)
EL_INSERT(el_after, after)
EL_INSERT(el_prepend, prepend)
EL_INSERT(el_append, append)
EL_INSERT(el_replace, replace)
EL_INSERT(el_setInnerContent, set_inner_content)
#undef EL_INSERT
JSValueRef el_remove(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                     std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    e->remove();
    return self;
}
JSValueRef el_removeAndKeepContent(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                                   std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    e->remove_and_keep_content();
    return self;
}

// Forward decl: end-tag callbacks share the generic trampoline below.
template <typename Handle>
hr::Directive call_js_handler(TransformCtx& tc, JSObjectRef fn, JSClassRef cls, Handle* handle);

// element.onEndTag(fn): registers a deferred handler fired at the element's
// end tag. The fn must survive past this element callback, so it is
// GC-protected and parked on the TransformCtx until the transform completes.
JSValueRef el_onEndTag(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                       std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return throw_used_after(ctx, exc);
    bool ok{argc >= 1 && JSValueIsObject(ctx, args[0]) &&
            JSObjectIsFunction(ctx, JSValueToObject(ctx, args[0], nullptr))};
    if (!ok) {
        if (exc) *exc = make_type_error(ctx, "Expected a function");
        return JSValueMakeUndefined(ctx);
    }
    JSObjectRef fn{JSValueToObject(ctx, args[0], nullptr)};
    if (TransformCtx* tc{g_current_tc}) {
        JSValueProtect(ctx, fn);
        tc->endTagFns.push_back(fn);
        e->on_end_tag([tc, fn](hr::EndTag& end) {
            return call_js_handler(*tc, fn, g_endtag_class, &end);
        });
    }
    return JSValueMakeUndefined(ctx);
}

// ---- Element static values (getters/setters) ------------------------------
JSValueRef el_getTagNameV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* e{live<hr::Element>(self)};
    if (!e) return JSValueMakeUndefined(ctx);
    return make_string(ctx, e->tag_name());
}
bool el_setTagNameV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef value, JSValueRef* exc) {
    auto* e{live<hr::Element>(self)};
    if (!e) return true;  // detached: no-op, never coerce the value
    std::string name{val_to_string(ctx, value)};
    check_result(ctx, e->set_tag_name(name), exc);
    return true;
}
JSValueRef el_getRemovedV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* e{live<hr::Element>(self)};
    if (!e) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, e->removed());
}
JSValueRef el_getSelfClosingV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* e{live<hr::Element>(self)};
    if (!e) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, e->self_closing());
}
JSValueRef el_getCanHaveContentV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* e{live<hr::Element>(self)};
    if (!e) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, e->can_have_content());
}
JSValueRef el_getNamespaceURIV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* e{live<hr::Element>(self)};
    if (!e) return JSValueMakeUndefined(ctx);
    return make_string(ctx, e->namespace_uri());
}
JSValueRef el_getAttributesV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    return make_attributes_iterator(ctx, live<hr::Element>(self));
}

static const JSStaticFunction kElementFns[]{
    {"getAttribute", el_getAttribute, kJSPropertyAttributeDontDelete},
    {"hasAttribute", el_hasAttribute, kJSPropertyAttributeDontDelete},
    {"setAttribute", el_setAttribute, kJSPropertyAttributeDontDelete},
    {"removeAttribute", el_removeAttribute, kJSPropertyAttributeDontDelete},
    {"before", el_before, kJSPropertyAttributeDontDelete},
    {"after", el_after, kJSPropertyAttributeDontDelete},
    {"prepend", el_prepend, kJSPropertyAttributeDontDelete},
    {"append", el_append, kJSPropertyAttributeDontDelete},
    {"replace", el_replace, kJSPropertyAttributeDontDelete},
    {"setInnerContent", el_setInnerContent, kJSPropertyAttributeDontDelete},
    {"remove", el_remove, kJSPropertyAttributeDontDelete},
    {"removeAndKeepContent", el_removeAndKeepContent, kJSPropertyAttributeDontDelete},
    {"onEndTag", el_onEndTag, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};
static const JSStaticValue kElementVals[]{
    {"tagName", el_getTagNameV, el_setTagNameV, kJSPropertyAttributeDontDelete},
    {"removed", el_getRemovedV, nullptr, kJSPropertyAttributeDontDelete},
    {"selfClosing", el_getSelfClosingV, nullptr, kJSPropertyAttributeDontDelete},
    {"canHaveContent", el_getCanHaveContentV, nullptr, kJSPropertyAttributeDontDelete},
    {"namespaceURI", el_getNamespaceURIV, nullptr, kJSPropertyAttributeDontDelete},
    {"attributes", el_getAttributesV, nullptr, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, nullptr, 0}};

// ===== Text methods =========================================================
#define TXT_INSERT(FNAME, METHOD)                                                   \
    JSValueRef FNAME(JSContextRef ctx, JSObjectRef, JSObjectRef self,               \
                     std::size_t argc, const JSValueRef args[], JSValueRef* exc) {  \
        auto* t{live<hr::TextChunk>(self)};                                         \
        if (!t) return throw_used_after(ctx, exc);                                  \
        bool html;                                                                  \
        std::string c{content_arg(ctx, argc, args, html)};                          \
        t->METHOD(c, html);                                                         \
        return self;                                                                \
    }
TXT_INSERT(txt_before, before)
TXT_INSERT(txt_after, after)
TXT_INSERT(txt_replace, replace)
#undef TXT_INSERT
JSValueRef txt_remove(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                      std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* t{live<hr::TextChunk>(self)};
    if (!t) return throw_used_after(ctx, exc);
    t->remove();
    return self;
}
JSValueRef txt_getTextV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* t{live<hr::TextChunk>(self)};
    if (!t) return JSValueMakeUndefined(ctx);
    return make_string(ctx, t->text());
}
JSValueRef txt_getLastV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* t{live<hr::TextChunk>(self)};
    if (!t) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, t->last_in_text_node());
}
JSValueRef txt_getRemovedV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* t{live<hr::TextChunk>(self)};
    if (!t) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, t->removed());
}
static const JSStaticFunction kTextFns[]{
    {"before", txt_before, kJSPropertyAttributeDontDelete},
    {"after", txt_after, kJSPropertyAttributeDontDelete},
    {"replace", txt_replace, kJSPropertyAttributeDontDelete},
    {"remove", txt_remove, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};
static const JSStaticValue kTextVals[]{
    {"text", txt_getTextV, nullptr, kJSPropertyAttributeDontDelete},
    {"lastInTextNode", txt_getLastV, nullptr, kJSPropertyAttributeDontDelete},
    {"removed", txt_getRemovedV, nullptr, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, nullptr, 0}};

// ===== Comment methods ======================================================
#define CMT_INSERT(FNAME, METHOD)                                                   \
    JSValueRef FNAME(JSContextRef ctx, JSObjectRef, JSObjectRef self,               \
                     std::size_t argc, const JSValueRef args[], JSValueRef* exc) {  \
        auto* c{live<hr::Comment>(self)};                                           \
        if (!c) return throw_used_after(ctx, exc);                                  \
        bool html;                                                                  \
        std::string s{content_arg(ctx, argc, args, html)};                          \
        c->METHOD(s, html);                                                         \
        return self;                                                                \
    }
CMT_INSERT(cmt_before, before)
CMT_INSERT(cmt_after, after)
CMT_INSERT(cmt_replace, replace)
#undef CMT_INSERT
JSValueRef cmt_remove(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                      std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* c{live<hr::Comment>(self)};
    if (!c) return throw_used_after(ctx, exc);
    c->remove();
    return self;
}
JSValueRef cmt_getTextV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* c{live<hr::Comment>(self)};
    if (!c) return JSValueMakeNull(ctx);  // detached comment.text -> null (bun)
    return make_string(ctx, c->text());
}
bool cmt_setTextV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef value, JSValueRef* exc) {
    auto* c{live<hr::Comment>(self)};
    if (!c) return true;  // detached: no-op, never coerce the value
    std::string v{val_to_string(ctx, value)};
    check_result(ctx, c->set_text(v), exc);
    return true;
}
JSValueRef cmt_getRemovedV(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* c{live<hr::Comment>(self)};
    if (!c) return JSValueMakeUndefined(ctx);
    return JSValueMakeBoolean(ctx, c->removed());
}
static const JSStaticFunction kCommentFns[]{
    {"before", cmt_before, kJSPropertyAttributeDontDelete},
    {"after", cmt_after, kJSPropertyAttributeDontDelete},
    {"replace", cmt_replace, kJSPropertyAttributeDontDelete},
    {"remove", cmt_remove, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};
static const JSStaticValue kCommentVals[]{
    {"text", cmt_getTextV, cmt_setTextV, kJSPropertyAttributeDontDelete},
    {"removed", cmt_getRemovedV, nullptr, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, nullptr, 0}};

// ===== Doctype ==============================================================
JSValueRef dt_getName(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* d{live<hr::Doctype>(self)};
    if (!d) return JSValueMakeNull(ctx);
    return optional_string(ctx, d->name());
}
JSValueRef dt_getPublicId(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* d{live<hr::Doctype>(self)};
    if (!d) return JSValueMakeNull(ctx);
    return optional_string(ctx, d->public_id());
}
JSValueRef dt_getSystemId(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* d{live<hr::Doctype>(self)};
    if (!d) return JSValueMakeNull(ctx);
    return optional_string(ctx, d->system_id());
}
JSValueRef dt_getRemoved(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* d{live<hr::Doctype>(self)};
    return JSValueMakeBoolean(ctx, d ? d->removed() : false);
}
JSValueRef dt_remove(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                     std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* d{live<hr::Doctype>(self)};
    if (!d) return throw_used_after(ctx, exc);
    d->remove();
    return self;
}
static const JSStaticFunction kDoctypeFns[]{
    {"remove", dt_remove, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};
static const JSStaticValue kDoctypeVals[]{
    {"name", dt_getName, nullptr, kJSPropertyAttributeDontDelete},
    {"publicId", dt_getPublicId, nullptr, kJSPropertyAttributeDontDelete},
    {"systemId", dt_getSystemId, nullptr, kJSPropertyAttributeDontDelete},
    {"removed", dt_getRemoved, nullptr, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, nullptr, 0}};

// ===== DocumentEnd ==========================================================
JSValueRef de_append(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                     std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* d{live<hr::DocumentEnd>(self)};
    if (!d) return throw_used_after(ctx, exc);
    bool html;
    std::string c{content_arg(ctx, argc, args, html)};
    d->append(c, html);
    return self;
}
static const JSStaticFunction kDocEndFns[]{
    {"append", de_append, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};

// ===== EndTag ===============================================================
JSValueRef et_getName(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef*) {
    auto* t{live<hr::EndTag>(self)};
    if (!t) return JSValueMakeUndefined(ctx);
    return make_string(ctx, t->name());
}
bool et_setName(JSContextRef ctx, JSObjectRef self, JSStringRef, JSValueRef value, JSValueRef* exc) {
    auto* t{live<hr::EndTag>(self)};
    if (!t) return true;  // detached: no-op, never coerce the value
    std::string name{val_to_string(ctx, value)};
    check_result(ctx, t->set_name(name), exc);
    return true;
}
#define ET_INSERT(FNAME, METHOD)                                                    \
    JSValueRef FNAME(JSContextRef ctx, JSObjectRef, JSObjectRef self,               \
                     std::size_t argc, const JSValueRef args[], JSValueRef* exc) {  \
        auto* t{live<hr::EndTag>(self)};                                            \
        if (!t) return throw_used_after(ctx, exc);                                  \
        bool html;                                                                  \
        std::string c{content_arg(ctx, argc, args, html)};                          \
        t->METHOD(c, html);                                                         \
        return self;                                                                \
    }
ET_INSERT(et_before, before)
ET_INSERT(et_after, after)
#undef ET_INSERT
JSValueRef et_remove(JSContextRef ctx, JSObjectRef, JSObjectRef self,
                     std::size_t, const JSValueRef[], JSValueRef* exc) {
    auto* t{live<hr::EndTag>(self)};
    if (!t) return throw_used_after(ctx, exc);
    t->remove();
    return self;
}
static const JSStaticFunction kEndTagFns[]{
    {"before", et_before, kJSPropertyAttributeDontDelete},
    {"after", et_after, kJSPropertyAttributeDontDelete},
    {"remove", et_remove, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, 0}};
static const JSStaticValue kEndTagVals[]{
    {"name", et_getName, et_setName, kJSPropertyAttributeDontDelete},
    {nullptr, nullptr, nullptr, 0}};

void ensure_classes() {
    if (g_element_class) return;
    JSClassDefinition d;
    d = kJSClassDefinitionEmpty; d.className = "Element";
    d.staticFunctions = kElementFns; d.staticValues = kElementVals;
    g_element_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "Text";
    d.staticFunctions = kTextFns; d.staticValues = kTextVals;
    g_text_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "Comment";
    d.staticFunctions = kCommentFns; d.staticValues = kCommentVals;
    g_comment_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "Doctype";
    d.staticFunctions = kDoctypeFns; d.staticValues = kDoctypeVals;
    g_doctype_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "DocumentEnd";
    d.staticFunctions = kDocEndFns;
    g_docend_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "EndTag";
    d.staticFunctions = kEndTagFns; d.staticValues = kEndTagVals;
    g_endtag_class = JSClassCreate(&d);
    d = kJSClassDefinitionEmpty; d.className = "AttributesIterator";
    d.staticFunctions = kAttrIterFns; d.finalize = attriter_finalize;
    g_attriter_class = JSClassCreate(&d);
}

// ---- generic trampoline: wrap the live handle, call the JS handler ----------
template <typename Handle>
hr::Directive call_js_handler(TransformCtx& tc, JSObjectRef fn, JSClassRef cls, Handle* handle) {
    if (tc.exception) return hr::Directive::STOP;
    JSContextRef ctx{tc.ctx};
    JSObjectRef wrapper{JSObjectMake(ctx, cls, handle)};
    JSValueRef arg{wrapper};
    JSValueRef exc{nullptr};
    JSValueRef rv{JSObjectCallAsFunction(ctx, fn, nullptr, 1, &arg, &exc)};
    // A handler returning a Promise cannot be awaited on the synchronous
    // transform path — bun treats this as an error (the pending handler poisons
    // the stream). Surface it as a thrown error so transform() rejects/throws.
    if (!exc && is_thenable(ctx, rv))
        exc = make_error(ctx, "HTMLRewriter: async handlers are not supported on this transform path");
    JSObjectSetPrivate(wrapper, nullptr);  // invalidate; handle dies with the callback
    ++(*tc.epoch);                          // detach any leaked attribute iterators
    if (exc) {
        tc.exception = exc;
        return hr::Directive::STOP;
    }
    return hr::Directive::CONTINUE;
}

// Engine-facing handler closure for one (JS function, wrapper class) pair.
template <typename Handle>
std::function<hr::Directive(Handle&)> js_handler(TransformCtx& tc, JSContextRef ctx,
                                                 JSObjectRef entry, const char* key,
                                                 JSClassRef cls) {
    JSValueRef fnV{get_prop(ctx, entry, key)};
    if (!JSValueIsObject(ctx, fnV)) return nullptr;
    JSObjectRef fn{JSValueToObject(ctx, fnV, nullptr)};
    if (!JSObjectIsFunction(ctx, fn)) return nullptr;
    // `fn` stays reachable from the spec array argument for the whole
    // synchronous transform, so no extra GC protection is needed here.
    return [&tc, fn, cls](Handle& handle) { return call_js_handler(tc, fn, cls, &handle); };
}

// ---- transform(spec, html) -> string ----------------------------------------
// spec: array of entries. Element entry: {selector, element?, comments?, text?}.
// Document entry: {document:true, doctype?, comments?, text?, end?}.
JSValueRef htmlrewriter_transform(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                  std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    ensure_classes();
    if (argc < 2 || !JSValueIsObject(ctx, args[0])) {
        if (exc) *exc = make_error(ctx, "HTMLRewriter.transform: bad arguments");
        return JSValueMakeUndefined(ctx);
    }
    JSObjectRef specArr{JSValueToObject(ctx, args[0], nullptr)};
    std::string html{val_to_string(ctx, args[1])};

    TransformCtx tc;
    tc.ctx = ctx;
    hr::Rewriter rewriter;

    int n{static_cast<int>(JSValueToNumber(ctx, get_prop(ctx, specArr, "length"), nullptr))};
    for (int i{0}; i < n; ++i) {
        JSValueRef ev{JSObjectGetPropertyAtIndex(ctx, specArr, static_cast<unsigned>(i), nullptr)};
        if (!JSValueIsObject(ctx, ev)) continue;
        JSObjectRef entry{JSValueToObject(ctx, ev, nullptr)};
        bool isDoc{JSValueToBoolean(ctx, get_prop(ctx, entry, "document"))};
        if (isDoc) {
            rewriter.on_document(hr::DocumentContentHandlers{
                .doctype = js_handler<hr::Doctype>(tc, ctx, entry, "doctype", g_doctype_class),
                .comments = js_handler<hr::Comment>(tc, ctx, entry, "comments", g_comment_class),
                .text = js_handler<hr::TextChunk>(tc, ctx, entry, "text", g_text_class),
                .end = js_handler<hr::DocumentEnd>(tc, ctx, entry, "end", g_docend_class),
            });
        } else {
            std::string selText{val_to_string(ctx, get_prop(ctx, entry, "selector"))};
            auto selector{hr::Selector::parse(selText)};
            if (!selector) {
                if (exc) *exc = make_error(ctx, selector.error());
                return JSValueMakeUndefined(ctx);
            }
            rewriter.on(std::move(*selector),
                        hr::ElementContentHandlers{
                            .element = js_handler<hr::Element>(tc, ctx, entry, "element", g_element_class),
                            .comments = js_handler<hr::Comment>(tc, ctx, entry, "comments", g_comment_class),
                            .text = js_handler<hr::TextChunk>(tc, ctx, entry, "text", g_text_class),
                        });
        }
    }

    // Expose tc to onEndTag / attribute iterators for the duration of the run.
    TransformCtx* prev{g_current_tc};
    g_current_tc = &tc;
    hr::TransformResult result{rewriter.transform(html)};
    g_current_tc = prev;

    // Release the onEndTag protections on every path (leak-tested by bun).
    for (JSObjectRef fn : tc.endTagFns) JSValueUnprotect(ctx, fn);
    ++(*tc.epoch);  // any iterator that escaped the run is now permanently done

    if (tc.exception) {
        if (exc) *exc = tc.exception;
        return JSValueMakeUndefined(ctx);
    }
    if (result.stopped) {
        if (exc) *exc = make_error(ctx, "HTMLRewriter: the rewriter was stopped");
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, result.output);
}

// validateSelector(sel): parse a CSS selector eagerly (bun rejects bad
// selectors at .on() time, not lazily at transform). Throws the parse error.
JSValueRef htmlrewriter_validate_selector(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                          std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    std::string sel{arg_string(ctx, argc, args, 0)};
    auto parsed{mbun::html_rewriter::Selector::parse(sel)};
    if (!parsed && exc) *exc = make_error(ctx, parsed.error());
    return JSValueMakeUndefined(ctx);
}

}  // namespace mbun_htmlrewriter

void install_htmlrewriter_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef n{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, n, "transform", mbun_htmlrewriter::htmlrewriter_transform);
    set_fn(ctx, n, "validateSelector", mbun_htmlrewriter::htmlrewriter_validate_selector);
    // Symbol.iterator installer for attribute iterators (C API can't address
    // well-known symbols; created once here, used by make_attributes_iterator).
    JSStringRef src{JSStringCreateWithUTF8CString(
        "(function(it){ it[Symbol.iterator] = function(){ return this; }; return it; })")};
    JSValueRef mk{JSEvaluateScript(ctx, src, nullptr, nullptr, 1, nullptr)};
    JSStringRelease(src);
    if (mk && JSValueIsObject(ctx, mk)) set_prop(ctx, n, "_mkIter", mk);
    set_prop(ctx, global, "__mbunHTMLRewriterNative", n);
}
