#!/usr/bin/env python3
"""Architecture evolution sensors built from repository facts.

The module deliberately does not define product semantics or an allowed dependency
graph.  It extracts direct source dependencies, aggregates them through the
project's existing architecture manifest, and compares snapshots and history.
"""

from __future__ import annotations

from collections import Counter, defaultdict, deque
from dataclasses import dataclass
from datetime import datetime, timedelta
import itertools
import math
from pathlib import PurePosixPath
import re
from typing import Callable, Iterable, Mapping, Sequence

from architecture_lint import path_matches


PRODUCTION_EXCLUDED_LAYERS = {"tests", "web_tests", "tools"}
TEST_LAYERS = {"tests", "web_tests"}
TOOL_LAYERS = {"tools"}


@dataclass(frozen=True, order=True)
class DependencyEdge:
    source: str
    target: str
    kind: str
    reference: str


@dataclass(frozen=True, order=True)
class UnresolvedDependency:
    source: str
    kind: str
    reference: str
    reason: str


@dataclass(frozen=True)
class CommitChange:
    sha: str
    committed_at: datetime
    files: frozenset[str]
    production_file_count: int


def normalize_path(path: str) -> str:
    parts: list[str] = []
    for part in path.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts and parts[-1] != "..":
                parts.pop()
            else:
                parts.append(part)
            continue
        parts.append(part)
    return "/".join(parts)


def domain_of(path: str) -> str:
    path = normalize_path(path)
    if path.startswith("domains/wire/"):
        return "wire"
    if path.startswith("domains/road/"):
        return "road"
    if path.startswith("viewer/"):
        return "viewer"
    if path.startswith("web/"):
        return "web"
    if path.startswith("tools/"):
        return "tooling"
    return "repository"


def scope_accepts(layer: str | None, scope: str) -> bool:
    if layer is None:
        return False
    if scope == "all":
        return True
    if scope == "production":
        return layer not in PRODUCTION_EXCLUDED_LAYERS
    if scope == "tests":
        return layer in TEST_LAYERS
    if scope == "tools":
        return layer in TOOL_LAYERS
    raise ValueError(f"unknown scope: {scope}")


def classify_paths(
    paths: Iterable[str], manifest: Mapping[str, object]
) -> tuple[dict[str, str], list[str]]:
    """Classify paths with the manifest's existing layer patterns."""
    raw_layers = manifest.get("layers", [])
    if not isinstance(raw_layers, list):
        return {}, ["manifest.layers must be a list"]
    layers: list[tuple[str, list[str]]] = []
    errors: list[str] = []
    for index, raw_layer in enumerate(raw_layers):
        if not isinstance(raw_layer, Mapping):
            errors.append(f"manifest.layers[{index}] must be an object")
            continue
        name = raw_layer.get("name")
        patterns = raw_layer.get("patterns")
        if not isinstance(name, str) or not isinstance(patterns, list):
            errors.append(f"manifest.layers[{index}] has invalid name or patterns")
            continue
        layers.append((name, [p for p in patterns if isinstance(p, str)]))

    classified: dict[str, str] = {}
    for raw_path in sorted(set(paths)):
        path = normalize_path(raw_path)
        matches = [
            name
            for name, patterns in layers
            if any(path_matches(path, pattern) for pattern in patterns)
        ]
        if len(matches) == 1:
            classified[path] = matches[0]
        else:
            errors.append(
                f"{path}: expected exactly one layer, found {matches or 'none'}"
            )
    return classified, errors


def _scan_configuration(
    manifest: Mapping[str, object],
) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    raw_scan = manifest.get("scan", {})
    if not isinstance(raw_scan, Mapping):
        return (), (), ()
    roots = tuple(item for item in raw_scan.get("roots", []) if isinstance(item, str))
    extensions = tuple(
        item for item in raw_scan.get("extensions", []) if isinstance(item, str)
    )
    excludes = tuple(
        item
        for item in raw_scan.get("exclude_patterns", [])
        if isinstance(item, str)
    )
    return roots, extensions, excludes


def source_paths(
    snapshot: Mapping[str, str], manifest: Mapping[str, object]
) -> list[str]:
    roots, extensions, excludes = _scan_configuration(manifest)
    result: list[str] = []
    for raw_path in snapshot:
        path = normalize_path(raw_path)
        if roots and not any(path == root or path.startswith(f"{root}/") for root in roots):
            continue
        if extensions and PurePosixPath(path).suffix not in extensions:
            continue
        if any(path_matches(path, pattern) for pattern in excludes):
            continue
        result.append(path)
    return sorted(result)


CPP_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
TS_IMPORT_RE = re.compile(
    r'(?:\b(?:import|export)\b[^\n;]*?\bfrom\s*|\bimport\s*)["\']([^"\']+)["\']'
)
TS_ASSET_SUFFIXES = {
    ".css",
    ".glb",
    ".jpeg",
    ".jpg",
    ".png",
    ".svg",
    ".webp",
}


def _resolve_cpp_reference(
    source: str, reference: str, paths: set[str]
) -> tuple[str | None, str | None]:
    parent = PurePosixPath(source).parent
    direct = normalize_path(str(parent / reference))
    if direct in paths:
        return direct, None
    root_relative = normalize_path(reference)
    if root_relative in paths:
        return root_relative, None
    suffix = f"/{root_relative}"
    candidates = sorted(path for path in paths if path.endswith(suffix))
    if len(candidates) == 1:
        return candidates[0], None
    if len(candidates) > 1:
        return None, f"ambiguous internal include: {', '.join(candidates)}"
    return None, "target is not a scanned repository source"


def _resolve_ts_reference(
    source: str, reference: str, paths: set[str]
) -> tuple[str | None, str | None]:
    if not reference.startswith("."):
        return None, "package or platform import"
    base = normalize_path(str(PurePosixPath(source).parent / reference))
    candidates = [base]
    suffix = PurePosixPath(base).suffix
    if not suffix:
        candidates.extend(
            [
                f"{base}.ts",
                f"{base}.svelte",
                f"{base}/index.ts",
                f"{base}/index.svelte",
            ]
        )
    elif suffix == ".js":
        candidates.append(f"{base[:-3]}.ts")
    resolved = [candidate for candidate in candidates if candidate in paths]
    if len(resolved) == 1:
        return resolved[0], None
    if len(resolved) > 1:
        return None, f"ambiguous relative import: {', '.join(resolved)}"
    return None, "target is generated, asset, or outside the scanned sources"


def build_graph(
    snapshot: Mapping[str, str],
    manifest: Mapping[str, object],
    *,
    scope: str = "production",
) -> dict[str, object]:
    files = source_paths(snapshot, manifest)
    classified, classification_errors = classify_paths(files, manifest)
    path_set = set(files)
    selected = [path for path in files if scope_accepts(classified.get(path), scope)]
    edges: set[DependencyEdge] = set()
    unresolved: set[UnresolvedDependency] = set()

    for source in selected:
        text = snapshot[source]
        suffix = PurePosixPath(source).suffix
        if suffix in {".cpp", ".hpp"}:
            for reference in CPP_INCLUDE_RE.findall(text):
                target, reason = _resolve_cpp_reference(source, reference, path_set)
                if target is not None:
                    edges.add(DependencyEdge(source, target, "cpp_include", reference))
                else:
                    unresolved.add(
                        UnresolvedDependency(source, "cpp_include", reference, reason or "unresolved")
                    )
        elif suffix in {".ts", ".svelte"}:
            for reference in TS_IMPORT_RE.findall(text):
                target, reason = _resolve_ts_reference(source, reference, path_set)
                if target is not None:
                    edges.add(DependencyEdge(source, target, "ts_import", reference))
                elif (
                    reference.startswith(".")
                    and "?url" not in reference
                    and PurePosixPath(reference).suffix.lower() not in TS_ASSET_SUFFIXES
                ):
                    unresolved.add(
                        UnresolvedDependency(source, "ts_import", reference, reason or "unresolved")
                    )

    nodes = [
        {
            "path": path,
            "domain": domain_of(path),
            "layer": classified.get(path),
            "physical_directory": str(PurePosixPath(path).parent),
        }
        for path in files
        if path in selected or any(edge.target == path for edge in edges)
    ]
    edge_rows = [
        {
            "source": edge.source,
            "target": edge.target,
            "kind": edge.kind,
            "reference": edge.reference,
            "source_domain": domain_of(edge.source),
            "source_layer": classified.get(edge.source),
            "target_domain": domain_of(edge.target),
            "target_layer": classified.get(edge.target),
        }
        for edge in sorted(edges)
    ]
    aggregate = aggregate_edges(edge_rows)
    return {
        "scope": scope,
        "nodes": sorted(nodes, key=lambda row: str(row["path"])),
        "edges": edge_rows,
        "unresolved": [item.__dict__ for item in sorted(unresolved)],
        "classification_errors": classification_errors,
        "aggregate_edges": aggregate,
        "cycles": strongly_connected_components(aggregate),
        "concentration": dependency_concentration(aggregate),
    }


def aggregate_key(domain: object, layer: object) -> str:
    return f"{domain}/{layer}"


def aggregate_edges(edges: Sequence[Mapping[str, object]]) -> list[dict[str, object]]:
    counts: Counter[tuple[str, str]] = Counter()
    for edge in edges:
        source = aggregate_key(edge.get("source_domain"), edge.get("source_layer"))
        target = aggregate_key(edge.get("target_domain"), edge.get("target_layer"))
        if source != target:
            counts[(source, target)] += 1
    return [
        {"source": source, "target": target, "count": count}
        for (source, target), count in sorted(counts.items())
    ]


def strongly_connected_components(
    aggregate_edges_rows: Sequence[Mapping[str, object]],
) -> list[list[str]]:
    graph: dict[str, set[str]] = defaultdict(set)
    nodes: set[str] = set()
    for edge in aggregate_edges_rows:
        source = str(edge["source"])
        target = str(edge["target"])
        graph[source].add(target)
        nodes.update((source, target))

    index = 0
    indices: dict[str, int] = {}
    lowlink: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    components: list[list[str]] = []

    def visit(node: str) -> None:
        nonlocal index
        indices[node] = index
        lowlink[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for target in sorted(graph.get(node, ())):
            if target not in indices:
                visit(target)
                lowlink[node] = min(lowlink[node], lowlink[target])
            elif target in on_stack:
                lowlink[node] = min(lowlink[node], indices[target])
        if lowlink[node] == indices[node]:
            component: list[str] = []
            while True:
                current = stack.pop()
                on_stack.remove(current)
                component.append(current)
                if current == node:
                    break
            if len(component) > 1:
                components.append(sorted(component))

    for node in sorted(nodes):
        if node not in indices:
            visit(node)
    return sorted(components)


def dependency_concentration(
    aggregate_edges_rows: Sequence[Mapping[str, object]],
) -> list[dict[str, object]]:
    incoming: Counter[str] = Counter()
    outgoing: Counter[str] = Counter()
    for edge in aggregate_edges_rows:
        count = int(edge.get("count", 0))
        outgoing[str(edge["source"])] += count
        incoming[str(edge["target"])] += count
    nodes = sorted(set(incoming) | set(outgoing))
    return [
        {"node": node, "fan_in": incoming[node], "fan_out": outgoing[node]}
        for node in sorted(nodes, key=lambda item: (-(incoming[item] + outgoing[item]), item))
    ]


def graph_markdown(graph: Mapping[str, object]) -> str:
    aggregate = list(graph.get("aggregate_edges", []))
    labels = sorted(
        {str(edge["source"]) for edge in aggregate}
        | {str(edge["target"]) for edge in aggregate}
    )
    counts = {
        (str(edge["source"]), str(edge["target"])): int(edge["count"])
        for edge in aggregate
    }
    lines = [
        "# Hierarchical Dependency Structure Matrix",
        "",
        f"Scope: `{graph.get('scope', 'unknown')}`",
        "",
        "Rows depend on columns. The matrix is aggregated as Repository → Domain → existing manifest layer; file edges remain in JSON drill-down.",
        "",
    ]
    if not labels:
        lines.append("No cross-layer dependency edges were extracted.")
    else:
        lines.append("| From \\ To | " + " | ".join(labels) + " |")
        lines.append("|---|" + "---|" * len(labels))
        for source in labels:
            cells = [str(counts.get((source, target), "")) for target in labels]
            lines.append(f"| {source} | " + " | ".join(cells) + " |")
    lines.extend(["", "## Cycles", ""])
    cycles = list(graph.get("cycles", []))
    if cycles:
        lines.extend(f"- {' → '.join(cycle)}" for cycle in cycles)
    else:
        lines.append("No cross-layer strongly connected component was found.")
    lines.extend(["", "## Unresolved direct references", ""])
    unresolved = list(graph.get("unresolved", []))
    if unresolved:
        lines.extend(
            f"- `{row['source']}`: `{row['reference']}` ({row['reason']})"
            for row in unresolved
        )
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def _edge_identity(edge: Mapping[str, object]) -> tuple[str, str, str]:
    return str(edge["source"]), str(edge["target"]), str(edge["kind"])


def graph_delta(
    base_graph: Mapping[str, object], current_graph: Mapping[str, object]
) -> dict[str, object]:
    base_edges = {_edge_identity(edge): edge for edge in base_graph.get("edges", [])}
    current_edges = {
        _edge_identity(edge): edge for edge in current_graph.get("edges", [])
    }
    base_aggregate = {
        (str(edge["source"]), str(edge["target"]))
        for edge in base_graph.get("aggregate_edges", [])
    }
    current_aggregate = {
        (str(edge["source"]), str(edge["target"]))
        for edge in current_graph.get("aggregate_edges", [])
    }
    return {
        "file_edges_added": [current_edges[key] for key in sorted(current_edges.keys() - base_edges.keys())],
        "file_edges_removed": [base_edges[key] for key in sorted(base_edges.keys() - current_edges.keys())],
        "layer_relations_added": [
            {"source": source, "target": target}
            for source, target in sorted(current_aggregate - base_aggregate)
        ],
        "layer_relations_removed": [
            {"source": source, "target": target}
            for source, target in sorted(base_aggregate - current_aggregate)
        ],
        "cycles_added": sorted(
            [cycle for cycle in current_graph.get("cycles", []) if cycle not in base_graph.get("cycles", [])]
        ),
        "cycles_removed": sorted(
            [cycle for cycle in base_graph.get("cycles", []) if cycle not in current_graph.get("cycles", [])]
        ),
    }


def reflexion_report(
    base_graph: Mapping[str, object],
    current_graph: Mapping[str, object],
    manifest: Mapping[str, object],
    *,
    existing_lint_diagnostics: Sequence[str] = (),
    required_relations: Sequence[tuple[str, str]] = (),
) -> dict[str, object]:
    actual = {
        (str(edge["source"]), str(edge["target"]))
        for edge in current_graph.get("aggregate_edges", [])
    }
    required = set(required_relations)
    delta = graph_delta(base_graph, current_graph)
    forbidden_contracts: list[dict[str, object]] = []
    for raw_layer in manifest.get("layers", []):
        if not isinstance(raw_layer, Mapping):
            continue
        tokens = raw_layer.get("forbidden_tokens", [])
        if tokens:
            forbidden_contracts.append(
                {"layer": raw_layer.get("name"), "forbidden_tokens": list(tokens)}
            )
    return {
        "model_limit": (
            "Wire has no comprehensive allowed-dependency model. Existing required/forbidden "
            "contracts are shown without inventing a global authority; all other actual edges are unmodeled."
        ),
        "convergence": [
            {"source": source, "target": target}
            for source, target in sorted(required & actual)
        ],
        "absence": [
            {"source": source, "target": target}
            for source, target in sorted(required - actual)
        ],
        "divergence_from_existing_lint": list(existing_lint_diagnostics),
        "forbidden_contracts_owned_by_existing_lint": forbidden_contracts,
        "unmodeled_relations": [
            {"source": source, "target": target}
            for source, target in sorted(actual - required)
        ],
        "new_unmodeled_file_dependencies": delta["file_edges_added"],
        "new_unmodeled_layer_relations": delta["layer_relations_added"],
    }


def parse_name_status_log(text: str) -> tuple[list[CommitChange], dict[str, str]]:
    """Parse first-parent log output and carry old paths through renames."""
    raw_commits: list[tuple[str, datetime, list[list[str]]]] = []
    for block in text.split("\x1e"):
        block = block.strip()
        if not block:
            continue
        lines = [line for line in block.splitlines() if line.strip()]
        header = lines[0].split("\x1f")
        if len(header) != 2:
            continue
        sha, committed_text = header
        changes = [line.split("\t") for line in lines[1:]]
        raw_commits.append((sha, datetime.fromisoformat(committed_text), changes))

    aliases: dict[str, str] = {}

    def resolve(path: str) -> str:
        seen: set[str] = set()
        path = normalize_path(path)
        while path in aliases and path not in seen:
            seen.add(path)
            path = aliases[path]
        return path

    commits: list[CommitChange] = []
    for sha, committed_at, changes in raw_commits:
        for cells in changes:
            if cells and cells[0].startswith("R") and len(cells) >= 3:
                aliases[normalize_path(cells[1])] = resolve(cells[2])
        files: set[str] = set()
        for cells in changes:
            if not cells:
                continue
            if cells[0].startswith(("R", "C")) and len(cells) >= 3:
                files.add(resolve(cells[2]))
            elif len(cells) >= 2:
                files.add(resolve(cells[1]))
        commits.append(
            CommitChange(
                sha=sha,
                committed_at=committed_at,
                files=frozenset(files),
                production_file_count=0,
            )
        )
    return commits, aliases


def _percentile(values: Sequence[int], percentile: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[math.floor((len(ordered) - 1) * percentile)]


def annotate_production_counts(
    commits: Sequence[CommitChange],
    classified: Mapping[str, str],
) -> list[CommitChange]:
    return [
        CommitChange(
            sha=commit.sha,
            committed_at=commit.committed_at,
            files=commit.files,
            production_file_count=sum(
                1
                for path in commit.files
                if scope_accepts(classified.get(path), "production")
            ),
        )
        for commit in commits
    ]


def _shortest_path_edges(
    relations: set[tuple[str, str]], source: str, target: str
) -> int | None:
    if source == target:
        return 0
    adjacency: dict[str, set[str]] = defaultdict(set)
    for left, right in relations:
        adjacency[left].add(right)
    pending = deque([(source, 0)])
    visited = {source}
    while pending:
        current, distance = pending.popleft()
        for next_node in sorted(adjacency.get(current, ())):
            if next_node == target:
                return distance + 1
            if next_node not in visited:
                visited.add(next_node)
                pending.append((next_node, distance + 1))
    return None


def _static_distance_fields(
    left: str, right: str, relations: set[tuple[str, str]]
) -> dict[str, object]:
    left_to_right = _shortest_path_edges(relations, left, right)
    right_to_left = _shortest_path_edges(relations, right, left)
    reachable = [
        distance
        for distance in (left_to_right, right_to_left)
        if distance is not None
    ]
    nearest = min(reachable) if reachable else None
    if nearest is None:
        label = "unreachable"
    elif nearest == 1:
        label = "direct"
    elif nearest == 2:
        label = "1_intermediate"
    elif nearest == 3:
        label = "2_intermediates"
    else:
        label = "3_plus_intermediates"
    return {
        "static_relation": nearest == 1,
        "static_distance": label,
        "static_path_left_to_right": left_to_right,
        "static_path_right_to_left": right_to_left,
    }


def cochange_rows(
    commits: Sequence[CommitChange],
    unit_of: Callable[[str], str | None],
    *,
    excluded_shas: set[str] | None = None,
    static_relations: set[tuple[str, str]] | None = None,
    limit: int | None = 50,
) -> list[dict[str, object]]:
    excluded_shas = excluded_shas or set()
    static_relations = static_relations or set()
    analyzed_commit_count = sum(1 for commit in commits if commit.sha not in excluded_shas)
    touches: Counter[str] = Counter()
    pairs: Counter[tuple[str, str]] = Counter()
    for commit in commits:
        if commit.sha in excluded_shas:
            continue
        units = sorted({unit for path in commit.files if (unit := unit_of(path))})
        for unit in units:
            touches[unit] += 1
        for left, right in itertools.combinations(units, 2):
            pairs[(left, right)] += 1
    ranked = sorted(pairs.items(), key=lambda item: (-item[1], item[0]))
    if limit is not None:
        ranked = ranked[:limit]
    return [
        {
            "left": left,
            "right": right,
            "cochange_commits": count,
            "support": count / analyzed_commit_count if analyzed_commit_count else 0.0,
            "confidence_left_to_right": count / touches[left],
            "confidence_right_to_left": count / touches[right],
            **_static_distance_fields(left, right, static_relations),
        }
        for (left, right), count in ranked
    ]


def cochange_strengthening_rows(
    previous_commits: Sequence[CommitChange],
    recent_commits: Sequence[CommitChange],
    unit_of: Callable[[str], str | None],
    *,
    previous_excluded_shas: set[str] | None = None,
    recent_excluded_shas: set[str] | None = None,
    static_relations: set[tuple[str, str]] | None = None,
    limit: int = 50,
) -> list[dict[str, object]]:
    static_relations = static_relations or set()
    previous = {
        (str(row["left"]), str(row["right"])): row
        for row in cochange_rows(
            previous_commits,
            unit_of,
            excluded_shas=previous_excluded_shas,
            static_relations=static_relations,
            limit=None,
        )
    }
    recent = {
        (str(row["left"]), str(row["right"])): row
        for row in cochange_rows(
            recent_commits,
            unit_of,
            excluded_shas=recent_excluded_shas,
            static_relations=static_relations,
            limit=None,
        )
    }
    rows: list[dict[str, object]] = []
    for left, right in sorted(previous.keys() | recent.keys()):
        before = previous.get((left, right), {})
        after = recent.get((left, right), {})
        support_delta = float(after.get("support", 0.0)) - float(
            before.get("support", 0.0)
        )
        left_delta = float(after.get("confidence_left_to_right", 0.0)) - float(
            before.get("confidence_left_to_right", 0.0)
        )
        right_delta = float(after.get("confidence_right_to_left", 0.0)) - float(
            before.get("confidence_right_to_left", 0.0)
        )
        if max(support_delta, left_delta, right_delta) <= 0.0:
            continue
        rows.append(
            {
                "left": left,
                "right": right,
                "previous_cochange_commits": int(
                    before.get("cochange_commits", 0)
                ),
                "recent_cochange_commits": int(after.get("cochange_commits", 0)),
                "previous_support": float(before.get("support", 0.0)),
                "recent_support": float(after.get("support", 0.0)),
                "support_delta": support_delta,
                "previous_confidence_left_to_right": float(
                    before.get("confidence_left_to_right", 0.0)
                ),
                "recent_confidence_left_to_right": float(
                    after.get("confidence_left_to_right", 0.0)
                ),
                "confidence_left_to_right_delta": left_delta,
                "previous_confidence_right_to_left": float(
                    before.get("confidence_right_to_left", 0.0)
                ),
                "recent_confidence_right_to_left": float(
                    after.get("confidence_right_to_left", 0.0)
                ),
                "confidence_right_to_left_delta": right_delta,
                **_static_distance_fields(left, right, static_relations),
            }
        )
    return sorted(
        rows,
        key=lambda row: (
            -float(row["support_delta"]),
            -max(
                float(row["confidence_left_to_right_delta"]),
                float(row["confidence_right_to_left_delta"]),
            ),
            -int(row["recent_cochange_commits"]),
            str(row["left"]),
            str(row["right"]),
        ),
    )[:limit]


def history_report(
    commits: Sequence[CommitChange],
    classified: Mapping[str, str],
    graph: Mapping[str, object],
    *,
    recent_days: int | None = None,
    window_commits: int = 50,
    limit: int = 50,
) -> dict[str, object]:
    if window_commits <= 0:
        raise ValueError("window_commits must be positive")
    annotated = annotate_production_counts(commits, classified)
    newest = max((commit.committed_at for commit in annotated), default=datetime.min)
    static_file = {
        (str(edge["source"]), str(edge["target"])) for edge in graph.get("edges", [])
    }
    static_module = {
        (str(edge["source"]), str(edge["target"]))
        for edge in graph.get("aggregate_edges", [])
    }

    def file_unit(path: str) -> str | None:
        layer = classified.get(path)
        return path if scope_accepts(layer, "production") else None

    def module_unit(path: str) -> str | None:
        layer = classified.get(path)
        if not scope_accepts(layer, "production"):
            return None
        return aggregate_key(domain_of(path), layer)

    recent_commits = annotated[:window_commits]
    previous_commits = annotated[window_commits : window_commits * 2]
    selections: list[tuple[str, Sequence[CommitChange]]] = [
        ("long_term", annotated),
        ("previous_commits", previous_commits),
        ("recent_commits", recent_commits),
    ]
    if recent_days is not None:
        recent_cutoff = newest - timedelta(days=recent_days)
        selections.append(
            (
                "recent_days",
                [commit for commit in annotated if commit.committed_at >= recent_cutoff],
            )
        )

    windows: dict[str, object] = {}
    excluded_by_window: dict[str, set[str]] = {}
    for name, selected in selections:
        cutoff = _percentile(
            [commit.production_file_count for commit in selected], 0.99
        )
        mass = {
            commit.sha for commit in selected if commit.production_file_count > cutoff
        }
        excluded_by_window[name] = mass
        file_inclusive = cochange_rows(
            selected, file_unit, static_relations=static_file, limit=limit
        )
        file_exclusive = cochange_rows(
            selected,
            file_unit,
            excluded_shas=mass,
            static_relations=static_file,
            limit=limit,
        )
        module_inclusive = cochange_rows(
            selected, module_unit, static_relations=static_module, limit=limit
        )
        module_exclusive = cochange_rows(
            selected,
            module_unit,
            excluded_shas=mass,
            static_relations=static_module,
            limit=limit,
        )
        windows[name] = {
            "commit_count": len(selected),
            "commit_shas": [commit.sha for commit in selected],
            "mass_change_rule": "production file count > p99 within this window",
            "mass_change_p99": cutoff,
            "mass_change_commits": [
                {"sha": commit.sha, "production_files": commit.production_file_count}
                for commit in selected
                if commit.sha in mass
            ],
            "file_cochange_inclusive": file_inclusive,
            "file_cochange_exclusive": file_exclusive,
            "module_cochange_inclusive": module_inclusive,
            "module_cochange_exclusive": module_exclusive,
            "file_cochange_without_direct_static_edge": [
                row for row in file_exclusive if not row["static_relation"]
            ],
            "module_cochange_without_direct_static_edge": [
                row for row in module_exclusive if not row["static_relation"]
            ],
        }
    return {
        "history_model": (
            "Each change set is the commit diff against its first parent on the first-parent chain. "
            "Merge commits are counted once, so merged branch commits are not counted again."
        ),
        "trend_model": (
            "Recently strengthened co-change compares equal-sized adjacent first-parent commit "
            "windows. Deltas are review evidence, not defect claims or quality gates."
        ),
        "recent_days": recent_days,
        "window_commits": window_commits,
        "windows": windows,
        "module_cochange_strengthening": cochange_strengthening_rows(
            previous_commits,
            recent_commits,
            module_unit,
            previous_excluded_shas=excluded_by_window["previous_commits"],
            recent_excluded_shas=excluded_by_window["recent_commits"],
            static_relations=static_module,
            limit=limit,
        ),
    }


def hotspot_rows(
    commits: Sequence[CommitChange],
    snapshot: Mapping[str, str],
    classified: Mapping[str, str],
    churn: Mapping[str, int],
    *,
    excluded_shas: set[str] | None = None,
    limit: int = 50,
) -> list[dict[str, object]]:
    excluded_shas = excluded_shas or set()
    changes: Counter[str] = Counter()
    for commit in commits:
        if commit.sha in excluded_shas:
            continue
        for path in commit.files:
            if scope_accepts(classified.get(path), "production"):
                changes[path] += 1
    rows = [
        {
            "file": path,
            "domain": domain_of(path),
            "layer": classified.get(path),
            "change_commits": count,
            "churn_lines": int(churn.get(path, 0)),
            "current_loc": len(snapshot.get(path, "").splitlines()),
        }
        for path, count in changes.items()
        if path in snapshot
    ]
    return sorted(
        rows,
        key=lambda row: (
            -int(row["change_commits"]),
            -int(row["churn_lines"]),
            -int(row["current_loc"]),
            str(row["file"]),
        ),
    )[:limit]


def parse_numstat_log(
    text: str,
    aliases: Mapping[str, str],
    *,
    excluded_shas: set[str] | None = None,
) -> dict[str, int]:
    churn: Counter[str] = Counter()
    excluded_shas = excluded_shas or set()

    def resolve(path: str) -> str:
        path = normalize_path(path)
        seen: set[str] = set()
        while path in aliases and path not in seen:
            seen.add(path)
            path = aliases[path]
        return path

    for block in text.split("\x1e"):
        lines = block.splitlines()
        if not lines:
            continue
        current_sha: str | None = None
        if "\x1f" in lines[0]:
            current_sha = lines[0].split("\x1f", 1)[0]
            lines = lines[1:]
        if current_sha in excluded_shas:
            continue
        for line in lines:
            cells = line.split("\t")
            if len(cells) != 3 or not cells[0].isdigit() or not cells[1].isdigit():
                continue
            path = cells[2]
            brace = re.search(r"\{([^{}]+) => ([^{}]+)\}", path)
            if brace:
                path = path[: brace.start()] + brace.group(2) + path[brace.end() :]
            elif " => " in path:
                path = path.split(" => ", 1)[1]
            churn[resolve(path)] += int(cells[0]) + int(cells[1])
    return dict(churn)


def sensor_retirement_note() -> str:
    return (
        "Retire or merge a sensor when a stronger mechanism absorbs it, another sensor "
        "provides the same evidence, it produces no useful signal, or its maintenance cost "
        "exceeds its review value. Sensor count is not a quality objective."
    )


def architecture_delta_report(
    base_graph: Mapping[str, object],
    current_graph: Mapping[str, object],
    changes: Sequence[Mapping[str, object]],
    *,
    fallback_term_evidence: Sequence[Mapping[str, object]] = (),
) -> dict[str, object]:
    """Separate directly observable facts from semantic review candidates."""
    dependency = graph_delta(base_graph, current_graph)
    touched = sorted(
        {
            normalize_path(str(path))
            for change in changes
            for path in change.get("paths", [])
        }
    )
    base_layers = {
        str(node["path"]): node.get("layer") for node in base_graph.get("nodes", [])
    }
    current_layers = {
        str(node["path"]): node.get("layer")
        for node in current_graph.get("nodes", [])
    }
    classification_changes = [
        {
            "path": path,
            "base_layer": base_layers.get(path),
            "current_layer": current_layers.get(path),
        }
        for path in sorted(set(base_layers) | set(current_layers))
        if base_layers.get(path) != current_layers.get(path)
    ]
    public_headers = [
        path
        for path in touched
        if re.match(r"^domains/[^/]+/include/.+\.hpp$", path)
    ]
    manifest_files = [path for path in touched if path == "tools/arch_manifest.json"]
    architecture_guard_files = [
        path
        for path in touched
        if path
        in {
            "tools/arch_manifest.json",
            "tools/arch_lint.py",
            "tools/harness/architecture_lint.py",
            "web/tests/architecture_boundary.test.ts",
        }
    ]
    candidates = [
        {
            "name": "public_api_review",
            "files": public_headers,
            "claim_limit": "A public header was touched; API semantics are not inferred.",
        },
        {
            "name": "authoritative_state_review",
            "files": [
                path
                for path in touched
                if "/state/" in path
                or "authoritative" in path.lower()
                or path.endswith("entities.hpp")
            ],
            "claim_limit": "Authority-related sources were touched; state meaning is not inferred.",
        },
        {
            "name": "persistence_review",
            "files": [path for path in touched if "persistence" in path or "archive" in path],
            "claim_limit": "Persistence-related files were touched; schema compatibility is not inferred.",
        },
        {
            "name": "operation_semantics_review",
            "files": [
                path
                for path in touched
                if path.endswith("operation_semantics.md")
                or path.endswith("backbone_operation_semantics.md")
            ],
            "claim_limit": "An operation-semantics document was touched; behavior change is not inferred.",
        },
        {
            "name": "decision_owner_review",
            "files": [
                path
                for path in touched
                if path.endswith("architecture.md")
                or path.endswith("arch_manifest.json")
                or path.endswith("spec_ledger.md")
            ],
            "claim_limit": "An owner-bearing source was touched; ownership change is not inferred.",
        },
        {
            "name": "fallback_or_special_path_review",
            "evidence": list(fallback_term_evidence),
            "claim_limit": "Added terms are review prompts only; no fallback semantics are inferred.",
        },
    ]
    return {
        "structural_facts": {
            "repository_changes": list(changes),
            "dependency_delta": dependency,
            "public_headers_touched": public_headers,
            "architecture_manifest_touched": manifest_files,
            "architecture_guard_files_touched": architecture_guard_files,
            "source_classification_changes": classification_changes,
        },
        "human_review_candidates": [
            candidate
            for candidate in candidates
            if candidate.get("files") or candidate.get("evidence")
        ],
        "sensor_retirement": sensor_retirement_note(),
    }


def reflexion_markdown(report: Mapping[str, object]) -> str:
    lines = [
        "# Software Reflexion Model",
        "",
        str(report.get("model_limit", "")),
        "",
    ]
    for heading, key in (
        ("Convergence", "convergence"),
        ("Divergence from existing lint", "divergence_from_existing_lint"),
        ("Absence", "absence"),
        ("New unmodeled layer relations", "new_unmodeled_layer_relations"),
        ("New unmodeled file dependencies", "new_unmodeled_file_dependencies"),
        ("All unmodeled relations", "unmodeled_relations"),
    ):
        lines.extend([f"## {heading}", ""])
        rows = list(report.get(key, []))
        if not rows:
            lines.append("None.")
        else:
            for row in rows:
                if isinstance(row, Mapping):
                    source = row.get("source", "")
                    target = row.get("target", "")
                    kind = f" ({row.get('kind')})" if row.get("kind") else ""
                    lines.append(f"- `{source}` → `{target}`{kind}")
                else:
                    lines.append(f"- {row}")
        lines.append("")
    return "\n".join(lines)


def delta_markdown(report: Mapping[str, object]) -> str:
    facts = report.get("structural_facts", {})
    candidates = report.get("human_review_candidates", [])
    lines = [
        "# Architecture Delta",
        "",
        "## Structural facts",
        "",
    ]
    dependency = facts.get("dependency_delta", {}) if isinstance(facts, Mapping) else {}
    for label, key in (
        ("Layer relations added", "layer_relations_added"),
        ("Layer relations removed", "layer_relations_removed"),
        ("File dependencies added", "file_edges_added"),
        ("File dependencies removed", "file_edges_removed"),
        ("Cycles added", "cycles_added"),
        ("Cycles removed", "cycles_removed"),
    ):
        rows = list(dependency.get(key, [])) if isinstance(dependency, Mapping) else []
        lines.append(f"### {label}")
        lines.append("")
        if rows:
            for row in rows:
                if isinstance(row, Mapping):
                    lines.append(f"- `{row.get('source', '')}` → `{row.get('target', '')}`")
                else:
                    lines.append(f"- {row}")
        else:
            lines.append("None.")
        lines.append("")
    if isinstance(facts, Mapping):
        lines.extend(["### Public headers touched", ""])
        headers = list(facts.get("public_headers_touched", []))
        lines.extend([f"- `{path}`" for path in headers] or ["None."])
        lines.extend(["", "### Architecture guard files touched", ""])
        guards = list(facts.get("architecture_guard_files_touched", []))
        lines.extend([f"- `{path}`" for path in guards] or ["None."])
        lines.extend(["", "### Source classification changes", ""])
        rows = list(facts.get("source_classification_changes", []))
        lines.extend(
            [
                f"- `{row['path']}`: `{row['base_layer']}` → `{row['current_layer']}`"
                for row in rows
            ]
            or ["None."]
        )
    lines.extend(["", "## Human review candidates", ""])
    if not candidates:
        lines.append("None.")
    else:
        for candidate in candidates:
            lines.append(f"### {candidate['name']}")
            lines.append("")
            lines.append(str(candidate["claim_limit"]))
            for path in candidate.get("files", []):
                lines.append(f"- `{path}`")
            for evidence in candidate.get("evidence", []):
                lines.append(
                    f"- `{evidence.get('path', '')}:{evidence.get('line', '')}`: "
                    f"`{evidence.get('text', '')}`"
                )
            lines.append("")
    lines.extend(["## Sensor retirement", "", str(report.get("sensor_retirement", "")), ""])
    return "\n".join(lines)


def history_markdown(report: Mapping[str, object]) -> str:
    lines = [
        "# Git Co-change / Logical Coupling",
        "",
        str(report.get("history_model", "")),
        "",
        str(report.get("trend_model", "")),
        "",
        (
            "Static distance is the shortest directed dependency path in either direction. "
            "`1_intermediate` means two dependency edges; `unreachable` means no path was extracted."
        ),
        "",
    ]
    trends = list(report.get("module_cochange_strengthening", []))
    lines.extend(["## Recently strengthened module co-change", ""])
    if not trends:
        lines.extend(["None.", ""])
    else:
        lines.append(
            "| Left | Right | Commits previous→recent | Δ support | Δ L→R | Δ R→L | Static distance | Static L→R / R→L |"
        )
        lines.append("|---|---|---:|---:|---:|---:|---|---|")
        for row in trends:
            left_path = row.get("static_path_left_to_right")
            right_path = row.get("static_path_right_to_left")
            lines.append(
                f"| {row['left']} | {row['right']} | "
                f"{row['previous_cochange_commits']}→{row['recent_cochange_commits']} | "
                f"{row['support_delta']:+.3f} | "
                f"{row['confidence_left_to_right_delta']:+.3f} | "
                f"{row['confidence_right_to_left_delta']:+.3f} | "
                f"{row['static_distance']} | "
                f"{left_path if left_path is not None else 'unreachable'} / "
                f"{right_path if right_path is not None else 'unreachable'} |"
            )
        lines.append("")
    windows = report.get("windows", {})
    if not isinstance(windows, Mapping):
        return "\n".join(lines)
    for name, raw_window in windows.items():
        if not isinstance(raw_window, Mapping):
            continue
        lines.extend(
            [
                f"## {name}",
                "",
                f"Commits: {raw_window.get('commit_count', 0)}",
                "",
                f"Mass-change separation: {raw_window.get('mass_change_rule', '')}; p99={raw_window.get('mass_change_p99', 0)}",
                "",
            ]
        )
        mass = list(raw_window.get("mass_change_commits", []))
        if mass:
            lines.append("Mass-change commits:")
            lines.append("")
            lines.extend(
                f"- `{row['sha']}` ({row['production_files']} production files)"
                for row in mass
            )
            lines.append("")
        for title, key in (
            ("Module co-change inclusive", "module_cochange_inclusive"),
            ("Module co-change mass-change exclusive", "module_cochange_exclusive"),
            (
                "Module co-change without a direct static edge",
                "module_cochange_without_direct_static_edge",
            ),
        ):
            lines.extend([f"### {title}", ""])
            rows = list(raw_window.get(key, []))
            if not rows:
                lines.append("None.")
            else:
                lines.append(
                    "| Left | Right | Commits | Support | L→R | R→L | Static distance | Static L→R / R→L |"
                )
                lines.append("|---|---|---:|---:|---:|---:|---|---|")
                for row in rows:
                    left_path = row.get("static_path_left_to_right")
                    right_path = row.get("static_path_right_to_left")
                    lines.append(
                        f"| {row['left']} | {row['right']} | {row['cochange_commits']} | "
                        f"{row['support']:.3f} | "
                        f"{row['confidence_left_to_right']:.3f} | "
                        f"{row['confidence_right_to_left']:.3f} | "
                        f"{row['static_distance']} | "
                        f"{left_path if left_path is not None else 'unreachable'} / "
                        f"{right_path if right_path is not None else 'unreachable'} |"
                    )
            lines.append("")
    lines.extend(["## Sensor retirement", "", sensor_retirement_note(), ""])
    return "\n".join(lines)


def hotspot_markdown(
    inclusive: Sequence[Mapping[str, object]],
    exclusive: Sequence[Mapping[str, object]],
) -> str:
    lines = [
        "# Hotspot Analysis",
        "",
        "Change frequency, churn, and current LOC are review sensors, not quality gates.",
        "Mass-change exclusion is applied to both change frequency and churn.",
        "",
    ]
    for title, rows in (
        ("Inclusive", inclusive),
        ("Mass-change exclusive", exclusive),
    ):
        lines.extend([f"## {title}", ""])
        lines.append("| File | Domain / layer | Change commits | Churn | LOC |")
        lines.append("|---|---|---:|---:|---:|")
        for row in rows:
            lines.append(
                f"| {row['file']} | {row['domain']} / {row['layer']} | "
                f"{row['change_commits']} | {row['churn_lines']} | {row['current_loc']} |"
            )
        lines.append("")
    lines.extend(["## Sensor retirement", "", sensor_retirement_note(), ""])
    return "\n".join(lines)
