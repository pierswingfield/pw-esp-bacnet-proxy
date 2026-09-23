#!/usr/bin/env python3
"""
ESP-BACnet Bridge E2E Test Suite Runner
Executes Tiers 1 through 4 across all feature areas, boundary conditions,
cross-feature combinations, and real-world application scenarios.
Outputs structured summary tables, coverage metrics, and pass/fail status.
"""
import argparse
import json
import os
import sys
import time
import pytest

def main():
    parser = argparse.ArgumentParser(description="Run ESP-BACnet E2E Test Suite")
    parser.add_argument("--tier", choices=["1", "2", "3", "4", "5", "all"], default="all",
                        help="Select test tier to execute (default: all)")
    parser.add_argument("--json-out", type=str, default=None,
                        help="Path to write structured JSON test metrics report")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Enable verbose test output")
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    tests_dir = os.path.join(repo_root, "tests")

    tier_map = {
        "1": ("Tier 1 (Feature Coverage)", os.path.join(tests_dir, "tier1_features")),
        "2": ("Tier 2 (Boundary & Corner Cases)", os.path.join(tests_dir, "tier2_boundaries")),
        "3": ("Tier 3 (Cross-Feature Combinations)", os.path.join(tests_dir, "tier3_combinations")),
        "4": ("Tier 4 (Real-World Scenarios)", os.path.join(tests_dir, "tier4_scenarios")),
        "5": ("Tier 5 (Adversarial Coverage Hardening)", os.path.join(tests_dir, "tier5_adversarial")),
    }

    tiers_to_run = list(tier_map.keys()) if args.tier == "all" else [args.tier]

    print("=" * 80)
    print("           ESP-BACnet Bridge End-to-End Test Suite Runner")
    print("=" * 80)
    print(f"Repository Root : {repo_root}")
    print(f"Selected Tier(s): {', '.join(tiers_to_run)}")
    print("=" * 80)

    total_passed = 0
    total_failed = 0
    total_errors = 0
    tier_results = []
    overall_start_time = time.time()

    for tier_id in tiers_to_run:
        tier_name, tier_path = tier_map[tier_id]
        print(f"\n==> Running {tier_name}...")
        t0 = time.time()

        pytest_args = [tier_path]
        if args.verbose:
            pytest_args.append("-v")
        else:
            pytest_args.append("-q")

        # Custom collector / runner via pytest.main
        class ResultCollector:
            def __init__(self):
                self.passed = 0
                self.failed = 0
                self.errors = 0
                self.skipped = 0

            def pytest_runtest_logreport(self, report):
                if report.when == "call":
                    if report.passed:
                        self.passed += 1
                    elif report.failed:
                        self.failed += 1
                    elif report.skipped:
                        self.skipped += 1
                elif report.when in ("setup", "teardown") and report.failed:
                    self.errors += 1

        collector = ResultCollector()
        exit_code = pytest.main(pytest_args, plugins=[collector])
        elapsed = time.time() - t0

        tier_summary = {
            "tier_id": tier_id,
            "name": tier_name,
            "passed": collector.passed,
            "failed": collector.failed,
            "errors": collector.errors,
            "skipped": collector.skipped,
            "elapsed_sec": round(elapsed, 3),
            "exit_code": int(exit_code)
        }
        tier_results.append(tier_summary)
        total_passed += collector.passed
        total_failed += collector.failed
        total_errors += collector.errors

        status_str = "PASS" if exit_code == 0 else "FAIL"
        print(f"[{status_str}] {tier_name}: {collector.passed} passed, {collector.failed} failed, {collector.errors} errors in {elapsed:.2f}s")

    total_elapsed = time.time() - overall_start_time

    print("\n" + "=" * 80)
    print("                         TEST EXECUTION SUMMARY")
    print("=" * 80)
    print(f"{'Tier':<38} | {'Passed':<8} | {'Failed':<8} | {'Errors':<8} | {'Duration'}")
    print("-" * 80)
    for tr in tier_results:
        print(f"{tr['name']:<38} | {tr['passed']:<8} | {tr['failed']:<8} | {tr['errors']:<8} | {tr['elapsed_sec']:.2f}s")
    print("-" * 80)
    print(f"{'TOTAL':<38} | {total_passed:<8} | {total_failed:<8} | {total_errors:<8} | {total_elapsed:.2f}s")
    print("=" * 80)

    # Output JSON report if requested
    if args.json_out:
        report_data = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "total_passed": total_passed,
            "total_failed": total_failed,
            "total_errors": total_errors,
            "total_duration_sec": round(total_elapsed, 3),
            "tier_results": tier_results
        }
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report_data, f, indent=2)
        print(f"JSON test metrics saved to: {args.json_out}")

    if total_failed == 0 and total_errors == 0:
        print("\nSUCCESS: All ESP-BACnet E2E test suites passed cleanly!")
        return 0
    else:
        print(f"\nFAILURE: {total_failed} tests failed, {total_errors} errors encountered.")
        return 1

if __name__ == "__main__":
    sys.exit(main())
