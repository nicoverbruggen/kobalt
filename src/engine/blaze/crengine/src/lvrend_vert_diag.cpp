// =============================================================================
// Ruby-table vertical-detection diagnostic counters.
//
// Fork-only file: extracted from m-tky/crengine lvrend.cpp during Phase D
// Step A1 to reduce soft-fork divergence in lvrend.cpp.  No upstream
// counterpart — these counters track a vertical-rl-specific code path.
//
// vert_ok:   ruby table correctly identified as vertical (vert_ruby=true).
// vert_miss: ruby table NOT identified as vertical (vert_ruby=false) yet
//            has cells with non-zero col->x — the bug that causes Y
//            displacement.
// col_x_max: the largest col->x used when vert_miss > 0.
//
// Populated through the record functions below.  Exposed to cre.cpp (Lua side)
// through lvrend_reset_ruby_diag() / lvrend_get_ruby_diag().
// =============================================================================

#include "../include/lvrend_vert_diag.h"

static int ruby_vert_ok   = 0;
static int ruby_vert_miss = 0;
static int ruby_col_x_max = 0;
static int list_marker_vert_ok = 0;
static int list_marker_vert_miss = 0;

void lvrend_record_ruby_diag(bool is_vertical, int column_x) {
    if ( is_vertical ) {
        ruby_vert_ok++;
    }
    else if ( column_x != 0 ) {
        ruby_vert_miss++;
        if ( column_x > ruby_col_x_max )
            ruby_col_x_max = column_x;
    }
}

void lvrend_record_list_marker_diag(bool is_vertical) {
    if ( is_vertical )
        list_marker_vert_ok++;
    else
        list_marker_vert_miss++;
}

void lvrend_reset_ruby_diag() {
    ruby_vert_ok = ruby_vert_miss = ruby_col_x_max = 0;
}

void lvrend_get_ruby_diag(int *ok_out, int *miss_out, int *col_x_max_out) {
    *ok_out        = ruby_vert_ok;
    *miss_out      = ruby_vert_miss;
    *col_x_max_out = ruby_col_x_max;
}

void lvrend_reset_list_marker_diag() {
    list_marker_vert_ok = 0;
    list_marker_vert_miss = 0;
}

void lvrend_get_list_marker_diag(int *ok_out, int *miss_out) {
    *ok_out = list_marker_vert_ok;
    *miss_out = list_marker_vert_miss;
}
