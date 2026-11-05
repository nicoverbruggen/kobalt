#include "reading/logical_word_counter.h"

#include "check.h"

#include <QString>
#include <QVector>

#include <string>

namespace {
qint64 words(const char *text)
{
    kobalt::LogicalWordCounter counter;
    for (const auto character : QString::fromUtf8(text).toUcs4()) {
        counter.append(char32_t(character));
    }
    return counter.words();
}
} // namespace

int main()
{
    require(words("") == 0 && words(" . -- ' ’  😀 ") == 0,
            "Punctuation or symbols counted as words");
    require(words("rainbow don't Reader’s café café co­operation A𐐀word 123") == 8,
            "Latin, marks, internal apostrophes, soft hyphens or supplementary letters split");
    require(words("one--two one-two one‑two") == 4, "Repeated punctuation joined separate words");
    require(words("日本語中文 عربي كلمتان עברית שלום 한국어 문장 ภาษาไทย") == 12,
            "The documented multilingual text-unit policy changed");
    kobalt::LogicalWordCounter counter;
    for (const char value : std::string("rain")) {
        counter.append(value);
    }
    for (const char value : std::string("bow")) {
        counter.append(value);
    }
    require(counter.words() == 1, "Inline text chunks split a word");
    counter.boundary();
    counter.append('x');
    require(counter.words() == 2, "Block boundary joined two words");
    puts("PASS: logical word counter preserves text chunks and the experimental Unicode policy");
}
