// Reader word-prefix runs. Keep Unicode marks with their letters and leave
// scripts that require contextual joining or word segmentation unchanged.
#ifndef LV_BIONIC_H
#define LV_BIONIC_H
#include <utf8proc.h>
#include <hb.h>
#include <vector>
struct LVBionicRun { int start, end; bool emphasized; };
inline bool lvBionicLetter(lChar32 ch) {
    auto script = hb_unicode_script(hb_unicode_funcs_get_default(), ch);
    bool supported = script == HB_SCRIPT_LATIN || script == HB_SCRIPT_GREEK
        || script == HB_SCRIPT_CYRILLIC || script == HB_SCRIPT_HEBREW;
    auto category = utf8proc_category(ch);
    return supported && category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO;
}
inline bool lvBionicMark(lChar32 ch) {
    auto category = utf8proc_category(ch);
    return category >= UTF8PROC_CATEGORY_MN && category <= UTF8PROC_CATEGORY_ME;
}
inline bool lvBionicWord(lChar32 ch) {
    return lvBionicLetter(ch) || lvBionicMark(ch) || utf8proc_category(ch) == UTF8PROC_CATEGORY_ND
        || ch == U'\'' || ch == 0x2019 || ch == 0x00ad;
}
inline std::vector<LVBionicRun> lvBionicRuns(const lChar32 *text, int length, bool continuation=false) {
    std::vector<LVBionicRun> runs;
    int normalStart = 0;
    for (int i = 0; i < length;) {
        if (!lvBionicLetter(text[i])) { ++i; continue; }
        int start = i, end = i + 1, letters = 1;
        while (end < length && lvBionicWord(text[end])) {
            if (lvBionicLetter(text[end])) ++letters;
            ++end;
        }
        // A word interrupted by inline markup has no new beginning here.
        if (continuation && start == 0) { i = end; continue; }
        int prefix = start, remaining = (letters + 1) / 2;
        while (prefix < end && remaining > 0) {
            if (lvBionicLetter(text[prefix])) --remaining;
            ++prefix;
        }
        while (prefix < end && lvBionicMark(text[prefix])) ++prefix;
        if (start > normalStart) runs.push_back({normalStart, start, false});
        runs.push_back({start, prefix, true});
        normalStart = prefix; i = end;
    }
    if (normalStart < length) runs.push_back({normalStart, length, false});
    return runs;
}
#endif
