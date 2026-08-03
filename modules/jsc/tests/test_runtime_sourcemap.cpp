import std;
import mbun.jsc.runtime;

int main() {
    auto result{mbun::jsc::runtime::eval_to_string(R"JS(
(() => {
  const api = globalThis.__mbunNativeModules["bun:internal-for-testing"].internalSourceMap;
  const blob = api.fromVLQ("AAAA,KAAI;ACCA");
  const found = api.find(blob, 0, 7);
  const invalidValues = [NaN, Infinity, -Infinity, 1.5, 2147483648, -2147483649];
  const invalidArgs = invalidValues.every(value => {
    for (const args of [[value, 0], [0, value]]) {
      try {
        api.find(blob, ...args);
        return false;
      } catch {}
    }
    return true;
  });
  return JSON.stringify({
    isBytes: blob instanceof Uint8Array,
    mappings: api.toVLQ(blob),
    found,
    before: api.find(blob, 0, -1),
    invalidArgs,
  });
})()
)JS")};
    constexpr std::string_view expected{
        R"JSON({"isBytes":true,"mappings":"AAAA,KAAI;ACCA","found":{"generatedLine":0,"generatedColumn":5,"originalLine":0,"originalColumn":4,"sourceIndex":0},"before":null,"invalidArgs":true})JSON"};
    if (!result || *result != expected) {
        std::println("runtime sourcemap mismatch: {}", result ? *result : result.error());
        return 1;
    }
    std::println("runtime sourcemap: ok");
    return 0;
}
