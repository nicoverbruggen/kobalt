// Fork-only diagnostics for vertical paths in lvrend.cpp.

#ifndef LVREND_VERT_DIAG_H_INCLUDED
#define LVREND_VERT_DIAG_H_INCLUDED

// Record observations without exposing the backing counters across
// translation units.
void lvrend_record_ruby_diag(bool is_vertical, int column_x);
void lvrend_record_list_marker_diag(bool is_vertical);

void lvrend_reset_ruby_diag();
void lvrend_get_ruby_diag(int *ok_out, int *miss_out, int *col_x_max_out);
void lvrend_reset_list_marker_diag();
void lvrend_get_list_marker_diag(int *ok_out, int *miss_out);

#endif // LVREND_VERT_DIAG_H_INCLUDED
