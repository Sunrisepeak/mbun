export module mbun.bun_core.context;

import mbun.bun_core.feature;
import mbun.bun_core.platform;
import mbun.bun_core.version;

namespace mbun::bun_core {

export struct RuntimeData {
    Version version { runtime_version() };
    platform::Platform platform { platform::current() };
    FeatureSet features { FeatureSet::defaults() };
};

export struct RuntimeContext {
    RuntimeData runtime_data {};
};

} // namespace mbun::bun_core
