// SPDX-License-Identifier: GPL-2.0-or-later

// This format stores patterns whose letters fit in Latin-1. Each bucket holds a sequence of records:
// length, Latin-1 letters, and length+1 ASCII weights. The offsets include the
// end of the final bucket. Words in the book are still full Unicode.
namespace {
constexpr unsigned packedHeader = 12;
constexpr unsigned packedData = packedHeader + 4 * (PATTERN_HASH_SIZE + 1);
constexpr unsigned maxPackedBytes = 4 * 1024 * 1024;

lUInt32 readPacked32(const lUInt8 *p)
{
    return p[0] | (lUInt32(p[1]) << 8) | (lUInt32(p[2]) << 16) | (lUInt32(p[3]) << 24);
}

void writePacked32(lUInt8 *p, lUInt32 value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = lUInt8(value >> (8 * i));
}
}

bool HyphMan::compilePatterns(LVStreamRef source, LVStreamRef output)
{
    TexHyph dictionary;
    lUInt32 crc;
    return !source.isNull() && !output.isNull() && source->getcrc32(crc) == LVERR_OK
            && dictionary.load(source) && dictionary.savePacked(output, crc);
}

bool TexHyph::savePacked(LVStreamRef output, lUInt32 sourceCrc) const
{
    // A dictionary that needs explicit modifier handling cannot use this
    // format. Reading text with modifiers still uses TexHyph::hyphenate.
    if (!_supported_modifiers.empty() || largest_overflowed_word)
        return false;
    std::vector<lUInt8> bytes(packedData);
    memcpy(bytes.data(), "KHG1", 4);
    writePacked32(bytes.data() + 4, sourceCrc);
    for (unsigned bucket = 0; bucket < PATTERN_HASH_SIZE; ++bucket) {
        writePacked32(bytes.data() + packedHeader + 4 * bucket, bytes.size());
        for (const TexPattern *p = table[bucket]; p; p = p->next) {
            const int n = lStr_len(p->word);
            if (!n || bytes.size() + 2 * n + 2 > maxPackedBytes)
                return false;
            bytes.push_back(n);
            for (int i = 0; i < n; ++i) {
                if (p->word[i] > 255)
                    return false;
                bytes.push_back(p->word[i]);
            }
            bytes.insert(bytes.end(), p->attr, p->attr + n + 1);
        }
    }
    writePacked32(bytes.data() + packedData - 4, bytes.size());
    writePacked32(bytes.data() + 8, lStr_crc32(0, bytes.data() + packedHeader,
                                                bytes.size() - packedHeader));
    lvsize_t written;
    return output->Write(bytes.data(), bytes.size(), &written) == LVERR_OK && written == bytes.size();
}

bool TexHyph::loadPacked(LVStreamRef source, LVStreamRef packed)
{
    if (packed.isNull())
        return false;
    const lvsize_t size = packed->GetSize();
    if (size <= packedData || size > maxPackedBytes)
        return false;
    std::vector<lUInt8> buffer(size);
    lvsize_t read;
    if (packed->Read(buffer.data(), size, &read) != LVERR_OK || read != size)
        return false;
    const auto *bytes = buffer.data();
    lUInt32 crc;
    if (memcmp(bytes, "KHG1", 4) || source->getcrc32(crc) != LVERR_OK
            || readPacked32(bytes + 4) != crc
            || readPacked32(bytes + 8) != lStr_crc32(0, bytes + packedHeader, size - packedHeader))
        return false;

    // Validate record boundaries once, before the matcher can follow an offset.
    unsigned pos = packedData;
    unsigned count = 0;
    for (unsigned bucket = 0; bucket < PATTERN_HASH_SIZE; ++bucket) {
        if (readPacked32(bytes + packedHeader + 4 * bucket) != pos)
            return false;
        const unsigned end = readPacked32(bytes + packedHeader + 4 * (bucket + 1));
        if (end < pos || end > size)
            return false;
        while (pos < end) {
            const unsigned n = bytes[pos];
            if (!n || n > MAX_PATTERN_SIZE || 2 * n + 2 > end - pos)
                return false;
            pos += 2 * n + 2;
            ++count;
        }
    }
    if (pos != size || !count)
        return false;
    packedPatterns.swap(buffer);
    _pattern_count = count;
    return true;
}

bool TexHyph::matchPacked(unsigned bucket, const lChar32 *text, char *mask) const
{
    const auto *bytes = packedPatterns.data();
    const auto *offset = bytes + packedHeader + 4 * bucket;
    const auto *end = bytes + readPacked32(offset + 4);
    bool found = false;
    for (const auto *p = bytes + readPacked32(offset); p < end; ) {
        const unsigned n = *p++;
        const auto *letters = p;
        const auto *weights = p + n;
        p = weights + n + 1;
        unsigned i = 0;
        while (i < n && text[i] && text[i] == letters[i])
            ++i;
        if (i != n)
            continue;
        found = true;
        for (i = 0; i <= n && mask[i]; ++i)
            if (weights[i] > mask[i])
                mask[i] = weights[i];
    }
    return found;
}
