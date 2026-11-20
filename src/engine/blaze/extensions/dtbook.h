// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>

namespace blaze {
// Translate DTBook XML events directly into the existing chapter writer.
class DtbookWriter : public LVXMLParserCallback
{
    ldomDocumentFragmentWriter &chapter;
    unsigned level = 0;

    const lChar32 *element(const lChar32 *name) const
    {
        if (!lStr_cmp(name, U"dtbook"))
            return U"html";
        if (!lStr_cmp(name, U"book"))
            return U"body";
        if (!lStr_cmp(name, U"level") || !lStr_cmp(name, U"frontmatter")
            || !lStr_cmp(name, U"bodymatter") || !lStr_cmp(name, U"rearmatter")
            || (lStr_len(name) == 6 && std::equal(name, name + 5, U"level") && name[5] >= U'1'
                && name[5] <= U'6'))
            return U"div";
        if (!lStr_cmp(name, U"hd")) {
            static const lChar32 *heads[] = { U"h1", U"h1", U"h2", U"h3", U"h4", U"h5", U"h6" };
            return heads[level < 6 ? level : 6];
        }
        if (!lStr_cmp(name, U"doctitle"))
            return U"h1";
        if (!lStr_cmp(name, U"docauthor"))
            return U"p";
        if (!lStr_cmp(name, U"pagenum"))
            return U"span";
        return name;
    }

public:
    explicit DtbookWriter(ldomDocumentFragmentWriter &writer) : chapter(writer) { }

    void OnStart(LVFileFormatParser *parser) override { chapter.OnStart(parser); }

    void OnStop() override { chapter.OnStop(); }

    lUInt32 getFlags() override { return chapter.getFlags(); }

    void setFlags(lUInt32 flags) override { chapter.setFlags(flags); }

    ldomNode *OnTagOpen(const lChar32 *, const lChar32 *name) override
    {
        if (!lStr_cmp(name, U"level"))
            ++level;
        return chapter.OnTagOpen(U"", element(name));
    }

    void OnTagBody() override { chapter.OnTagBody(); }

    void OnTagClose(const lChar32 *, const lChar32 *name, bool empty) override
    {
        chapter.OnTagClose(U"", element(name), empty);
        if (level && !lStr_cmp(name, U"level"))
            --level;
    }

    void OnAttribute(const lChar32 *ns, const lChar32 *name, const lChar32 *value) override
    {
        chapter.OnAttribute(ns, name, value);
    }

    void OnText(const lChar32 *text, int len, lUInt32 flags) override
    {
        chapter.OnText(text, len, flags);
    }

    bool OnBlob(lString32 name, const lUInt8 *data, int size) override
    {
        return chapter.OnBlob(name, data, size);
    }
};
} // namespace blaze
