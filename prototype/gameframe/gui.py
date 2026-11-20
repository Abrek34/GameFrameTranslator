"""GameFrame Translator - basit GUI (Tkinter).

Çok adımlı komut satırını gizler; pencere seçimi, başlat/durdur, tek kare
test, benchmark ve log görüntüleme tek yerden yapılır.
"""
from __future__ import annotations

import os
import subprocess
import sys
import threading
import time
import yaml
from pathlib import Path
from tkinter import (END, BOTH, LEFT, RIGHT, TOP, BOTTOM, X, Y, W, E, N, S, BooleanVar,
                     StringVar, Tk, Toplevel, Frame, Label, Entry, Button, Listbox,
                     Scrollbar, Text, ttk, messagebox)

try:
    from Xlib import display as xdisplay
except Exception:
    xdisplay = None

VENV_PYTHON = None
# Tercihen proje venv'i kullan
for cand in (
    Path(__file__).resolve().parents[2] / "prototype" / ".venv" / "bin" / "python",
    Path(sys.executable),
):
    if cand.exists():
        VENV_PYTHON = str(cand)
        break
if VENV_PYTHON is None:
    VENV_PYTHON = sys.executable

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROTOTYPE_DIR = PROJECT_ROOT / "prototype"


class App:
    def __init__(self, root: Tk):
        self.root = root
        root.title("GameFrame Translator")
        root.geometry("920x640")
        self.proc: subprocess.Popen | None = None
        self.reader_thread: threading.Thread | None = None
        self.stop_flag = False

        # ---- üst: durum ----
        top = Frame(root)
        top.pack(fill=X, padx=8, pady=6)
        self.status_var = StringVar(value="Hazır")
        Label(top, text="Durum:", font=("DejaVu Sans", 10, "bold")).pack(side=LEFT)
        Label(top, textvariable=self.status_var, fg="#1f6feb").pack(side=LEFT, padx=6)

        # ---- ayarlar ----
        cfg = Frame(root, bd=1, relief="groove")
        cfg.pack(fill=X, padx=8, pady=4)
        Label(cfg, text="Ayarlar", font=("DejaVu Sans", 11, "bold")).grid(row=0, column=0, columnspan=4, sticky=W, padx=6, pady=4)

        def row(r, label, var, width=28):
            Label(cfg, text=label).grid(row=r, column=0, sticky=W, padx=6, pady=2)
            e = Entry(cfg, textvariable=var, width=width)
            e.grid(row=r, column=1, sticky=W, padx=6, pady=2)
            return e

        self.v_window = StringVar(value="")
        self.v_monitor = StringVar(value="1")
        self.v_provider = StringVar(value="auto")
        self.v_overlay = StringVar(value="auto")
        self.v_lang = StringVar(value="eng")
        self.v_target = StringVar(value="tr")
        self.v_duration = StringVar(value="0")
        self.v_fps = StringVar(value="30")
        self.v_config = StringVar(value="")
        self.v_synthetic = BooleanVar(value=False)

        row(1, "Oyun penceresi (başlık):", self.v_window, 40)
        row(2, "Monitör:", self.v_monitor, 6)
        row(3, "Çeviri sağlayıcı:", self.v_provider, 12)
        row(4, "Overlay:", self.v_overlay, 12)
        row(5, "OCR dili:", self.v_lang, 12)
        row(6, "Hedef dil:", self.v_target, 6)
        row(7, "FPS:", self.v_fps, 6)
        row(8, "Süre (sn, 0=sınırsız):", self.v_duration, 8)
        row(9, "Ayar dosyası (opsiyonel):", self.v_config, 40)
        self.v_gamecmd = StringVar(value="teeworlds")
        row(10, "Oyun komutu:", self.v_gamecmd, 40)
        from tkinter import Checkbutton
        Checkbutton(cfg, text="Sentetik test modu (ekransız)", variable=self.v_synthetic).grid(row=10, column=0, columnspan=2, sticky=W, padx=6, pady=2)

        # pencere listesi
        winf = Frame(root)
        winf.pack(fill=X, padx=8, pady=4)
        Label(winf, text="Çalışan pencereler:", font=("DejaVu Sans", 10, "bold")).pack(anchor=W)
        lf = Frame(winf)
        lf.pack(fill=X)
        self.win_combo = ttk.Combobox(lf, textvariable=self.v_window, width=60, state="readonly")
        self.win_combo.pack(side=LEFT, fill=X, expand=True)
        self.win_combo.bind("<<ComboboxSelected>>", lambda e: None)
        self.winlist = Listbox(lf, height=4)
        self.winlist.pack(side=LEFT, fill=BOTH, expand=True)
        sb = Scrollbar(lf, command=self.winlist.yview)
        sb.pack(side=RIGHT, fill=Y)
        self.winlist.config(yscrollcommand=sb.set)
        self.winlist.bind("<Double-Button-1>", self._pick_window)
        btnf = Frame(winf)
        btnf.pack(fill=X, pady=2)
        Button(btnf, text="Pencereleri yenile", command=self.refresh_windows).pack(side=LEFT, padx=2)
        Button(btnf, text="Seçili pencereyi kullan", command=self._pick_window).pack(side=LEFT, padx=2)
        Button(btnf, text="Aktif pencereyi kullan", command=self.use_active_window).pack(side=LEFT, padx=2)
        Button(btnf, text="5s bekle + yenile", command=self.rescan_delayed).pack(side=LEFT, padx=2)

        # ---- butonlar ----
        bf = Frame(root)
        bf.pack(fill=X, padx=8, pady=6)
        self.btn_start = Button(bf, text="▶ Başlat", bg="#2ea043", fg="white", width=12, command=self.start)
        self.btn_start.pack(side=LEFT, padx=2)
        self.btn_stop = Button(bf, text="■ Durdur", bg="#da3633", fg="white", width=12, command=self.stop, state="disabled")
        self.btn_stop.pack(side=LEFT, padx=2)
        Button(bf, text="Tek kare test", command=self.run_once).pack(side=LEFT, padx=2)
        Button(bf, text="Sentetik test", command=self.run_synthetic_once).pack(side=LEFT, padx=2)
        Button(bf, text="Benchmark (30s)", command=self.run_benchmark).pack(side=LEFT, padx=2)
        Button(bf, text="Log klasörü", command=self.open_logs).pack(side=LEFT, padx=2)
        Button(bf, text="Yardım", command=self.show_help).pack(side=LEFT, padx=2)
        Button(bf, text="Oyunu X11 ile başlat", command=self.launch_game_x11).pack(side=LEFT, padx=2)
        Button(bf, text="Google test", command=self.test_google).pack(side=LEFT, padx=2)
        Button(bf, text="Ayar kaydet", command=self.save_config).pack(side=LEFT, padx=2)
        Button(bf, text="Ayar yükle", command=self.load_config).pack(side=LEFT, padx=2)

        # ---- çıktı ----
        out = Frame(root)
        out.pack(fill=BOTH, expand=True, padx=8, pady=6)
        Label(out, text="Çıktı / log", font=("DejaVu Sans", 10, "bold")).pack(anchor=W)
        self.output = Text(out, wrap="word", height=12, font=("DejaVu Sans Mono", 9))
        self.output.pack(side=LEFT, fill=BOTH, expand=True)
        osb = Scrollbar(out, command=self.output.yview)
        osb.pack(side=RIGHT, fill=Y)
        self.output.config(yscrollcommand=osb.set)

        self.refresh_windows()
        root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------- pencere listesi ----------
    def refresh_windows(self):
        self.winlist.delete(0, END)
        self.win_combo["values"] = []
        if xdisplay is None:
            self.log("Xlib yok; pencere listesi alınamadı.")
            return
        try:
            d = xdisplay.Display()
            rootw = d.screen().root
            seen = set()
            stack = [rootw]
            while stack and len(seen) < 300:
                w = stack.pop()
                try:
                    name = w.get_wm_name()
                    if name and len(str(name)) > 2:
                        s = str(name)
                        if "GameFrame Translator" in s:
                            continue
                        if s not in seen:
                            seen.add(s)
                            self.winlist.insert(END, s)
                            vals = list(self.win_combo["values"])
                            vals.append(s)
                            self.win_combo["values"] = vals
                    stack.extend(w.query_tree().children)
                except Exception:
                    continue
            if len(seen) == 0:
                self.log("X11 penceresi bulunamadı. Oyun Wayland native olabilir. "
                         "Steam başlatma seçeneklerine 'SDL_VIDEODRIVER=x11 %command%' ekleyin "
                         "veya oyunu XWayland ile çalıştırın.")
        except Exception as e:
            self.log(f"Pencere listesi hatası: {e}")

    def _pick_window(self, _event=None):
        sel = self.winlist.curselection()
        if not sel:
            return
        val = self.winlist.get(sel[0])
        self.v_window.set(val)

    def use_active_window(self):
        if xdisplay is None:
            return
        try:
            d = xdisplay.Display()
            rootw = d.screen().root
            atom = d.intern_atom("_NET_ACTIVE_WINDOW")
            from Xlib import X
            prop = rootw.get_full_property(atom, X.AnyPropertyType)
            if prop and prop.value:
                w = d.create_resource_object("window", int(prop.value[0]))
                name = w.get_wm_name()
                if name:
                    self.v_window.set(str(name))
        except Exception as e:
            self.log(f"Aktif pencere alınamadı: {e}")

    # ---------- komut oluşturma ----------
    def build_cmd(self, once=False, synthetic=False, benchmark=False):
        if benchmark:
            return [VENV_PYTHON, str(PROJECT_ROOT / "prototype" / "tools" / "benchmark.py"),
                    "--duration", "30"]
        py = VENV_PYTHON
        cmd = [py, "-m", "gameframe"]
        if self.v_config.get().strip():
            cmd += ["--config", self.v_config.get().strip()]
        if once:
            cmd.append("--once")
        if synthetic or self.v_synthetic.get():
            cmd += ["--source", "synthetic"]
        else:
            cmd += ["--source", "xcomposite"]
            if self.v_window.get().strip():
                cmd += ["--window-title", self.v_window.get().strip()]
        if self.v_monitor.get().strip():
            cmd += ["--monitor", self.v_monitor.get().strip()]
        if self.v_provider.get().strip():
            cmd += ["--provider", self.v_provider.get().strip()]
        if self.v_overlay.get().strip():
            cmd += ["--overlay", self.v_overlay.get().strip()]
        if self.v_lang.get().strip():
            cmd += ["--lang", self.v_lang.get().strip()]
        if self.v_target.get().strip():
            cmd += ["--target", self.v_target.get().strip()]
        if self.v_fps.get().strip():
            cmd += ["--fps", self.v_fps.get().strip()]
        dur = self.v_duration.get().strip()
        if dur and dur != "0":
            cmd += ["--duration", dur]
        if self.v_window.get().strip() == "" and not synthetic and not once:
            # root capture + overlay geri besleme riski; overlay kapat
            if self.v_overlay.get().strip() not in ("none",):
                cmd.append("--no-overlay")
        return cmd

    def start(self):
        if self.proc and self.proc.poll() is None:
            self.log("Zaten çalışıyor.")
            return
        if not self.v_synthetic.get() and not self.v_window.get().strip():
            from tkinter import messagebox
            if not messagebox.askyesno("Pencere seçimi", "Oyun penceresi seçmediniz. Sentetik test modu ile mi çalıştırılsın?"):
                self.log("İptal edildi. Önce pencere seçin.")
                return
            self.v_synthetic.set(True)

        # Oyun penceresi seçildiyse X11'de var mı kontrol et (5 sn bekle)
        if not self.v_synthetic.get():
            title = self.v_window.get().strip()
            found = self._window_exists(title, wait_s=5)
            if not found:
                from tkinter import messagebox
                if not messagebox.askyesno("Pencere bulunamadı",
                                           f"'{title}' X11 pencere listesinde yok.\n"
                                           "Oyun Wayland native çalışıyor olabilir.\n\n"
                                           "Sentetik test modu ile mi çalıştırılsın?"):
                    self.log("İptal edildi.")
                    return
                self.v_synthetic.set(True)

        cmd = self.build_cmd()
        self.log("Başlatılıyor: " + " ".join(cmd))
        self._launch(cmd)

    def run_once(self):
        if self.proc and self.proc.poll() is None:
            self.log("Önce çalışan işlemi durdurun.")
            return
        cmd = self.build_cmd(once=True)
        self.log("Tek kare test: " + " ".join(cmd))
        self._launch(cmd)

    def run_synthetic_once(self):
        if self.proc and self.proc.poll() is None:
            self.log("Önce çalışan işlemi durdurun.")
            return
        cmd = self.build_cmd(once=True, synthetic=True)
        self.log("Sentetik tek kare: " + " ".join(cmd))
        self._launch(cmd)

    def run_benchmark(self):
        if self.proc and self.proc.poll() is None:
            self.log("Önce çalışan işlemi durdurun.")
            return
        cmd = self.build_cmd(benchmark=True)
        self.log("Benchmark: " + " ".join(cmd))
        self._launch(cmd, cwd=str(PROJECT_ROOT))

    def _launch(self, cmd, cwd=None):
        self.stop_flag = False
        env = dict(os.environ)
        env.setdefault("DISPLAY", os.environ.get("DISPLAY", ":0"))
        try:
            self.proc = subprocess.Popen(
                cmd, cwd=cwd or str(PROTOTYPE_DIR), env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
            )
        except Exception as e:
            self.log(f"Başlatma hatası: {e}")
            return
        self.status_var.set("Çalışıyor…")
        self.btn_start.config(state="disabled")
        self.btn_stop.config(state="normal")
        self.reader_thread = threading.Thread(target=self._reader, daemon=True)
        self.reader_thread.start()
        self._watch()

    def _reader(self):
        assert self.proc is not None
        for line in self.proc.stdout:
            self.root.after(0, lambda l=line: self.log(l.rstrip()))
        self.proc.wait()
        rc = self.proc.returncode
        self.root.after(0, lambda: self._finished(rc))

    def _watch(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.root.after(500, self._watch)
        else:
            self._finished(self.proc.returncode)

    def _finished(self, rc):
        self.status_var.set(f"Bitti (kod {rc})")
        self.btn_start.config(state="normal")
        self.btn_stop.config(state="disabled")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.log("Durduruluyor…")
            try:
                self.proc.terminate()
            except Exception as e:
                self.log(f"Durdurma hatası: {e}")
        self.stop_flag = True

    def log(self, msg):
        self.output.insert(END, msg + "\n")
        self.output.see(END)

    def open_logs(self):
        logdir = Path.home() / ".local" / "state" / "gameframe"
        if not logdir.exists():
            messagebox.showinfo("Log", "Henüz log yok.")
            return
        try:
            subprocess.Popen(["xdg-open", str(logdir)])
        except Exception:
            self.log(f"Log klasörü: {logdir}")

    def show_help(self):
        messagebox.showinfo(
            "Yardım",
            "1) 'Pencereleri yenile' ile oyun penceresini seçin.\n"
            "2) 'Başlat' ile yakalama + OCR + çeviri + overlay başlar.\n"
            "3) Pencere seçmeden başlatırsanız overlay geri besleme riski nedeniyle kapatılır.\n"
            "4) 'Tek kare test' OCR/çeviri çıktısını loga yazar.\n"
            "5) 'Sentetik test' ekransız doğrulama yapar.\n"
            "6) Durdurmak için 'Durdur' veya Ctrl+C.\n\n"
            "Not: Wayland native oyunlar için XComposite çalışmayabilir; XWayland/Proton oyunları hedeflenir.",
        )

    def test_google(self):
        def worker():
            import urllib.request, urllib.parse, json, time
            q = "Hello world"
            url = "https://translate.googleapis.com/translate_a/single?" + urllib.parse.urlencode(
                {"client": "gtx", "sl": "en", "tl": "tr", "dt": "t", "q": q})
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            try:
                with urllib.request.urlopen(req, timeout=6) as r:
                    data = json.loads(r.read().decode())
                    out = "".join(s[0] for s in data[0] if s and s[0])
                    self.root.after(0, lambda: self.log(f"Google OK: {q!r} -> {out!r}"))
            except Exception as e:
                self.root.after(0, lambda: self.log(f"Google hata: {e} (ağ/rate-limit; sözlük modu kullanın)"))
        threading.Thread(target=worker, daemon=True).start()

    def rescan_delayed(self):
        self.log("5 saniye bekleniyor; oyun penceresini açın...")
        def worker():
            time.sleep(5)
            self.root.after(0, self.refresh_windows)
        threading.Thread(target=worker, daemon=True).start()

    def _settings_dict(self):
        return {
            "capture": {"backend": "xcomposite", "monitor": int(self.v_monitor.get() or 1),
                        "window_title": self.v_window.get().strip(), "fps": int(self.v_fps.get() or 30)},
            "ocr": {"lang": self.v_lang.get().strip() or "eng"},
            "translate": {"provider": self.v_provider.get().strip() or "auto",
                          "target_lang": self.v_target.get().strip() or "tr"},
            "overlay": {"backend": self.v_overlay.get().strip() or "auto"},
            "runtime": {"debug": False},
        }

    def save_config(self):
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(defaultextension=".yaml", filetypes=[("YAML", "*.yaml"), ("Tümü", "*.*")])
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                yaml.safe_dump(self._settings_dict(), f, allow_unicode=True, sort_keys=False)
            self.v_config.set(path)
            self.log(f"Ayar kaydedildi: {path}")
        except Exception as e:
            self.log(f"Ayar kaydetme hatası: {e}")

    def load_config(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(filetypes=[("YAML", "*.yaml"), ("Tümü", "*.*")])
        if not path:
            return
        try:
            import yaml as _y
            data = _y.safe_load(open(path, encoding="utf-8")) or {}
            c = data.get("capture", {}) or {}
            if "window_title" in c:
                self.v_window.set(c.get("window_title") or "")
            if "monitor" in c:
                self.v_monitor.set(str(c.get("monitor")))
            if "fps" in c:
                self.v_fps.set(str(c.get("fps")))
            o = data.get("ocr", {}) or {}
            if "lang" in o:
                self.v_lang.set(o.get("lang"))
            t = data.get("translate", {}) or {}
            if "provider" in t:
                self.v_provider.set(t.get("provider"))
            if "target_lang" in t:
                self.v_target.set(t.get("target_lang"))
            ov = data.get("overlay", {}) or {}
            if "backend" in ov:
                self.v_overlay.set(ov.get("backend"))
            self.v_config.set(path)
            self.log(f"Ayar yüklendi: {path}")
        except Exception as e:
            self.log(f"Ayar yükleme hatası: {e}")

    def launch_game_x11(self):
        cmd = self.v_gamecmd.get().strip() or "teeworlds"
        env = dict(os.environ)
        env["SDL_VIDEODRIVER"] = "x11"
        env["GDK_BACKEND"] = "x11"
        try:
            subprocess.Popen(cmd.split(), env=env)
            self.log(f"X11 modunda başlatıldı: {cmd} (SDL_VIDEODRIVER=x11)")
            self.root.after(3000, self.refresh_windows)
        except Exception as e:
            self.log(f"Oyun başlatma hatası: {e}")

    def _window_exists(self, title, wait_s=0):
        if not title:
            return False
        deadline = time.time() + wait_s
        while time.time() < deadline:
            try:
                wins = self._list_window_names()
                if any(title.lower() in w.lower() for w in wins):
                    return True
            except Exception:
                pass
            time.sleep(1)
        # son kontrol
        try:
            wins = self._list_window_names()
            return any(title.lower() in w.lower() for w in wins)
        except Exception:
            return False

    def _list_window_names(self):
        if xdisplay is None:
            return []
        d = xdisplay.Display()
        rootw = d.screen().root
        seen = set()
        stack = [rootw]
        while stack and len(seen) < 500:
            w = stack.pop()
            try:
                name = w.get_wm_name()
                if name and len(str(name)) > 2:
                    seen.add(str(name))
                stack.extend(w.query_tree().children)
            except Exception:
                continue
        return list(seen)

    def on_close(self):
        self.stop()
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.wait(timeout=3)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        self.root.destroy()


def main():
    root = Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
