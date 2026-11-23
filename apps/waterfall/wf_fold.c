// SPDX-License-Identifier: AGPL-3.0-or-later
// Adapted from M0LTE/esp32-knob-pages main/wf_fold.c (AGPL-3.0): lines per row is a field.
// Moved here from the firmware repo (M0LTE/yealink-t48s native/wf_fold.c).
#include "wf_fold.h"

#include <math.h>
#include <string.h>

void wf_map_build(wf_map_t *m, float lo, float hi, float bin_hz, int n_bins)
{
    float w = (hi - lo) / WF_W;
    for (int c = 0; c < WF_W; c++) {
        float f0 = lo + c * w, f1 = f0 + w;
        int b0 = (int)ceilf(f0 / bin_hz), b1 = (int)ceilf(f1 / bin_hz) - 1;  // bins with f0 <= f < f1
        if (b1 < b0) b0 = b1 = (int)lroundf((f0 + w / 2) / bin_hz);         // none inside: the nearest
        if (b0 < 0) b0 = 0;
        if (b1 < 0) b1 = 0;
        if (b0 > n_bins - 1) b0 = n_bins - 1;
        if (b1 > n_bins - 1) b1 = n_bins - 1;
        m->b0[c] = (uint16_t)b0;
        m->b1[c] = (uint16_t)b1;
    }
}

void wf_fold_line(const wf_map_t *m, const uint8_t *bins, uint8_t out[WF_W])
{
    for (int c = 0; c < WF_W; c++) {
        const uint8_t *p = bins + m->b0[c], *e = bins + m->b1[c];
        uint8_t v = *p;
        while (++p <= e)
            if (*p > v) v = *p;
        out[c] = v;
    }
}

void wf_span(const float *lo, const float *hi, int n, float sr, float *slo, float *shi)
{
    if (n <= 0) {
        *slo = 0;
        *shi = 3000;
        return;
    }
    float a = lo[0], b = hi[0];
    for (int i = 1; i < n; i++) {
        if (lo[i] < a) a = lo[i];
        if (hi[i] > b) b = hi[i];
    }
    a -= 150;
    b += 150;
    if (a < 0) a = 0;
    if (b > sr / 2) b = sr / 2;
    if (b <= a) {
        a = 0;
        b = 3000;
    }
    *slo = a;
    *shi = b;
}

void wf_rows_reset(wf_rows_t *r, uint32_t next_row, int lpr)
{
    memset(r, 0, sizeof *r);
    r->lpr = lpr > 0 ? lpr : 1;
    r->base = next_row;
    r->cur_row = next_row;
}

bool wf_line_row(const wf_rows_t *r, uint32_t line, uint32_t *row)
{
    if (!r->have_origin || line < r->origin) return false;
    *row = r->base + (line - r->origin) / (uint32_t)r->lpr;
    return true;
}

static void emit(wf_rows_t *r, wf_row_cb_t cb, void *ctx)
{
    cb(ctx, r->cur_row, r->acc, r->acc_tx);
    memset(r->acc, 0, sizeof r->acc);
    r->acc_tx = false;
    r->acc_any = false;
    r->cur_row++;
}

void wf_rows_feed(wf_rows_t *r, uint32_t line, const uint8_t cols[WF_W], bool tx, wf_row_cb_t cb, void *ctx)
{
    uint32_t row;
    if (!r->have_origin) {
        r->have_origin = true;
        r->origin = line;
        r->cur_row = r->base;
    }
    if (!wf_line_row(r, line, &row) || row + 1 < r->cur_row || row > r->cur_row + WF_HIST) {
        // The stream restarted or jumped: finish what we have and start again.
        uint32_t next = r->cur_row + (r->acc_any ? 1 : 0);
        if (r->acc_any) emit(r, cb, ctx);
        wf_rows_reset(r, next, r->lpr);
        r->have_origin = true;
        r->origin = line;
        row = r->base;
    }
    if (row < r->cur_row) row = r->cur_row;  // a late line: into the current row
    while (row > r->cur_row) emit(r, cb, ctx);  // finish this row, and any empty ones
    for (int c = 0; c < WF_W; c++)
        if (cols[c] > r->acc[c]) r->acc[c] = cols[c];
    r->acc_tx |= tx;
    r->acc_any = true;
    // The row is complete with its last line.
    if ((line - r->origin) % (uint32_t)r->lpr == (uint32_t)r->lpr - 1) emit(r, cb, ctx);
}

bool wf_tag_rows(const wf_rows_t *r, uint32_t line, uint32_t burst_lines, uint32_t *start_row, uint16_t *rows)
{
    uint32_t end;
    if (!wf_line_row(r, line, &end)) return false;
    // start = end - ceil(burst / lpr), and at least one row.
    uint32_t n = (burst_lines + r->lpr - 1) / (uint32_t)r->lpr;
    if (n > 0xFFFF) n = 0xFFFF;
    uint32_t start = end >= n ? end - n : 0;
    if (start < r->base) start = r->base;
    *start_row = start;
    *rows = (uint16_t)(end - start >= 1 ? end - start : 1);
    return true;
}
