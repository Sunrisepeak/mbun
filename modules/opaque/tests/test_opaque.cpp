// Tests for the runtime-independent portion of bun's src/opaque/lib.rs.
import std;
import mbun.opaque;

namespace {

using mbun::opaque::ConversionError;
using mbun::opaque::OpaqueHandle;
using mbun::opaque::OpaqueLifetime;
using mbun::opaque::OpaqueTag;

int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

struct ForeignObject {
    int value{7};
};

enum class TestDiscriminant : std::uint8_t { zero = 0, one = 1 };
static_assert(mbun::opaque::assert_ffi_layout<ForeignObject, sizeof(ForeignObject), alignof(ForeignObject)>());
static_assert(mbun::opaque::assert_ffi_discriminant<TestDiscriminant, std::uint8_t>());

void test_handle_round_trip() {
    ForeignObject object{};
    auto handle{mbun::opaque::erase(&object, OpaqueTag::user, OpaqueLifetime::borrowed)};
    check(handle && handle.data() == &object, "erase stores pointer");
    check(handle.tag() == OpaqueTag::user, "tag survives erasure");
    check(handle.lifetime() == OpaqueLifetime::borrowed, "lifetime survives erasure");

    auto recovered{handle.as<ForeignObject>(OpaqueTag::user)};
    check(recovered.has_value() && (*recovered)->value == 7, "typed conversion round-trips");
    (*recovered)->value = 9;
    check(object.value == 9, "conversion preserves mutation seam");
    auto wrongTag{handle.as<ForeignObject>(OpaqueTag::runtime)};
    check(!wrongTag && wrongTag.error() == ConversionError::tag_mismatch, "tag rejects mismatch");
}

void test_lifetime_gate() {
    ForeignObject object{};
    auto handle{OpaqueHandle::from(&object, OpaqueTag::user, OpaqueLifetime::borrowed)};
    auto owned{handle.as_mut<ForeignObject>(OpaqueTag::user, OpaqueLifetime::owned)};
    check(!owned && owned.error() == ConversionError::lifetime_mismatch, "lifetime rejects mismatch");
    auto borrowed{handle.as_mut<ForeignObject>(OpaqueTag::user, OpaqueLifetime::borrowed)};
    check(borrowed.has_value(), "matching lifetime converts");
}

void test_null_and_ffi_helpers() {
    auto empty{OpaqueHandle::null()};
    auto recovered{empty.as<ForeignObject>(OpaqueTag::user)};
    check(!recovered && recovered.error() == ConversionError::null_handle, "null rejects conversion");

    constexpr char16_t text[]{u"mbun"};
    check(mbun::opaque::ffi::wcslen(text) == 4, "UTF-16 length");
    check(mbun::opaque::ffi::wstr_units(text).size() == 4, "UTF-16 view");
    auto emptySlice{mbun::opaque::ffi::slice<int>(nullptr, 0)};
    check(emptySlice.has_value() && emptySlice->empty(), "null zero slice is empty");
    auto invalidSlice{mbun::opaque::ffi::slice<int>(nullptr, 1)};
    check(!invalidSlice && invalidSlice.error() == ConversionError::null_handle,
          "null nonzero slice rejects");
}

}  // namespace

int main() {
    test_handle_round_trip();
    test_lifetime_gate();
    test_null_and_ffi_helpers();
    std::println("test_opaque: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
