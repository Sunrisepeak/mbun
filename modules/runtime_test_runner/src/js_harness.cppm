// JS binding inventory and prelude seam, based on bun's test_runner
// collection/execution modules. JSC installation remains an adapter concern.
export module mbun.runtime_test_runner.js_harness;

import std;

export namespace mbun::runtime_test_runner {

enum class BindingKind : std::uint8_t {
    test,
    it,
    describe,
    hook,
    expect,
    timer,
};

struct Binding {
    std::string_view name {};
    BindingKind kind { BindingKind::test };
};

inline std::vector<Binding> default_bindings() {
    return {
        { "test", BindingKind::test },       { "it", BindingKind::it },
        { "describe", BindingKind::describe }, { "beforeEach", BindingKind::hook },
        { "afterEach", BindingKind::hook },   { "beforeAll", BindingKind::hook },
        { "afterAll", BindingKind::hook },    { "expect", BindingKind::expect },
        { "setTimeout", BindingKind::timer }, { "clearTimeout", BindingKind::timer },
    };
}

inline std::string_view harness_prelude() noexcept {
    return R"JS((function(global) {
  const state = { tests: [], hooks: { beforeEach: [], afterEach: [], beforeAll: [], afterAll: [] } };
  const add = (name, fn) => state.tests.push({ name, fn });
  const test = (name, fn) => add(name, fn);
  test.skip = () => {};
  test.todo = () => {};
  const describe = (_name, fn) => fn();
  const hook = name => fn => state.hooks[name].push(fn);
  const expect = value => global.__mbunRuntimeTest.expect(value);
  global.__mbunRuntimeTest = { state, test, it: test, describe, expect,
    beforeEach: hook("beforeEach"), afterEach: hook("afterEach"),
    beforeAll: hook("beforeAll"), afterAll: hook("afterAll") };
})(globalThis);)JS";
}

}  // namespace mbun::runtime_test_runner
