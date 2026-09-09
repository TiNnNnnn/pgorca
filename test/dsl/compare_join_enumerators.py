#!/usr/bin/env python3
"""Compare DPHyp and experimental top-down enumeration in an isolated PG cluster.

Audit is a separate, traced run. Timings are untraced, warmed up, and alternate
algorithm order. Both arms use the same schema, statistics, rules and budgets.
The checked-in benchmark schemas contain no production data; E2E has test rows.
"""

from __future__ import annotations

import argparse
from collections import Counter
import fnmatch
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from run_workload_comparison import (
    explain_times, optimization_time, optimizer_name, plan_tree, produced_xforms, psql, run,
)
from run_dphyper_stability import parse_dphyper_events


ROOT = Path(__file__).resolve().parent
VERIFY = re.compile(r"DPHyperVerify: status=(\w+)")


def settings(top_down: bool, budget: int, verify: bool = False, dsl: bool = False) -> str:
    return f"""
LOAD 'pg_orca';
SET pg_orca.enable_orca=on;
SET pg_orca.enable_dsl_rule={'on' if dsl else 'off'};
SET pg_orca.trace_dsl_rule={'on' if verify else 'off'};
SET pg_orca.enable_dphyper=on;
SET pg_orca.dphyper_shadow=off;
SET pg_orca.dphyper_top_down={'on' if top_down else 'off'};
SET pg_orca.dphyper_verify={'on' if verify else 'off'};
SET pg_orca.dphyper_pair_budget={budget};
SET pg_orca.trace_fallback=on;
SET optimizer_print_xform=off;
SET optimizer_print_xform_results=off;
SET optimizer_print_plan=off;
SET optimizer_print_optimization_stats=off;
SET client_min_messages={'log' if verify else 'warning'};
"""


def row_bag(text: str) -> Counter:
    # PostgreSQL COPY text escapes embedded tabs/newlines/backslashes and
    # distinguishes NULL (\N) from empty strings and literal backslashes.
    # Compare the encoded fields without decoding away those distinctions.
    return Counter(tuple(row.split("\t")) for row in text.splitlines())


def compare(args, binary: Path, socket: Path, database: str, query: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    sql = query.read_text().strip().removesuffix(";")
    audits, plans, rows = {}, {}, {}
    timings = {mode: [] for mode in ("bottom_up", "top_down")}
    failures = []
    for mode in timings:
        base = settings(mode == "top_down", args.pair_budget, verify=True, dsl=args.dsl)
        stdout, stderr, rc, _ = psql(binary, socket, args.port, database,
            base + "SET optimizer_print_xform=on; SET optimizer_print_xform_results=on;"
            + "SET optimizer_print_plan=on;"
            + "SET optimizer_print_optimization_stats=on;"
            + f"EXPLAIN (FORMAT JSON) {sql};", args.timeout)
        (output / f"{mode}.audit.trace").write_text(stderr)
        (output / f"{mode}.plan.json").write_text(stdout)
        plans[mode] = plan_tree(stdout)
        statuses = Counter(VERIFY.findall(stderr))
        cost = re.search(r"Physical plan:\s*\n[^\n]*cost:([0-9.eE+-]+)", stderr)
        audits[mode] = {
            "rc": rc, "optimizer": optimizer_name(stdout),
            "optimizer_cost": float(cost[1]) if cost else None,
            "traced_optimization_ms": optimization_time(stderr),
            "verification": dict(statuses),
            "events": parse_dphyper_events(stderr),
            "xforms": produced_xforms(stderr),
        }
        if rc or plans[mode] is None or optimizer_name(stdout) == "postgres":
            failures.append(f"{mode}: audit failed or PostgreSQL fallback")
        if statuses["mismatch"]:
            failures.append(f"{mode}: cut/provenance mismatch")
    if not failures:
        # One warm-up per arm, followed by alternating AB/BA measured rounds.
        for repeat in range(args.repeats + 1):
            modes = list(timings)
            if repeat % 2:
                modes.reverse()
            for mode in modes:
                stdout, stderr, rc, _ = psql(binary, socket, args.port, database,
                    settings(mode == "top_down", args.pair_budget, dsl=args.dsl)
                    + f"EXPLAIN (ANALYZE, TIMING OFF, BUFFERS OFF, FORMAT JSON) {sql};",
                    args.timeout)
                planning, execution = explain_times(stdout)
                if rc or planning is None or optimizer_name(stdout) == "postgres":
                    failures.append(f"{mode}: timing run {repeat} failed or fell back")
                    (output / f"{mode}.{repeat}.error").write_text(stderr)
                    break
                if repeat:
                    timings[mode].append({"planning_ms": planning, "execution_ms": execution})
            if failures:
                break
        for mode in timings:
            stdout, stderr, rc, _ = psql(binary, socket, args.port, database,
                settings(mode == "top_down", args.pair_budget, dsl=args.dsl)
                + f"COPY ({sql}) TO STDOUT WITH (FORMAT text);", args.timeout)
            (output / f"{mode}.rows.txt").write_text(stdout)
            if rc:
                failures.append(f"{mode}: execution failed")
                (output / f"{mode}.rows.error").write_text(stderr)
            else:
                rows[mode] = row_bag(stdout)
    rows_equal = len(rows) == 2 and rows["bottom_up"] == rows["top_down"]
    if not rows_equal and not failures:
        failures.append("result bags differ")
    postgres_equal = False
    if len(rows) == 2:
        stdout, stderr, rc, _ = psql(binary, socket, args.port, database,
            "LOAD 'pg_orca'; SET pg_orca.enable_orca=off;"
            + f"COPY ({sql}) TO STDOUT WITH (FORMAT text);", args.timeout)
        (output / "postgres.rows.txt").write_text(stdout)
        postgres_equal = rc == 0 and all(bag == row_bag(stdout) for bag in rows.values())
        if not postgres_equal:
            failures.append("PostgreSQL reference failed or result bags differ")
            (output / "postgres.rows.error").write_text(stderr)
    medians = {
        mode: {field: statistics.median(sample[field] for sample in samples)
               for field in ("planning_ms", "execution_ms")}
        for mode, samples in timings.items() if samples
    }
    costs = {mode: plan.get("Total Cost") if plan else None for mode, plan in plans.items()}
    bounded = any(audit["verification"].get("inconclusive_budget") for audit in audits.values())
    cuts_verified = not bounded and all(
        audit["verification"].get("equal", 0) > 0 and
        not audit["verification"].get("mismatch", 0) for audit in audits.values()
    )
    plan_equal = plans["bottom_up"] is not None and plans["bottom_up"] == plans["top_down"]
    result = {
        "query": str(query), "failures": failures, "rows_equal": rows_equal,
        "postgres_equal": postgres_equal,
        "row_count": sum(rows.get("bottom_up", {}).values()),
        "audit": audits, "budget_limited": bounded, "cuts_verified": cuts_verified,
        "plan_equal": plan_equal,
        "plan_cost": costs, "timings": timings, "median": medians,
        "trigger_order_equal": audits["bottom_up"]["xforms"] == audits["top_down"]["xforms"],
        "trigger_set_equal": set(audits["bottom_up"]["xforms"]) == set(audits["top_down"]["xforms"]),
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pg-config", type=Path, required=True)
    parser.add_argument("--workload", choices=("e2e", "tpch", "tpcds", "job", "sqlstorm"), action="append")
    parser.add_argument("-t", "--test", action="append", default=[])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--pair-budget", type=int, default=100000)
    parser.add_argument("--dsl", action="store_true", help="enable the same DSL library in both arms")
    parser.add_argument("--port", type=int, default=60474)
    parser.add_argument("--output", type=Path, default=ROOT.parent.parent / "output/dphyper-counter-strike/comparison")
    args = parser.parse_args()
    if min(args.repeats, args.timeout, args.pair_budget) < 1 or not 0 < args.port < 65536:
        parser.error("positive repeats, timeout, budget and a valid port required")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = Path(subprocess.check_output([str(args.pg_config), "--bindir"], text=True).strip())

    def checked(command, **kwargs):
        result = run([str(value) for value in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
        if result.returncode:
            raise RuntimeError(result.stdout)

    results = []
    with tempfile.TemporaryDirectory(prefix="pgorca-tdhyper.") as directory:
        root = Path(directory)
        data, socket = root / "data", root / "socket"
        socket.mkdir()
        rules = root / "rules.txt"
        rules.write_text("\n".join((ROOT / "rules" / name).read_text()
                                   for name in ("framework.rules", "orca_replacements.rules")))
        checked([binary / "initdb", "-D", data, "--no-locale", "--encoding=UTF8", "--auth=trust"])
        checked([binary / "pg_ctl", "-D", data, "-l", args.output / "postgresql.log",
                 "-o", f"-c listen_addresses='' -k {socket} -p {args.port}", "start"],
                env={**os.environ, "MONSOON_DSL_RULES": str(rules)})
        try:
            for workload in args.workload or ["e2e"]:
                database = f"tdhyper_{workload}"
                checked([binary / "createdb", "-h", socket, "-p", args.port, database])
                folder = ROOT / "e2e" if workload == "e2e" else ROOT / "workloads" / workload
                schema = folder / "sql/_setup.sql" if workload == "e2e" else folder / "schema.sql"
                checked([binary / "psql", "-X", "-v", "ON_ERROR_STOP=1", "-h", socket,
                         "-p", args.port, "-d", database, "-f", schema])
                patterns = args.test or (["dphyper_*"] if workload == "e2e" else ["q*"])
                for query in sorted((folder / "sql").glob("*.sql")):
                    if query.name.startswith("_") or not any(
                        fnmatch.fnmatch(query.stem, pattern) or
                        fnmatch.fnmatch(f"{workload}/{query.stem}", pattern) for pattern in patterns
                    ):
                        continue
                    result = compare(args, binary / "psql", socket, database, query,
                                     args.output / workload / query.stem)
                    results.append(result)
                    print(f"{workload}/{query.stem}: rows={result['rows_equal']} "
                          f"plan={result['plan_equal']} budget_limited={result['budget_limited']} "
                          f"failures={result['failures']}", flush=True)
        finally:
            checked([binary / "pg_ctl", "-D", data, "stop", "-m", "fast"])
    summary = {
        "queries": len(results), "pair_budget": args.pair_budget, "repeats": args.repeats,
        "dsl": args.dsl,
        "rows_equal": sum(result["rows_equal"] for result in results),
        "postgres_equal": sum(result["postgres_equal"] for result in results),
        "cuts_verified": sum(result["cuts_verified"] for result in results),
        "plans_equal": sum(result["plan_equal"] for result in results),
        "budget_limited": sum(result["budget_limited"] for result in results),
        "failed": sum(bool(result["failures"]) for result in results),
        "results": results,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return int(not results or summary["failed"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
