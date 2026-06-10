#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/wait.h>

#define HISTORY_MAX 60

typedef struct {
    char date_str[16];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
} DailyStat;

typedef struct {
    double elapsed_seconds;
    char time_str[32];
    char interface[32];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    double peak_rx;
    double peak_tx;
    double avg_rx;
    double avg_tx;
} StatSnapshot;

typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    double peak_rx;
    double peak_tx;
    double sum_rx;
    double sum_tx;
    unsigned long tick_count;
    double start_time;
} IntervalState;

typedef struct {
    GtkWidget *window;
    GtkWidget *combo_iface;
    GtkWidget *lbl_rx_speed;
    GtkWidget *lbl_tx_speed;
    GtkWidget *lbl_rx_total;
    GtkWidget *lbl_tx_total;
    GtkWidget *lbl_duration;
    GtkWidget *lbl_peak_rx;
    GtkWidget *lbl_peak_tx;
    GtkWidget *lbl_avg_rx;
    GtkWidget *lbl_avg_up;
    GtkWidget *lbl_iface_val;
    GtkWidget *drawing_area;

    char current_interface[32];

    // Real-time speed calculations
    unsigned long long last_rx_bytes;
    unsigned long long last_tx_bytes;
    double last_time;

    // Totals since selecting the interface
    unsigned long long initial_rx_bytes;
    unsigned long long initial_tx_bytes;
    double selection_time;

    // Stat aggregates
    double peak_rx_speed;
    double peak_tx_speed;
    unsigned long long tick_count;
    double sum_rx_speed;
    double sum_tx_speed;

    // History for graph
    double rx_history[HISTORY_MAX];
    double tx_history[HISTORY_MAX];
    int history_len;

    // Limit UI elements
    GtkWidget *card_limit;
    GtkWidget *lbl_limit_val;
    GtkWidget *lbl_limit_total;

    // Limit state variables
    unsigned long long limit_mb;
    gboolean alarm_triggered;

    // Periodic statistics tracking
    GList *daily_stats;
    GList *snapshots_15;
    GList *snapshots_30;
    GList *snapshots_60;

    IntervalState intv15;
    IntervalState intv30;
    IntervalState intv60;

    double program_start_time;
    unsigned long long last_tick_rx;
    unsigned long long last_tick_tx;
    gboolean last_tick_valid;
} AppState;

void play_alarm_sound(void);
void on_set_limit_activated(GtkMenuItem *item, gpointer user_data);

// Format speed value in bytes/s to human readable
void format_speed(double bytes_per_sec, char *buf, size_t buf_size) {
    if (bytes_per_sec < 1024.0) {
        snprintf(buf, buf_size, "%.1f B/s", bytes_per_sec);
    } else if (bytes_per_sec < 1024.0 * 1024.0) {
        snprintf(buf, buf_size, "%.1f KB/s", bytes_per_sec / 1024.0);
    } else if (bytes_per_sec < 1024.0 * 1024.0 * 1024.0) {
        snprintf(buf, buf_size, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_size, "%.1f GB/s", bytes_per_sec / (1024.0 * 1024.0 * 1024.0));
    }
}

// Format total bytes to human readable
void format_size(unsigned long long bytes, char *buf, size_t buf_size) {
    double size = (double)bytes;
    if (size < 1024.0) {
        snprintf(buf, buf_size, "%llu B", bytes);
    } else if (size < 1024.0 * 1024.0) {
        snprintf(buf, buf_size, "%.2f KB", size / 1024.0);
    } else if (size < 1024.0 * 1024.0 * 1024.0) {
        snprintf(buf, buf_size, "%.2f MB", size / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_size, "%.2f GB", size / (1024.0 * 1024.0 * 1024.0));
    }
}

// Format time in seconds to HH:MM:SS
void format_duration(double duration_sec, char *buf, size_t buf_size) {
    int total_sec = (int)duration_sec;
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;
    snprintf(buf, buf_size, "%02d:%02d:%02d", h, m, s);
}

// Read interface list from /proc/net/dev
int get_interface_list(char list[][32], int max_count) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return 0;

    char line[512];
    // Skip headers
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }

    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < max_count) {
        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        
        char name[32];
        g_strlcpy(name, p, sizeof(name));
        
        // Strip trailing spaces
        char *end = name + strlen(name) - 1;
        while (end > name && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }

        // Add interface to list
        g_strlcpy(list[count], name, 32);
        count++;
    }
    fclose(fp);
    return count;
}

// Read stats for a single interface
int get_interface_stats(const char *target_iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return 0;

    char line[512];
    // Skip headers
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }

    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = '\0';
        char name[32];
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        
        g_strlcpy(name, p, sizeof(name));
        
        char *end = name + strlen(name) - 1;
        while (end > name && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }

        if (strcmp(name, target_iface) == 0) {
            unsigned long long rx = 0, tx = 0;
            unsigned long long dummy;
            // Scan columns: RX bytes is 1st, TX bytes is 9th
            if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 9) {
                *rx_bytes = rx;
                *tx_bytes = tx;
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

// Rounded-rectangle helper for clipping the plot area
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);
}

// Trace a smooth (Catmull-Rom -> cubic Bezier) curve through the series, then
// drop down to the baseline so the shape can be filled.
static void build_smooth_area(cairo_t *cr, const double *series, int len,
                              double dx, double max_speed, int height) {
    double px[HISTORY_MAX], py[HISTORY_MAX];
    if (len < 2) return;
    for (int i = 0; i < len; i++) {
        px[i] = i * dx;
        double y = height - (series[i] / max_speed) * height;
        if (y < 1.5) y = 1.5;
        if (y > height) y = height;
        py[i] = y;
    }

    cairo_move_to(cr, px[0], (double)height);
    cairo_line_to(cr, px[0], py[0]);
    for (int i = 0; i < len - 1; i++) {
        double x0 = px[i > 0 ? i - 1 : i],     y0 = py[i > 0 ? i - 1 : i];
        double x1 = px[i],                      y1 = py[i];
        double x2 = px[i + 1],                  y2 = py[i + 1];
        double x3 = px[i + 2 < len ? i + 2 : i + 1], y3 = py[i + 2 < len ? i + 2 : i + 1];
        double c1x = x1 + (x2 - x0) / 6.0, c1y = y1 + (y2 - y0) / 6.0;
        double c2x = x2 - (x3 - x1) / 6.0, c2y = y2 - (y3 - y1) / 6.0;
        cairo_curve_to(cr, c1x, c1y, c2x, c2y, x2, y2);
    }
    cairo_line_to(cr, px[len - 1], (double)height);
    cairo_close_path(cr);
}

// Cairo draw callback for speed graph
gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    AppState *state = (AppState *)data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    // Rounded clip so the plot reads as an inset panel
    rounded_rect(cr, 0.5, 0.5, width - 1.0, height - 1.0, 12.0);
    cairo_clip_preserve(cr);

    // Deep gradient backdrop
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.066, 0.072, 0.105);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.043, 0.047, 0.078);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    // Grid lines
    cairo_set_source_rgba(cr, 0.32, 0.36, 0.52, 0.22);
    cairo_set_line_width(cr, 1.0);
    int grid_rows = 4;
    for (int i = 1; i < grid_rows; i++) {
        double y = (height / (double)grid_rows) * i;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, width, y);
        cairo_stroke(cr);
    }
    int grid_cols = 6;
    for (int i = 1; i < grid_cols; i++) {
        double x = (width / (double)grid_cols) * i;
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, height);
        cairo_stroke(cr);
    }

    // Determine max speed scale dynamically
    double max_speed = 10240.0; // Min scale: 10 KB/s
    for (int i = 0; i < state->history_len; i++) {
        if (state->rx_history[i] > max_speed) max_speed = state->rx_history[i];
        if (state->tx_history[i] > max_speed) max_speed = state->tx_history[i];
    }
    max_speed *= 1.15; // 15% head room

    // Scale labels: top, midpoint, baseline
    cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    char scale_str[32];
    cairo_set_source_rgba(cr, 0.55, 0.58, 0.72, 0.9);
    format_speed(max_speed, scale_str, sizeof(scale_str));
    cairo_move_to(cr, 10, 16); cairo_show_text(cr, scale_str);
    format_speed(max_speed / 2.0, scale_str, sizeof(scale_str));
    cairo_move_to(cr, 10, height / 2.0 + 4); cairo_show_text(cr, scale_str);
    cairo_move_to(cr, 10, height - 9); cairo_show_text(cr, "0.0 B/s");

    double dx = width / (double)(HISTORY_MAX - 1);

    if (state->history_len > 1) {
        struct { const double *series; double r, g, b; } layers[2] = {
            { state->tx_history, 0.97, 0.46, 0.56 }, // upload  #f7768e (drawn first)
            { state->rx_history, 0.62, 0.81, 0.42 }, // download #9ece6a (on top)
        };

        for (int L = 0; L < 2; L++) {
            double r = layers[L].r, g = layers[L].g, b = layers[L].b;

            // Filled area with vertical gradient
            build_smooth_area(cr, layers[L].series, state->history_len, dx, max_speed, height);
            cairo_pattern_t *fill = cairo_pattern_create_linear(0, 0, 0, height);
            cairo_pattern_add_color_stop_rgba(fill, 0.0, r, g, b, 0.32);
            cairo_pattern_add_color_stop_rgba(fill, 1.0, r, g, b, 0.0);
            cairo_set_source(cr, fill);
            cairo_fill(cr);
            cairo_pattern_destroy(fill);

            // Re-trace just the top edge for the stroked line
            build_smooth_area(cr, layers[L].series, state->history_len, dx, max_speed, height);

            // Outer glow pass
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_source_rgba(cr, r, g, b, 0.30);
            cairo_set_line_width(cr, 6.0);
            cairo_stroke_preserve(cr);

            // Crisp neon line
            cairo_set_source_rgb(cr, r, g, b);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);

            // Glowing dot on the most recent sample
            int last = state->history_len - 1;
            double lx = last * dx;
            double ly = height - (layers[L].series[last] / max_speed) * height;
            if (ly < 1.5) ly = 1.5;
            if (ly > height) ly = height;
            cairo_set_source_rgba(cr, r, g, b, 0.25);
            cairo_arc(cr, lx, ly, 5.0, 0, 2 * G_PI); cairo_fill(cr);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_arc(cr, lx, ly, 2.5, 0, 2 * G_PI); cairo_fill(cr);
        }
    }

    // Subtle vignette + crisp rounded border to finish the frame
    cairo_reset_clip(cr);
    rounded_rect(cr, 0.5, 0.5, width - 1.0, height - 1.0, 12.0);
    cairo_set_source_rgba(cr, 0.48, 0.55, 0.86, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    return FALSE;
}

// Timer tick event
gboolean on_timer_tick(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (strlen(state->current_interface) == 0) return TRUE;

    unsigned long long current_rx = 0, current_tx = 0;
    if (!get_interface_stats(state->current_interface, &current_rx, &current_tx)) {
        return TRUE;
    }

    double current_time = g_get_monotonic_time() / 1000000.0;
    double delta_t = current_time - state->last_time;
    if (delta_t <= 0.0) delta_t = 1.0;

    double rx_speed = 0.0;
    double tx_speed = 0.0;

    if (current_rx >= state->last_rx_bytes) {
        rx_speed = (current_rx - state->last_rx_bytes) / delta_t;
    }
    if (current_tx >= state->last_tx_bytes) {
        tx_speed = (current_tx - state->last_tx_bytes) / delta_t;
    }

    // Daily Stats Accumulation
    if (!state->last_tick_valid) {
        state->last_tick_rx = current_rx;
        state->last_tick_tx = current_tx;
        state->last_tick_valid = TRUE;
    }
    unsigned long long delta_rx = 0;
    unsigned long long delta_tx = 0;
    if (current_rx >= state->last_tick_rx) delta_rx = current_rx - state->last_tick_rx;
    if (current_tx >= state->last_tick_tx) delta_tx = current_tx - state->last_tick_tx;
    state->last_tick_rx = current_rx;
    state->last_tick_tx = current_tx;

    time_t t_now = time(NULL);
    struct tm tm_now;
    localtime_r(&t_now, &tm_now);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);

    DailyStat *today_stat = NULL;
    for (GList *l = state->daily_stats; l != NULL; l = l->next) {
        DailyStat *ds = (DailyStat *)l->data;
        if (strcmp(ds->date_str, date_str) == 0) {
            today_stat = ds;
            break;
        }
    }
    if (!today_stat) {
        today_stat = g_new0(DailyStat, 1);
        g_strlcpy(today_stat->date_str, date_str, sizeof(today_stat->date_str));
        state->daily_stats = g_list_append(state->daily_stats, today_stat);
    }
    today_stat->rx_bytes += delta_rx;
    today_stat->tx_bytes += delta_tx;

    // Interval Stats Accumulation
    #define UPDATE_INTERVAL(intv, duration_sec, snapshot_list) \
    do { \
        (intv).rx_bytes += delta_rx; \
        (intv).tx_bytes += delta_tx; \
        if (rx_speed > (intv).peak_rx) (intv).peak_rx = rx_speed; \
        if (tx_speed > (intv).peak_tx) (intv).peak_tx = tx_speed; \
        (intv).sum_rx += rx_speed; \
        (intv).sum_tx += tx_speed; \
        (intv).tick_count++; \
        if (current_time - (intv).start_time >= (duration_sec)) { \
            StatSnapshot *snap = g_new0(StatSnapshot, 1); \
            snap->elapsed_seconds = current_time - state->program_start_time; \
            char time_buf[32]; \
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now); \
            g_strlcpy(snap->time_str, time_buf, sizeof(snap->time_str)); \
            g_strlcpy(snap->interface, state->current_interface, sizeof(snap->interface)); \
            snap->rx_bytes = (intv).rx_bytes; \
            snap->tx_bytes = (intv).tx_bytes; \
            snap->peak_rx = (intv).peak_rx; \
            snap->peak_tx = (intv).peak_tx; \
            snap->avg_rx = (intv).tick_count > 0 ? ((intv).sum_rx / (intv).tick_count) : 0.0; \
            snap->avg_tx = (intv).tick_count > 0 ? ((intv).sum_tx / (intv).tick_count) : 0.0; \
            (snapshot_list) = g_list_append((snapshot_list), snap); \
            memset(&(intv), 0, sizeof(IntervalState)); \
            (intv).start_time = current_time; \
        } \
    } while(0)

    UPDATE_INTERVAL(state->intv15, 900.0, state->snapshots_15);
    UPDATE_INTERVAL(state->intv30, 1800.0, state->snapshots_30);
    UPDATE_INTERVAL(state->intv60, 3600.0, state->snapshots_60);
    #undef UPDATE_INTERVAL

    // Totals since selecting the interface
    unsigned long long total_rx = 0;
    unsigned long long total_tx = 0;
    if (current_rx >= state->initial_rx_bytes) {
        total_rx = current_rx - state->initial_rx_bytes;
    }
    if (current_tx >= state->initial_tx_bytes) {
        total_tx = current_tx - state->initial_tx_bytes;
    }

    // Shifting array for history
    if (state->history_len < HISTORY_MAX) {
        state->rx_history[state->history_len] = rx_speed;
        state->tx_history[state->history_len] = tx_speed;
        state->history_len++;
    } else {
        for (int i = 1; i < HISTORY_MAX; i++) {
            state->rx_history[i - 1] = state->rx_history[i];
            state->tx_history[i - 1] = state->tx_history[i];
        }
        state->rx_history[HISTORY_MAX - 1] = rx_speed;
        state->tx_history[HISTORY_MAX - 1] = tx_speed;
    }

    // Peak stats
    if (rx_speed > state->peak_rx_speed) state->peak_rx_speed = rx_speed;
    if (tx_speed > state->peak_tx_speed) state->peak_tx_speed = tx_speed;

    state->tick_count++;
    state->sum_rx_speed += rx_speed;
    state->sum_tx_speed += tx_speed;

    state->last_rx_bytes = current_rx;
    state->last_tx_bytes = current_tx;
    state->last_time = current_time;

    // Formats and updates labels
    char speed_buf[64];
    format_speed(rx_speed, speed_buf, sizeof(speed_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_rx_speed), speed_buf);

    format_speed(tx_speed, speed_buf, sizeof(speed_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_tx_speed), speed_buf);

    char size_buf[64];
    char total_buf[128];

    format_size(total_rx, size_buf, sizeof(size_buf));
    snprintf(total_buf, sizeof(total_buf), "Session: %s", size_buf);
    gtk_label_set_text(GTK_LABEL(state->lbl_rx_total), total_buf);

    format_size(total_tx, size_buf, sizeof(size_buf));
    snprintf(total_buf, sizeof(total_buf), "Session: %s", size_buf);
    gtk_label_set_text(GTK_LABEL(state->lbl_tx_total), total_buf);

    if (state->limit_mb > 0) {
        unsigned long long limit_bytes = state->limit_mb * 1024ULL * 1024ULL;
        unsigned long long total_session = total_rx + total_tx;
        unsigned long long remaining_bytes = 0;
        if (limit_bytes > total_session) {
            remaining_bytes = limit_bytes - total_session;
        }

        format_size(remaining_bytes, size_buf, sizeof(size_buf));
        gtk_label_set_text(GTK_LABEL(state->lbl_limit_val), size_buf);

        snprintf(total_buf, sizeof(total_buf), "Total Limit: %llu MB", state->limit_mb);
        gtk_label_set_text(GTK_LABEL(state->lbl_limit_total), total_buf);

        GtkStyleContext *context = gtk_widget_get_style_context(state->lbl_limit_val);
        if (remaining_bytes == 0) {
            gtk_style_context_remove_class(context, "stat-val-limit");
            gtk_style_context_add_class(context, "stat-val-limit-exceeded");
            if (!state->alarm_triggered) {
                state->alarm_triggered = TRUE;
                play_alarm_sound();
            }
        } else {
            gtk_style_context_remove_class(context, "stat-val-limit-exceeded");
            gtk_style_context_add_class(context, "stat-val-limit");
        }
    }

    double duration = current_time - state->selection_time;
    char duration_buf[64];
    format_duration(duration, duration_buf, sizeof(duration_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_duration), duration_buf);

    char peak_buf[64];
    format_speed(state->peak_rx_speed, peak_buf, sizeof(peak_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_peak_rx), peak_buf);

    format_speed(state->peak_tx_speed, peak_buf, sizeof(peak_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_peak_tx), peak_buf);

    double avg_rx = state->sum_rx_speed / state->tick_count;
    double avg_tx = state->sum_tx_speed / state->tick_count;

    format_speed(avg_rx, peak_buf, sizeof(peak_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_avg_rx), peak_buf);

    format_speed(avg_tx, peak_buf, sizeof(peak_buf));
    gtk_label_set_text(GTK_LABEL(state->lbl_avg_up), peak_buf);

    // Queue draw to update the drawing area
    gtk_widget_queue_draw(state->drawing_area);

    return TRUE;
}

// Load saved interface from ~/.config/usage/usage.conf
void load_saved_interface(char *buf, size_t size) {
    buf[0] = '\0';
    const char *config_dir = g_get_user_config_dir();
    char *config_file = g_build_filename(config_dir, "usage", "usage.conf", NULL);
    FILE *fp = fopen(config_file, "r");
    if (fp) {
        if (fgets(buf, size, fp)) {
            char *end = buf + strlen(buf) - 1;
            while (end >= buf && isspace((unsigned char)*end)) {
                *end = '\0';
                end--;
            }
        }
        fclose(fp);
    }
    g_free(config_file);
}

// Save selected interface to ~/.config/usage/usage.conf
void save_selected_interface(const char *iface) {
    const char *config_dir = g_get_user_config_dir();
    char *config_path = g_build_filename(config_dir, "usage", NULL);
    g_mkdir_with_parents(config_path, 0755);
    char *config_file = g_build_filename(config_path, "usage.conf", NULL);
    FILE *fp = fopen(config_file, "w");
    if (fp) {
        fprintf(fp, "%s\n", iface);
        fclose(fp);
    }
    g_free(config_path);
    g_free(config_file);
}

// Load saved limit from ~/.config/usage/limit.conf
unsigned long long load_saved_limit(void) {
    const char *config_dir = g_get_user_config_dir();
    char *config_file = g_build_filename(config_dir, "usage", "limit.conf", NULL);
    FILE *fp = fopen(config_file, "r");
    unsigned long long limit = 0;
    if (fp) {
        if (fscanf(fp, "%llu", &limit) != 1) {
            limit = 0;
        }
        fclose(fp);
    }
    g_free(config_file);
    return limit;
}

// Save limit to ~/.config/usage/limit.conf
void save_limit(unsigned long long limit) {
    const char *config_dir = g_get_user_config_dir();
    char *config_path = g_build_filename(config_dir, "usage", NULL);
    g_mkdir_with_parents(config_path, 0755);
    char *config_file = g_build_filename(config_path, "limit.conf", NULL);
    FILE *fp = fopen(config_file, "w");
    if (fp) {
        fprintf(fp, "%llu\n", limit);
        fclose(fp);
    }
    g_free(config_path);
    g_free(config_file);
}

// Play alarm sound asynchronously
void play_alarm_sound(void) {
    if (!g_spawn_command_line_async("canberra-gtk-play --id=\"alarm-clock-elapsed\"", NULL)) {
        if (!g_spawn_command_line_async("pw-play /usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga", NULL)) {
            g_spawn_command_line_async("aplay /usr/share/sounds/sound-icons/prompt.wav", NULL);
        }
    }
}

// "Set limit" callback
void on_set_limit_activated(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    AppState *state = (AppState *)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Set Limit",
                                                    GTK_WINDOW(state->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_OK", GTK_RESPONSE_OK,
                                                    NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *label = gtk_label_new("Enter session limit in Megabytes (MB):\n(Set to 0 to disable)");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkAdjustment *adj = gtk_adjustment_new((double)state->limit_mb, 0.0, 10000000.0, 100.0, 1000.0, 0.0);
    GtkWidget *spin = gtk_spin_button_new(adj, 100.0, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), spin, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        double val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
        unsigned long long new_limit = (unsigned long long)val;
        state->limit_mb = new_limit;
        state->alarm_triggered = FALSE;
        save_limit(new_limit);

        if (new_limit > 0) {
            gtk_widget_show(state->card_limit);
            // Trigger an immediate UI update
            on_timer_tick(state);
        } else {
            gtk_widget_hide(state->card_limit);
        }
    }

    gtk_widget_destroy(dialog);
}

// Interface ComboBox change callback
void on_interface_changed(GtkComboBox *combo, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    char *selected = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (selected) {
        g_strlcpy(state->current_interface, selected, sizeof(state->current_interface));
        save_selected_interface(state->current_interface);
        g_free(selected);

        // Update display text for Interface Name in Grid
        gtk_label_set_text(GTK_LABEL(state->lbl_iface_val), state->current_interface);

        // Read new initial stats
        unsigned long long rx = 0, tx = 0;
        if (get_interface_stats(state->current_interface, &rx, &tx)) {
            state->last_rx_bytes = rx;
            state->last_tx_bytes = tx;
            state->initial_rx_bytes = rx;
            state->initial_tx_bytes = tx;
            state->last_tick_rx = rx;
            state->last_tick_tx = tx;
            state->last_tick_valid = TRUE;
        } else {
            state->last_rx_bytes = 0;
            state->last_tx_bytes = 0;
            state->initial_rx_bytes = 0;
            state->initial_tx_bytes = 0;
            state->last_tick_rx = 0;
            state->last_tick_tx = 0;
            state->last_tick_valid = FALSE;
        }

        state->last_time = g_get_monotonic_time() / 1000000.0;
        state->selection_time = state->last_time;

        // Reset statistics counters
        state->peak_rx_speed = 0.0;
        state->peak_tx_speed = 0.0;
        state->tick_count = 0;
        state->sum_rx_speed = 0.0;
        state->sum_tx_speed = 0.0;

        // Reset history
        memset(state->rx_history, 0, sizeof(state->rx_history));
        memset(state->tx_history, 0, sizeof(state->tx_history));
        state->history_len = 0;

        // Trigger updates and redraws
        gtk_widget_queue_draw(state->drawing_area);
        on_timer_tick(state);
    }
}

// "Stay on Top" toggle menu callback
void on_stay_on_top_toggled(GtkCheckMenuItem *menu_item, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    gboolean active = gtk_check_menu_item_get_active(menu_item);
    gtk_window_set_keep_above(GTK_WINDOW(state->window), active);
}

// "About" dialog callback
void on_about_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    AppState *state = (AppState *)user_data;
    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "Usage");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.0.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), "Real-time Internet Usage & Speed Monitor");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://github.com/user/usage");
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "usage");
    
    // License
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog), GTK_LICENSE_MIT_X11);
    
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(state->window));
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// Escape special LaTeX characters
void escape_latex(const char *src, char *dest, size_t dest_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dest_size - 4; i++) {
        if (src[i] == '%' || src[i] == '_' || src[i] == '&' || src[i] == '$' || src[i] == '#' || src[i] == '{' || src[i] == '}') {
            dest[j++] = '\\';
            dest[j++] = src[i];
        } else if (src[i] == '\\') {
            dest[j++] = '\\';
            dest[j++] = 't';
            dest[j++] = 'e';
            dest[j++] = 'x';
            dest[j++] = 't';
            dest[j++] = 'b';
            dest[j++] = 'a';
            dest[j++] = 'c';
            dest[j++] = 'k';
            dest[j++] = 's';
            dest[j++] = 'l';
            dest[j++] = 'a';
            dest[j++] = 's';
            dest[j++] = 'h';
            dest[j++] = '{';
            dest[j++] = '}';
        } else {
            dest[j++] = src[i];
        }
    }
    dest[j] = '\0';
}

// Write the statistical report in LaTeX format
void write_latex_report(AppState *state, const char *filepath) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) return;

    // Header and preamble
    fprintf(fp, "%% Generated by Usage Traffic Monitor\n");
    fprintf(fp, "\\documentclass[11pt,a4paper]{article}\n");
    fprintf(fp, "\\usepackage[utf8]{inputenc}\n");
    fprintf(fp, "\\usepackage[margin=1in]{geometry}\n");
    fprintf(fp, "\\usepackage{booktabs}\n");
    fprintf(fp, "\\usepackage{xcolor}\n");
    fprintf(fp, "\\usepackage{fancyhdr}\n");
    fprintf(fp, "\\usepackage{datetime}\n");
    fprintf(fp, "\\usepackage{microtype}\n");
    fprintf(fp, "\\usepackage{hyperref}\n");
    fprintf(fp, "\\usepackage{longtable}\n\n");

    // Colors styled after Tokyo Night theme
    fprintf(fp, "\\definecolor{tokyonightdark}{HTML}{1A1B26}\n");
    fprintf(fp, "\\definecolor{tokyonightblue}{HTML}{7AA2F7}\n");
    fprintf(fp, "\\definecolor{tokyonightgreen}{HTML}{9ECE6A}\n");
    fprintf(fp, "\\definecolor{tokyonightred}{HTML}{F7768E}\n\n");

    // Page style
    fprintf(fp, "\\pagestyle{fancy}\n");
    fprintf(fp, "\\fancyhf{}\n");
    fprintf(fp, "\\setlength{\\headheight}{15pt}\n");
    fprintf(fp, "\\lhead{\\textcolor{tokyonightblue}{\\bfseries Usage Monitor Statistics}}\n");
    fprintf(fp, "\\rhead{\\thepage}\n");
    fprintf(fp, "\\cfoot{\\small Generated by Usage Monitor on \\today}\n\n");

    // Title and metadata
    fprintf(fp, "\\title{\\Huge\\bfseries\\textcolor{tokyonightdark}{Network Statistics \\& Bandwidth Report}}\n");
    fprintf(fp, "\\author{\\textbf{Usage Traffic Monitor}}\n");
    fprintf(fp, "\\date{\\today}\n\n");

    fprintf(fp, "\\begin{document}\n");
    fprintf(fp, "\\maketitle\n\n");

    fprintf(fp, "\\section{Executive Summary}\n");
    fprintf(fp, "This report contains the network usage statistics logged by the Usage Monitor program.\n");
    
    // Overall Stats
    double current_time = g_get_monotonic_time() / 1000000.0;
    double duration = current_time - state->selection_time;
    char dur_buf[64];
    format_duration(duration, dur_buf, sizeof(dur_buf));

    char iface_escaped[64];
    escape_latex(state->current_interface, iface_escaped, sizeof(iface_escaped));

    fprintf(fp, "\\begin{itemize}\n");
    fprintf(fp, "  \\item \\textbf{Monitored Interface:} %s\n", iface_escaped);
    fprintf(fp, "  \\item \\textbf{Total Duration:} %s\n", dur_buf);
    
    unsigned long long total_rx = 0, total_tx = 0;
    unsigned long long current_rx = 0, current_tx = 0;
    if (get_interface_stats(state->current_interface, &current_rx, &current_tx)) {
        if (current_rx >= state->initial_rx_bytes) total_rx = current_rx - state->initial_rx_bytes;
        if (current_tx >= state->initial_tx_bytes) total_tx = current_tx - state->initial_tx_bytes;
    }
    char rx_buf[64], tx_buf[64], total_buf[64];
    format_size(total_rx, rx_buf, sizeof(rx_buf));
    format_size(total_tx, tx_buf, sizeof(tx_buf));
    format_size(total_rx + total_tx, total_buf, sizeof(total_buf));
    
    char rx_peak[64], tx_peak[64], rx_avg[64], tx_avg[64];
    format_speed(state->peak_rx_speed, rx_peak, sizeof(rx_peak));
    format_speed(state->peak_tx_speed, tx_peak, sizeof(tx_peak));
    double avg_rx = state->tick_count > 0 ? (state->sum_rx_speed / state->tick_count) : 0.0;
    double avg_tx = state->tick_count > 0 ? (state->sum_tx_speed / state->tick_count) : 0.0;
    format_speed(avg_rx, rx_avg, sizeof(rx_avg));
    format_speed(avg_tx, tx_avg, sizeof(tx_avg));

    fprintf(fp, "  \\item \\textbf{Total Downloaded:} %s\n", rx_buf);
    fprintf(fp, "  \\item \\textbf{Total Uploaded:} %s\n", tx_buf);
    fprintf(fp, "  \\item \\textbf{Combined Traffic:} %s\n", total_buf);
    fprintf(fp, "  \\item \\textbf{Peak Download Speed:} %s\n", rx_peak);
    fprintf(fp, "  \\item \\textbf{Peak Upload Speed:} %s\n", tx_peak);
    fprintf(fp, "  \\item \\textbf{Average Download Speed:} %s\n", rx_avg);
    fprintf(fp, "  \\item \\textbf{Average Upload Speed:} %s\n", tx_avg);
    fprintf(fp, "\\end{itemize}\n\n");

    // 1. Daily usage table
    fprintf(fp, "\\section{Daily Data Usage}\n");
    fprintf(fp, "The following table records traffic aggregated per calendar day since the monitor started running:\\par\\vspace{0.5em}\n");
    if (state->daily_stats == NULL) {
        fprintf(fp, "\\textit{No daily statistics recorded yet.}\n");
    } else {
        fprintf(fp, "{\\small\n");
        fprintf(fp, "\\begin{longtable}{lrrr}\n");
        fprintf(fp, "\\toprule\n");
        fprintf(fp, "\\textbf{Date} & \\textbf{Downloaded} & \\textbf{Uploaded} & \\textbf{Total} \\\\\n");
        fprintf(fp, "\\midrule\n");
        fprintf(fp, "\\endhead\n");
        for (GList *l = state->daily_stats; l != NULL; l = l->next) {
            DailyStat *ds = (DailyStat *)l->data;
            char ds_rx[64], ds_tx[64], ds_tot[64];
            format_size(ds->rx_bytes, ds_rx, sizeof(ds_rx));
            format_size(ds->tx_bytes, ds_tx, sizeof(ds_tx));
            format_size(ds->rx_bytes + ds->tx_bytes, ds_tot, sizeof(ds_tot));
            fprintf(fp, "%s & %s & %s & %s \\\\\n", ds->date_str, ds_rx, ds_tx, ds_tot);
        }
        fprintf(fp, "\\bottomrule\n");
        fprintf(fp, "\\end{longtable}}\n");
    }
    fprintf(fp, "\n\n");

    // Helper macro to print snapshot tables
    #define PRINT_SNAP_TABLE(list, interval_name, intv_struct, show_ongoing) \
    do { \
        fprintf(fp, "\\section{%s-Minute Statistics}\n", (interval_name)); \
        fprintf(fp, "Traffic statistics recorded at %s-minute intervals:\\par\\vspace{0.5em}\n", (interval_name)); \
        if ((list) == NULL && !(show_ongoing)) { \
            fprintf(fp, "\\textit{No snapshots recorded yet (requires program to be open for %s minutes).}\n", (interval_name)); \
        } else { \
            fprintf(fp, "{\\small\n"); \
            fprintf(fp, "\\begin{longtable}{llrrrrr}\n"); \
            fprintf(fp, "\\toprule\n"); \
            fprintf(fp, "\\textbf{Timestamp} & \\textbf{Iface} & \\textbf{Down} & \\textbf{Up} & \\textbf{Peak Down} & \\textbf{Peak Up} & \\textbf{Avg Combined} \\\\\n"); \
            fprintf(fp, "\\midrule\n"); \
            fprintf(fp, "\\endhead\n"); \
            for (GList *l = (list); l != NULL; l = l->next) { \
                StatSnapshot *s = (StatSnapshot *)l->data; \
                char s_rx[64], s_tx[64], s_prx[64], s_ptx[64], s_avg[64]; \
                format_size(s->rx_bytes, s_rx, sizeof(s_rx)); \
                format_size(s->tx_bytes, s_tx, sizeof(s_tx)); \
                format_speed(s->peak_rx, s_prx, sizeof(s_prx)); \
                format_speed(s->peak_tx, s_ptx, sizeof(s_ptx)); \
                format_speed(s->avg_rx + s->avg_tx, s_avg, sizeof(s_avg)); \
                char time_esc[64], iface_esc[64]; \
                escape_latex(s->time_str, time_esc, sizeof(time_esc)); \
                escape_latex(s->interface, iface_esc, sizeof(iface_esc)); \
                fprintf(fp, "%s & %s & %s & %s & %s & %s & %s \\\\\n", time_esc, iface_esc, s_rx, s_tx, s_prx, s_ptx, s_avg); \
            } \
            if (show_ongoing) { \
                char s_rx[64], s_tx[64], s_prx[64], s_ptx[64], s_avg[64]; \
                format_size((intv_struct).rx_bytes, s_rx, sizeof(s_rx)); \
                format_size((intv_struct).tx_bytes, s_tx, sizeof(s_tx)); \
                format_speed((intv_struct).peak_rx, s_prx, sizeof(s_prx)); \
                format_speed((intv_struct).peak_tx, s_ptx, sizeof(s_ptx)); \
                double cur_avg_rx = (intv_struct).tick_count > 0 ? ((intv_struct).sum_rx / (intv_struct).tick_count) : 0.0; \
                double cur_avg_tx = (intv_struct).tick_count > 0 ? ((intv_struct).sum_tx / (intv_struct).tick_count) : 0.0; \
                format_speed(cur_avg_rx + cur_avg_tx, s_avg, sizeof(s_avg)); \
                char iface_esc[64]; \
                escape_latex(state->current_interface, iface_esc, sizeof(iface_esc)); \
                fprintf(fp, "\\hline\\textit{Ongoing*} & %s & %s & %s & %s & %s & %s \\\\\n", iface_esc, s_rx, s_tx, s_prx, s_ptx, s_avg); \
            } \
            fprintf(fp, "\\bottomrule\n"); \
            fprintf(fp, "\\end{longtable}}\n"); \
            if (show_ongoing) { \
                fprintf(fp, "\\par\\small\\textit{*Note: Ongoing interval captures statistics accumulated since the last checkpoint.}\\par\n"); \
            } \
        } \
        fprintf(fp, "\\par\\vspace{1em}\n"); \
    } while(0)

    PRINT_SNAP_TABLE(state->snapshots_15, "15", state->intv15, TRUE);
    PRINT_SNAP_TABLE(state->snapshots_30, "30", state->intv30, TRUE);
    PRINT_SNAP_TABLE(state->snapshots_60, "60", state->intv60, TRUE);
    #undef PRINT_SNAP_TABLE

    fprintf(fp, "\\end{document}\n");
    fclose(fp);
}

// "Save stats" button callback
void on_save_stats_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    AppState *state = (AppState *)user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save Statistics Report",
                                                    GTK_WINDOW(state->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Save", GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    gtk_file_chooser_set_current_name(chooser, "usage_report.tex");

    // Add filter for .tex files
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "LaTeX Source Files (*.tex)");
    gtk_file_filter_add_pattern(filter, "*.tex");
    gtk_file_chooser_add_filter(chooser, filter);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, "All Files");
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(chooser, filter_all);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(chooser);
        write_latex_report(state, filename);

        // Compile with pdflatex
        gchar *dirname = g_path_get_dirname(filename);
        gchar *cmd = g_strdup_printf("pdflatex -interaction=nonstopmode -output-directory=\"%s\" \"%s\"", dirname, filename);
        
        gint exit_status = 0;
        GError *err = NULL;
        gboolean success = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, &err);
        
        gboolean compiled_ok = FALSE;
        if (success) {
            if (WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0) {
                compiled_ok = TRUE;
            }
        }
        
        g_free(cmd);
        g_free(dirname);

        GtkWidget *msg_dialog;
        if (compiled_ok) {
            char pdf_path[512];
            g_strlcpy(pdf_path, filename, sizeof(pdf_path));
            char *dot = strrchr(pdf_path, '.');
            if (dot && strcmp(dot, ".tex") == 0) {
                strcpy(dot, ".pdf");
            } else {
                g_strlcat(pdf_path, ".pdf", sizeof(pdf_path));
            }

            msg_dialog = gtk_message_dialog_new(GTK_WINDOW(state->window),
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_INFO,
                                                GTK_BUTTONS_OK,
                                                "Report saved and compiled successfully!\n\nLaTeX Source: %s\nPDF Generated: %s",
                                                filename, pdf_path);
        } else {
            msg_dialog = gtk_message_dialog_new(GTK_WINDOW(state->window),
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_OK,
                                                "LaTeX source saved to: %s\n\nFailed to compile the report using pdflatex.\nMake sure pdflatex is installed and try again.",
                                                filename);
            if (err) {
                g_error_free(err);
            }
        }
        
        gtk_dialog_run(GTK_DIALOG(msg_dialog));
        gtk_widget_destroy(msg_dialog);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    AppState *state = g_new0(AppState, 1);
    state->limit_mb = load_saved_limit();
    state->alarm_triggered = FALSE;
    state->program_start_time = g_get_monotonic_time() / 1000000.0;
    state->intv15.start_time = state->program_start_time;
    state->intv30.start_time = state->program_start_time;
    state->intv60.start_time = state->program_start_time;
    state->last_tick_valid = FALSE;

    // Apply custom Tokyo Night themed stylesheet
    GtkCssProvider *css_provider = gtk_css_provider_new();
    const char *css =
        /* ---- Aurora · Tokyo Night Storm ---------------------------------- */
        "window {"
        "  background-image: radial-gradient(circle at 12% -8%, #2a2f45 0%, #1a1b26 42%),"
        "                    radial-gradient(circle at 110% 120%, #232a47 0%, rgba(26,27,38,0) 55%);"
        "  background-color: #16161e;"
        "  color: #a9b1d6;"
        "  font-family: 'Inter', 'Cantarell', sans-serif;"
        "}\n"
        ".main-container { padding: 18px; }\n"

        /* Glass cards with a soft inner highlight and depth shadow */
        ".card {"
        "  background-image: linear-gradient(145deg, rgba(41,46,66,0.96), rgba(31,35,53,0.96));"
        "  border: 1px solid rgba(122,162,247,0.16);"
        "  border-radius: 14px;"
        "  padding: 18px;"
        "  box-shadow: 0 8px 24px rgba(0,0,0,0.35);"
        "}\n"
        /* Color-coded accent edge per metric card */
        ".card-in    { border-left: 3px solid #9ece6a; }\n"
        ".card-out   { border-left: 3px solid #f7768e; }\n"
        ".card-limit { border-left: 3px solid #ff9e64; }\n"

        ".stat-title {"
        "  font-size: 11px; font-weight: 800; color: #6b74a0;"
        "  letter-spacing: 1.4px;"
        "}\n"
        ".stat-val {"
        "  font-size: 30px; font-weight: 800;"
        "  margin-top: 4px; margin-bottom: 4px;"
        "  text-shadow: 0 0 18px rgba(122,162,247,0.28);"
        "}\n"
        ".stat-val-in            { color: #9ece6a; text-shadow: 0 0 20px rgba(158,206,106,0.45); }\n"
        ".stat-val-out           { color: #f7768e; text-shadow: 0 0 20px rgba(247,118,142,0.45); }\n"
        ".stat-val-limit         { color: #ff9e64; text-shadow: 0 0 20px rgba(255,158,100,0.45); }\n"
        ".stat-val-limit-exceeded{ color: #f7768e; text-shadow: 0 0 22px rgba(247,118,142,0.65); }\n"
        ".stat-sub { font-size: 12px; color: #7e85ad; font-weight: 600; }\n"

        ".graph-title {"
        "  font-size: 12px; font-weight: 800; color: #7aa2f7;"
        "  letter-spacing: 1.6px; margin-bottom: 8px;"
        "}\n"
        ".grid-label-title { font-size: 11px; font-weight: 800; color: #6b74a0; letter-spacing: 1px; }\n"
        ".grid-label-val   { font-size: 15px; font-weight: 700; color: #c0caf5; }\n"

        "headerbar {"
        "  background-image: linear-gradient(to bottom, #2a2f45, #1d2030);"
        "  border-bottom: 1px solid rgba(122,162,247,0.25);"
        "  box-shadow: 0 2px 12px rgba(0,0,0,0.4);"
        "  padding: 8px 10px;"
        "}\n"
        "headerbar .title { font-weight: 800; color: #c0caf5; letter-spacing: 0.4px; }\n"
        "headerbar .subtitle { color: #7aa2f7; font-weight: 600; }\n"

        "combobox {"
        "  background-image: linear-gradient(145deg, #2a2f45, #24283b);"
        "  border: 1px solid rgba(122,162,247,0.28); color: #c0caf5;"
        "  border-radius: 9px; padding: 2px 6px;"
        "}\n"
        "combobox:hover { border-color: #7aa2f7; }\n"
        "combobox button { background: none; border: none; box-shadow: none; color: #c0caf5; }\n"

        "button {"
        "  background-image: linear-gradient(145deg, #2a2f45, #24283b);"
        "  border: 1px solid rgba(122,162,247,0.28); color: #c0caf5;"
        "  border-radius: 9px; padding: 4px 10px;"
        "  transition: all 160ms ease;"
        "}\n"
        "button:hover {"
        "  background-image: linear-gradient(145deg, #7aa2f7, #5a7fe0);"
        "  border-color: #7aa2f7; color: #16161e;"
        "  box-shadow: 0 0 16px rgba(122,162,247,0.5);"
        "}\n"
        "menu, .menu { background-color: #1f2335; border: 1px solid #414868; border-radius: 8px; }\n"
        "menuitem:hover { background-color: #7aa2f7; color: #16161e; }\n"
        "tooltip { background-color: #1f2335; color: #c0caf5; border: 1px solid #414868; }\n";

    gtk_css_provider_load_from_data(css_provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    // Create primary window
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "Usage");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 680, 500);
    g_signal_connect(state->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Start horizontally centered, flush against the top of the screen
    {
        GdkDisplay *disp = gdk_display_get_default();
        GdkMonitor *mon = disp ? gdk_display_get_primary_monitor(disp) : NULL;
        if (!mon && disp) mon = gdk_display_get_monitor(disp, 0);
        if (mon) {
            GdkRectangle geo;
            gdk_monitor_get_workarea(mon, &geo);
            int win_w = 680, win_h = 500;
            gtk_window_get_default_size(GTK_WINDOW(state->window), &win_w, &win_h);
            // Cap height at 500px and clamp to the usable work area so the
            // window never spills off-screen
            if (win_h > 500) win_h = 500;
            if (win_w > geo.width)  win_w = geo.width;
            if (win_h > geo.height) win_h = geo.height;
            gtk_window_set_default_size(GTK_WINDOW(state->window), win_w, win_h);
            int x = geo.x + (geo.width - win_w) / 2;
            if (x < geo.x) x = geo.x;
            gtk_window_move(GTK_WINDOW(state->window), x, geo.y);
        }
    }

    // Set application icons
    GError *icon_err = NULL;
    if (!gtk_window_set_icon_from_file(GTK_WINDOW(state->window), "/usr/share/icons/hicolor/256x256/apps/usage.png", &icon_err)) {
        g_clear_error(&icon_err);
        gtk_window_set_icon_from_file(GTK_WINDOW(state->window), "usage.png", NULL);
    }

    // Set up modern HeaderBar
    GtkWidget *headerbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(headerbar), "Usage");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(headerbar), "Live Bandwidth · Aurora");
    gtk_window_set_titlebar(GTK_WINDOW(state->window), headerbar);

    // Dropdown to select network interfaces
    state->combo_iface = gtk_combo_box_text_new();
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerbar), state->combo_iface);

    // Load saved interface
    char saved_iface[32];
    load_saved_interface(saved_iface, sizeof(saved_iface));
    int has_saved = (strlen(saved_iface) > 0);

    // Dropdown options populator
    char interfaces[16][32];
    int iface_count = get_interface_list(interfaces, 16);
    int default_idx = -1;
    int backup_idx = 0;
    for (int i = 0; i < iface_count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->combo_iface), interfaces[i]);
        if (has_saved && strcmp(interfaces[i], saved_iface) == 0) {
            default_idx = i;
        }
        // Set backup default to first interface that is not "lo"
        if (strcmp(interfaces[i], "lo") != 0 && backup_idx == 0) {
            backup_idx = i;
        }
    }
    if (default_idx == -1) {
        default_idx = (iface_count > 0) ? backup_idx : 0;
    }
    
    // Stay-on-top menu setup in HeaderBar
    GtkWidget *menu = gtk_menu_new();
    
    GtkWidget *stay_on_top_item = gtk_check_menu_item_new_with_label("Stay on Top");
    g_signal_connect(stay_on_top_item, "toggled", G_CALLBACK(on_stay_on_top_toggled), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), stay_on_top_item);

    GtkWidget *set_limit_item = gtk_menu_item_new_with_label("Set limit");
    g_signal_connect(set_limit_item, "activate", G_CALLBACK(on_set_limit_activated), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), set_limit_item);
    
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

    GtkWidget *about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(about_item, "activate", G_CALLBACK(on_about_clicked), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), about_item);
    
    gtk_widget_show_all(menu);

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(menu_btn), menu);
    GtkWidget *menu_icon = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(menu_btn), menu_icon);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerbar), menu_btn);

    // Create and add Save Stats button
    GtkWidget *save_btn = gtk_button_new();
    GtkWidget *save_icon = gtk_image_new_from_icon_name("document-save-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *save_label = gtk_label_new(" Save Stats ");
    GtkWidget *save_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(save_box), save_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(save_box), save_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(save_btn), save_box);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_stats_clicked), state);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerbar), save_btn);

    // Main vertical box layout
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(main_box), "main-container");
    gtk_container_add(GTK_CONTAINER(state->window), main_box);

    // Cards container (horizontal row at top)
    GtkWidget *cards_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(cards_box), TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), cards_box, FALSE, FALSE, 0);

    // Download Speed Card
    GtkWidget *card_rx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_rx), "card");
    gtk_style_context_add_class(gtk_widget_get_style_context(card_rx), "card-in");
    gtk_box_pack_start(GTK_BOX(cards_box), card_rx, TRUE, TRUE, 0);

    GtkWidget *lbl_rx_title = gtk_label_new("DOWNLOAD SPEED");
    gtk_widget_set_halign(lbl_rx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_rx_title), "stat-title");
    gtk_box_pack_start(GTK_BOX(card_rx), lbl_rx_title, FALSE, FALSE, 0);

    state->lbl_rx_speed = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_rx_speed, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_rx_speed), "stat-val");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_rx_speed), "stat-val-in");
    gtk_box_pack_start(GTK_BOX(card_rx), state->lbl_rx_speed, FALSE, FALSE, 0);

    state->lbl_rx_total = gtk_label_new("Session: 0.00 B");
    gtk_widget_set_halign(state->lbl_rx_total, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_rx_total), "stat-sub");
    gtk_box_pack_start(GTK_BOX(card_rx), state->lbl_rx_total, FALSE, FALSE, 0);

    // Upload Speed Card
    GtkWidget *card_tx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_tx), "card");
    gtk_style_context_add_class(gtk_widget_get_style_context(card_tx), "card-out");
    gtk_box_pack_start(GTK_BOX(cards_box), card_tx, TRUE, TRUE, 0);

    GtkWidget *lbl_tx_title = gtk_label_new("UPLOAD SPEED");
    gtk_widget_set_halign(lbl_tx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_tx_title), "stat-title");
    gtk_box_pack_start(GTK_BOX(card_tx), lbl_tx_title, FALSE, FALSE, 0);

    state->lbl_tx_speed = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_tx_speed, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_tx_speed), "stat-val");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_tx_speed), "stat-val-out");
    gtk_box_pack_start(GTK_BOX(card_tx), state->lbl_tx_speed, FALSE, FALSE, 0);

    state->lbl_tx_total = gtk_label_new("Session: 0.00 B");
    gtk_widget_set_halign(state->lbl_tx_total, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_tx_total), "stat-sub");
    gtk_box_pack_start(GTK_BOX(card_tx), state->lbl_tx_total, FALSE, FALSE, 0);

    // Limit Card
    state->card_limit = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->card_limit), "card");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->card_limit), "card-limit");
    gtk_box_pack_start(GTK_BOX(cards_box), state->card_limit, TRUE, TRUE, 0);

    GtkWidget *lbl_limit_title = gtk_label_new("LIMIT REMAINING");
    gtk_widget_set_halign(lbl_limit_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_limit_title), "stat-title");
    gtk_box_pack_start(GTK_BOX(state->card_limit), lbl_limit_title, FALSE, FALSE, 0);

    state->lbl_limit_val = gtk_label_new("0.00 B");
    gtk_widget_set_halign(state->lbl_limit_val, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_limit_val), "stat-val");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_limit_val), "stat-val-limit");
    gtk_box_pack_start(GTK_BOX(state->card_limit), state->lbl_limit_val, FALSE, FALSE, 0);

    state->lbl_limit_total = gtk_label_new("Total Limit: 0 MB");
    gtk_widget_set_halign(state->lbl_limit_total, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_limit_total), "stat-sub");
    gtk_box_pack_start(GTK_BOX(state->card_limit), state->lbl_limit_total, FALSE, FALSE, 0);

    // Initially hide if limit is not set
    if (state->limit_mb > 0) {
        gtk_widget_show_all(state->card_limit);
    } else {
        gtk_widget_hide(state->card_limit);
    }

    // Real-Time Graph Section
    GtkWidget *graph_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(graph_card), "card");
    gtk_box_pack_start(GTK_BOX(main_box), graph_card, TRUE, TRUE, 0);

    GtkWidget *lbl_graph_title = gtk_label_new("REAL-TIME BANDWIDTH HISTORY");
    gtk_widget_set_halign(lbl_graph_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_graph_title), "graph-title");
    gtk_box_pack_start(GTK_BOX(graph_card), lbl_graph_title, FALSE, FALSE, 0);

    state->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(state->drawing_area, -1, 120);
    g_signal_connect(G_OBJECT(state->drawing_area), "draw", G_CALLBACK(on_draw), state);
    gtk_box_pack_start(GTK_BOX(graph_card), state->drawing_area, TRUE, TRUE, 0);

    // Stats Grid Section
    GtkWidget *bottom_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bottom_card), "card");
    gtk_box_pack_start(GTK_BOX(main_box), bottom_card, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_container_add(GTK_CONTAINER(bottom_card), grid);

    // Column 0 - Info
    GtkWidget *lbl_iface_title = gtk_label_new("INTERFACE");
    gtk_widget_set_halign(lbl_iface_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_iface_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_iface_title, 0, 0, 1, 1);

    state->lbl_iface_val = gtk_label_new("-");
    gtk_widget_set_halign(state->lbl_iface_val, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_iface_val), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_iface_val, 0, 1, 1, 1);

    GtkWidget *lbl_dur_title = gtk_label_new("MONITORED TIME");
    gtk_widget_set_halign(lbl_dur_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_dur_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_dur_title, 0, 2, 1, 1);

    state->lbl_duration = gtk_label_new("00:00:00");
    gtk_widget_set_halign(state->lbl_duration, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_duration), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_duration, 0, 3, 1, 1);

    // Column 1 - Peaks
    GtkWidget *lbl_peak_rx_title = gtk_label_new("PEAK DOWNLOAD");
    gtk_widget_set_halign(lbl_peak_rx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_peak_rx_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_peak_rx_title, 1, 0, 1, 1);

    state->lbl_peak_rx = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_peak_rx, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_peak_rx), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_peak_rx, 1, 1, 1, 1);

    GtkWidget *lbl_peak_tx_title = gtk_label_new("PEAK UPLOAD");
    gtk_widget_set_halign(lbl_peak_tx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_peak_tx_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_peak_tx_title, 1, 2, 1, 1);

    state->lbl_peak_tx = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_peak_tx, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_peak_tx), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_peak_tx, 1, 3, 1, 1);

    // Column 2 - Averages
    GtkWidget *lbl_avg_rx_title = gtk_label_new("AVG DOWNLOAD");
    gtk_widget_set_halign(lbl_avg_rx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_avg_rx_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_avg_rx_title, 2, 0, 1, 1);

    state->lbl_avg_rx = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_avg_rx, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_avg_rx), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_avg_rx, 2, 1, 1, 1);

    GtkWidget *lbl_avg_tx_title = gtk_label_new("AVG UPLOAD");
    gtk_widget_set_halign(lbl_avg_tx_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_avg_tx_title), "grid-label-title");
    gtk_grid_attach(GTK_GRID(grid), lbl_avg_tx_title, 2, 2, 1, 1);

    state->lbl_avg_up = gtk_label_new("0.0 B/s");
    gtk_widget_set_halign(state->lbl_avg_up, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->lbl_avg_up), "grid-label-val");
    gtk_grid_attach(GTK_GRID(grid), state->lbl_avg_up, 2, 3, 1, 1);

    // Connect change handler and select default interface
    g_signal_connect(state->combo_iface, "changed", G_CALLBACK(on_interface_changed), state);
    if (iface_count > 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(state->combo_iface), default_idx);
    }

    gtk_widget_show_all(state->window);

    // Register 1-second timeout timer
    g_timeout_add(1000, on_timer_tick, state);

    // Launch GTK main loop
    gtk_main();

    g_free(state);
    return 0;
}
