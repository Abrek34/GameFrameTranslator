#!/usr/bin/env python3
"""Sentetik diyalog sahnesi.

Bir oyun penceresi gibi davranır: sabit arka plan + periyodik değişen diyalog.
Benchmark, sahnenin gösterdiği metin zamanı ile GFT'nin çeviri/overlay zamanını
aynı CLOCK_MONOTONIC üzerinden karşılaştırır.

Kullanım:
  DISPLAY=:0 python tools/synthetic_scene.py --duration 30 --log /tmp/scene.jsonl
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import tkinter as tk
from pathlib import Path

LINES = [
    ("Continue", "Press E to interact"),
    ("Settings", "Audio, Video, Controls"),
    ("Inventory", "Health Potion x3"),
    ("Quest updated", "Find the missing blacksmith"),
    ("Game paused", "Resume   Save   Quit"),
    ("Level up!", "You reached level 12"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--interval", type=float, default=2.5)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--x", type=int, default=80)
    ap.add_argument("--y", type=int, default=80)
    ap.add_argument("--title", default="GFT Synthetic Scene")
    ap.add_argument("--log", default="")
    ap.add_argument("--fullscreen", action="store_true")
    args = ap.parse_args()

    log_path = Path(args.log).expanduser() if args.log else None
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    fh = log_path.open("w", encoding="utf-8") if log_path else None

    def emit(kind: str, **fields) -> None:
        ev = {"kind": kind, "t_ns": time.monotonic_ns(), "wall": time.time(), **fields}
        if fh:
            fh.write(json.dumps(ev, ensure_ascii=False) + "\n")
            fh.flush()
        print(json.dumps(ev, ensure_ascii=False), flush=True)

    root = tk.Tk()
    root.title(args.title)
    root.configure(bg="#12141c")
    try:
        root.attributes("-topmost", True)
    except tk.TclError:
        pass
    if args.fullscreen:
        root.attributes("-fullscreen", True)
    else:
        root.geometry(f"{args.width}x{args.height}+{args.x}+{args.y}")
    canvas = tk.Canvas(root, bg="#12141c", highlightthickness=0)
    canvas.pack(fill="both", expand=True)
    root.update_idletasks()

    # Statik oyun HUD'ı (OCR'ın kare kare değişmediğini görmek için)
    canvas.create_rectangle(0, 0, 3000, 46, fill="#0b0d12", outline="")
    canvas.create_text(16, 12, anchor="nw", fill="#8b94a8", font=("DejaVu Sans", 13),
                       text="GFT SYNTHETIC SCENE  |  presented-frame test fixture")
    canvas.create_text(16, 62, anchor="nw", fill="#657089", font=("DejaVu Sans", 12),
                       text="Health 100     Mana 50     Gold 1234")

    box = canvas.create_rectangle(0, 0, 10, 10, fill="#080a10", outline="#4b586e", width=2)
    title_id = canvas.create_text(0, 0, anchor="nw", fill="#ffffff", font=("DejaVu Sans", 28))
    body_id = canvas.create_text(0, 0, anchor="nw", fill="#dce2ec", font=("DejaVu Sans", 28))
    hint_id = canvas.create_text(0, 0, anchor="nw", fill="#96a0b4", font=("DejaVu Sans", 16))

    state = {"idx": 0, "start": time.monotonic(), "done": False}
    emit("scene_start", title=args.title, width=root.winfo_width(), height=root.winfo_height())

    def layout(idx: int) -> None:
        root.update_idletasks()
        w = max(640, root.winfo_width())
        h = max(400, root.winfo_height())
        bw, bh = int(w * 0.80), int(h * 0.27)
        x0, y0 = int((w - bw) / 2), int(h * 0.64)
        canvas.coords(box, x0, y0, x0 + bw, y0 + bh)
        canvas.coords(title_id, x0 + 28, y0 + 24)
        canvas.coords(body_id, x0 + 28, y0 + 84)
        canvas.coords(hint_id, x0 + 28, y0 + bh - 42)
        title, body = LINES[idx % len(LINES)]
        canvas.itemconfigure(title_id, text=title)
        canvas.itemconfigure(body_id, text=body)
        canvas.itemconfigure(hint_id, text="Press Space to continue")
        root.update_idletasks()
        emit("line_shown", index=idx, title=title, body=body)

    layout(0)

    def tick() -> None:
        if state["done"]:
            return
        elapsed = time.monotonic() - state["start"]
        if elapsed >= args.duration:
            state["done"] = True
            emit("scene_end")
            root.after(100, root.destroy)
            return
        idx = int(elapsed / max(0.5, args.interval))
        if idx != state["idx"]:
            state["idx"] = idx
            layout(idx)
        root.after(50, tick)

    root.after(50, tick)
    try:
        root.mainloop()
    finally:
        if fh:
            fh.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
