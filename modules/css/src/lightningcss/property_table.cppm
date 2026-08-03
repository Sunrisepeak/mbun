// Lightning CSS property-dispatch seam.
//
// Blueprint: bun Rust css/properties/properties_generated.rs and
// properties_impl.rs; Zig css/properties/properties_generated.zig. This seam
// deliberately reuses mbun.css.property_tables so tokenizer and LightningCSS
// callers cannot acquire divergent property IDs.
export module mbun.css.lightningcss.property_table;

import std;
import mbun.css.property_tables;

export namespace mbun::css::lightningcss {

using PropertyId = mbun::css::PropertyIdTag;
using PropertyKind = mbun::css::PropertyKind;

struct PropertyEntry {
    std::string_view name;
    PropertyId id;
    PropertyKind kind;
};

constexpr PropertyEntry lookup_property(std::string_view name) {
    return PropertyEntry{name, mbun::css::property_id(name), mbun::css::property_kind(name)};
}

}  // namespace mbun::css::lightningcss
