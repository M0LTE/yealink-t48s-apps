// SPDX-License-Identifier: AGPL-3.0-or-later
// Waterfall maths for the pdn-soundmodem station page stream: spectrum lines
// folded to screen columns, lines folded into rows, and decoded frames placed
// on rows. Adapted from M0LTE/esp32-knob-pages main/wf_fold.h (AGPL-3.0):
// wider rows, a deeper history and a lines-per-row chosen from the line rate.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WF_W 600       // waterfall row width, one byte per screen column
#define WF_HIST 352    // rows of history kept (the screen shows fewer)

// Which bins feed each column: bins b0[c] .. b1[c] inclusive.
typedef struct {
    uint16_t b0[WF_W], b1[WF_W];
} wf_map_t;

// Columns cover lo_hz..hi_hz evenly; bin i is at i * bin_hz. A column with
// no bin inside it takes the nearest one. n_bins is the line length.
void wf_map_build(wf_map_t *m, float lo_hz, float hi_hz, float bin_hz, int n_bins);
// One spectrum line to WF_W columns, the maximum over each column's bins.
void wf_fold_line(const wf_map_t *m, const uint8_t *bins, uint8_t out[WF_W]);

// Span from the modems' bands: min(lo) - 150 .. max(hi) + 150, clamped to
// 0..sample_rate/2; 0..3000 when there are none.
void wf_span(const float *lo, const float *hi, int n, float sample_rate, float *span_lo, float *span_hi);

// Lines into rows: the first line seen (after a reset) starts row `base`;
// row = base + (line - origin) / lpr.
typedef struct {
    int lpr;                // lines per row
    bool have_origin;
    uint32_t origin;        // line index of the first line
    uint32_t base;          // row number of that line's row
    uint32_t cur_row;       // row being built
    uint8_t acc[WF_W];
    bool acc_tx, acc_any;
} wf_rows_t;

void wf_rows_reset(wf_rows_t *r, uint32_t next_row, int lpr);
// The row a line index falls in; false before the origin is known or for a
// line from before it.
bool wf_line_row(const wf_rows_t *r, uint32_t line, uint32_t *row);

// Called for each finished row, in order, with no gaps (a gap in the lines
// gives empty rows).
typedef void (*wf_row_cb_t)(void *ctx, uint32_t row, const uint8_t cols[WF_W], bool tx);
// Feeds one line (already folded to columns). Lines that go backwards, or
// jump more than WF_HIST rows ahead, restart the numbering at the next row.
void wf_rows_feed(wf_rows_t *r, uint32_t line, const uint8_t cols[WF_W], bool tx, wf_row_cb_t cb, void *ctx);

// A decoded frame's place: it ended in line `line` and lasted burst_lines
// (0 = unknown). False if the line cannot be placed.
bool wf_tag_rows(const wf_rows_t *r, uint32_t line, uint32_t burst_lines, uint32_t *start_row, uint16_t *rows);
