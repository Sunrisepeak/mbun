// headers.cppm — mbun.http.headers: the Fetch `Headers` map (pure-string core).
//
// Ported (re-implemented) from bun's real source:
//   ref: bun src/picohttp/lib.rs Header::List::get (case-insensitive lookup)
//   ref: bun src/jsc/bindings/webcore/* Headers / lowercaseHeaderName kernel —
//        name lowercasing, comma-combining, set-cookie separation, sorted
//        normalized iteration.
//   Behavior pinned by test/js/web/fetch/headers.test.ts (see tests).
//
// MC++ notes: names are stored ASCII-lowercased once; the map keeps insertion
// order with one entry per distinct name (set-cookie may repeat). This is the
// pure-string contract — JS-value coercion (Number/BigInt/object toString),
// the >0xFF iso-8859-1 rejection, and record [[Get]] interleaving live in the
// JSC binding layer and are DEFERRED(S1).
export module mbun.http.headers;

import std;
import mbun.http.message;  // is_valid_field_name / is_token_char

namespace mbun::http {

// ASCII-only lowercase: fold 'A'..'Z'; every other byte (digits, punctuation,
// bytes >= 0x80) is left intact. ref: WebCore::lowercaseHeaderName scalar spec.
export std::string lowercase_ascii(std::string_view s) {
    std::string out{};
    out.reserve(s.size());
    for (char c : s) {
        unsigned char u{static_cast<unsigned char>(c)};
        if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(u + 0x20));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Non-exported, external linkage (referenced by Headers' inline members, so
// anonymous-namespace internal linkage must be avoided).
constexpr bool is_http_ws(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}
// Fetch "normalize": strip leading/trailing HTTP whitespace.
inline std::string_view normalize_value(std::string_view v) {
    std::size_t start{0};
    std::size_t end{v.size()};
    while (start < end && is_http_ws(v[start])) {
        ++start;
    }
    while (end > start && is_http_ws(v[end - 1])) {
        --end;
    }
    return v.substr(start, end - start);
}
inline constexpr std::string_view SET_COOKIE{"set-cookie"};

export class Headers {
private:
    struct Entry {
        std::string name;   // already ASCII-lowercased
        std::string value;
    };
    std::vector<Entry> entries_{};

    // Value is a valid header value iff it contains no NUL / CR / LF.
    static bool valid_value_(std::string_view v) {
        return v.find('\0') == std::string_view::npos && v.find('\n') == std::string_view::npos &&
               v.find('\r') == std::string_view::npos;
    }

public:
    Headers() = default;

    // Append: combine with an existing same-name value via ", " (set-cookie is
    // kept as a separate entry). Returns false if name/value is invalid
    // (the JS layer maps that to a thrown TypeError).
    bool append(std::string_view name, std::string_view value) {
        if (!is_valid_field_name(name)) {
            return false;
        }
        std::string_view v{normalize_value(value)};
        if (!valid_value_(v)) {
            return false;
        }
        std::string lname{lowercase_ascii(name)};
        if (lname != SET_COOKIE) {
            for (auto& e : entries_) {
                if (e.name == lname) {
                    e.value.append(", ");
                    e.value.append(v);
                    return true;
                }
            }
        }
        entries_.push_back({std::move(lname), std::string{v}});
        return true;
    }

    // Set: replace every same-name entry with a single value.
    bool set(std::string_view name, std::string_view value) {
        if (!is_valid_field_name(name)) {
            return false;
        }
        std::string_view v{normalize_value(value)};
        if (!valid_value_(v)) {
            return false;
        }
        std::string lname{lowercase_ascii(name)};
        bool placed{false};
        std::vector<Entry> next{};
        next.reserve(entries_.size() + 1);
        for (auto& e : entries_) {
            if (e.name == lname) {
                if (!placed) {
                    next.push_back({lname, std::string{v}});
                    placed = true;
                }
                continue;  // drop other same-name entries
            }
            next.push_back(std::move(e));
        }
        if (!placed) {
            next.push_back({std::move(lname), std::string{v}});
        }
        entries_ = std::move(next);
        return true;
    }

    void remove(std::string_view name) {
        std::string lname{lowercase_ascii(name)};
        std::erase_if(entries_, [&](const Entry& e) { return e.name == lname; });
    }

    bool has(std::string_view name) const {
        std::string lname{lowercase_ascii(name)};
        for (const auto& e : entries_) {
            if (e.name == lname) {
                return true;
            }
        }
        return false;
    }

    // get(): combined value ("v1, v2"); all set-cookie values joined by ", ".
    // nullopt if the name is absent.
    std::optional<std::string> get(std::string_view name) const {
        std::string lname{lowercase_ascii(name)};
        if (lname == SET_COOKIE) {
            std::string out{};
            bool any{false};
            for (const auto& e : entries_) {
                if (e.name == lname) {
                    if (any) {
                        out.append(", ");
                    }
                    out.append(e.value);
                    any = true;
                }
            }
            if (!any) {
                return std::nullopt;
            }
            return out;
        }
        for (const auto& e : entries_) {
            if (e.name == lname) {
                return e.value;  // already comma-combined on append
            }
        }
        return std::nullopt;
    }

    // Each individual Set-Cookie value, in insertion order.
    std::vector<std::string> get_set_cookie() const {
        std::vector<std::string> out{};
        for (const auto& e : entries_) {
            if (e.name == SET_COOKIE) {
                out.push_back(e.value);
            }
        }
        return out;
    }

    // Distinct header-name count (combined names count once). ref: Headers.count.
    std::size_t count() const {
        std::vector<std::string_view> seen{};
        for (const auto& e : entries_) {
            bool found{false};
            for (auto s : seen) {
                if (s == e.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                seen.push_back(e.name);
            }
        }
        return seen.size();
    }

    // Sorted, normalized (name, value) pairs — the Headers iterator order.
    // Names are sorted ascending; same-name values are already combined, so
    // there is one pair per distinct name (set-cookie values combined too).
    std::vector<std::pair<std::string, std::string>> entries() const {
        // Collect distinct names in first-seen order, combining set-cookie.
        std::vector<std::pair<std::string, std::string>> out{};
        for (const auto& e : entries_) {
            bool merged{false};
            for (auto& p : out) {
                if (p.first == e.name) {
                    p.second.append(", ");
                    p.second.append(e.value);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                out.push_back({e.name, e.value});
            }
        }
        std::ranges::sort(out, [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    }

    std::vector<std::string> keys() const {
        std::vector<std::string> out{};
        for (auto& p : entries()) {
            out.push_back(p.first);
        }
        return out;
    }

    std::vector<std::string> values() const {
        std::vector<std::string> out{};
        for (auto& p : entries()) {
            out.push_back(p.second);
        }
        return out;
    }

    bool empty() const {
        return entries_.empty();
    }
};

}  // namespace mbun::http
