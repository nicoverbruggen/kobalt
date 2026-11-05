#pragma once
#include <QString>
#include <memory>

namespace kobalt {
class AssetBundle;
}

namespace kobalt::android {
// Keep the returned owner for startup and reading, so its verified metadata is reused.
std::unique_ptr<AssetBundle> installAssets(const QString &applicationData);
} // namespace kobalt::android
