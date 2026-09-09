#!/usr/bin/env python3
from __future__ import annotations

from datetime import datetime, timezone
import subprocess
import sys
import unittest
from pathlib import Path


HARNESS_ROOT = Path(__file__).resolve().parent
REPOSITORY_ROOT = HARNESS_ROOT.parents[1]
sys.path.insert(0, str(HARNESS_ROOT))

from architecture_observation import (
    CommitChange,
    architecture_delta_report,
    build_graph,
    history_report,
    parse_name_status_log,
    parse_numstat_log,
    reflexion_report,
    sensor_retirement_note,
)


def manifest() -> dict[str, object]:
    return {
        "scan": {
            "roots": ["domains/wire", "viewer", "web", "tools"],
            "extensions": [".cpp", ".hpp", ".ts", ".svelte", ".py"],
            "exclude_patterns": ["web/src/generated/**"],
        },
        "layers": [
            {
                "name": "wire_public",
                "patterns": ["domains/wire/include/**/*.hpp"],
                "forbidden_tokens": ["viewer/"],
            },
            {
                "name": "wire_core",
                "patterns": ["domains/wire/src/**/*.cpp", "domains/wire/src/**/*.hpp"],
                "forbidden_tokens": ["viewer/"],
            },
            {
                "name": "viewer",
                "patterns": ["viewer/src/**/*.cpp", "viewer/src/**/*.hpp"],
                "forbidden_tokens": [],
            },
            {
                "name": "web_model",
                "patterns": ["web/src/model.ts"],
                "forbidden_tokens": [],
            },
            {
                "name": "web_actions",
                "patterns": ["web/src/actions/*.ts"],
                "forbidden_tokens": [],
            },
            {
                "name": "web_tests",
                "patterns": ["web/tests/*.ts"],
                "forbidden_tokens": [],
            },
            {
                "name": "tools",
                "patterns": ["tools/*.py"],
                "forbidden_tokens": [],
            },
        ],
    }


def snapshot(*, extra_import: bool = False) -> dict[str, str]:
    action_import = 'import type { Model } from "../model";'
    model_source = "export type Model = {};"
    if extra_import:
        model_source = 'import type { Run } from "./actions/other";\nexport type Model = Run;'
    return {
        "domains/wire/include/city/wire/api.hpp": "struct Api {};",
        "domains/wire/src/local.hpp": "#pragma once",
        "domains/wire/src/core.cpp": '#include "local.hpp"\n#include "city/wire/api.hpp"',
        "viewer/src/view.cpp": '#include "city/wire/api.hpp"',
        "web/src/model.ts": model_source,
        "web/src/actions/run.ts": action_import,
        "web/src/actions/other.ts": "export type Run = {};",
        "web/tests/run.test.ts": 'import "../src/actions/run";',
        "tools/sample.py": "pass",
        "web/src/generated/ignored.ts": "export {};",
    }


class ArchitectureObservationTest(unittest.TestCase):
    def test_graph_uses_domain_and_existing_manifest_layer(self) -> None:
        graph = build_graph(snapshot(), manifest())
        relations = {
            (edge["source"], edge["target"])
            for edge in graph["aggregate_edges"]
        }
        self.assertIn(("viewer/viewer", "wire/wire_public"), relations)
        self.assertIn(("web/web_actions", "web/web_model"), relations)
        self.assertFalse(any(node["layer"] == "web_tests" for node in graph["nodes"]))
        self.assertFalse(any(node["layer"] == "tools" for node in graph["nodes"]))
        self.assertTrue(
            all("physical_directory" in node for node in graph["nodes"])
        )

    def test_tests_and_tools_are_separate_scopes(self) -> None:
        test_graph = build_graph(snapshot(), manifest(), scope="tests")
        tool_graph = build_graph(snapshot(), manifest(), scope="tools")
        self.assertEqual(
            ["web/tests/run.test.ts"],
            [node["path"] for node in test_graph["nodes"] if node["layer"] == "web_tests"],
        )
        self.assertEqual(
            ["tools/sample.py"],
            [node["path"] for node in tool_graph["nodes"]],
        )

    def test_reflexion_keeps_unmodeled_edges_and_reuses_lint_result(self) -> None:
        base = build_graph(snapshot(), manifest())
        current = build_graph(snapshot(extra_import=True), manifest())
        report = reflexion_report(
            base,
            current,
            manifest(),
            existing_lint_diagnostics=["arch-lint: existing rule failed"],
            required_relations=[
                ("viewer/viewer", "wire/wire_public"),
                ("web/web_model", "wire/wire_public"),
            ],
        )
        self.assertEqual(
            [{"source": "viewer/viewer", "target": "wire/wire_public"}],
            report["convergence"],
        )
        self.assertEqual(
            [{"source": "web/web_model", "target": "wire/wire_public"}],
            report["absence"],
        )
        self.assertEqual(
            ["arch-lint: existing rule failed"],
            report["divergence_from_existing_lint"],
        )
        self.assertTrue(report["new_unmodeled_file_dependencies"])
        self.assertTrue(current["cycles"])

    def test_delta_separates_facts_from_review_candidates(self) -> None:
        base = build_graph(snapshot(), manifest())
        current = build_graph(snapshot(extra_import=True), manifest())
        report = architecture_delta_report(
            base,
            current,
            [
                {
                    "status": "M",
                    "paths": ["domains/wire/include/city/wire/api.hpp"],
                },
                {"status": "M", "paths": ["docs/wire/backbone_operation_semantics.md"]},
                {"status": "M", "paths": ["tools/arch_lint.py"]},
            ],
            fallback_term_evidence=[
                {"path": "domains/wire/src/core.cpp", "line": 9, "text": "fallback"}
            ],
        )
        facts = report["structural_facts"]
        self.assertEqual(
            ["domains/wire/include/city/wire/api.hpp"],
            facts["public_headers_touched"],
        )
        self.assertEqual(["tools/arch_lint.py"], facts["architecture_guard_files_touched"])
        candidates = {row["name"]: row for row in report["human_review_candidates"]}
        self.assertIn("public_api_review", candidates)
        self.assertIn("operation_semantics_review", candidates)
        self.assertIn("fallback_or_special_path_review", candidates)
        self.assertIn("not inferred", candidates["public_api_review"]["claim_limit"])

    def test_first_parent_log_rename_maps_older_changes_to_current_path(self) -> None:
        text = (
            "\x1ec2\x1f2026-01-02T00:00:00+00:00\nR100\told.cpp\tnew.cpp\n"
            "\x1ec1\x1f2026-01-01T00:00:00+00:00\nM\told.cpp\n"
        )
        commits, aliases = parse_name_status_log(text)
        self.assertEqual(frozenset({"new.cpp"}), commits[0].files)
        self.assertEqual(frozenset({"new.cpp"}), commits[1].files)
        self.assertEqual("new.cpp", aliases["old.cpp"])

    def test_history_separates_mass_changes_and_marks_thin_static_pairs(self) -> None:
        classified = {
            "domains/wire/src/a.cpp": "wire_core",
            "viewer/src/b.cpp": "viewer",
            "web/src/model.ts": "web_model",
        }
        when = datetime(2026, 1, 1, tzinfo=timezone.utc)
        commits = [
            CommitChange("c1", when, frozenset({"domains/wire/src/a.cpp", "viewer/src/b.cpp"}), 0),
            CommitChange("c2", when, frozenset({"domains/wire/src/a.cpp", "viewer/src/b.cpp"}), 0),
            CommitChange(
                "mass",
                when,
                frozenset(
                    {
                        "domains/wire/src/a.cpp",
                        "viewer/src/b.cpp",
                        "web/src/model.ts",
                    }
                ),
                0,
            ),
        ]
        graph = {"edges": [], "aggregate_edges": []}
        report = history_report(commits, classified, graph, recent_days=180)
        window = report["windows"]["long_term"]
        self.assertEqual(2, window["mass_change_p99"])
        self.assertEqual([{"sha": "mass", "production_files": 3}], window["mass_change_commits"])
        pair = window["module_cochange_exclusive"][0]
        self.assertFalse(pair["static_relation"])
        self.assertEqual("unreachable", pair["static_distance"])
        self.assertIsNone(pair["static_path_left_to_right"])
        self.assertIsNone(pair["static_path_right_to_left"])
        self.assertEqual(2, pair["cochange_commits"])
        self.assertEqual(1.0, pair["support"])
        self.assertEqual(
            pair,
            window["module_cochange_without_direct_static_edge"][0],
        )
        self.assertIn("first parent", report["history_model"])

    def test_history_reports_indirect_static_distance(self) -> None:
        classified = {
            "viewer/src/view.cpp": "viewer",
            "domains/wire/include/public.hpp": "wire_public_api",
            "domains/wire/src/state.cpp": "wire_state_internal",
        }
        when = datetime(2026, 1, 1, tzinfo=timezone.utc)
        commits = [
            CommitChange(
                "c1",
                when,
                frozenset({"viewer/src/view.cpp", "domains/wire/src/state.cpp"}),
                0,
            )
        ]
        graph = {
            "edges": [],
            "aggregate_edges": [
                {"source": "viewer/viewer", "target": "wire/wire_public_api"},
                {"source": "wire/wire_public_api", "target": "wire/wire_state_internal"},
            ],
        }

        report = history_report(commits, classified, graph, window_commits=1)
        pair = report["windows"]["recent_commits"]["module_cochange_exclusive"][0]

        self.assertFalse(pair["static_relation"])
        self.assertEqual(2, pair["static_path_left_to_right"])
        self.assertIsNone(pair["static_path_right_to_left"])
        self.assertEqual("1_intermediate", pair["static_distance"])

    def test_history_compares_recent_and_previous_equal_commit_windows(self) -> None:
        classified = {
            "domains/road/src/a.cpp": "road_a",
            "domains/road/src/a2.cpp": "road_a",
            "viewer/src/b.cpp": "viewer_b",
        }
        when = datetime(2026, 1, 1, tzinfo=timezone.utc)
        both = frozenset({"domains/road/src/a.cpp", "viewer/src/b.cpp"})
        left_only = frozenset(
            {"domains/road/src/a.cpp", "domains/road/src/a2.cpp"}
        )
        commits = [
            CommitChange("recent-1", when, both, 0),
            CommitChange("recent-2", when, both, 0),
            CommitChange("previous-1", when, both, 0),
            CommitChange("previous-2", when, left_only, 0),
        ]

        report = history_report(
            commits,
            classified,
            {"edges": [], "aggregate_edges": []},
            window_commits=2,
        )

        self.assertEqual(
            ["previous-1", "previous-2"],
            report["windows"]["previous_commits"]["commit_shas"],
        )
        self.assertEqual(
            ["recent-1", "recent-2"],
            report["windows"]["recent_commits"]["commit_shas"],
        )
        trend = report["module_cochange_strengthening"][0]
        self.assertEqual(1, trend["previous_cochange_commits"])
        self.assertEqual(2, trend["recent_cochange_commits"])
        self.assertAlmostEqual(0.5, trend["support_delta"])
        self.assertAlmostEqual(0.5, trend["confidence_left_to_right_delta"])
        self.assertAlmostEqual(0.0, trend["confidence_right_to_left_delta"])
        self.assertEqual("unreachable", trend["static_distance"])

    def test_numstat_rename_is_attributed_to_current_path(self) -> None:
        text = (
            "\x1ekeep\x1f2026-01-02T00:00:00+00:00\n"
            "1\t2\t{old => new}/file.cpp\n"
            "\x1emass\x1f2026-01-01T00:00:00+00:00\n"
            "3\t4\told.cpp\n"
        )
        churn = parse_numstat_log(text, {"old.cpp": "new.cpp"})
        self.assertEqual(3, churn["new/file.cpp"])
        self.assertEqual(7, churn["new.cpp"])
        exclusive = parse_numstat_log(
            text, {"old.cpp": "new.cpp"}, excluded_shas={"mass"}
        )
        self.assertNotIn("new.cpp", exclusive)

    def test_sensor_retirement_is_explicit(self) -> None:
        note = sensor_retirement_note()
        self.assertIn("Retire or merge", note)
        self.assertIn("not a quality objective", note)

    def test_repository_cli_has_bounded_subcommands(self) -> None:
        result = subprocess.run(
            [sys.executable, str(REPOSITORY_ROOT / "tools" / "architecture_observation.py"), "--help"],
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        for command in ("graph", "reflexion", "delta", "history", "hotspot"):
            self.assertIn(command, result.stdout)
        history_help = subprocess.run(
            [
                sys.executable,
                str(REPOSITORY_ROOT / "tools" / "architecture_observation.py"),
                "history",
                "--help",
            ],
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("--window-commits", history_help.stdout)


if __name__ == "__main__":
    unittest.main()
