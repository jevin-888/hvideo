#!/usr/bin/env python3
"""Run Hplayer JSON flow cases."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

from runner import FlowRunner, load_cases


def case_files(target: Path) -> list[Path]:
    if target.is_file():
        return [target]
    return sorted(target.glob("*.json"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Hplayer flow test runner")
    parser.add_argument("--case", type=Path, default=Path(__file__).parent / "cases")
    parser.add_argument("--list", action="store_true", help="list cases without executing")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial")
    parser.add_argument("--token", help="default Bearer token for HTTP steps")
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", default="admin*")
    parser.add_argument("--no-login", action="store_true")
    parser.add_argument("--server-url", default="http://192.168.2.105:9898")
    parser.add_argument("--include-destructive", action="store_true")
    parser.add_argument(
        "--artifacts", type=Path, default=Path(__file__).parent / ".artifacts"
    )
    args = parser.parse_args()
    files = case_files(args.case)
    if not files:
        print(f"No JSON cases found: {args.case}", file=sys.stderr)
        return 2
    cases = []
    for path in files:
        try:
            cases.extend((path, case) for case in load_cases(path))
        except Exception as error:  # noqa: BLE001
            print(f"Invalid case file {path}: {error}", file=sys.stderr)
            return 2
    if args.list:
        for path, case in cases:
            print(f"{case.get('id', 'unnamed')}\t{case.get('name', '')}\t{path}")
        return 0

    run_id = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    artifact_dir = args.artifacts / run_id
    artifact_dir.mkdir(parents=True, exist_ok=True)
    runner = FlowRunner(
        base_url=args.base_url,
        adb=args.adb,
        serial=args.serial,
        artifact_dir=artifact_dir,
        default_headers={"Authorization": f"Bearer {args.token}"} if args.token else None,
        global_variables={"server_url": args.server_url},
    )
    if not args.no_login and not args.token and args.username and args.password:
        login = runner.http.login(args.username, args.password)
        print(f"Login passed: {login['url']}")
    results = []
    for path, case in cases:
        result = runner.run_case(case, include_destructive=args.include_destructive)
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
        print(f"[{result.status.upper():7}] {result.case_id} - {result.name}")
        if result.error:
            print(f"  {result.error}", file=sys.stderr)
    report = {
        "schema_version": 1,
        "run_id": run_id,
        "base_url": args.base_url,
        "serial": args.serial,
        "results": results,
    }
    report_path = artifact_dir / "report.json"
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    failed = sum(result["status"] == "failed" for result in results)
    passed = sum(result["status"] == "passed" for result in results)
    blocked = sum(result["status"] == "blocked" for result in results)
    skipped = sum(result["status"] == "skipped" for result in results)
    print(f"Summary: passed={passed}, failed={failed}, blocked={blocked}, skipped={skipped}")
    print(f"Report: {report_path}")
    return 1 if failed else (2 if blocked else 0)


if __name__ == "__main__":
    raise SystemExit(main())
