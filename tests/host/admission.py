# contract: run.py starts a test only where the free memory holds its peak beside what the tests running may still grow
# by, and a test never measured runs alone; a test alone starts whatever its peak, and a run records each test's peak
# (finish, admitted). the free memory and the footprints are stubbed, the tests are real processes, and each says when
# it started and ended, so the overlaps are what ran at once.
import importlib.util
import json
import sys
import tempfile
import threading
from pathlib import Path

FREE = 8 << 30
# a test is sampled after a second, so each runs longer than that
TEST = "import time; print(time.time(), flush=True); time.sleep(1.5); print(time.time())"


def main():
    spec = importlib.util.spec_from_file_location('run', sys.argv[1])
    run = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(run)
    run.available = lambda: FREE
    run.pressure = lambda: 1
    run.footprint = lambda group: 0
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        run.PEAKS = Path(temporary, 'peaks.json')

        def together(case, peaks, want):
            """runs one test of each peak (None: never measured) at once; whether each pair overlapped as `want` says"""
            run.PEAKS.write_text(json.dumps({f"{Path(sys.executable).name} -c {i}": peak
                                             for i, peak in enumerate(peaks) if peak is not None}))
            logs = [Path(temporary, f"{case}.{i}") for i in range(len(peaks))]
            threads = [threading.Thread(target=run.finish, args=([sys.executable, '-c', f"{TEST} #{i}"], {}, 30, log))
                       for i, log in enumerate(logs)]
            # the key names the program and its arguments: the test's text, here told apart by a comment
            run.measured = lambda command, env: f"{Path(command[0]).name} -c {command[2].rsplit('#', 1)[1]}"
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            spans = [list(map(float, log.read_text().split())) for log in logs]
            for (a, b), overlap in want.items():
                got = spans[a][0] < spans[b][1] and spans[b][0] < spans[a][1]
                if got != overlap:
                    failures.append(f"{case}: tests {a} and {b} {'ran' if got else 'did not run'} at once")
            return json.loads(run.PEAKS.read_text())

        together('fits', [FREE // 3, FREE // 3], {(0, 1): True})
        together('too much', [FREE // 3 * 2, FREE // 3 * 2], {(0, 1): False})
        together('never measured', [None, 1], {(0, 1): False})
        together('alone past the free memory', [FREE * 2], {})
        held_by = FREE // 4
        run.footprint = lambda group: held_by
        recorded = together('recorded', [None], {})
        if recorded.get(f"{Path(sys.executable).name} -c 0") != held_by:
            failures.append(f"recorded: peak {recorded} after a test held {held_by}")
    for failure in failures:
        print(f"wrong: {failure}")
    print(f"{'failed' if failures else 'passed'}: {len(failures)} wrong results")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
