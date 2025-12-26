/*
 * ck-clock-time.c - Left time view widget for ck-clock.
 */

#include "ck-clock.h"

#include <Xm/DrawingA.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const double PM_INDICATOR_P_OFFSETS[3] = { 0.0, -0.35, 0.0 };

enum {
    LED_SEG_BAR_T = 0x01,
    LED_SEG_R_T   = 0x02,
    LED_SEG_R_B   = 0x04,
    LED_SEG_BAR_B = 0x08,
    LED_SEG_L_B   = 0x10,
    LED_SEG_L_T   = 0x20,
    LED_SEG_BAR_M = 0x40
};

static const unsigned char PM_INDICATOR_LEAD = LED_SEG_BAR_T | LED_SEG_R_T |
                                               LED_SEG_L_T | LED_SEG_L_B |
                                               LED_SEG_BAR_M;
static const unsigned char AM_INDICATOR_LEAD = LED_SEG_BAR_T | LED_SEG_R_T |
                                               LED_SEG_R_B | LED_SEG_L_T |
                                               LED_SEG_L_B | LED_SEG_BAR_M;
static const unsigned char INDICATOR_M_ARCH = LED_SEG_BAR_T | LED_SEG_L_T |
                                              LED_SEG_L_B | LED_SEG_R_T |
                                              LED_SEG_R_B;

static unsigned char led_segmap_for_digit(int d)
{
    static const unsigned char map[10] = {
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_BAR_B |
            LED_SEG_L_B | LED_SEG_L_T,
        LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_BAR_M | LED_SEG_L_B |
            LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_BAR_M | LED_SEG_R_B |
            LED_SEG_BAR_B,
        LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_B |
            LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_L_B |
            LED_SEG_R_B | LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_BAR_B |
            LED_SEG_L_B | LED_SEG_L_T | LED_SEG_BAR_M,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_T |
            LED_SEG_R_B | LED_SEG_BAR_B
    };
    if (d < 0 || d > 9) return 0;
    return map[d];
}

static unsigned char led_segmap_for_char(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'A': return LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_L_T |
                 LED_SEG_L_B | LED_SEG_BAR_M;
    case 'P': return LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_L_T | LED_SEG_L_B |
                 LED_SEG_BAR_M;
    case 'M': return LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_L_B | LED_SEG_R_T |
                 LED_SEG_R_B;
    default: return 0;
    }
}

static void draw_led_segment(cairo_t *cr,
                             double x,
                             double y,
                             double w,
                             double h,
                             int enabled,
                             double on_r,
                             double on_g,
                             double on_b,
                             double off_r,
                             double off_g,
                             double off_b)
{
    cairo_set_source_rgb(cr,
                         enabled ? on_r : off_r,
                         enabled ? on_g : off_g,
                         enabled ? on_b : off_b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void draw_led_digit(cairo_t *cr,
                           double x,
                           double y,
                           double w,
                           double h,
                           unsigned char segs,
                           double on_r,
                           double on_g,
                           double on_b,
                           double off_r,
                           double off_g,
                           double off_b)
{
    double thickness = h * 0.12;
    if (thickness < 2.0) thickness = 2.0;
    double half_h = h / 2.0;

    draw_led_segment(cr, x + thickness, y, w - 2.0 * thickness, thickness,
                     (segs & 0x01) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + w - thickness, y + thickness, thickness,
                     half_h - thickness, (segs & 0x02) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + w - thickness, y + half_h + 1.0, thickness,
                     half_h - thickness - 1.0, (segs & 0x04) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + thickness, y + h - thickness, w - 2.0 * thickness, thickness,
                     (segs & 0x08) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x, y + half_h + 1.0, thickness,
                     half_h - thickness - 1.0, (segs & 0x10) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x, y + thickness, thickness,
                     half_h - thickness, (segs & 0x20) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + thickness, y + half_h - thickness / 2.0,
                     w - 2.0 * thickness, thickness,
                     (segs & 0x40) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
}

static void draw_led_time(cairo_t *cr,
                          int hour,
                          int minute,
                          bool show_colon,
                          double zone_x,
                          double zone_y,
                          double zone_w,
                          double zone_h,
                          double on_r,
                          double on_g,
                          double on_b,
                          double off_r,
                          double off_g,
                          double off_b,
                          const char *indicator)
{
    if (zone_w <= 0.0 || zone_h <= 0.0) return;
    double margin = fmax(3.0, fmin(zone_w, zone_h) * 0.02);
    double max_height = zone_h - 2.0 * margin;
    if (max_height <= 0.0) return;

    double digit_height = max_height * 0.96;
    double digit_width = digit_height * 0.5;
    double gap = digit_width * 0.25;
    double colon_width = digit_width * 0.22;
    if (colon_width < 2.0) colon_width = 2.0;

    double available_width = zone_w - 2.0 * margin;
    bool use_triplet = indicator &&
                       (strcmp(indicator, "PM") == 0 ||
                        strcmp(indicator, "AM") == 0);
    unsigned char indicator_masks[4] = {0};
    int indicator_len = 0;
    if (use_triplet) {
        indicator_len = 3;
        unsigned char lead =
            (strcmp(indicator, "PM") == 0)
                ? PM_INDICATOR_LEAD
                : AM_INDICATOR_LEAD;
        indicator_masks[0] = lead;
        indicator_masks[1] = INDICATOR_M_ARCH;
        indicator_masks[2] = INDICATOR_M_ARCH;
    } else if (indicator && indicator[0]) {
        indicator_len = (int)strlen(indicator);
        if (indicator_len > 3) indicator_len = 3;
        for (int i = 0; i < indicator_len; ++i) {
            indicator_masks[i] = led_segmap_for_char(indicator[i]);
        }
    }
    double indicator_gap = gap;
    double indicator_digit_width = digit_width * 0.54;
    double indicator_digit_height = digit_height * 0.65;
    double indicator_thickness = indicator_digit_height * 0.12;
    if (indicator_thickness < 2.0) indicator_thickness = 2.0;
    double indicator_lead_gap = 0;
    double indicator_offsets[4] = {0};
    double indicator_min_off = 0.0;
    double indicator_max_off = 0.0;
    if (use_triplet) {
        for (int i = 0; i < indicator_len; ++i) {
            if (i == 2) {
                indicator_offsets[i] = indicator_offsets[1] - (indicator_gap + indicator_thickness) +
                                       (PM_INDICATOR_P_OFFSETS[2] * indicator_gap);
            } else {
                indicator_offsets[i] = PM_INDICATOR_P_OFFSETS[i] * indicator_gap;
            }
            indicator_min_off = fmin(indicator_min_off, indicator_offsets[i]);
            indicator_max_off = fmax(indicator_max_off, indicator_offsets[i]);
        }
    }
    bool leading_zero_hour = (hour < 10);
    double digits_total = leading_zero_hour
                          ? (3.0 * digit_width + 2.0 * gap + colon_width)
                          : (4.0 * digit_width + 3.0 * gap + colon_width);
    double indicator_section = 0.0;
    if (indicator_len > 0) {
        indicator_section = indicator_len * indicator_digit_width +
                            (indicator_len - 1) * indicator_gap;
    }
    double indicator_span_left = 0.0;
    double indicator_span_right = 0.0;
    if (indicator_len > 0) {
        indicator_span_left = -fmin(indicator_min_off, 0.0);
        indicator_span_right = fmax(indicator_max_off, 0.0);
    }
    double overlap_comp = (use_triplet && indicator_len == 3)
                          ? (indicator_gap + indicator_thickness)
                          : 0.0;
    double total_width = digits_total +
                         ((indicator_len > 0)
                              ? (indicator_lead_gap + indicator_section -
                                 overlap_comp +
                                 indicator_span_left + indicator_span_right)
                              : 0.0);

    if (total_width > available_width && total_width > 0.0) {
        double scale = available_width / total_width;
        digit_width *= scale;
        digit_height *= scale;
        gap *= scale;
        colon_width *= scale;
        indicator_gap = gap;
        indicator_digit_width = digit_width * 0.54;
        indicator_digit_height = digit_height * 0.65;
        indicator_thickness = indicator_digit_height * 0.12;
        if (indicator_thickness < 2.0) indicator_thickness = 2.0;
        indicator_lead_gap = indicator_thickness;
        indicator_min_off = 0.0;
        indicator_max_off = 0.0;
        if (use_triplet) {
            for (int i = 0; i < indicator_len; ++i) {
                if (i == 2) {
                    indicator_offsets[i] = indicator_offsets[1] - (indicator_gap + indicator_thickness) +
                                           (PM_INDICATOR_P_OFFSETS[2] * indicator_gap);
                } else {
                    indicator_offsets[i] = PM_INDICATOR_P_OFFSETS[i] * indicator_gap;
                }
                indicator_min_off = fmin(indicator_min_off, indicator_offsets[i]);
                indicator_max_off = fmax(indicator_max_off, indicator_offsets[i]);
            }
        }
        overlap_comp = (use_triplet && indicator_len == 3)
                       ? (indicator_gap + indicator_thickness)
                       : 0.0;
        indicator_span_left = indicator_len > 0 ? -fmin(indicator_min_off, 0.0) : 0.0;
        indicator_span_right = indicator_len > 0 ? fmax(indicator_max_off, 0.0) : 0.0;
        digits_total = leading_zero_hour
                       ? (3.0 * digit_width + 2.0 * gap + colon_width)
                       : (4.0 * digit_width + 3.0 * gap + colon_width);
        indicator_section = indicator_len > 0
                            ? indicator_len * indicator_digit_width +
                              (indicator_len - 1) * indicator_gap
                            : 0.0;
        total_width = digits_total +
                      ((indicator_len > 0)
                           ? (indicator_lead_gap + indicator_section -
                              overlap_comp +
                              indicator_span_left + indicator_span_right)
                           : 0.0);
    }

    double start_x = zone_x + margin + (available_width - total_width) / 2.0;
    if (start_x < zone_x + margin) start_x = zone_x + margin;
    double start_y = zone_y + (zone_h - digit_height) / 2.0;
    if (start_y < zone_y) start_y = zone_y;

    double gap_mid = gap + colon_width;
    double positions[4];
    positions[0] = start_x;
    if (leading_zero_hour) {
        positions[1] = positions[0] + digit_width + gap_mid;
        positions[2] = positions[1] + digit_width + gap;
        positions[3] = positions[2] + digit_width + gap;
    } else {
        positions[1] = positions[0] + digit_width + gap;
        positions[2] = positions[1] + digit_width + gap_mid;
        positions[3] = positions[2] + digit_width + gap;
    }

    unsigned char digits[4];
    digits[0] = (hour / 10) % 10;
    digits[1] = hour % 10;
    digits[2] = (minute / 10) % 10;
    digits[3] = minute % 10;

    if (leading_zero_hour) {
        draw_led_digit(cr, positions[0], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[1]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
        draw_led_digit(cr, positions[1], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[2]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
        draw_led_digit(cr, positions[2], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[3]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
    } else {
        for (int i = 0; i < 4; ++i) {
            draw_led_digit(cr, positions[i], start_y, digit_width, digit_height,
                           led_segmap_for_digit(digits[i]), on_r, on_g, on_b,
                           off_r, off_g, off_b);
        }
    }

    double gap_start = leading_zero_hour ? (positions[0] + digit_width)
                                         : (positions[1] + digit_width);
    double gap_end = leading_zero_hour ? positions[1] : positions[2];
    double colon_space = gap_end - gap_start;
    if (colon_space < colon_width) colon_space = colon_width;
    double colon_x = gap_start + (colon_space - colon_width) / 2.0;
    double colon_height = digit_height * 0.1;
    if (colon_height < 2.0) colon_height = 2.0;
    double colon_spacing = colon_height * 1.5;
    double colon_top_y = start_y + digit_height * 0.25;
    double colon_bottom_y = colon_top_y + colon_height + colon_spacing;

    if (show_colon) {
        cairo_set_source_rgb(cr, on_r, on_g, on_b);
        cairo_rectangle(cr, colon_x, colon_top_y, colon_width, colon_height);
        cairo_fill(cr);
        cairo_rectangle(cr, colon_x, colon_bottom_y, colon_width, colon_height);
        cairo_fill(cr);
    }

    if (indicator_len > 0) {
        double digits_width = digits_total;
        double indicator_x = start_x + digits_width + indicator_lead_gap +
                             indicator_span_left;
        double current_x = indicator_x;
        double indicator_y = zone_y + (zone_h - indicator_digit_height) / 2.0;
        if (indicator_y < zone_y) indicator_y = zone_y;
        for (int i = 0; i < indicator_len; ++i) {
            unsigned char segs = indicator_masks[i];
            double char_x = current_x + indicator_offsets[i];
            draw_led_digit(cr, char_x, indicator_y,
                           indicator_digit_width, indicator_digit_height,
                           segs, on_r, on_g, on_b,
                           off_r, off_g, off_b);
            current_x += indicator_digit_width + indicator_gap;
        }
    }
}

static void draw_calendar_paper(cairo_t *cr,
                                CkClockApp *app,
                                double cal_left,
                                double cal_top,
                                double cal_width,
                                double cal_height,
                                double paper_r,
                                double paper_g,
                                double paper_b,
                                double text_r,
                                double text_g,
                                double text_b,
                                double day_font,
                                double weekday_font,
                                double month_font)
{
    cairo_set_source_rgb(cr, paper_r, paper_g, paper_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_fill(cr);

    double spacing = cal_height * 0.05;
    if (spacing < 4.0) spacing = 4.0;
    double label_padding = cal_height * 0.02;
    if (label_padding < 2.0) label_padding = 2.0;

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    char day_buf[8];
    snprintf(day_buf, sizeof(day_buf), "%d", app->current_local_tm.tm_mday);
    cairo_set_font_size(cr, day_font);
    cairo_text_extents_t day_ext = {0};
    cairo_text_extents(cr, day_buf, &day_ext);

    double remaining_height = cal_height - day_ext.height - 2.0 * spacing - label_padding;
    if (remaining_height < 0.0) remaining_height = 0.0;
    double label_font = (remaining_height / 2.0) * 1.15;
    double label_max = day_font / 1.5;
    if (label_font > label_max) label_font = label_max;
    if (label_font < 10.0) label_font = 10.0;
    weekday_font = label_font;
    month_font = weekday_font;

    double center_x = cal_left + cal_width / 2.0;
    double space_remain = cal_height - day_ext.height;
    if (space_remain < 0.0) space_remain = 0.0;
    double half_space = space_remain / 2.0;
    double weekday_y = cal_top + half_space / 2.0;
    double day_y     = cal_top + half_space + day_ext.height / 2.0;
    double month_y   = cal_top + half_space + day_ext.height + half_space / 2.0;

    cairo_set_source_rgb(cr, text_r, text_g, text_b);
    cairo_set_font_size(cr, day_font);
    ck_clock_draw_centered_text(cr, day_buf, center_x, day_y, &day_ext, NULL);

    cairo_set_font_size(cr, weekday_font);
    ck_clock_draw_centered_text(cr,
                                ck_clock_weekday_labels[(app->current_local_tm.tm_wday + 7) % 7],
                                center_x, weekday_y,
                                NULL, NULL);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, month_font);
    ck_clock_draw_centered_text(cr,
                                ck_clock_month_labels[app->current_local_tm.tm_mon % 12],
                                center_x, month_y,
                                NULL, NULL);
}

static void ensure_time_cairo(CkClockApp *app)
{
    if (!app->time_widget || !XtIsRealized(app->time_widget)) return;
    Dimension w = 0;
    Dimension h = 0;
    XtVaGetValues(app->time_widget, XmNwidth, &w, XmNheight, &h, NULL);
    if (w == 0 || h == 0) {
        return;
    }
    if (app->time_cs && app->time_w == (int)w && app->time_h == (int)h) {
        return;
    }

    if (app->time_cr) {
        cairo_destroy(app->time_cr);
        app->time_cr = NULL;
    }
    if (app->time_cs) {
        cairo_surface_destroy(app->time_cs);
        app->time_cs = NULL;
    }

    Window win = XtWindow(app->time_widget);
    Visual *visual = DefaultVisual(app->dpy, app->screen);
    app->time_w = (int)w;
    app->time_h = (int)h;
    app->time_cs = cairo_xlib_surface_create(app->dpy, win, visual, app->time_w, app->time_h);
    app->time_cr = cairo_create(app->time_cs);

    cairo_set_antialias(app->time_cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(app->time_cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(app->time_cr, CAIRO_LINE_JOIN_ROUND);
}

void ck_time_view_render(cairo_t *cr, CkClockApp *app, double width, double height)
{
    if (!cr || !app || !app->have_local_time) return;

    double bg_r, bg_g, bg_b;
    double fg_r, fg_g, fg_b;
    double ts_r, ts_g, ts_b;
    double bs_r, bs_g, bs_b;
    ck_clock_pixel_to_rgb(app, app->bg_pixel,  &bg_r,  &bg_g,  &bg_b);
    ck_clock_pixel_to_rgb(app, app->fg_pixel,  &fg_r,  &fg_g,  &fg_b);
    ck_clock_pixel_to_rgb(app, app->ts_pixel,  &ts_r,  &ts_g,  &ts_b);
    ck_clock_pixel_to_rgb(app, app->bs_pixel,  &bs_r,  &bs_g,  &bs_b);

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    double time_area_h = height / 3.0;

    double led_on_r = 0.4;
    double led_on_g = 0.95;
    double led_on_b = 0.55;
    const double LED_OFF_SCALE = 0.22;
    const double LED_OFF_BIAS = 0.02;
    double led_off_r = ck_clock_clamp01(fg_r * LED_OFF_SCALE + LED_OFF_BIAS);
    double led_off_g = ck_clock_clamp01(fg_g * LED_OFF_SCALE + LED_OFF_BIAS);
    double led_off_b = ck_clock_clamp01(fg_b * LED_OFF_SCALE + LED_OFF_BIAS);

    int display_hour = app->current_local_tm.tm_hour;
    if (app->show_ampm_indicator) {
        if (display_hour == 0) {
            display_hour = 12;
        } else if (display_hour > 12) {
            display_hour -= 12;
        }
    }
    const char *ampm_indicator = app->show_ampm_indicator ? app->current_ampm : NULL;

    double time_pad = fmax(3.0, time_area_h * 0.08);
    double indent_offset = time_pad * 0.25;
    double inner_x = time_pad;
    double inner_y = time_pad + indent_offset;
    double inner_w = width - 2.0 * time_pad;
    double inner_h = time_area_h - time_pad - indent_offset;
    if (inner_h < 12.0) inner_h = 12.0;
    if (inner_w < 20.0) inner_w = 20.0;

    const double BAR_BG_FACTOR = 0.4;
    double bar_r = ck_clock_clamp01(bs_r * BAR_BG_FACTOR);
    double bar_g = ck_clock_clamp01(bs_g * BAR_BG_FACTOR);
    double bar_b = ck_clock_clamp01(bs_b * BAR_BG_FACTOR);
    cairo_set_source_rgb(cr, bar_r, bar_g, bar_b);
    cairo_rectangle(cr, inner_x, inner_y, inner_w, inner_h);
    cairo_fill(cr);

    double frame_width = 2.0;
    cairo_set_line_width(cr, frame_width);
    cairo_set_source_rgb(cr, bs_r, bs_g, bs_b);
    cairo_move_to(cr, inner_x, inner_y);
    cairo_line_to(cr, inner_x + inner_w, inner_y);
    cairo_move_to(cr, inner_x, inner_y);
    cairo_line_to(cr, inner_x, inner_y + inner_h);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, ts_r, ts_g, ts_b);
    cairo_move_to(cr, inner_x, inner_y + inner_h);
    cairo_line_to(cr, inner_x + inner_w, inner_y + inner_h);
    cairo_move_to(cr, inner_x + inner_w, inner_y);
    cairo_line_to(cr, inner_x + inner_w, inner_y + inner_h);
    cairo_stroke(cr);

    draw_led_time(cr,
                  display_hour,
                  app->current_local_tm.tm_min,
                  app->show_colon,
                  inner_x,
                  inner_y,
                  inner_w,
                  inner_h,
                  led_on_r, led_on_g, led_on_b,
                  led_off_r, led_off_g, led_off_b,
                  ampm_indicator);

    double cal_margin = width * 0.05;
    if (cal_margin < 6.0) cal_margin = 6.0;
    double cal_top = time_area_h + cal_margin * 0.4;
    double cal_height = height - cal_top - cal_margin;
    if (cal_height <= 12.0) return;
    double cal_max_width = width - 2.0 * cal_margin;
    if (cal_max_width <= 12.0) return;

    double weekday_font = cal_height * 0.1725;
    if (weekday_font < 10.0) weekday_font = 10.0;
    double day_font = cal_height * 0.55;
    if (day_font < weekday_font * 1.5) day_font = weekday_font * 1.5;
    if (day_font > cal_height * 0.7) day_font = cal_height * 0.7;
    double desired_width = day_font * 2.0;
    if (desired_width < 48.0) desired_width = 48.0;
    double cal_width = fmin(cal_max_width, desired_width);
    if (cal_width <= 12.0) return;
    double cal_left = (width - cal_width) / 2.0;

    double shadow_offset = width * 0.015;
    if (shadow_offset < 2.0) shadow_offset = 2.0;
    double shadow_offset_x = shadow_offset * 2.0;
    double shadow_offset_y = shadow_offset * 2.0;
    cairo_set_source_rgb(cr,
                         ck_clock_clamp01(bs_r * 0.85 + bg_r * 0.15),
                         ck_clock_clamp01(bs_g * 0.85 + bg_g * 0.15),
                         ck_clock_clamp01(bs_b * 0.85 + bg_b * 0.15));
    cairo_rectangle(cr, cal_left + shadow_offset_x,
                    cal_top + shadow_offset_y,
                    cal_width, cal_height);
    cairo_fill(cr);

    double paper_r = 1.0;
    double paper_g = 1.0;
    double paper_b = 1.0;
    cairo_set_source_rgb(cr, paper_r, paper_g, paper_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.5);
    double paper_fg_r, paper_fg_g, paper_fg_b;
    ck_clock_choose_contrast_color(paper_r, paper_g, paper_b, fg_r, fg_g, fg_b,
                                   &paper_fg_r, &paper_fg_g, &paper_fg_b);
    cairo_set_source_rgb(cr, paper_fg_r, paper_fg_g, paper_fg_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_stroke(cr);

    double month_font = weekday_font;
    draw_calendar_paper(cr,
                        app,
                        cal_left,
                        cal_top,
                        cal_width,
                        cal_height,
                        paper_r,
                        paper_g,
                        paper_b,
                        paper_fg_r,
                        paper_fg_g,
                        paper_fg_b,
                        day_font,
                        weekday_font,
                        month_font);
}

static void time_view_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    CkClockApp *app = (CkClockApp *)client_data;
    if (!app || !event) return;
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0) {
            ck_time_view_draw(app);
        }
        break;
    case ConfigureNotify:
        ck_time_view_draw(app);
        break;
    default:
        break;
    }
}

Widget ck_time_view_create(CkClockApp *app, Widget parent)
{
    if (!app || !parent) return NULL;
    app->time_widget = XmCreateDrawingArea(parent, (char *)"timeView", NULL, 0);
    XtManageChild(app->time_widget);
    XtAddEventHandler(app->time_widget,
                      ExposureMask | StructureNotifyMask,
                      False,
                      time_view_event_handler,
                      app);
    return app->time_widget;
}

void ck_time_view_update_layout(CkClockApp *app, const CkLayout *layout)
{
    if (!app || !layout || !app->time_widget) return;

    if (!layout->split_mode) {
        XtVaSetValues(app->time_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      NULL);
    } else {
        XtVaSetValues(app->time_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_NONE,
                      XmNwidth, (Dimension)layout->left_w,
                      NULL);
    }
}

void ck_time_view_draw(CkClockApp *app)
{
    if (!app || !app->time_widget || !app->have_local_time) return;
    if (!XtIsRealized(app->time_widget)) return;

    ensure_time_cairo(app);
    if (!app->time_cr) return;

    ck_time_view_render(app->time_cr, app, app->time_w, app->time_h);
    cairo_surface_flush(app->time_cs);
}
