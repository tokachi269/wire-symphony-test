#!/usr/bin/env python3
"""Repository entry point for architecture evolution observation."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
from typing import Mapping


TOOLS_ROOT = Path(__file__).resolve().parent
HARNESS_ROOT = TOOLS_ROOT / "harness"
sys.path.insert(0, str(HARNESS_ROOT))

from architecture_observation import (  # noqa: E402
    architecture_delta_report,
    build_graph,
    classify_paths,
    delta_markdown,
    graph_markdown,
    history_markdown,
    history_report,
    hotspot_markdown,
    hotspot_rows,
    parse_name_status_log,
    parse_numstat_log,
    reflexion_markdown,
    reflexion_report,
    source_paths,
)


def run_git(root: Path, *args: str, text: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args],
        cwd=root,
        check=True,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
    )


def load_manifest(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"manifest must be an object: {path}")
    return value


def load_worktree_snapshot(
    root: Path, manifest: Mapping[str, object]
) -> dict[str, str]:
    raw_scan = manifest.get("scan", {})
    if not isinstance(raw_scan, Mapping):
        raise ValueError("manifest.scan must be an object")
    extensions = {
        item for item in raw_scan.get("extensions", []) if isinstance(item, str)
    }
    snapshot: dict[str, str] = {}
    for raw_root in raw_scan.get("roots", []):
        if not isinstance(raw_root, str):
            continue
        scan_root = root / raw_root
        if not scan_root.is_dir():
            continue
        for path in scan_root.rglob("*"):
            if not path.is_file() or path.suffix not in extensions:
                continue
            relative = path.relative_to(root).as_posix()
            snapshot[relative] = path.read_text(encoding="utf-8", errors="replace")
    selected = set(source_paths(snapshot, manifest))
    return {path: snapshot[path] for path in sorted(selected)}


def load_git_snapshot(root: Path, ref: str) -> tuple[dict[str, str], dict[str, object]]:
    archive = run_git(root, "archive", "--format=tar", ref, text=False).stdout
    all_text: dict[str, str] = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as stream:
        for member in stream.getmembers():
            if not member.isfile():
                continue
            extracted = stream.extractfile(member)
            if extracted is None:
                continue
            try:
                all_text[PurePosixPath(member.name).as_posix()] = extracted.read().decode(
                    "utf-8", errors="replace"
                )
            finally:
                extracted.close()
    manifest_text = all_text.get("tools/arch_manifest.json")
    if manifest_text is None:
        raise ValueError(f"{ref} does not contain tools/arch_manifest.json")
    manifest = json.loads(manifest_text)
    if not isinstance(manifest, dict):
        raise ValueError(f"{ref}: architecture manifest must be an object")
    selected = set(source_paths(all_text, manifest))
    return {path: all_text[path] for path in sorted(selected)}, manifest


def existing_lint_diagnostics(root: Path) -> list[str]:
    result = subprocess.run(
        [sys.executable, str(root / "tools" / "arch_lint.py"), "--root", str(root)],
        cwd=root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode == 0:
        return []
    return [line.strip() for line in result.stderr.splitlines() if line.strip()]


def changed_paths(root: Path, base: str, base_snapshot: Mapping[str, str], current_snapshot: Mapping[str, str]) -> list[dict[str, object]]:
    output = run_git(root, "diff", "--name-status", "-M", base, "--").stdout
    changes: list[dict[str, object]] = []
    seen: set[str] = set()
    for line in output.splitlines():
        cells = line.split("\t")
        if not cells:
            continue
        status = cells[0]
        paths = cells[1:]
        if not paths:
            continue
        changes.append({"status": status, "paths": paths})
        seen.update(paths)
    for path in sorted(current_snapshot.keys() - base_snapshot.keys() - seen):
        changes.append({"status": "A", "paths": [path]})
    for path in sorted(base_snapshot.keys() - current_snapshot.keys() - seen):
        changes.append({"status": "D", "paths": [path]})
    return sorted(changes, key=lambda row: (str(row["paths"]), str(row["status"])))


def added_fallback_terms(
    root: Path, base: str, eligible_paths: set[str]
) -> list[dict[str, object]]:
    output = run_git(root, "diff", "--unified=0", base, "--").stdout
    result: list[dict[str, object]] = []
    current_path = ""
    current_line = 0
    for line in output.splitlines():
        if line.startswith("+++ b/"):
            current_path = line[6:]
        elif line.startswith("@@"):
            marker = line.split("+")[1].split(" ")[0]
            current_line = int(marker.split(",")[0])
        elif (
            line.startswith("+")
            and not line.startswith("+++")
            and current_path in eligible_paths
        ):
            text = line[1:].strip()
            if any(term in text.lower() for term in ("fallback", "special path")):
                result.append({"path": current_path, "line": current_line, "text": text})
            current_line += 1
        elif not line.startswith("-"):
            current_line += 1
    return result


def history_inputs(root: Path):
    log = run_git(
        root,
        "log",
        "--first-parent",
        "--diff-merges=first-parent",
        "--find-renames",
        "--name-status",
        "--format=%x1e%H%x1f%cI",
        "HEAD",
    ).stdout
    commits, aliases = parse_name_status_log(log)
    numstat = run_git(
        root,
        "log",
        "--first-parent",
        "--diff-merges=first-parent",
        "--find-renames",
        "--numstat",
        "--format=%x1e%H%x1f%cI",
        "HEAD",
    ).stdout
    return commits, aliases, numstat


def emit(value: object, rendered: str, args: argparse.Namespace) -> None:
    output = json.dumps(value, indent=2, ensure_ascii=False) + "\n" if args.format == "json" else rendered
    if args.output:
        path = Path(args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Observe architecture structure and evolution without defining product semantics."
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--manifest", type=Path, default=Path("tools/arch_manifest.json"))
    subparsers = parser.add_subparsers(dest="command", required=True)

    def common(command: str, *, base: bool = False) -> argparse.ArgumentParser:
        child = subparsers.add_parser(command)
        child.add_argument("--scope", choices=("production", "tests", "tools", "all"), default="production")
        child.add_argument("--format", choices=("markdown", "json"), default="markdown")
        child.add_argument("--output")
        if base:
            child.add_argument("--base", required=True)
        return child

    common("graph")
    common("reflexion", base=True)
    common("delta", base=True)
    history_parser = common("history")
    history_parser.add_argument(
        "--window-commits",
        type=int,
        default=50,
        help="compare the newest N first-parent commits with the preceding N (default: 50)",
    )
    history_parser.add_argument(
        "--recent-days",
        type=int,
        help="also report an optional day-based recent window",
    )
    history_parser.add_argument("--limit", type=int, default=50)
    hotspot_parser = common("hotspot")
    hotspot_parser.add_argument("--limit", type=int, default=50)

    args = parser.parse_args()
    root = args.root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else root / args.manifest
    manifest = load_manifest(manifest_path)
    current_snapshot = load_worktree_snapshot(root, manifest)
    current_graph = build_graph(current_snapshot, manifest, scope=args.scope)

    if args.command == "graph":
        emit(current_graph, graph_markdown(current_graph), args)
        return 0

    if args.command in {"reflexion", "delta"}:
        base_snapshot, base_manifest = load_git_snapshot(root, args.base)
        base_graph = build_graph(base_snapshot, base_manifest, scope=args.scope)
        if args.command == "reflexion":
            report = reflexion_report(
                base_graph,
                current_graph,
                manifest,
                existing_lint_diagnostics=existing_lint_diagnostics(root),
            )
            emit(report, reflexion_markdown(report), args)
        else:
            report = architecture_delta_report(
                base_graph,
                current_graph,
                changed_paths(root, args.base, base_snapshot, current_snapshot),
                fallback_term_evidence=added_fallback_terms(
                    root,
                    args.base,
                    {str(node["path"]) for node in current_graph.get("nodes", [])},
                ),
            )
            emit(report, delta_markdown(report), args)
        return 0

    files = source_paths(current_snapshot, manifest)
    classified, classification_errors = classify_paths(files, manifest)
    if classification_errors:
        for error in classification_errors:
            print(f"architecture-observation: {error}", file=sys.stderr)
        return 1
    commits, aliases, numstat = history_inputs(root)
    history = history_report(
        commits,
        classified,
        current_graph,
        recent_days=getattr(args, "recent_days", None),
        window_commits=getattr(args, "window_commits", 50),
        limit=args.limit,
    )
    if args.command == "history":
        emit(history, history_markdown(history), args)
        return 0

    long_term = history["windows"]["long_term"]
    mass = {row["sha"] for row in long_term["mass_change_commits"]}
    churn = parse_numstat_log(numstat, aliases)
    churn_excluding_mass = parse_numstat_log(numstat, aliases, excluded_shas=mass)
    inclusive = hotspot_rows(
        commits, current_snapshot, classified, churn, limit=args.limit
    )
    exclusive = hotspot_rows(
        commits,
        current_snapshot,
        classified,
        churn_excluding_mass,
        excluded_shas=mass,
        limit=args.limit,
    )
    value = {
        "inclusive": inclusive,
        "mass_change_exclusive": exclusive,
        "mass_change_rule": long_term["mass_change_rule"],
        "mass_change_p99": long_term["mass_change_p99"],
    }
    emit(value, hotspot_markdown(inclusive, exclusive), args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
