"""Tkinter fallback overlay.

Tk X11'de gerçek per-pixel alfa vermez. Bu yüzden her metin bölgesi için ayrı,
küçük, yarı saydam bir üst pencere oluşturulur; tüm ekran karartılmaz. GTK
backend'i yoksa devreye girer. Faz 0'ın "Tkinter overlay" hedefini karşılar.
"""
from __future__ import annotations

import logging
import tkinter as tk
from typing import Optional

from .config import OverlayConfig
from .types import OverlayItem, OverlayModel
from .x11util import set_input_shape_empty

log = logging.getLogger(__name__)


class TkOverlay:
    name = "tk"

    def __init__(self, cfg: OverlayConfig, models, stop_event):
        self.cfg = cfg
        self.models = models
        self.stop_event = stop_event
        self.root: Optional[tk.Tk] = None
        self._version = -1
        self._windows: dict[int, tk.Toplevel] = {}
        self._labels: dict[int, tk.Label] = {}

    def init(self) -> None:
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title("GameFrame Translator")
        log.info("Tk overlay hazır (fallback)")

    def _make_window(self, item: OverlayItem) -> tk.Toplevel:
        assert self.root is not None
        top = tk.Toplevel(self.root)
        top.withdraw()
        top.overrideredirect(True)
        top.attributes("-topmost", True)
        try:
            top.attributes("-alpha", 0.88)
        except tk.TclError:
            pass
        top.configure(bg=self.cfg.bg_color)
        label = tk.Label(
            top,
            text=item.text,
            fg=self.cfg.text_color,
            bg=self.cfg.bg_color,
            font=(self.cfg.font_family, max(self.cfg.min_font_size, self.cfg.font_size)),
            justify="left",
            anchor="nw",
            wraplength=max(120, int(item.rect.w * self.cfg.max_width_scale)),
            padx=self.cfg.padding,
            pady=max(2, self.cfg.padding // 2),
        )
        label.pack(fill="both", expand=True)
        top.update_idletasks()
        w = max(item.rect.w, min(900, label.winfo_reqwidth() + 2 * self.cfg.padding))
        h = max(item.rect.h, label.winfo_reqheight() + self.cfg.padding)
        x = max(0, item.rect.x)
        y = max(0, item.rect.y)
        top.geometry(f"{w}x{h}+{x}+{y}")
        top.deiconify()
        top.update_idletasks()
        if self.cfg.click_through:
            set_input_shape_empty(top.winfo_id())
        self._windows[item.region_id] = top
        self._labels[item.region_id] = label
        return top

    def _update(self, model: Optional[OverlayModel]) -> None:
        if self.root is None or model is None:
            return
        active: set[int] = set()
        for item in model.items:
            active.add(item.region_id)
            top = self._windows.get(item.region_id)
            label = self._labels.get(item.region_id)
            if top is None or label is None:
                top = self._make_window(item)
                label = self._labels[item.region_id]
            else:
                if label.cget("text") != item.text:
                    label.configure(text=item.text)
                w = max(item.rect.w, min(900, label.winfo_reqwidth() + 2 * self.cfg.padding))
                h = max(item.rect.h, label.winfo_reqheight() + self.cfg.padding)
                top.geometry(f"{w}x{h}+{max(0, item.rect.x)}+{max(0, item.rect.y)}")
                if self.cfg.click_through:
                    set_input_shape_empty(top.winfo_id())
        for rid in list(self._windows):
            if rid not in active:
                try:
                    self._windows[rid].destroy()
                except Exception:
                    pass
                self._windows.pop(rid, None)
                self._labels.pop(rid, None)

    def _tick(self) -> None:
        if self.stop_event.is_set():
            self.shutdown()
            return
        model, version = self.models.get()
        if version != self._version:
            self._version = version
            try:
                self._update(model)
            except Exception as exc:
                log.warning("Tk overlay güncelleme hatası: %s", exc)
        if self.root is not None:
            self.root.after(50, self._tick)

    def run(self) -> None:
        if self.root is None:
            self.init()
        assert self.root is not None
        self.root.after(50, self._tick)
        self.root.mainloop()

    def shutdown(self) -> None:
        for top in list(self._windows.values()):
            try:
                top.destroy()
            except Exception:
                pass
        self._windows.clear()
        self._labels.clear()
        if self.root is not None:
            try:
                self.root.destroy()
            except Exception:
                pass
            self.root = None
