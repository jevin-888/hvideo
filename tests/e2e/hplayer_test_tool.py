#!/usr/bin/env python3
"""Desktop launcher for the Hplayer flow test runner."""

from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from runner import FlowRunner, load_cases


def resource_root() -> Path:
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))


class HplayerTestTool(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Hplayer 自动化测试工具")
        self.geometry("980x700")
        self.minsize(820, 560)
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.running = False
        self.include_destructive = tk.BooleanVar(value=False)
        self.case_items: list[tuple[Path, dict]] = []
        self._build_ui()
        self.after(100, self._drain_events)
        self._load_cases()
        self.after(200, self._discover_devices)

    def _build_ui(self) -> None:
        self.columnconfigure(1, weight=1)
        self.rowconfigure(9, weight=1)
        fields = [
            ("测试用例目录", "case_dir", str(resource_root() / "cases")),
            ("播放器地址", "base_url", "http://127.0.0.1:8080"),
            ("服务器地址", "server_url", "http://192.168.2.105:9898"),
            ("ADB 路径", "adb", "adb"),
            ("设备序列号", "serial", ""),
            ("登录账号", "username", "admin"),
            ("登录密码", "password", "admin*"),
            ("Bearer Token", "token", ""),
        ]
        self.vars: dict[str, tk.StringVar] = {}
        for row, (label, key, default) in enumerate(fields):
            ttk.Label(self, text=label).grid(row=row, column=0, padx=10, pady=7, sticky="w")
            variable = tk.StringVar(value=default)
            self.vars[key] = variable
            if key == "serial":
                entry = ttk.Combobox(self, textvariable=variable, state="normal")
            else:
                entry = ttk.Entry(self, textvariable=variable, show="*" if key == "token" else "")
            entry.grid(row=row, column=1, padx=10, pady=7, sticky="ew")
            if key == "case_dir":
                ttk.Button(self, text="选择", command=self._choose_case_dir).grid(
                    row=row, column=2, padx=10, pady=7
                )
            if key == "serial":
                self.device_combo = entry
                self.device_combo.bind("<<ComboboxSelected>>", lambda _event: self._update_base_url_from_device())
                ttk.Button(self, text="刷新设备", command=self._discover_devices).grid(
                    row=row, column=2, padx=10, pady=7
                )
        toolbar_row = len(fields)
        toolbar = ttk.Frame(self)
        toolbar.grid(row=toolbar_row, column=0, columnspan=3, padx=10, pady=8, sticky="ew")
        toolbar.columnconfigure(1, weight=1)
        self.run_button = ttk.Button(toolbar, text="运行测试", command=self._start)
        self.run_button.grid(row=0, column=0, padx=(0, 10))
        ttk.Button(toolbar, text="刷新用例", command=self._load_cases).grid(row=0, column=1, sticky="w")
        ttk.Checkbutton(toolbar, text="包含破坏性测试", variable=self.include_destructive).grid(
            row=0, column=2, padx=10
        )
        self.summary = ttk.Label(toolbar, text="未运行")
        self.summary.grid(row=0, column=3, padx=10)
        self.case_list = tk.Listbox(self, height=8, selectmode=tk.EXTENDED)
        self.case_list.grid(row=toolbar_row + 1, column=0, columnspan=3, padx=10, pady=(0, 10), sticky="ew")
        ttk.Label(self, text="运行日志").grid(row=toolbar_row + 2, column=0, padx=10, sticky="nw")
        self.log = tk.Text(self, wrap="word", state="disabled")
        self.log.grid(row=toolbar_row + 2, column=1, columnspan=2, padx=10, pady=(0, 10), sticky="nsew")

    def _choose_case_dir(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.vars["case_dir"].get())
        if selected:
            self.vars["case_dir"].set(selected)
            self._load_cases()

    def _discover_devices(self) -> None:
        adb = self.vars["adb"].get().strip() or "adb"
        try:
            completed = subprocess.run(
                [adb, "devices"],
                capture_output=True,
                text=True,
                timeout=8,
            )
            if completed.returncode != 0:
                raise RuntimeError(completed.stderr.strip() or f"adb exited {completed.returncode}")
            devices = []
            for line in completed.stdout.splitlines():
                parts = line.strip().split()
                if len(parts) >= 2 and parts[1] == "device":
                    devices.append(parts[0])
            self.device_combo["values"] = devices
            current = self.vars["serial"].get().strip()
            if current not in devices:
                self.vars["serial"].set(devices[0] if len(devices) == 1 else "")
            self._update_base_url_from_device()
            if devices:
                self._append(f"发现在线设备 {len(devices)} 台：{', '.join(devices)}")
            else:
                self._append("未发现在线 ADB 设备，请连接播放器后点击刷新设备")
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            self.device_combo["values"] = []
            self._append(f"设备发现失败：{error}")

    def _update_base_url_from_device(self) -> None:
        serial = self.vars["serial"].get().strip()
        if ":" in serial and serial.rsplit(":", 1)[1].isdigit():
            host = serial.rsplit(":", 1)[0]
            current = self.vars["base_url"].get().strip()
            if not current or "127.0.0.1" in current or "localhost" in current:
                self.vars["base_url"].set(f"http://{host}:8080")

    def _append(self, message: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", message + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _load_cases(self) -> None:
        self.case_list.delete(0, "end")
        self.case_items.clear()
        directory = Path(self.vars["case_dir"].get())
        if not directory.exists():
            self._append(f"用例目录不存在：{directory}")
            return
        for path in sorted(directory.glob("*.json")):
            try:
                for case in load_cases(path):
                    self.case_items.append((path, case))
                    self.case_list.insert(
                        "end", f"{case.get('id', 'unnamed')} | {case.get('name', '')}"
                    )
            except Exception as error:  # noqa: BLE001
                self._append(f"读取失败 {path}: {error}")
        self._append(f"已加载用例目录：{directory}")

    def _start(self) -> None:
        if self.running:
            return
        self.running = True
        self.run_button.configure(state="disabled")
        self.summary.configure(text="运行中")
        self._append("开始执行测试...")
        selected = self.case_list.curselection()
        cases = [self.case_items[index] for index in selected] if selected else list(self.case_items)
        options = {
            "base_url": self.vars["base_url"].get().strip(),
            "server_url": self.vars["server_url"].get().strip(),
            "adb": self.vars["adb"].get().strip() or "adb",
            "serial": self.vars["serial"].get().strip() or None,
            "username": self.vars["username"].get().strip(),
            "password": self.vars["password"].get(),
            "token": self.vars["token"].get().strip(),
            "case_dir": Path(self.vars["case_dir"].get()),
            "include_destructive": self.include_destructive.get(),
        }
        threading.Thread(target=self._run_worker, args=(cases, options), daemon=True).start()

    def _run_worker(self, cases: list[tuple[Path, dict]], options: dict) -> None:
        try:
            run_id = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            if getattr(sys, "frozen", False):
                app_data = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "HplayerTestTool"
                artifact_dir = app_data / "artifacts" / run_id
            else:
                artifact_dir = options["case_dir"].parent / ".artifacts" / run_id
            artifact_dir.mkdir(parents=True, exist_ok=True)
            token = options["token"]
            runner = FlowRunner(
                base_url=options["base_url"],
                adb=options["adb"],
                serial=options["serial"],
                artifact_dir=artifact_dir,
                default_headers={"Authorization": f"Bearer {token}"} if token else None,
                global_variables={"server_url": options["server_url"]},
            )
            if not token and options["username"] and options["password"]:
                login = runner.http.login(options["username"], options["password"])
                self.events.put(("log", f"播放器登录成功：{login['url']}"))
            results = []
            for path, case in cases:
                result = runner.run_case(
                    case, include_destructive=options["include_destructive"]
                )
                results.append(
                    {
                        "case_file": str(path),
                        "case_id": result.case_id,
                        "name": result.name,
                        "status": result.status,
                        "duration_ms": result.duration_ms,
                        "steps": result.steps,
                        "error": result.error,
                    }
                )
                self.events.put(
                    ("log", f"[{result.status.upper()}] {result.case_id}: {result.name}")
                )
            report = {
                "schema_version": 1,
                "run_id": run_id,
                "base_url": options["base_url"],
                "serial": options["serial"],
                "results": results,
            }
            report_path = artifact_dir / "report.json"
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
            counts = {
                status: sum(item["status"] == status for item in results)
                for status in ("passed", "failed", "blocked", "skipped")
            }
            self.events.put(("done", (counts, report_path)))
        except Exception as error:  # noqa: BLE001
            self.events.put(("error", str(error)))

    def _drain_events(self) -> None:
        try:
            while True:
                event, payload = self.events.get_nowait()
                if event == "log":
                    self._append(str(payload))
                elif event == "done":
                    counts, report_path = payload
                    self.summary.configure(
                        text=f"通过 {counts['passed']} / 失败 {counts['failed']} / 阻塞 {counts['blocked']} / 跳过 {counts['skipped']}"
                    )
                    self._append(f"报告：{report_path}")
                    self.running = False
                    self.run_button.configure(state="normal")
                elif event == "error":
                    self._append(f"运行错误：{payload}")
                    messagebox.showerror("测试执行失败", str(payload))
                    self.summary.configure(text="执行失败")
                    self.running = False
                    self.run_button.configure(state="normal")
        except queue.Empty:
            pass
        self.after(100, self._drain_events)


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        cases = resource_root() / "cases"
        artifact_dir = Path(tempfile.mkdtemp(prefix="hplayer-test-tool-selftest-"))
        runner = FlowRunner(
            base_url="http://127.0.0.1",
            adb="adb",
            serial=None,
            artifact_dir=artifact_dir,
        )
        results = [
            runner.run_case(case)
            for path in sorted(cases.glob("framework_self_test.json"))
            for case in load_cases(path)
        ]
        failed = [result for result in results if result.status == "failed"]
        print(f"self-test: {len(results) - len(failed)} passed, {len(failed)} failed")
        raise SystemExit(1 if failed or not results else 0)
    HplayerTestTool().mainloop()
