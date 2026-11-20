// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace blaze {
// Resolve once per manifest item. A missing ID or cycle keeps the original
// spine item on the importer's existing best-effort path.
inline EpubItem *spineFallback(EpubItem *item, EpubItems &manifest)
{
    LVArray<EpubItem *> path;
    while (item && !item->resolved && !item->resolving) {
        if (item->is_xhtml || item->mediaType == "image/svg+xml" || item->mediaType == "text/html") {
            item->resolved = item;
            break;
        }
        if (item->fallback.empty()) {
            item = NULL;
            break;
        }
        item->resolving = true;
        path.add(item);
        item = manifest.findById(item->fallback);
    }
    EpubItem *result = item ? item->resolved : NULL;
    for (int i = 0; i < path.length(); ++i)
        path[i]->resolved = result;
    return result;
}
}
