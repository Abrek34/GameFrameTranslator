#include "overlay.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <cairo.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {

struct GtkOverlayData {
    const OverlayConfig* cfg;
    LatestModelSlot* models;
    std::atomic<bool>* stop;
    GtkWidget* win{nullptr};
    GdkVisual* visual{nullptr};
    int screenW{1920};
    int screenH{1080};
    OverlayModel model;
    uint64_t version{0};
    bool initialized{false};
};

static void parseColor(const std::string& hex, double& r, double& g, double& b) {
    std::string v = hex;
    if (!v.empty() && v[0] == '#') v.erase(0, 1);
    if (v.size() == 3) v = std::string() + v[0] + v[0] + v[1] + v[1] + v[2] + v[2];
    if (v.size() != 6) {
        r = 0.04; g = 0.05; b = 0.08;
        return;
    }
    unsigned int rr, gg, bb;
    if (std::sscanf(v.c_str(), "%02x%02x%02x", &rr, &gg, &bb) == 3) {
        r = rr / 255.0;
        g = gg / 255.0;
        b = bb / 255.0;
    } else {
        r = 0.04; g = 0.05; b = 0.08;
    }
}

static void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::max(0.0, std::min({r, w / 2.0, h / 2.0}));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static PangoLayout* makeLayout(GtkOverlayData* d, cairo_t* cr, const std::string& text, int width,
                               int size) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc =
        pango_font_description_from_string((d->cfg->fontFamily + " " + std::to_string(size)).c_str());
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_width(layout, std::max(32, width) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    return layout;
}

static gboolean onDraw(GtkWidget* /*widget*/, cairo_t* cr, gpointer user) {
    auto* d = static_cast<GtkOverlayData*>(user);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // copy model
    OverlayModel model;
    {
        auto [m, v] = d->models->get();
        model = m;
        d->version = v;
        d->model = model;
    }

    double bgR, bgG, bgB, txtR, txtG, txtB;
    parseColor(d->cfg->bgColor, bgR, bgG, bgB);
    parseColor(d->cfg->textColor, txtR, txtG, txtB);

    for (const auto& item : model.items) {
        int pad = std::max(2, d->cfg->padding);
        int maxW = std::min(std::max(static_cast<int>(item.rect.w * d->cfg->maxWidthScale), 160),
                            d->screenW - item.rect.x - 8);
        maxW = std::max(80, maxW);
        int y = std::max(0, item.rect.y);
        int maxH = std::max(60, std::min(d->screenH - y - 8, std::max(item.rect.h * 3, 140)));
        int size = std::max(d->cfg->minFontSize, d->cfg->fontSize);
        int innerW = maxW - 2 * pad;
        PangoLayout* layout = makeLayout(d, cr, item.text, innerW, size);
        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        while (th > maxH && size > d->cfg->minFontSize) {
            --size;
            g_object_unref(layout);
            layout = makeLayout(d, cr, item.text, innerW, size);
            pango_layout_get_pixel_size(layout, &tw, &th);
        }
        PangoLayout* sourceLayout = nullptr;
        int sourceH = 0;
        if (d->cfg->showOriginal && !item.sourceText.empty()) {
            sourceLayout = makeLayout(d, cr, item.sourceText, innerW,
                                     std::max(d->cfg->minFontSize, size - 4));
            int sw, sh;
            pango_layout_get_pixel_size(sourceLayout, &sw, &sh);
            sourceH = sh + 2;
        }
        int boxW = std::min(maxW, std::max(item.rect.w, tw + 2 * pad));
        int boxH = std::min(maxH, th + sourceH + 2 * pad);
        int x = std::max(0, std::min(item.rect.x, d->screenW - boxW - 4));
        y = std::max(0, std::min(y, d->screenH - boxH - 4));

        double alpha = d->cfg->bgAlpha * (item.stale ? 0.55 : 1.0);
        cairo_set_source_rgba(cr, bgR, bgG, bgB, alpha);
        roundedRect(cr, x, y, boxW, boxH, 6);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.55, 0.65, 0.80, 0.35 * (item.stale ? 0.5 : 1.0));
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        int ty = y + pad;
        if (sourceLayout) {
            cairo_set_source_rgba(cr, 0.70, 0.75, 0.82, item.stale ? 0.45 : 0.80);
            cairo_move_to(cr, x + pad, ty);
            pango_cairo_show_layout(cr, sourceLayout);
            ty += sourceH;
        }
        cairo_set_source_rgba(cr, txtR, txtG, txtB, item.stale ? 0.55 : 1.0);
        cairo_move_to(cr, x + pad, ty);
        pango_cairo_show_layout(cr, layout);

        g_object_unref(layout);
        if (sourceLayout) g_object_unref(sourceLayout);
    }

    if (d->cfg->debug) {
        cairo_set_source_rgba(cr, 0, 1, 0.7, 0.35);
        cairo_set_line_width(cr, 1.0);
        for (const auto& item : model.items) {
            cairo_rectangle(cr, item.rect.x, item.rect.y, item.rect.w, item.rect.h);
            cairo_stroke(cr);
        }
        // debug text
        PangoLayout* dl = makeLayout(d, cr, model.debug, 900, 12);
        int tw, th;
        pango_layout_get_pixel_size(dl, &tw, &th);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
        cairo_rectangle(cr, 8, 8, tw + 12, th + 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.7, 1, 0.85, 0.9);
        cairo_move_to(cr, 14, 12);
        pango_cairo_show_layout(cr, dl);
        g_object_unref(dl);
    }
    return FALSE;
}

static gboolean onTick(gpointer user) {
    auto* d = static_cast<GtkOverlayData*>(user);
    if (d->stop->load()) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    auto [m, v] = d->models->get();
    if (v != d->version) {
        d->version = v;
        d->model = m;
        if (d->win) gtk_widget_queue_draw(d->win);
    }
    return G_SOURCE_CONTINUE;
}

class GtkOverlayImpl : public Overlay {
public:
    GtkOverlayImpl(const OverlayConfig& cfg, LatestModelSlot& models, std::atomic<bool>& stop)
        : data_(new GtkOverlayData()) {
        data_->cfg = &cfg;
        data_->models = &models;
        data_->stop = &stop;
        cfg_ = cfg;
    }
    ~GtkOverlayImpl() override {
        if (data_->win) {
            gtk_widget_destroy(data_->win);
            data_->win = nullptr;
        }
        delete data_;
    }

    std::string name() const override { return "gtk"; }

    void init() override {
        if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("GTK başlatılamadı");
        GdkScreen* screen = gdk_screen_get_default();
        if (!screen) throw std::runtime_error("GDK screen yok");
        data_->visual = gdk_screen_get_rgba_visual(screen);
        if (!data_->visual) data_->visual = gdk_screen_get_system_visual(screen);

        // CSS şeffaf arka plan
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(provider, "window { background-color: transparent; }", -1,
                                       nullptr);
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider),
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        data_->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(data_->win), "GameFrame Translator Overlay");
        gtk_window_set_decorated(GTK_WINDOW(data_->win), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(data_->win), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(data_->win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(data_->win), TRUE);
        gtk_window_set_keep_above(GTK_WINDOW(data_->win), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(data_->win), FALSE);
        gtk_window_set_focus_on_map(GTK_WINDOW(data_->win), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(data_->win), GDK_WINDOW_TYPE_HINT_UTILITY);

        if (data_->visual) gtk_widget_set_visual(data_->win, data_->visual);
        gtk_widget_set_app_paintable(data_->win, TRUE);

        GdkRectangle geo;
        int monitor = std::max(0, cfg_.monitor - 1);
        if (monitor >= gdk_screen_get_n_monitors(screen)) monitor = 0;
        gdk_screen_get_monitor_geometry(screen, monitor, &geo);
        data_->screenW = geo.width;
        data_->screenH = geo.height;
        gtk_window_set_default_size(GTK_WINDOW(data_->win), geo.width, geo.height);
        gtk_window_move(GTK_WINDOW(data_->win), geo.x, geo.y);

        g_signal_connect(data_->win, "draw", G_CALLBACK(onDraw), data_);
        g_signal_connect(data_->win, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) {
                              return TRUE;
                          }),
                         nullptr);

        gtk_widget_show_all(data_->win);
        // click-through: boş input region
        if (cfg_.clickThrough) {
            GdkWindow* gdkwin = gtk_widget_get_window(data_->win);
            if (gdkwin) {
                cairo_region_t* empty = cairo_region_create();
                gdk_window_input_shape_combine_region(gdkwin, empty, 0, 0);
                cairo_region_destroy(empty);
            }
        }
        data_->initialized = true;
        GFT_INFO("overlay", "GTK overlay hazır: %dx%d", data_->screenW, data_->screenH);
    }

    void run() override {
        if (!data_->initialized) init();
        g_timeout_add(16, onTick, data_);
        gtk_main();
    }

    void shutdown() override {
        if (data_->win) {
            gtk_widget_destroy(data_->win);
            data_->win = nullptr;
        }
    }

private:
    OverlayConfig cfg_;
    GtkOverlayData* data_;
};

}  // namespace

std::unique_ptr<Overlay> createOverlay(const OverlayConfig& cfg, LatestModelSlot& models,
                                       std::atomic<bool>& stop) {
    if (cfg.backend == "none") return std::make_unique<NullOverlay>(models, stop);
    if (cfg.backend == "gtk" || cfg.backend == "auto") {
        return std::make_unique<GtkOverlayImpl>(cfg, models, stop);
    }
    GFT_WARN("overlay", "Tk backend C++ tarafında henüz yok; null kullanılıyor");
    return std::make_unique<NullOverlay>(models, stop);
}

}  // namespace gft
