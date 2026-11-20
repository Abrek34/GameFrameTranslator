"""Overlay fabrikası. GTK/Tk modülleri yalnızca seçilen backend için yüklenir."""
from __future__ import annotations

import logging

from .config import OverlayConfig

log = logging.getLogger(__name__)


class OverlayError(RuntimeError):
    pass


class NullOverlay:
    name = "none"

    def __init__(self, cfg, models, stop_event):
        self.stop_event = stop_event

    def init(self) -> None:
        pass

    def run(self) -> None:
        self.stop_event.wait()

    def shutdown(self) -> None:
        pass


def create_overlay(cfg: OverlayConfig, models, stop_event):
    backend = cfg.backend
    if backend == "none":
        return NullOverlay(cfg, models, stop_event)
    if backend == "gtk":
        from .overlay_gtk import GtkOverlay

        return GtkOverlay(cfg, models, stop_event)
    if backend == "tk":
        from .overlay_tk import TkOverlay

        return TkOverlay(cfg, models, stop_event)
    if backend == "auto":
        try:
            from .overlay_gtk import GtkOverlay

            return GtkOverlay(cfg, models, stop_event)
        except Exception as exc:
            log.warning("GTK overlay kullanılamadı (%s), Tk fallback", exc)
            from .overlay_tk import TkOverlay

            return TkOverlay(cfg, models, stop_event)
    raise OverlayError(f"Bilinmeyen overlay backend'i: {backend}")
