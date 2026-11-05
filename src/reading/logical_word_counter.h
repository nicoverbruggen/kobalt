#pragma once

#include <QChar>
#include <QtGlobal>

namespace kobalt {
// Counting policy, independent of pagination. Letter/number runs
// form words. Marks and format characters continue a run; one internal
// apostrophe or hyphen joins it. Han and kana letters count individually.
// This is a count of text units, without dictionary segmentation for Thai
// or Japanese. Changing these rules requires a new document-cache version.
class LogicalWordCounter
{
public:
    void append(char32_t character)
    {
        const auto value = uint(character);
        if (value < 128) {
            if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9')) {
                letter();
            } else if (value == '\'' || value == '-') {
                connector();
            } else {
                boundary();
            }
            return;
        }
        const auto category = QChar::category(value);
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing || category == QChar::Other_Format) {
            return;
        }
        if (value == 0x2019 || value == 0x2010 || value == 0x2011) {
            connector();
            return;
        }
        if (QChar::isLetterOrNumber(value)) {
            const auto script = QChar::script(value);
            if (script == QChar::Script_Han || script == QChar::Script_Hiragana
                || script == QChar::Script_Katakana) {
                ++words_;
                boundary();
            } else {
                letter();
            }
        } else {
            boundary();
        }
    }

    void boundary()
    {
        inWord_ = false;
        connector_ = false;
    }

    qint64 words() const { return words_; }

private:
    void letter()
    {
        if (!inWord_) {
            ++words_;
        }
        inWord_ = true;
        connector_ = false;
    }

    void connector()
    {
        if (connector_) {
            boundary();
        } else {
            connector_ = inWord_;
        }
    }

    qint64 words_ = 0;
    bool inWord_ = false;
    bool connector_ = false;
};
} // namespace kobalt
