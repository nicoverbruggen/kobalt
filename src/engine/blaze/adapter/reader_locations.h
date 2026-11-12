#pragma once
#include <QVector>

struct ldomNode;

// How a shared Location maps to CREngine's DOM. A spine item is a
// DocFragment with one <body>. CREngine adds wrapper elements during layout
// and elements for generated content; neither is in the source XHTML, so a
// source path passes through wrappers and ignores generated content.
namespace kobalt::enginelocation {
bool wrapper(ldomNode *node);
bool generated(ldomNode *node);
// The <body> of a spine item, or null when it does not have exactly one.
ldomNode *body(ldomNode *fragment);
// The nearest ancestor that the source also has.
ldomNode *sourceParent(ldomNode *node);
bool elementPath(ldomNode *body, ldomNode *element, QVector<int> &path);
ldomNode *elementAt(ldomNode *body, const QVector<int> &path);
// The text nodes of an element that the source also has, in document order.
QVector<ldomNode *> textNodes(ldomNode *element);
} // namespace kobalt::enginelocation
