// Fork-only diagnostics for vertical text formatting and drawing.

#ifndef LVTEXTFM_VERT_DIAG_H_INCLUDED
#define LVTEXTFM_VERT_DIAG_H_INCLUDED

// The backing counters are shared by lvtextfm.cpp and its included
// lvtextfm_vert.cpp implementation fragment.  Other translation units should
// use the reset/get accessors below.
extern int ltext_vert_ruby_adv_diff_total;
extern int ltext_vert_ruby_adv_diff_max;
extern int ltext_vert_bleed_count;
extern int ltext_vert_bleed_max_px;
extern int ltext_vert_ib_layout_gap_total;
extern int ltext_vert_ib_layout_gap_max;
extern int ltext_vert_char_overlap_count;
extern int ltext_vert_char_overlap_max_px;
extern int ltext_vert_trailing_space_trim_count;
extern int ltext_vert_trailing_space_trim_chars;
extern int ltext_vert_image_draw_count;
extern int ltext_vert_image_draw_drift_count;
extern int ltext_vert_image_draw_drift_max_px;
extern int ltext_vert_image_cross_underreserve_count;
extern int ltext_vert_image_cross_underreserve_max_px;
extern int ltext_vert_mixed_image_axis_sample_count;
extern int ltext_vert_mixed_image_axis_drift_count;
extern int ltext_vert_mixed_image_axis_drift_max_px;
extern int ltext_vert_single_image_placement_sample_count;
extern int ltext_vert_single_image_clip_overflow_count;
extern int ltext_vert_single_image_clip_overflow_max_px;
extern int ltext_vert_single_image_center_error_max_px;
extern int ltext_vert_exact_hanging_attempt_count;
extern int ltext_vert_hanging_layout_count;
extern int ltext_vert_exact_hanging_clip_recovery_count;
extern int ltext_vert_exact_hanging_clip_reject_count;
extern int ltext_vert_exact_hanging_draw_count;
extern int ltext_vert_exact_hanging_font_entry_count;
extern int ltext_vert_exact_hanging_outside_regular_count;
extern int ltext_vert_exact_hanging_active_clip_count;
extern int ltext_vert_exact_hanging_regular_bottom;
extern int ltext_vert_exact_hanging_last_regular_bottom;
extern int ltext_vert_exact_hanging_last_active_bottom;
extern int ltext_vert_exact_hanging_last_glyph_top;
extern int ltext_vert_exact_hanging_last_glyph_bottom;
extern int ltext_vert_fallback_size_sample_count;
extern int ltext_vert_fallback_size_mismatch_count;
extern int ltext_vert_fallback_size_mismatch_max_px;

// Opt-in, bounded draw trace: 1=underline, 2=overline, 3=right border.
// Rectangles are screen coordinates [x0,x1) x [y0,y1); owner is a DOM data index.
void ltext_reset_vert_decoration_trace();
void ltext_stop_vert_decoration_trace();
void ltext_get_vert_decoration_trace_stats(int *count_out, int *overflow_out);
bool ltext_get_vert_decoration_trace_event(int index, int *kind_out,
        int *owner_out, int *char_out, int *x0_out, int *y0_out,
        int *x1_out, int *y1_out);
void ltext_set_vert_decoration_trace_context(int owner_id, int first_codepoint);
void ltext_record_vert_decoration_trace(int kind, int x0, int y0, int x1, int y1);
void ltext_record_vert_border_trace(int owner_id, int x0, int y0, int x1, int y1);
void ltext_reset_vert_ruby_adv_diff();
void ltext_get_vert_ruby_adv_diff(int *total_out, int *max_out);
void ltext_reset_vert_bleed();
void ltext_get_vert_bleed(int *count_out, int *max_px_out);
void ltext_reset_vert_ib_layout_gap();
void ltext_get_vert_ib_layout_gap(int *total_out, int *max_out);
void ltext_reset_vert_char_overlap();
void ltext_get_vert_char_overlap(int *count_out, int *max_px_out);
void ltext_reset_vert_trailing_space_trim();
void ltext_get_vert_trailing_space_trim(int *count_out, int *chars_out);
void ltext_reset_vert_image_draw_drift();
void ltext_get_vert_image_draw_drift(
    int *draw_count_out, int *drift_count_out, int *max_px_out);
void ltext_reset_vert_image_cross_underreserve();
void ltext_get_vert_image_cross_underreserve(int *count_out, int *max_px_out);
void ltext_reset_vert_mixed_image_axis();
void ltext_get_vert_mixed_image_axis(
    int *sample_count_out, int *drift_count_out, int *max_px_out);
void ltext_reset_vert_single_image_placement();
void ltext_get_vert_single_image_placement(
    int *sample_count_out, int *clip_overflow_count_out,
    int *clip_overflow_max_px_out, int *center_error_max_px_out);
void ltext_reset_vert_exact_hanging_clip();
void ltext_get_vert_exact_hanging_clip(
    int *attempt_count_out, int *recovery_count_out, int *reject_count_out,
    int *draw_count_out, int *layout_count_out);
void ltext_get_vert_exact_hanging_glyph(
    int *font_entry_count_out, int *outside_regular_count_out,
    int *active_clip_count_out, int *regular_bottom_out,
    int *active_bottom_out, int *glyph_top_out, int *glyph_bottom_out);
void ltext_reset_vert_fallback_size();
void ltext_get_vert_fallback_size(
    int *sample_count_out, int *mismatch_count_out, int *mismatch_max_px_out);

#endif // LVTEXTFM_VERT_DIAG_H_INCLUDED
