// test_core_collections.cpp — T1.3 mbun.core.collections behavior tests.
//
// Sources (bun's original implementation/tests; assertion semantics preserved):
//   - src/collections/bit_set.zig > testBitSet/testPureBitSet helpers
//   - src/collections/bit_set.rs (Integer/Array/Dynamic bit sets)
//   - src/css/small_list.zig (servo/rust-smallvec-derived SmallList)
//   - src/collections/lib.rs > SmallList<T, N>
//   - src/install_types/SemverString.zig > String.Builder/StringPool
//   - src/semver/lib.rs > string::Builder/StringPool (hash-collision fallback)
//
// The bun bit-set helpers are library-level Zig tests rather than bun:test JS.
// They are translated here as C++ vectors because T1.3 is an internal pure-
// logic module with no JavaScript API surface. StringPool and SmallVec tests
// pin their load-bearing storage/lifetime invariants, including boundaries not
// observable from JavaScript.
import std;
import mbun.core.collections;

namespace collections = mbun::core::collections;

namespace {

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{60};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void check(bool condition, std::string what) {
    ++gChecks;
    if (!condition) {
        report_failure(what);
    }
}

template <class A, class B>
void check_eq(const A& actual, const B& expected, std::string what) {
    ++gChecks;
    if (!(actual == expected)) {
        report_failure(std::format("{}: actual != expected", what));
    }
}

template <class Iterator>
std::vector<std::size_t> collect_indices(Iterator iterator) {
    std::vector<std::size_t> result;
    while (auto index{iterator.next()}) {
        result.push_back(*index);
    }
    return result;
}

std::vector<std::size_t> expected_indices(std::size_t length, bool setBits, bool forward,
                                          auto&& predicate) {
    std::vector<std::size_t> result;
    for (std::size_t i{0}; i < length; ++i) {
        if (predicate(i) == setBits) {
            result.push_back(i);
        }
    }
    if (!forward) {
        std::ranges::reverse(result);
    }
    return result;
}

template <class Set>
void test_pure_bit_set(const Set& prototype, std::size_t length, std::string_view label) {
    // Exact assertion matrix from bun:
    // src/collections/bit_set.zig > testPureBitSet/testSubsetOf/testSupersetOf.
    Set empty{prototype};
    empty.clear();
    Set full{prototype};
    full.set_all(true);
    Set even{prototype};
    even.clear();
    Set odd{prototype};
    odd.clear();
    for (std::size_t i{0}; i < length; ++i) {
        even.set_value(i, (i & 1U) == 0);
        odd.set_value(i, (i & 1U) == 1);
    }

    check(empty.subset_of(empty), std::format("{} empty subset empty", label));
    check(empty.subset_of(full), std::format("{} empty subset full", label));
    check(full.subset_of(full), std::format("{} full subset full", label));
    if (length == 0) {
        check(even.subset_of(odd), std::format("{} zero even subset odd", label));
        check(odd.subset_of(even), std::format("{} zero odd subset even", label));
    } else if (length == 1) {
        check(!even.subset_of(odd), std::format("{} one-bit even not subset odd", label));
        check(odd.subset_of(even), std::format("{} one-bit odd subset even", label));
    } else {
        check(!even.subset_of(odd), std::format("{} even not subset odd", label));
        check(!odd.subset_of(even), std::format("{} odd not subset even", label));
    }

    check(full.superset_of(full), std::format("{} full superset full", label));
    check(full.superset_of(empty), std::format("{} full superset empty", label));
    check(empty.superset_of(empty), std::format("{} empty superset empty", label));
    if (length == 0) {
        check(even.superset_of(odd), std::format("{} zero even superset odd", label));
        check(odd.superset_of(even), std::format("{} zero odd superset even", label));
    } else if (length == 1) {
        check(even.superset_of(odd), std::format("{} one-bit even superset odd", label));
        check(!odd.superset_of(even), std::format("{} one-bit odd not superset even", label));
    } else {
        check(!even.superset_of(odd), std::format("{} even not superset odd", label));
        check(!odd.superset_of(even), std::format("{} odd not superset even", label));
    }

    check(empty.complement().eql(full), std::format("{} complement empty", label));
    check(full.complement().eql(empty), std::format("{} complement full", label));
    check(even.complement().eql(odd), std::format("{} complement even", label));
    check(odd.complement().eql(even), std::format("{} complement odd", label));

    check(empty.union_with(empty).eql(empty), std::format("{} union empty empty", label));
    check(empty.union_with(full).eql(full), std::format("{} union empty full", label));
    check(full.union_with(full).eql(full), std::format("{} union full full", label));
    check(full.union_with(empty).eql(full), std::format("{} union full empty", label));
    check(even.union_with(odd).eql(full), std::format("{} union even odd", label));
    check(odd.union_with(even).eql(full), std::format("{} union odd even", label));

    check(empty.intersect_with(empty).eql(empty),
          std::format("{} intersection empty empty", label));
    check(empty.intersect_with(full).eql(empty),
          std::format("{} intersection empty full", label));
    check(full.intersect_with(full).eql(full),
          std::format("{} intersection full full", label));
    check(full.intersect_with(empty).eql(empty),
          std::format("{} intersection full empty", label));
    check(even.intersect_with(odd).eql(empty),
          std::format("{} intersection even odd", label));
    check(odd.intersect_with(even).eql(empty),
          std::format("{} intersection odd even", label));

    check(empty.xor_with(empty).eql(empty), std::format("{} xor empty empty", label));
    check(empty.xor_with(full).eql(full), std::format("{} xor empty full", label));
    check(full.xor_with(full).eql(empty), std::format("{} xor full full", label));
    check(full.xor_with(empty).eql(full), std::format("{} xor full empty", label));
    check(even.xor_with(odd).eql(full), std::format("{} xor even odd", label));
    check(odd.xor_with(even).eql(full), std::format("{} xor odd even", label));

    check(empty.difference_with(empty).eql(empty),
          std::format("{} difference empty empty", label));
    check(empty.difference_with(full).eql(empty),
          std::format("{} difference empty full", label));
    check(full.difference_with(full).eql(empty),
          std::format("{} difference full full", label));
    check(full.difference_with(empty).eql(full),
          std::format("{} difference full empty", label));
    check(full.difference_with(odd).eql(even),
          std::format("{} difference full odd", label));
    check(full.difference_with(even).eql(odd),
          std::format("{} difference full even", label));
}

template <class Set>
void exercise_bit_set(Set& a, Set& b, std::size_t length, std::string_view label) {
    // source: src/collections/bit_set.zig > testBitSet
    check_eq(a.capacity(), length, std::format("{} capacity(a)", label));
    check_eq(b.capacity(), length, std::format("{} capacity(b)", label));

    for (std::size_t i{0}; i < length; ++i) {
        a.set_value(i, (i & 1U) == 0);
        b.set_value(i, (i & 2U) == 0);
    }
    check_eq(a.count(), (length + 1) / 2, std::format("{} even count", label));
    check_eq(b.count(), (length + 3) / 4 + (length + 2) / 4,
             std::format("{} two-on/two-off count", label));

    check_eq(collect_indices(a.template iterator<true, true>()),
             expected_indices(length, true, true, [](std::size_t i) { return (i & 1U) == 0; }),
             std::format("{} set forward iterator", label));
    check_eq(collect_indices(a.template iterator<false, true>()),
             expected_indices(length, false, true, [](std::size_t i) { return (i & 1U) == 0; }),
             std::format("{} unset forward iterator", label));
    check_eq(collect_indices(a.template iterator<true, false>()),
             expected_indices(length, true, false, [](std::size_t i) { return (i & 1U) == 0; }),
             std::format("{} set reverse iterator", label));
    check_eq(collect_indices(a.template iterator<false, false>()),
             expected_indices(length, false, false, [](std::size_t i) { return (i & 1U) == 0; }),
             std::format("{} unset reverse iterator", label));

    // Exact forward iterator exhaustion sequence from bun's testBitSet.
    auto initialSetIterator{a.template iterator<true, true>()};
    for (std::size_t i{0}; i < length; i += 2) {
        check_eq(initialSetIterator.next(), std::optional<std::size_t>{i},
                 std::format("{} initial iterator {}", label, i));
    }
    check(!initialSetIterator.next().has_value(),
          std::format("{} initial iterator exhausted once", label));
    check(!initialSetIterator.next().has_value(),
          std::format("{} initial iterator exhausted twice", label));
    check(!initialSetIterator.next().has_value(),
          std::format("{} initial iterator exhausted thrice", label));

    a.toggle_all();
    for (std::size_t i{0}; i < length; ++i) {
        check(a.is_set(i) == ((i & 1U) != 0), std::format("{} toggle_all bit {}", label, i));
    }

    // Exact source assertions: bun bit_set.zig > testBitSet, the set-forward
    // iterator after toggleAll yields every odd index, then stays exhausted.
    auto oddSetIterator{a.template iterator<true, true>()};
    for (std::size_t i{1}; i < length; i += 2) {
        check_eq(oddSetIterator.next(), std::optional<std::size_t>{i},
                 std::format("{} toggled odd iterator {}", label, i));
    }
    for (int exhaustion{1}; exhaustion <= 3; ++exhaustion) {
        check(!oddSetIterator.next().has_value(),
              std::format("{} toggled odd exhaustion {}", label, exhaustion));
    }

    // Exact source assertions: b's two-on/two-off pattern has unset indices
    // 2,3,6,7,...; its forward unset iterator also remains exhausted.
    auto twoOnTwoOffUnset{b.template iterator<false, true>()};
    for (std::size_t i{2}; i < length; i += 4) {
        check_eq(twoOnTwoOffUnset.next(), std::optional<std::size_t>{i},
                 std::format("{} b unset iterator {}", label, i));
        if (i + 1 < length) {
            check_eq(twoOnTwoOffUnset.next(), std::optional<std::size_t>{i + 1},
                     std::format("{} b unset iterator {}", label, i + 1));
        }
    }
    for (int exhaustion{1}; exhaustion <= 3; ++exhaustion) {
        check(!twoOnTwoOffUnset.next().has_value(),
              std::format("{} b unset exhaustion {}", label, exhaustion));
    }

    a.set_union(b);
    for (std::size_t i{0}; i < length; ++i) {
        check(a.is_set(i) == (((i & 1U) != 0) || ((i & 2U) == 0)),
              std::format("{} union bit {}", label, i));
    }
    auto reverseSet{a.template iterator<true, false>()};
    auto reverseUnset{a.template iterator<false, false>()};
    for (std::size_t i{length}; i > 0;) {
        --i;
        if ((i & 1U) != 0 || (i & 2U) == 0) {
            check_eq(reverseSet.next(), std::optional<std::size_t>{i},
                     std::format("{} reverse set iterator {}", label, i));
        } else {
            check_eq(reverseUnset.next(), std::optional<std::size_t>{i},
                     std::format("{} reverse unset iterator {}", label, i));
        }
    }
    for (int exhaustion{1}; exhaustion <= 3; ++exhaustion) {
        check(!reverseSet.next().has_value(),
              std::format("{} reverse set exhaustion {}", label, exhaustion));
        check(!reverseUnset.next().has_value(),
              std::format("{} reverse unset exhaustion {}", label, exhaustion));
    }
    check(a.has_intersection(b) == (length > 0), std::format("{} intersection exists", label));

    a.toggle_set(b);
    check_eq(a.count(), length / 4, std::format("{} xor count", label));
    for (std::size_t i{0}; i < length; ++i) {
        check(a.is_set(i) == (((i & 1U) != 0) && ((i & 2U) != 0)),
              std::format("{} xor bit {}", label, i));
        if ((i & 1U) == 0) {
            a.set(i);
        } else {
            a.unset(i);
        }
    }

    a.set_intersection(b);
    check_eq(a.count(), (length + 3) / 4, std::format("{} intersection count", label));
    a.toggle_set(a);
    check_eq(a.count(), std::size_t{0}, std::format("{} self xor empty", label));
    auto emptyForward{a.template iterator<true, true>()};
    auto emptyReverse{a.template iterator<true, false>()};
    for (int exhaustion{1}; exhaustion <= 3; ++exhaustion) {
        check(!emptyForward.next().has_value(),
              std::format("{} empty forward exhaustion {}", label, exhaustion));
        check(!emptyReverse.next().has_value(),
              std::format("{} empty reverse exhaustion {}", label, exhaustion));
    }

    constexpr std::array<std::size_t, 22> TEST_BITS{0,  1,  2,   3,   4,   5,   6,  7,
                                                    9,  10, 11,  22,  31,  32,  63, 64,
                                                    66, 95, 127, 160, 192, 1000};
    for (std::size_t i : TEST_BITS) {
        if (i < length) {
            a.set(i);
        }
    }
    for (std::size_t i : TEST_BITS) {
        if (i < length) {
            check_eq(a.find_first_set(), std::optional<std::size_t>{i},
                     std::format("{} find first {}", label, i));
            check_eq(a.toggle_first_set(), std::optional<std::size_t>{i},
                     std::format("{} toggle first {}", label, i));
        }
    }
    check(!a.find_first_set().has_value(), std::format("{} no first set", label));
    check(!a.toggle_first_set().has_value(), std::format("{} no toggle first", label));

    a.set_range_value(0, length, true);
    check_eq(a.count(), length, std::format("{} full range true", label));
    a.set_range_value(0, length, false);
    check_eq(a.count(), std::size_t{0}, std::format("{} full range false", label));
    a.set_range_value(0, 0, true);
    a.set_range_value(length, length, true);
    check_eq(a.count(), std::size_t{0}, std::format("{} empty ranges", label));

    if (length > 0) {
        a.set_range_value(0, 1, true);
        check(a.is_set(0), std::format("{} first singleton", label));
        check_eq(a.count(), std::size_t{1}, std::format("{} first singleton count", label));

        a.set_all(false);
        a.set_range_value(0, length - 1, true);
        check_eq(a.count(), length - 1, std::format("{} prefix excluding last count", label));
        check(!a.is_set(length - 1), std::format("{} prefix excludes last", label));

        a.set_all(false);
        a.set_range_value(1, length, true);
        check_eq(a.count(), length - 1, std::format("{} suffix excluding first count", label));
        check(!a.is_set(0), std::format("{} suffix excludes first", label));

        a.set_all(false);
        a.set_range_value(length - 1, length, true);
        check(a.is_set(length - 1), std::format("{} last singleton", label));
        check_eq(a.count(), std::size_t{1}, std::format("{} last singleton count", label));
    }
    if (length >= 4) {
        a.set_all(false);
        a.set_range_value(1, length - 2, true);
        check_eq(a.count(), length - 3, std::format("{} interior range count", label));
        check(!a.is_set(0) && a.is_set(1) && a.is_set(length - 3) && !a.is_set(length - 2) &&
                  !a.is_set(length - 1),
              std::format("{} interior range boundaries", label));
    }

    Set empty{a};
    empty.clear();
    Set full{a};
    full.set_all(true);
    check(empty.subset_of(full), std::format("{} empty subset full", label));
    check(full.superset_of(empty), std::format("{} full superset empty", label));
    check(full.complement().eql(empty), std::format("{} full complement empty", label));
    check(full.difference_with(full).eql(empty), std::format("{} self difference empty", label));
    check(full.union_with(empty).eql(full), std::format("{} union value op", label));
    check(full.intersect_with(empty).eql(empty), std::format("{} intersect value op", label));
    test_pure_bit_set(a, length, label);
}

template <std::size_t Size>
void test_static_bit_set_size() {
    collections::StaticBitSet<Size> a{};
    collections::StaticBitSet<Size> b{collections::StaticBitSet<Size>::init_full()};
    exercise_bit_set(a, b, Size, std::format("StaticBitSet<{}>", Size));
}

void test_static_bit_sets() {
    test_static_bit_set_size<0>();
    test_static_bit_set_size<1>();
    test_static_bit_set_size<63>();
    test_static_bit_set_size<64>();
    test_static_bit_set_size<65>();
    test_static_bit_set_size<127>();
    test_static_bit_set_size<128>();
    test_static_bit_set_size<129>();
    test_static_bit_set_size<1001>();

    collections::StaticBitSet<130> bits{};
    bits.set_range_value(60, 70, true);
    check_eq(bits.count(), std::size_t{10}, "static range crosses 64-bit word");
    for (std::size_t i{0}; i < 130; ++i) {
        check(bits.is_set(i) == (i >= 60 && i < 70), std::format("static cross-word bit {}", i));
    }
}

void test_dynamic_bit_sets() {
    constexpr std::array<std::size_t, 9> LENGTHS{0, 1, 63, 64, 65, 127, 128, 129, 1001};
    for (std::size_t length : LENGTHS) {
        collections::DynamicBitSet a{length};
        collections::DynamicBitSet b{length, true};
        exercise_bit_set(a, b, length, std::format("DynamicBitSet({})", length));
    }

    // source: DynamicBitSet.resize — new bits use fill; truncated padding is
    // cleared so it cannot reappear after a later grow.
    collections::DynamicBitSet bits{63, true};
    bits.resize(130, true);
    check_eq(bits.count(), std::size_t{130}, "dynamic grow true across two words");
    bits.resize(65, false);
    check_eq(bits.count(), std::size_t{65}, "dynamic shrink preserves prefix");
    bits.unset(64);
    bits.resize(129, false);
    check_eq(bits.count(), std::size_t{64}, "dynamic regrow false clears padding/new words");
    check(!bits.is_set(64) && !bits.is_set(65) && !bits.is_set(128),
          "dynamic regrow boundary bits remain false");
    bits.resize(64, false);
    bits.resize(65, true);
    check(bits.is_set(64), "dynamic same-word grow true restores only new bit");

    collections::DynamicBitSet original{130};
    original.set(0);
    original.set(64);
    original.set(129);
    collections::DynamicBitSet copy{original};
    original.clear();
    check_eq(copy.count(), std::size_t{3}, "dynamic deep copy owns masks");
    collections::DynamicBitSet moved{std::move(copy)};
    check_eq(moved.count(), std::size_t{3}, "dynamic move preserves masks");
}

template <class T>
concept HasAutoValueAlgebra = requires(const T& left, const T& right) {
    left.union_with(right);
    left.intersect_with(right);
    left.xor_with(right);
    left.difference_with(right);
};

template <class T>
concept HasAutoSubsetAlgebra = requires(const T& left, const T& right) {
    left.subset_of(right);
    left.superset_of(right);
};

template <class T>
concept HasAutoMutatingAlgebra = requires(T& left, const T& right) {
    left.toggle_set(right);
    left.set_union(right);
    left.set_intersection(right);
    left.set_exclude(right);
};

// AutoBitSet is a deliberately narrow tagged wrapper in bun. The pure set
// algebra belongs to StaticBitSet/DynamicBitSet and must not leak into Auto.
static_assert(!HasAutoValueAlgebra<collections::AutoBitSet>);
static_assert(!HasAutoSubsetAlgebra<collections::AutoBitSet>);
static_assert(!HasAutoMutatingAlgebra<collections::AutoBitSet>);

void test_auto_bit_sets() {
    // Exact public contract from bun's Rust/Zig AutoBitSet implementations.
    // Requests <=127 select the complete StaticBitSet<127> arm.
    for (std::size_t request : std::array<std::size_t, 4>{0, 1, 126, 127}) {
        collections::AutoBitSet bits{request};
        check(!collections::AutoBitSet::needs_dynamic(request),
              std::format("AutoBitSet({}) static selector", request));
        check_eq(bits.count(), std::size_t{0}, std::format("AutoBitSet({}) empty", request));
        bits.set(126);
        check(bits.is_set(126), std::format("AutoBitSet({}) exposes static bit 126", request));
        bits.unset(126);
        bits.set_all(true);
        check_eq(bits.count(), std::size_t{127},
                 std::format("AutoBitSet({}) fixed static set_all", request));
    }

    for (std::size_t length : std::array<std::size_t, 3>{128, 129, 257}) {
        collections::AutoBitSet bits{length};
        check(collections::AutoBitSet::needs_dynamic(length),
              std::format("AutoBitSet({}) dynamic selector", length));
        bits.set(length - 1);
        check(bits.is_set(length - 1), std::format("AutoBitSet({}) last bit", length));
        check_eq(bits.find_first_set(), std::optional<std::size_t>{length - 1},
                 std::format("AutoBitSet({}) find last", length));
        auto iterator{bits.iterator<true, true>()};
        check_eq(iterator.next(), std::optional<std::size_t>{length - 1},
                 std::format("AutoBitSet({}) iterate last", length));
        check(!iterator.next().has_value(), std::format("AutoBitSet({}) iterator end", length));
    }

    collections::AutoBitSet zeroRequest{0};
    collections::AutoBitSet maxStatic{127};
    check(zeroRequest.eql(maxStatic), "auto static requests share fixed empty raw bytes");

    // source: AutoBitSet::eql compares raw_bytes(); has_intersection dispatches
    // only when the tagged arms match. Cover 0/127/128/129 explicitly.
    collections::AutoBitSet dynamic128{128};
    check(maxStatic.eql(dynamic128), "auto static-127 and dynamic-128 empty raw bytes equal");
    maxStatic.set(0);
    dynamic128.set(0);
    check(maxStatic.eql(dynamic128), "auto static-127 and dynamic-128 matching raw bytes equal");
    check(!maxStatic.has_intersection(dynamic128),
          "auto mixed static/dynamic intersection stays false");
    check(!dynamic128.has_intersection(maxStatic),
          "auto mixed dynamic/static intersection stays false");

    collections::AutoBitSet dynamic129{129};
    check(!dynamic128.eql(dynamic129) && !dynamic129.eql(dynamic128),
          "auto unequal raw byte lengths compare false both directions");
    check(!dynamic128.has_intersection(dynamic129),
          "auto short/long empty dynamic intersection false");
    check(!dynamic129.has_intersection(dynamic128),
          "auto long/short empty dynamic intersection false");

    dynamic129.set(0);
    check(dynamic128.has_intersection(dynamic129),
          "auto short/long common-word intersection true");
    check(dynamic129.has_intersection(dynamic128),
          "auto long/short common-word intersection true");
    dynamic128.unset(0);
    dynamic129.set(128);
    check(!dynamic128.has_intersection(dynamic129),
          "auto short/long ignores extra set bit safely");
    check(!dynamic129.has_intersection(dynamic128),
          "auto long/short ignores extra set bit safely");

    collections::AutoBitSet dynamic190{190};
    dynamic190.set(0);
    dynamic190.set(128);
    check(dynamic129.eql(dynamic190),
          "auto equal raw word counts ignore exact dynamic bit length");

    collections::AutoBitSet inlineCopy{maxStatic};
    check_eq(inlineCopy.count(), std::size_t{1}, "auto inline deep copy");

    collections::AutoBitSet heapBits{129};
    heapBits.set(0);
    heapBits.set(128);
    collections::AutoBitSet moved{std::move(heapBits)};
    check_eq(moved.count(), std::size_t{2}, "auto dynamic move preserves words");
    check_eq(heapBits.count(), std::size_t{0}, "auto moved-from state is valid empty static arm");
    check_eq(sizeof(collections::AutoBitSet), 3 * sizeof(std::size_t),
             "auto bitset remains three machine words");
}

void test_string_pool() {
    // source: SemverString Builder StringPool. Unlike the Zig hash-only map,
    // the Rust rewrite compares bytes on an existing hash; the C++ pool must
    // therefore key by complete byte strings and be collision-safe.
    collections::StringPool pool;
    auto emptyResult{pool.intern("")};
    check(emptyResult.has_value(), "string pool interns empty string");
    auto alphaResult{pool.intern("alpha")};
    auto alphaAgain{pool.intern(std::string_view{"alpha"})};
    check(alphaResult.has_value() && alphaAgain.has_value(), "string pool interns alpha");
    check_eq(*alphaResult, *alphaAgain, "string pool duplicate keeps id");
    check_eq(pool.size(), std::size_t{2}, "string pool duplicate does not grow");
    check_eq(pool.get(*emptyResult), std::string_view{}, "string pool reads empty string");
    check_eq(pool.get(*alphaResult), std::string_view{"alpha"}, "string pool reads alpha");

    const std::string withNul{"ab\0cd", 5};
    auto nulResult{pool.intern(withNul)};
    check(nulResult.has_value(), "string pool interns embedded NUL");
    check_eq(pool.get(*nulResult), std::string_view{withNul}, "string pool preserves embedded NUL");
    check(pool.find(withNul) == nulResult, "string pool finds embedded NUL");
    check(!pool.find("missing").has_value(), "string pool missing lookup");

    const auto alphaId{*alphaResult};
    const std::string_view alphaView{pool.get(alphaId)};
    const char* alphaData{alphaView.data()};
    std::vector<collections::StringId> ids;
    ids.reserve(3000);
    for (int i{0}; i < 3000; ++i) {
        std::string value{std::format("value-{:04}-{}", i, std::string(48 + (i % 31), 'x'))};
        auto result{pool.intern(value)};
        check(result.has_value(), std::format("string pool bulk intern {}", i));
        if (result) {
            ids.push_back(*result);
            check_eq(pool.get(*result), std::string_view{value},
                     std::format("string pool bulk round-trip {}", i));
        }
    }
    check_eq(pool.get(alphaId), std::string_view{"alpha"}, "string pool old id survives growth");
    check(pool.get(alphaId).data() == alphaData, "string pool view address stable across growth");
    check_eq(alphaView, std::string_view{"alpha"}, "held string_view survives growth");

    collections::StringPool moved{std::move(pool)};
    check_eq(moved.get(alphaId), std::string_view{"alpha"}, "string pool move preserves ids");
    check(moved.get(alphaId).data() == alphaData, "string pool move preserves view addresses");
    check(moved.bytes_size() >= 3000U * 48U, "string pool reports stored bytes");
    check(pool.empty() && pool.bytes_size() == 0, "string pool move resets source accounting");

    collections::StringPool assigned;
    [[maybe_unused]] auto oldValue{assigned.intern("old-destination-value")};
    assigned = std::move(moved);
    check_eq(assigned.get(alphaId), std::string_view{"alpha"},
             "string pool move assignment replaces populated destination");
    check(assigned.get(alphaId).data() == alphaData,
          "string pool move assignment preserves view addresses");
    check(moved.empty() && moved.bytes_size() == 0,
          "string pool move assignment resets source accounting");

    auto overflow{assigned.reserve(std::numeric_limits<std::size_t>::max())};
    check(!overflow.has_value() &&
              overflow.error() == collections::CollectionError::capacity_overflow,
          "string pool reserve reports capacity overflow");
    check_eq(assigned.size(), ids.size() + 3,
             "string pool failed reserve leaves entry count unchanged");
    check_eq(assigned.get(alphaId), std::string_view{"alpha"},
             "string pool failed reserve leaves stable entries intact");
}

struct LifetimeProbe {
    inline static int live{0};
    inline static int constructed{0};
    inline static int destroyed{0};

    int value{0};

    explicit LifetimeProbe(int valueIn = 0) : value{valueIn} {
        ++live;
        ++constructed;
    }
    LifetimeProbe(const LifetimeProbe& other) : value{other.value} {
        ++live;
        ++constructed;
    }
    LifetimeProbe(LifetimeProbe&& other) noexcept : value{other.value} {
        other.value = -1;
        ++live;
        ++constructed;
    }
    LifetimeProbe& operator=(const LifetimeProbe&) = default;
    LifetimeProbe& operator=(LifetimeProbe&&) = default;
    ~LifetimeProbe() {
        --live;
        ++destroyed;
    }
    friend bool operator==(const LifetimeProbe&, const LifetimeProbe&) = default;
};

struct alignas(128) OverAligned {
    std::uint64_t value{0};
};

struct ThrowingMoveOnly {
    inline static int moveAttempts{0};
    int value{0};

    explicit ThrowingMoveOnly(int valueIn) : value{valueIn} {}
    ThrowingMoveOnly(const ThrowingMoveOnly&) = delete;
    ThrowingMoveOnly& operator=(const ThrowingMoveOnly&) = delete;
    ThrowingMoveOnly(ThrowingMoveOnly&& other) : value{other.value} {
        ++moveAttempts;
        if (moveAttempts == 2) {
            throw std::runtime_error{"intentional move failure"};
        }
        other.value = -1;
    }
    ThrowingMoveOnly& operator=(ThrowingMoveOnly&&) = default;
};

struct CopyOnly {
    std::string value;

    explicit CopyOnly(std::string valueIn) : value{std::move(valueIn)} {}
    CopyOnly(const CopyOnly&) = default;
    CopyOnly& operator=(const CopyOnly&) = default;
    CopyOnly(CopyOnly&&) = delete;
    CopyOnly& operator=(CopyOnly&&) = delete;
};

struct CopyPreferred {
    inline static int copies{0};
    inline static int moves{0};
    int value{0};

    explicit CopyPreferred(int valueIn) : value{valueIn} {}
    CopyPreferred(const CopyPreferred& other) noexcept : value{other.value} {
        ++copies;
    }
    CopyPreferred& operator=(const CopyPreferred&) = default;
    CopyPreferred(CopyPreferred&&) : value{0} {
        ++moves;
        throw std::runtime_error{"move must not be selected when copy is noexcept"};
    }
    CopyPreferred& operator=(CopyPreferred&&) = default;
};

struct ThrowingCopyable {
    inline static int copyAttempts{0};
    int value{0};

    explicit ThrowingCopyable(int valueIn) : value{valueIn} {}
    ThrowingCopyable(const ThrowingCopyable& other) : value{other.value} {
        ++copyAttempts;
        if (copyAttempts == 2) {
            throw std::runtime_error{"intentional copy failure"};
        }
    }
    ThrowingCopyable& operator=(const ThrowingCopyable&) = default;
    ThrowingCopyable(ThrowingCopyable&&) {
        throw std::runtime_error{"throwing move must not be selected while copy exists"};
    }
    ThrowingCopyable& operator=(ThrowingCopyable&&) = default;
};

void test_small_vec() {
    // source: src/css/small_list.zig — inline for <= N, spill above N, retain
    // capacity on clear, ordered/swap removal, and 1.5x+8 cold growth.
    collections::SmallVec<int, 4> values;
    check(values.empty() && values.is_inline(), "smallvec starts empty and inline");
    check(values.capacity() == 4, "smallvec initial capacity is inline N");
    for (int i{0}; i < 4; ++i) {
        values.push_back(i);
    }
    check(values.is_inline(), "smallvec stays inline through N");
    const int* inlineAddress{values.data()};
    values.push_back(4);
    check(!values.is_inline(), "smallvec spills at N+1");
    check(values.data() != inlineAddress, "smallvec spill changes data address");
    check_eq(std::vector<int>{values.begin(), values.end()}, std::vector<int>{0, 1, 2, 3, 4},
             "smallvec spill preserves order");

    values.insert(2, 99);
    check_eq(std::vector<int>{values.begin(), values.end()}, std::vector<int>{0, 1, 99, 2, 3, 4},
             "smallvec insert shifts tail once");
    check_eq(values.ordered_remove(2), 99, "smallvec ordered_remove returns element");
    check_eq(std::vector<int>{values.begin(), values.end()}, std::vector<int>{0, 1, 2, 3, 4},
             "smallvec ordered_remove preserves order");
    check_eq(values.swap_remove(1), 1, "smallvec swap_remove returns element");
    check_eq(std::vector<int>{values.begin(), values.end()}, std::vector<int>{0, 4, 2, 3},
             "smallvec swap_remove fills hole with last");
    values.clear();
    check(values.empty() && !values.is_inline(), "smallvec clear retains spilled capacity");

    collections::SmallVec<std::string, 1> copyAlias;
    copyAlias.emplace_back("payload");
    copyAlias.push_back(copyAlias[0]);
    check_eq(std::vector<std::string>{copyAlias.begin(), copyAlias.end()},
             std::vector<std::string>{"payload", "payload"},
             "smallvec copy push alias survives spill");

    collections::SmallVec<std::string, 1> emplaceCopyAlias;
    emplaceCopyAlias.emplace_back("payload");
    emplaceCopyAlias.emplace_back(emplaceCopyAlias[0]);
    check_eq(std::vector<std::string>{emplaceCopyAlias.begin(), emplaceCopyAlias.end()},
             std::vector<std::string>{"payload", "payload"},
             "smallvec copy emplace alias survives spill");

    // Defined internal-rvalue semantics: construct the appended value first,
    // then relocate the old range. The source slot is valid but moved-from;
    // the appended slot receives its pre-move value.
    collections::SmallVec<std::string, 1> moveAlias;
    moveAlias.emplace_back("payload");
    moveAlias.push_back(std::move(moveAlias[0]));
    check(moveAlias.size() == 2 && moveAlias[0].empty() && moveAlias[1] == "payload",
          "smallvec move push alias has defined moved-from source");

    collections::SmallVec<std::string, 1> emplaceMoveAlias;
    emplaceMoveAlias.emplace_back("payload");
    emplaceMoveAlias.emplace_back(std::move(emplaceMoveAlias[0]));
    check(emplaceMoveAlias.size() == 2 && emplaceMoveAlias[0].empty() &&
              emplaceMoveAlias[1] == "payload",
          "smallvec move emplace alias has defined moved-from source");

    collections::SmallVec<std::unique_ptr<int>, 2> moveOnly;
    moveOnly.emplace_back(std::make_unique<int>(7));
    moveOnly.push_back(std::make_unique<int>(8));
    moveOnly.push_back(std::make_unique<int>(9));
    check(*moveOnly[0] == 7 && *moveOnly[1] == 8 && *moveOnly[2] == 9,
          "smallvec supports move-only values across spill");
    collections::SmallVec<std::unique_ptr<int>, 2> moved{std::move(moveOnly)};
    check(moved.size() == 3 && *moved.back() == 9, "smallvec move steals spilled storage");
    check(moveOnly.empty() && moveOnly.is_inline(), "smallvec moved-from state is empty inline");

    collections::SmallVec<OverAligned, 2> aligned;
    aligned.push_back(OverAligned{1});
    check(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(OverAligned) == 0,
          "smallvec inline storage respects over-alignment");
    aligned.push_back(OverAligned{2});
    aligned.push_back(OverAligned{3});
    check(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(OverAligned) == 0,
          "smallvec heap storage respects over-alignment");

    LifetimeProbe::live = LifetimeProbe::constructed = LifetimeProbe::destroyed = 0;
    {
        collections::SmallVec<LifetimeProbe, 2> probes;
        probes.emplace_back(1);
        probes.emplace_back(2);
        probes.emplace_back(3);
        collections::SmallVec<LifetimeProbe, 2> copy{probes};
        check(copy.size() == 3 && copy[2].value == 3, "smallvec copy constructs elements");
        collections::SmallVec<LifetimeProbe, 2> assigned;
        assigned = copy;
        check(assigned.size() == 3 && assigned[1].value == 2, "smallvec copy assignment");
        assigned = assigned;
        check(assigned.size() == 3 && assigned[1].value == 2, "smallvec self-copy assignment");
        assigned = std::move(assigned);
        check(assigned.size() == 3 && assigned[1].value == 2, "smallvec self-move assignment");
        [[maybe_unused]] auto removed{probes.ordered_remove(1)};
        probes.pop_back();
        check(probes.size() == 1, "smallvec removals update lifetime-bearing size");
    }
    check_eq(LifetimeProbe::live, 0, "smallvec destroys every live element");
    check_eq(LifetimeProbe::constructed, LifetimeProbe::destroyed,
             "smallvec construction/destruction counts balance");

    collections::SmallVec<int, 0> noInline;
    check(noInline.empty() && noInline.capacity() == 0 && noInline.is_inline(),
          "smallvec N=0 has valid empty representation");
    noInline.push_back(42);
    check(!noInline.is_inline() && noInline[0] == 42, "smallvec N=0 spills on first element");

    collections::SmallVec<CopyOnly, 1> copyOnly;
    copyOnly.emplace_back("first");
    copyOnly.emplace_back("second");
    collections::SmallVec<CopyOnly, 1> copyOnlyAssigned;
    copyOnlyAssigned = copyOnly;
    copyOnlyAssigned = copyOnlyAssigned;
    copyOnlyAssigned = std::move(copyOnlyAssigned);
    check(copyOnlyAssigned.size() == 2 && copyOnlyAssigned[0].value == "first" &&
              copyOnlyAssigned[1].value == "second",
          "smallvec Big Five accepts copy-only elements");

    collections::SmallVec<int, 4> overflow;
    auto reserveResult{overflow.try_reserve(std::numeric_limits<std::size_t>::max())};
    check(!reserveResult.has_value() &&
              reserveResult.error() == collections::CollectionError::capacity_overflow,
          "smallvec reserve reports multiplication/capacity overflow");

    collections::SmallVec<ThrowingMoveOnly, 2> throwing;
    throwing.emplace_back(1);
    throwing.emplace_back(2);
    ThrowingMoveOnly::moveAttempts = 0;
    auto throwingResult{throwing.try_reserve(8)};
    check(!throwingResult.has_value() &&
              throwingResult.error() == collections::CollectionError::element_operation_failed,
          "smallvec try_reserve reports throwing element relocation");
    check_eq(throwing.size(), std::size_t{2},
             "smallvec remains destructible after throwing relocation");
    check(throwing[0].value == -1 && throwing[1].value == 2,
          "smallvec move-only throwing relocation documents basic guarantee state");

    collections::SmallVec<CopyPreferred, 2> copyPreferred;
    copyPreferred.emplace_back(1);
    copyPreferred.emplace_back(2);
    CopyPreferred::copies = CopyPreferred::moves = 0;
    auto preferredResult{copyPreferred.try_reserve(8)};
    check(preferredResult.has_value() && CopyPreferred::copies == 2 && CopyPreferred::moves == 0,
          "smallvec relocation prefers noexcept copy over throwing move");
    check(copyPreferred[0].value == 1 && copyPreferred[1].value == 2,
          "smallvec copy-preferred relocation preserves values");

    collections::SmallVec<ThrowingCopyable, 2> throwingCopy;
    throwingCopy.emplace_back(1);
    throwingCopy.emplace_back(2);
    ThrowingCopyable::copyAttempts = 0;
    auto copyFailure{throwingCopy.try_reserve(8)};
    check(!copyFailure.has_value() &&
              copyFailure.error() == collections::CollectionError::element_operation_failed,
          "smallvec reports throwing copy relocation");
    check(throwingCopy.size() == 2 && throwingCopy[0].value == 1 && throwingCopy[1].value == 2,
          "smallvec copyable throwing relocation rolls back unchanged");
}

}  // namespace

int main() {
    test_static_bit_sets();
    test_dynamic_bit_sets();
    test_auto_bit_sets();
    test_string_pool();
    test_small_vec();

    if (gFailures > MAX_FAILURE_PRINTS) {
        std::println("  ... {} more failures not shown", gFailures - MAX_FAILURE_PRINTS);
    }
    std::println("test_core_collections: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
