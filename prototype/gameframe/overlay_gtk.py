"""GTK3 tabanlı şeffaf, tıklama geçirgen overlay.

Neden GTK? Tk'nin X11'de pencere başına gerçek alfa desteği yok; GTK3 RGBA
visual + cairo ile metin arkası yarı saydam kutular çizebiliyor. Faz 2'de
Wayland layer-shell backend'i eklenecek; bu prototip X11/XWayland üzerinde
çalışır.
"""
from __future__ import annotations

import logging
import os
from typing import Callable, Optional

from .config import OverlayConfig
from .types import OverlayModel, OverlayItem, Rect

log = logging.getLogger(__name__)

# XWayland üzerinden X11 overlay; native Wayland pencerelerinin üstüne çıkmayı
# garanti etmez. Native için Faz 2: wlr-layer-shell.
if os.environ.get("GDK_BACKEND") is None and os.environ.get("DISPLAY"):
    os.environ["GDK_BACKEND"] = "x11"

import cairo  # noqa: E402
import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Pango", "1.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Gdk, GLib, Gtk, Pango, PangoCairo  # noqa: E402


class OverlayError(RuntimeError):
    pass


def _hex_rgb(value: str, fallback: tuple[float, float, float]) -> tuple[float, float, float]:
    value = (value or "").strip().lstrip("#")
    if len(value) == 3:
        value = "".join(ch * 2 for ch in value)
    if len(value) != 6:
        return fallback
    try:
        return tuple(int(value[i : i + 2], 16) / 255.0 for i in (0, 2, 4))  # type: ignore[return-value]
    except ValueError:
        return fallback


def _round_rect(cr: cairo.Context, x: float, y: float, w: float, h: float, r: float) -> None:
    r = max(0.0, min(r, w / 2.0, h / 2.0))
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -1.5707963267948966, 0)
    cr.arc(x + w - r, y + h - r, r, 0, 1.5707963267948966)
    cr.arc(x + r, y + h - r, r, 1.5707963267948966, 3.141592653589793)
    cr.arc(x + r, y + r, r, 3.141592653589793, 4.71238898038469)
    cr.close_path()


class GtkOverlay:
    name = "gtk"

    def __init__(
        self,
        cfg: OverlayConfig,
        models,
        stop_event,
    ):
        self.cfg = cfg
        self.models = models
        self.stop_event = stop_event
        self._model: Optional[OverlayModel] = None
        self._version = -1
        self._screen_w = 1920
        self._screen_h = 1080
        self.win: Optional[Gtk.Window] = None

    # ------------------------------------------------------------------
    def init(self) -> None:
        display = Gdk.Display.get_default()
        if display is None:
            raise OverlayError("GTK display yok (DISPLAY/WAYLAND_DISPLAY kontrol edin)")
        monitor = None
        try:
            n = display.get_n_monitors()
            idx = max(0, min(int(self.cfg.monitor) - 1, n - 1))
            monitor = display.get_monitor(idx)
        except Exception:
            monitor = None
        if monitor is not None:
            geo = monitor.get_geometry()
            self._screen_w, self._screen_h = geo.width, geo.height
        else:
            self._screen_w = display.get_default_screen().get_width()
            self._screen_h = display.get_default_screen().get_height()

        win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
        win.set_title("GameFrame Translator Overlay")
        win.set_decorated(False)
        win.set_resizable(False)
        win.set_skip_taskbar_hint(True)
        win.set_skip_pager_hint(True)
        win.set_keep_above(True)
        win.set_accept_focus(False)
        win.set_focus_on_map(False)
        try:
            win.set_type_hint(Gdk.WindowTypeHint.UTILITY)
        except Exception:
            pass
        screen = win.get_screen()
        visual = screen.get_rgba_visual()
        if visual is not None:
            win.set_visual(visual)
        win.set_app_paintable(True)
        win.set_default_size(self._screen_w, self._screen_h)
        win.move(0, 0)
        win.connect("draw", self._on_draw)
        win.connect("realize", self._on_realize)
        win.connect("map-event", self._on_map)
        win.connect("delete-event", lambda *_: True)
        self.win = win
        win.show_all()
        self._apply_click_through()
        log.info("GTK overlay hazır: %dx%d (X11=%s)", self._screen_w, self._screen_h, os.environ.get("GDK_BACKEND"))

    def _on_realize(self, *_args) -> None:
        self._apply_click_through()

    def _on_map(self, *_args) -> bool:
        self._apply_click_through()
        return False

    def _apply_click_through(self) -> None:
        if not self.cfg.click_through or self.win is None:
            return
        gdk_win = self.win.get_window()
        if gdk_win is None:
            return
        try:
            # Boş input region: tüm pencere fare/klavye olaylarını geçirir.
            gdk_win.input_shape_combine_region(cairo.Region(), 0, 0)
        except Exception as exc:
            log.warning("GTK input shape uygulanamadı: %s", exc)

    # ------------------------------------------------------------------
    def _on_draw(self, _widget, cr: cairo.Context) -> bool:
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(0.0, 0.0, 0.0, 0.0)
        cr.paint()
        cr.set_operator(cairo.OPERATOR_OVER)
        model = self._model
        self._occupied = []
        if model is not None:
            for item in model.items:
                try:
                    self._draw_item(cr, item)
                except Exception as exc:
                    log.warning("Overlay çizim hatası: %s", exc)
            if self.cfg.debug:
                self._draw_debug(cr, model)
        return False

    def _layout(self, cr, text: str, width: int, size: int):
        layout = PangoCairo.create_layout(cr)
        desc = Pango.FontDescription.from_string(f"{self.cfg.font_family} {int(size)}")
        layout.set_font_description(desc)
        layout.set_text(text, -1)
        layout.set_width(max(32, width) * Pango.SCALE)
        layout.set_wrap(Pango.WrapMode.WORD_CHAR)
        layout.set_ellipsize(Pango.EllipsizeMode.NONE)
        return layout

    def _draw_item(self, cr: cairo.Context, item: OverlayItem) -> None:
        pad = max(2, int(self.cfg.padding))
        # Orijinal metin alanına yakın kutu: konum ve boyut "tam otursun".
        base_w = max(24, int(item.rect.w))
        base_h = max(14, int(item.rect.h))
        max_w = int(min(max(base_w * self.cfg.max_width_scale, base_w + 2 * pad), self._screen_w - item.rect.x - 8))
        max_w = max(base_w, max_w)
        max_h = int(min(max(base_h * 1.6, base_h + 2 * pad), self._screen_h - item.rect.y - 8))
        max_h = max(base_h, max_h)

        inner_w = max(20, max_w - 2 * pad)
        inner_h = max(10, max_h - 2 * pad)
        size = min(self.cfg.font_size, max(self.cfg.min_font_size, int(base_h * 0.90)))

        layout = self._layout(cr, item.text, inner_w, size)
        tw, th = layout.get_pixel_size()
        # Önce yüksekliğe sığdırmak için fontu küçült.
        while th > inner_h and size > self.cfg.min_font_size:
            size -= 1
            layout = self._layout(cr, item.text, inner_w, size)
            tw, th = layout.get_pixel_size()

        source_layout = None
        source_h = 0
        if self.cfg.show_original and item.source_text:
            source_layout = self._layout(cr, item.source_text, inner_w, max(self.cfg.min_font_size, size - 2))
            _sw, source_h = source_layout.get_pixel_size()
            source_h += 2
            while th + source_h > inner_h and size > self.cfg.min_font_size:
                size -= 1
                layout = self._layout(cr, item.text, inner_w, size)
                tw, th = layout.get_pixel_size()
                source_layout = self._layout(cr, item.source_text, inner_w, max(self.cfg.min_font_size, size - 2))
                _sw, source_h = source_layout.get_pixel_size()
                source_h += 2

        # Kutu orijinal alanı kaplasın; gerektiğinde sınırlı büyüsün.
        box_w = min(max_w, max(base_w, tw + 2 * pad))
        box_h = min(max_h, max(base_h, th + source_h + 2 * pad))
        x = max(0, min(item.rect.x, self._screen_w - box_w - 4))
        y = max(0, min(item.rect.y, self._screen_h - box_h - 4))

        r, g, b = _hex_rgb(self.cfg.bg_color, (0.04, 0.05, 0.08))
        alpha = self.cfg.bg_alpha * (0.55 if item.stale else 1.0)
        cr.set_source_rgba(r, g, b, alpha)
        _round_rect(cr, x, y, box_w, box_h, 6)
        cr.fill_preserve()
        cr.set_source_rgba(0.55, 0.65, 0.80, 0.35 * (0.5 if item.stale else 1.0))
        cr.set_line_width(1.0)
        cr.stroke()

        ty = y + pad
        if source_layout is not None:
            cr.set_source_rgba(0.70, 0.75, 0.82, 0.80 if not item.stale else 0.45)
            cr.move_to(x + pad, ty)
            PangoCairo.show_layout(cr, source_layout)
            ty += source_h
        tr, tg, tb = _hex_rgb(self.cfg.text_color, (1.0, 1.0, 1.0))
        cr.set_source_rgba(tr, tg, tb, 1.0 if not item.stale else 0.55)
        cr.move_to(x + pad, ty)
        PangoCairo.show_layout(cr, layout)

    def _draw_debug(self, cr: cairo.Context, model: OverlayModel) -> None:
        cr.set_source_rgba(0.0, 1.0, 0.7, 0.35)
        cr.set_line_width(1.0)
        for item in model.items:
            cr.rectangle(item.rect.x, item.rect.y, item.rect.w, item.rect.h)
            cr.stroke()
        text = model.debug or "gft"
        layout = self._layout(cr, text, 900, 12)
        tw, th = layout.get_pixel_size()
        cr.set_source_rgba(0.0, 0.0, 0.0, 0.65)
        cr.rectangle(8, 8, tw + 12, th + 8)
        cr.fill()
        cr.set_source_rgba(0.7, 1.0, 0.85, 0.9)
        cr.move_to(14, 12)
        PangoCairo.show_layout(cr, layout)

    # ------------------------------------------------------------------
    def _tick(self) -> bool:
        model, version = self.models.get()
        if version != self._version:
            self._version = version
            self._model = model
            if self.win is not None:
                self.win.queue_draw()
        return True

    def _check_stop(self) -> bool:
        if self.stop_event.is_set():
            Gtk.main_quit()
            return False
        return True

    def run(self) -> None:
        if self.win is None:
            self.init()
        GLib.timeout_add(16, self._tick)
        GLib.timeout_add(100, self._check_stop)
        Gtk.main()

    def shutdown(self) -> None:
        if self.win is not None:
            try:
                self.win.destroy()
            except Exception:
                pass
            self.win = None
