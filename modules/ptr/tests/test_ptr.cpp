import std;
import mbun.ptr;

namespace {
int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_tagged_pointer_round_trip() {
    int value{42};
    auto taggedResult{mbun::ptr::TaggedPointer::from(&value, 7)};
    check(taggedResult.has_value(), "tagged pointer stores a non-null address");
    if (!taggedResult) {
        return;
    }
    auto tagged{*taggedResult};
    check(tagged.tag() == 7, "tag survives packing");
    check(tagged.get<int>() == &value, "pointer survives packing");
    check(mbun::ptr::TaggedPointer::null().is_null(), "null pointer is representable");
}

void test_checked_offsets() {
    std::array<std::byte, 8> bytes{};
    auto valid{mbun::ptr::checked_offset(bytes.size(), 3, 4)};
    check(valid.has_value() && *valid == 7, "offset accepts an in-bounds range");
    auto out_of_bounds{mbun::ptr::checked_offset(bytes.size(), 6, 3)};
    check(!out_of_bounds && out_of_bounds.error() == mbun::ptr::OffsetError::out_of_bounds,
          "offset rejects an out-of-bounds range");
    auto overflow{mbun::ptr::checked_offset(std::numeric_limits<std::size_t>::max(),
                                             std::numeric_limits<std::size_t>::max(), 1)};
    check(!overflow && overflow.error() == mbun::ptr::OffsetError::overflow,
          "offset rejects arithmetic overflow");
}

void test_ownership_view_seam() {
    std::array<int, 3> input{1, 2, 3};
    auto borrowed{mbun::ptr::OwnedView<int>::borrowed(input)};
    check(!borrowed.is_owned() && borrowed.size() == 3, "borrowed view has no owner");
    borrowed[1] = 9;
    check(input[1] == 9, "borrowed view aliases its source");

    auto owned{mbun::ptr::OwnedView<int>::owned(std::vector<int>{4, 5})};
    check(owned.is_owned() && owned.size() == 2, "owned view carries storage");
    check(owned[0] == 4 && owned[1] == 5, "owned view exposes its contents");
}
} // namespace

int main() {
    test_tagged_pointer_round_trip();
    test_checked_offsets();
    test_ownership_view_seam();
    std::println("test_ptr: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
