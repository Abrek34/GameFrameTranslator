"""Kare kaynakları.

Linux öncelikli:
- xcomposite: hedef oyun penceresini XComposite pixmap'inden yakalar. Overlay
  ayrı bir pencere olduğu için kareye karışmaz (Windows'taki
  WDA_EXCLUDEFROMCAPTURE'ın Linux tarafındaki karşılığı).
- mss: root/ekran yakalama. Hızlı prototip; overlay açıkken overlay'i de
  yakalayabileceği için üretimde önerilmez.
- synthetic: ekransız testler ve benchmark için sahte diyalog sahnesi.

Wayland native oyunlar için Faz 2'de Vulkan layer / PipeWire kaynağı eklenecek.
"""
from __future__ import annotations

import logging
import os
import threading
import time
from abc import ABC, abstractmethod
from typing import Callable, Optional

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from .config import AppConfig, CaptureConfig
from .types import Frame, Rect

log = logging.getLogger(__name__)

FrameSink = Callable[[Frame], None]


class CaptureError(RuntimeError):
    pass


class FrameSource(ABC):
    name = "base"

    def __init__(self, cfg: CaptureConfig):
        self.cfg = cfg
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._seq = 0
        self._last_error = ""
        self._stats_lock = threading.Lock()
        self._captured = 0
        self._fps = 0.0

    @abstractmethod
    def grab_once(self) -> Frame:
        raise NotImplementedError

    def start(self, sink: FrameSink) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, args=(sink,), name=f"capture-{self.name}", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3.0)
            self._thread = None

    def _run(self, sink: FrameSink) -> None:
        period = 1.0 / max(1, self.cfg.fps)
        next_t = time.monotonic()
        fps_t = time.monotonic()
        fps_n = 0
        while not self._stop.is_set():
            try:
                frame = self.grab_once()
                self._seq += 1
                frame.seq = self._seq
                with self._stats_lock:
                    self._captured += 1
                    fps_n += 1
                sink(frame)
                now = time.monotonic()
                if now - fps_t >= 1.0:
                    with self._stats_lock:
                        self._fps = fps_n / (now - fps_t)
                    fps_n = 0
                    fps_t = now
                next_t += period
                delay = next_t - time.monotonic()
                if delay > 0:
                    self._stop.wait(delay)
                elif delay < -1.0:
                    next_t = time.monotonic()
            except Exception as exc:  # capture thread must survive transient errors
                self._last_error = f"{type(exc).__name__}: {exc}"
                log.warning("Yakalama hatası (%s): %s", self.name, exc)
                self._stop.wait(0.5)

    @property
    def last_error(self) -> str:
        return self._last_error

    @property
    def fps(self) -> float:
        with self._stats_lock:
            return self._fps


# ---------------------------------------------------------------------------
# mss / root ekran
# ---------------------------------------------------------------------------
class MSSFrameSource(FrameSource):
    name = "mss"

    def __init__(self, cfg: CaptureConfig):
        super().__init__(cfg)
        self._local = threading.local()

    def _sct(self):
        import mss

        sct = getattr(self._local, "sct", None)
        if sct is None:
            sct = mss.mss()
            self._local.sct = sct
        return sct

    def _monitor(self, sct) -> dict:
        if self.cfg.region:
            x, y, w, h = self.cfg.region
            return {"left": int(x), "top": int(y), "width": int(w), "height": int(h)}
        idx = max(1, min(int(self.cfg.monitor), len(sct.monitors) - 1))
        return dict(sct.monitors[idx])

    def grab_once(self) -> Frame:
        sct = self._sct()
        mon = self._monitor(sct)
        shot = sct.grab(mon)
        # mss BGRA verir; RGB'ye çevir (kopya, mss buffer'ı yeniden kullanılır).
        bgra = np.frombuffer(shot.rgb, dtype=np.uint8).reshape(shot.height, shot.width, 4)
        rgb = np.ascontiguousarray(bgra[:, :, [2, 1, 0]])
        return Frame(
            image=rgb,
            origin_x=int(mon["left"]),
            origin_y=int(mon["top"]),
            captured_at_ns=time.monotonic_ns(),
            source="mss",
        )


# ---------------------------------------------------------------------------
# XComposite pencere yakalama
# ---------------------------------------------------------------------------
def _find_window(dpy, title: str, wm_class: str, window_id: Optional[int]):
    from Xlib import X

    root = dpy.screen().root
    if window_id:
        return dpy.create_resource_object("window", int(window_id))
    want_title = (title or "").casefold()
    want_class = (wm_class or "").casefold()
    if not want_title and not want_class:
        try:
            atom = dpy.intern_atom("_NET_ACTIVE_WINDOW")
            prop = root.get_full_property(atom, X.AnyPropertyType)
            if prop and len(prop.value) > 0 and int(prop.value[0]):
                return dpy.create_resource_object("window", int(prop.value[0]))
        except Exception:
            pass
        return None

    queue = [root]
    seen = 0
    while queue and seen < 4000:
        win = queue.pop(0)
        seen += 1
        try:
            name = (win.get_wm_name() or "")
            klass = win.get_wm_class() or ("", "")
            if want_title and want_title in str(name).casefold():
                return win
            if want_class and any(want_class in str(k).casefold() for k in klass):
                return win
            queue.extend(win.query_tree().children)
        except Exception:
            continue
    return None


class XCompositeFrameSource(FrameSource):
    name = "xcomposite"

    def __init__(self, cfg: CaptureConfig):
        super().__init__(cfg)
        self._dpy = None
        self._target = None
        self._pixmap = None
        self._geom = None
        self._origin = (0, 0)
        self._last_refresh = 0.0
        self._last_pixmap_refresh = 0.0

    def _ensure_window(self) -> None:
        from Xlib import display
        from Xlib.ext import composite  # noqa: F401  (extension init)

        if self._dpy is None:
            self._dpy = display.Display(self.cfg.display or None)
            log.info("X11 bağlantısı: %s", self._dpy.get_display_name())
        if self._target is not None:
            return
        target = _find_window(self._dpy, self.cfg.window_title, self.cfg.window_class, self.cfg.window_id)
        if target is None:
            raise CaptureError(
                "Hedef pencere bulunamadı. --window-title / capture.window_title verin "
                "(veya pencereyi öne getirin)."
            )
        geom = target.get_geometry()
        if geom.width < 8 or geom.height < 8:
            raise CaptureError("Hedef pencere görünür değil veya 8px'den küçük")
        self._target = target
        self._geom = geom
        self._refresh_pixmap()
        log.info(
            "XComposite hedefi: '%s' %dx%d",
            target.get_wm_name() or hex(target.id),
            geom.width,
            geom.height,
        )

    def _refresh_pixmap(self) -> None:
        assert self._target is not None
        if self._pixmap is not None:
            try:
                self._pixmap.free()
            except Exception:
                pass
        self._pixmap = self._target.composite_name_window_pixmap()

    def _refresh_geometry(self) -> None:
        assert self._target is not None and self._dpy is not None
        try:
            geom = self._target.get_geometry()
        except Exception:
            self._target = None
            self._pixmap = None
            self._geom = None
            raise CaptureError("Hedef pencere kapandı")
        resized = self._geom is None or (geom.width, geom.height) != (self._geom.width, self._geom.height)
        if resized:
            self._geom = geom
        now = time.monotonic()
        # XComposite pixmap'i bazı sunucularda/kurulumlarda içerik değişimini
        # otomatik yansıtmıyor; periyodik olarak yeniden name'lendiriyoruz.
        if resized or now - self._last_pixmap_refresh >= 0.1:
            self._last_pixmap_refresh = now
            self._refresh_pixmap()
        root = self._dpy.screen().root
        tr = root.translate_coords(self._target, 0, 0)
        self._origin = (int(tr.x), int(tr.y))

    def grab_once(self) -> Frame:
        from Xlib import X

        self._ensure_window()
        self._refresh_geometry()
        assert self._dpy is not None and self._pixmap is not None and self._geom is not None
        w, h = int(self._geom.width), int(self._geom.height)
        img = self._pixmap.get_image(0, 0, w, h, X.ZPixmap, 0xFFFFFFFF)
        data = img.data
        need = w * h * 4
        if len(data) < need:
            raise CaptureError(f"XImage verisi beklenenden kısa: {len(data)} < {need}")
        bgra = np.frombuffer(data[:need], dtype=np.uint8).reshape(h, w, 4)
        rgb = np.ascontiguousarray(bgra[:, :, [2, 1, 0]])
        return Frame(
            image=rgb,
            origin_x=self._origin[0],
            origin_y=self._origin[1],
            captured_at_ns=time.monotonic_ns(),
            source="xcomposite",
        )


# ---------------------------------------------------------------------------
# Sentetik sahne (ekransız test)
# ---------------------------------------------------------------------------
class SyntheticFrameSource(FrameSource):
    name = "synthetic"

    LINES = [
        ("Continue", "Press E to interact"),
        ("Settings", "Audio, Video, Controls"),
        ("Inventory", "Health Potion x3"),
        ("Quest updated", "Find the missing blacksmith"),
        ("Game paused", "Resume   Save   Quit"),
        ("Level up!", "You reached level 12"),
    ]

    def __init__(self, cfg: CaptureConfig, interval_s: float = 2.5):
        super().__init__(cfg)
        self.interval_s = max(0.5, interval_s)
        self._t0 = time.monotonic()
        self._font = self._load_font(30)
        self._small = self._load_font(18)

    @staticmethod
    def _load_font(size: int):
        for path in (
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        ):
            if os.path.exists(path):
                try:
                    return ImageFont.truetype(path, size)
                except OSError:
                    pass
        return ImageFont.load_default(size=size)

    def grab_once(self) -> Frame:
        w, h = 1280, 720
        im = Image.new("RGB", (w, h), (18, 20, 28))
        d = ImageDraw.Draw(im)
        # basit "oyun" arka planı
        for y in range(h):
            shade = int(18 + 20 * y / h)
            d.line([(0, y), (w, y)], fill=(shade, shade + 2, shade + 10))
        d.rectangle([0, 0, w, 40], fill=(10, 12, 18))
        d.text((16, 10), "GFT SYNTHETIC SCENE  |  not a generated frame", font=self._small, fill=(140, 150, 170))
        idx = int((time.monotonic() - self._t0) / self.interval_s) % len(self.LINES)
        title, body = self.LINES[idx]
        box = [120, 470, 1160, 650]
        d.rounded_rectangle(box, radius=10, fill=(8, 10, 16), outline=(70, 85, 110), width=2)
        d.text((150, 495), title, font=self._font, fill=(255, 255, 255))
        d.text((150, 550), body, font=self._font, fill=(220, 225, 235))
        d.text((150, 605), "Press Space to continue", font=self._small, fill=(150, 160, 180))
        return Frame(
            image=np.asarray(im, dtype=np.uint8).copy(),
            origin_x=0,
            origin_y=0,
            captured_at_ns=time.monotonic_ns(),
            source="synthetic",
        )


# ---------------------------------------------------------------------------
def create_source(cfg: AppConfig) -> FrameSource:
    backend = cfg.capture.backend
    if backend == "mss":
        return MSSFrameSource(cfg.capture)
    if backend == "xcomposite":
        return XCompositeFrameSource(cfg.capture)
    if backend == "synthetic":
        return SyntheticFrameSource(cfg.capture)
    raise CaptureError(f"Bilinmeyen yakalama backend'i: {backend}")
