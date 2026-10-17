#pragma once
#include "publication/resource_source.h"
#include "publication/source_lease.h"
#include "platform/api/position_codec.h"
#include <QMap>

namespace kobalt {
// Prepared by the host after its eligibility and source-open checks. The
// factory runs on the engine worker and owns values, never borrowed handles.
// Path is a materialized local archive used for revision/legacy hash checks.
struct OpenRequest {
    QString identity, path;
    QSize size;
    QJsonObject saved;
    QString platformLocation, layoutCachePath;
    std::shared_ptr<SourceLease> lease;
    ResourceFactory resources;
    PositionCodec positions{};
    // Where parsed books may be kept between openings. Empty disables it.
    QString documentCachePath{};
    // A host may pin one opening without saving its appearance. Keep the raw
    // stored appearance separately so checkpoints retain absent keys too.
    QJsonObject temporaryAppearance{}, originalAppearance{};
    // Optional user CSS. Read on the engine worker for every opening.
    QString userStyleSheetPath{};
    // Applied after checking the saved publication fingerprint, before temporary appearance.
    QJsonObject appearanceDefaults{};
};
}
