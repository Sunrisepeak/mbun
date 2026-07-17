// css_run — dev-only classification driver (NOT a test). Reads a batch of
// vectors on stdin, one record per line:  <mode>\t<src-b64>\t<expected-b64>
// where mode is "minify" or "css". Prints one result line per record:
//   PASS  or  FAIL\t<got-b64>
// base64 keeps embedded newlines/tabs intact across the pipe.
import std;
import mbun.css;

static std::string b64decode(std::string_view s) {
    static const std::string T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int lut[256];
    for (int i = 0; i < 256; i++) lut[i] = -1;
    for (int i = 0; i < 64; i++) lut[(unsigned char)T[i]] = i;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=' || lut[c] == -1) continue;
        val = (val << 6) + lut[c];
        bits += 6;
        if (bits >= 0) {
            out += char((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

static std::string b64encode(std::string_view s) {
    static const std::string T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : s) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += T[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += T[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

static std::string strip_ws(std::string_view s) {
    std::string o;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') o += c;
    return o;
}

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto t1 = line.find('\t');
        auto t2 = line.find('\t', t1 + 1);
        std::string mode = line.substr(0, t1);
        std::string src = b64decode(line.substr(t1 + 1, t2 - t1 - 1));
        std::string exp = b64decode(line.substr(t2 + 1));
        bool minify = (mode == "minify");
        std::string got = mbun::css::transform(src, minify);
        bool ok = minify ? (got == exp) : (strip_ws(got) == strip_ws(exp));
        if (ok)
            std::println("PASS");
        else
            std::println("FAIL\t{}", b64encode(got));
    }
    return 0;
}
