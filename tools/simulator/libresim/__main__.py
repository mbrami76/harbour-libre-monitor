# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Run: python3 -m libresim --scenario basic --port 8080 [--speed 60]"""

import argparse
import sys

from .scenario import ScenarioError, load_scenario
from .server import Clock, SimServer


def main(argv=None):
    ap = argparse.ArgumentParser(prog="libresim", description="Simulated LibreLinkUp-style source (synthetic data).")
    ap.add_argument("--scenario", default="basic", help="path or shipped name (basic, bands, multi, mmol, faults, gaps)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1", help="default: localhost only")
    ap.add_argument("--speed", type=float, default=1.0, help="simulated seconds per real second")
    ap.add_argument("--seed", type=int, help="override the scenario seed")
    ap.add_argument("--verbose", action="store_true", help="log request lines")
    args = ap.parse_args(argv)
    try:
        scen = load_scenario(args.scenario)
        if args.seed is not None:
            scen.seed = args.seed
    except ScenarioError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    srv = SimServer(scen, Clock(scen.start_epoch, args.speed), args.host, args.port, args.verbose)
    print("SIMULATED DATA - scenario %r (%s), seed %s, speed %gx" % (scen.name, scen.source, scen.seed, args.speed))
    print("serving on %s (plain HTTP); accounts:" % srv.url)
    for a in scen.accounts:
        print("  %s / %s  (%s, %d Subject(s))" % (a.email, a.password, a.unit, len(a.subjects)))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
