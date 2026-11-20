"""Küçük X11 yardımcıları (Tk fallback için input shape)."""
from __future__ import annotations

import ctypes
import ctypes.util
import logging

log = logging.getLogger(__name__)

_libx11 = None
_libxext = None
_display = None

ShapeInput = 2
ShapeSet = 0
Unsorted = 0


class XRectangle(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_short),
        ("y", ctypes.c_short),
        ("width", ctypes.c_ushort),
        ("height", ctypes.c_ushort),
    ]


def _load():
    global _libx11, _libxext, _display
    if _libx11 is not None:
        return True
    try:
        x11_name = ctypes.util.find_library("X11") or "libX11.so.6"
        xext_name = ctypes.util.find_library("Xext") or "libXext.so.6"
        _libx11 = ctypes.CDLL(x11_name)
        _libxext = ctypes.CDLL(xext_name)
        _libx11.XOpenDisplay.restype = ctypes.c_void_p
        _libx11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        _libx11.XFlush.argtypes = [ctypes.c_void_p]
        _libx11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        _libxext.XShapeCombineRectangles.argtypes = [
            ctypes.c_void_p,
            ctypes.c_ulong,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(XRectangle),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
        ]
        _display = _libx11.XOpenDisplay(None)
        return bool(_display)
    except Exception as exc:
        log.warning("X11 input shape yüklenemedi: %s", exc)
        return False


def set_input_shape_empty(window_id: int) -> bool:
    """Pencereyi fare/klavye için tıklama geçirgen yapar (XShape input)."""
    if not _load() or not _display:
        return False
    try:
        _libxext.XShapeCombineRectangles(
            ctypes.c_void_p(_display),
            ctypes.c_ulong(window_id),
            ShapeInput,
            0,
            0,
            None,
            0,
            ShapeSet,
            Unsorted,
        )
        _libx11.XFlush(ctypes.c_void_p(_display))
        return True
    except Exception as exc:
        log.warning("Input shape uygulanamadı: %s", exc)
        return False
