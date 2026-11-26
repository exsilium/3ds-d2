#!/usr/bin/env python3
"""A command's wall time and peak memory (issue #144: the recompilers
compared), on stderr after it ends; its exit status passed on.

    python3 -I tools/aot/measure.py build/aot-rs/target/release/recompile Game.exe ...
"""
import resource
import subprocess
import sys
import time

t0 = time.time()
rc = subprocess.call(sys.argv[1:])
kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print("measure: %s: %.1f s, peak %d MB, exit %d" % (sys.argv[1], time.time() - t0, kb // 1024, rc),
      file=sys.stderr, flush=True)
sys.exit(rc)
