#!/usr/bin/env python3
"""profiles workloads whose frames are held by a known cause (tests/d3d12/d3d12_held.cpp, tests/d3d11/d3d11_held.cpp)
and checks that tools/mullion-diag finds that cause, through each front end. each program measures its frames
without the profile and says so in 'fact' lines:

gpu: the GPU holds each frame; the tool's GPU time in each frame is at least the program's own timestamp difference
of the frame's draw, and the frame's bound is the GPU when the draw took longer than the program's own time outside
its submission and Present ('fact app'), and the application otherwise (as a first frame is, starting Wine's window).
app: the program sleeps with nothing on the GPU; the tool gives the application at least that sleep in each frame.
hitch: the middle frame makes new pipelines; the tool counts that frame a hitch, and conversion or pipeline making
is among the bridge calls that took it longest.
events: each frame draws in the application's events "frame" and, inside it, "draw <frame>", which D3D12 gives in
every encoding PIX has; the tool names each frame's draw by that path.

the first frame is not judged: it has no frame before it to start from (mullion-diag, bounds).
"""
import argparse
import importlib.machinery
import importlib.util
import re
import shutil
import sys
from pathlib import Path

import run

loader = importlib.machinery.SourceFileLoader("mullion_diag", str(Path(__file__).resolve().parent.parent / "tools" / "mullion-diag"))
tool = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(tool)

# the programs, and the arguments that pick a front end before the mode
PROGRAMS = {"d3d12_held": ["dxil"], "d3d11_held": []}
MODES = ("gpu", "app", "hitch", "events")


def judge(mode, facts, record):
    """what the tool got wrong about the run, one line each"""
    rows = {row["frame"]: row for row in tool.bounds(record)}
    wrong = []
    if not rows:
        return ["the tool found no frames to judge"]
    if mode == "gpu":
        for frame, measured in facts.get("gpu", {}).items():
            bound = "gpu" if measured > facts["app"][frame] else "application"
            if frame in rows and (rows[frame]["causes"]["gpu"] < measured or rows[frame]["bound"] != bound):
                wrong.append(f"frame {frame}: GPU {rows[frame]['causes']['gpu']} ns, bound {rows[frame]['bound']}; "
                             f"the draw took {measured} ns, the program {facts['app'][frame]} ns")
    elif mode == "app":
        slept, = facts["sleep"]
        for frame, row in rows.items():
            if row["causes"]["application"] < slept:
                wrong.append(f"frame {frame}: application {row['causes']['application']} ns of a {slept} ns sleep")
    elif mode == "events":
        for frame, encoding in facts["event"].items():
            named = {record.events.get((p.queue, p.cmdbuf, p.number)) for p in record.passes_in(frame)
                     if p.kind == "Render"}
            if frame in rows and named != {("frame", f"draw {frame}")}:
                wrong.append(f"frame {frame} (encoding {encoding}): its render passes are named {named}")
    else:
        (frame, took), = facts["hitch"].items()
        calls = record.unix[frame] if frame < len(record.unix) else {}
        busiest = sorted(((time, code) for code, (count, time) in calls.items()), reverse=True)[:3]
        if not rows.get(frame, {}).get("hitch"):
            wrong.append(f"frame {frame} made pipelines for {took} ns and is not a hitch")
        if not any(record.compiling({code: time}) != (0, 0) for time, code in busiest):
            wrong.append(f"frame {frame}: the longest bridge calls are "
                         f"{[record.calls.get(code, code) for _, code in busiest]}")
    return wrong


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.arguments(parser)
    parser.add_argument("--out", required=True, type=Path, help="where the records and logs go")
    parser.add_argument("--frames", type=int, default=48, help="frames a run presents (default: %(default)s)")
    parser.add_argument("--seconds", type=int, default=300, help="the most a run may take (default: %(default)s)")
    parser.add_argument("--family", default="native", help="the Apple GPU family to run as, as tests/run.py names it")
    args = parser.parse_args()
    with run.session(args) as (builds, binary, base):
        base = run.as_family(base, args.family)
        failed = 0
        for program, front in PROGRAMS.items():
            exe = next(path for build in builds for path in build.glob(f"tests/*/{program}.exe"))
            for mode in MODES:
                out = (args.out / f"{program}.{mode}").resolve()
                shutil.rmtree(out, ignore_errors=True)
                out.mkdir(parents=True)
                env = dict(base, MULLION_DIAG="profile", MULLION_DIAG_PATH=str(out))
                status = run.finish([binary, exe, *front, mode, str(args.frames)], env, args.seconds, out / "log.txt")
                text = (out / "log.txt").read_text(errors="replace")
                # 'fact <kind> <frame> <ns>', or 'fact sleep <ns>'
                facts = {}
                for kind, first, second in re.findall(r"^fact (\w+) (\d+)(?: (\d+))?$", text, re.M):
                    facts.setdefault(kind, {})[int(first)] = int(second or first)
                records = sorted(out.glob("*.diag"))
                wrong = ([f"exit status {status}"] if status else []) + (
                    judge(mode, facts, tool.Record(records[-1])) if len(records) == 1 else [f"{len(records)} records"])
                failed += bool(wrong)
                print(f"{program} {mode}: {'failed' if wrong else 'passed'}", *(f"  {line}" for line in wrong[:8]),
                      sep="\n", flush=True)
        sys.exit(failed != 0)


if __name__ == "__main__":
    main()
