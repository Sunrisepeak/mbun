// Smoke test — verifies the toolchain builds and a binary runs.
// mcpp test discovers tests/test_*.cpp automatically (one binary per file).
import std;

int main() {
    std::println("test_smoke: ok");
    return 0;
}
