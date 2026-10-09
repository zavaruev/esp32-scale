#!/usr/bin/env python3
"""Stage 1 log analysis: characterise the HX711 + load cell.

Reads the CSV stream produced by main/stage1_main.c and reports the numbers that
decide whether 0.1 g is reachable on this hardware.

Usage:
    ./tools/analyze_stage1.py <logfile> [--cal-factor COUNTS_PER_GRAM]

Without --cal-factor the gram-domain figures are skipped; counts-domain statistics
are always reported. cal_factor is only known once a known weight has been applied.
"""

import argparse
import math
import statistics
import sys

# The 6 kg cell is nominally ~2 mV/V. Full-scale input of the HX711 at gain 128
# is Vref/gain = 2.048/128 = 16 mV, and full scale is 2**23 counts. These constants
# let us sanity-check a measured cal_factor against theory.
HX711_FULL_SCALE_COUNTS = 2 ** 23
HX711_REF_VOLTS = 2.048
HX711_GAIN = 128


def parse(path):
    samples = []  # (seq, micros, raw)
    reported_rate = None
    errors = 0
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#R,"):
                reported_rate = float(line.split(",")[2])
            elif line.startswith("#E,"):
                errors += 1
            elif line.startswith("#"):
                continue
            else:
                try:
                    seq, micros, raw = line.split(",")
                    samples.append((int(seq), int(micros), int(raw)))
                except ValueError:
                    continue
    return samples, reported_rate, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("--cal-factor", type=float, default=None,
                    help="measured counts per gram")
    ap.add_argument("--skip-seconds", type=float, default=3.0,
                    help="discard this much from the start (warm-up)")
    args = ap.parse_args()

    samples, reported_rate, errors = parse(args.logfile)
    if not samples:
        print("no samples parsed", file=sys.stderr)
        return 1

    t0 = samples[0][1]
    warmup = args.skip_seconds * 1e6
    kept = [(s, (t - t0) / 1e6, raw) for s, t, raw in samples if t - t0 > warmup]

    raw_values = [r for _, _, r in kept]
    n = len(raw_values)

    mean = statistics.fmean(raw_values)
    sigma = statistics.stdev(raw_values) if n > 1 else 0.0
    minimum = min(raw_values)
    maximum = max(raw_values)

    print(f"file                : {args.logfile}")
    print(f"samples total       : {len(samples)}  (analysed {n}, "
          f"{args.skip_seconds:.0f}s warm-up discarded)")
    print(f"reported rate       : {reported_rate} Hz" if reported_rate
          else "reported rate       : n/a")
    if errors:
        print(f"read timeouts       : {errors}")
    print()
    print(f"mean                : {mean:,.1f} counts")
    print(f"sigma               : {sigma:.1f} counts")
    print(f"min / max           : {minimum:,} / {maximum:,}")
    print(f"peak-to-peak        : {maximum - minimum:,} counts")
    print(f"peak-to-peak / sigma: {(maximum - minimum) / sigma:.1f} "
          f"(a clean Gaussian run sits near 6)")
    print()

    # Drift: a load cell that has not warmed up reads a moving value. Fit a line
    # and report the total movement across the captured window.
    if n > 10:
        times = [t for _, t, _ in kept]
        t_mean = statistics.fmean(times)
        num = sum((t - t_mean) * (r - mean) for t, (_, _, r) in zip(times, kept))
        den = sum((t - t_mean) ** 2 for t in times)
        slope = num / den if den else 0.0
        span = times[-1] - times[0]
        drift = slope * span
        print(f"drift over window   : {drift:+,.1f} counts "
              f"({slope:+.2f} counts/s over {span:.1f}s)")
        print(f"                     = {drift / sigma:+.1f} sigma")
        print()

    # Per-second sigma: reveals whether noise is steady or has a warm-up tail.
    buckets = {}
    for _, t, raw in kept:
        buckets.setdefault(int(t), []).append(raw)
    sigmas = [statistics.stdev(v) for v in buckets.values() if len(v) > 1]
    if sigmas:
        print(f"per-second sigma    : min {min(sigmas):.1f}, "
              f"median {statistics.median(sigmas):.1f}, max {max(sigmas):.1f}")
        print()

    # Outliers matter directly: the spec's median-of-5 stage exists to remove
    # exactly these spikes.
    outliers = [r for r in raw_values if abs(r - mean) > 3 * sigma]
    print(f"outliers >3 sigma   : {len(outliers)} of {n} "
          f"({100.0 * len(outliers) / n:.2f}%)")
    if outliers:
        worst = max(outliers, key=lambda r: abs(r - mean))
        print(f"worst deviation     : {worst - mean:+,.0f} counts")
    print()

    if args.cal_factor:
        cf = args.cal_factor
        print(f"cal_factor          : {cf:,.1f} counts/g")
        one_lsb = 1.0 / cf
        print(f"1 LSB               : {one_lsb * 1000:.4f} g")
        print(f"sigma               : {sigma:.1f} counts = "
              f"{sigma / cf * 1000:.1f} mg = {sigma / cf:.4f} g")
        print(f"peak-to-peak        : {(maximum - minimum) / cf * 1000:.1f} mg")
        print(f"0.1 g in counts     : {0.1 * cf:.0f}  "
              f"(sigma is {100.0 * 0.1 / (sigma / cf):.0f}x smaller)")
        print(f"0.01 g in counts    : {0.01 * cf:.1f}  "
              f"(sigma is {0.01 * cf / sigma:.1f}x the noise) "
              f"-> {'REACHABLE' if sigma < 0.01 * cf else 'NOT reachable without averaging'}")
        print()
        print("Theory cross-check: for a 2 mV/V cell, expected cal_factor is")
        cell_fs_mv = 2.0 * 3.3  # sensitivity * 3.3 V excitation
        fs_counts = HX711_FULL_SCALE_COUNTS * (cell_fs_mv / 1000.0) / (
            HX711_REF_VOLTS / HX711_GAIN)
        print(f"  ~{fs_counts / 6000:,.0f} counts/g at 3.3 V excitation, "
              f"~{fs_counts * 5.0 / 3.3 / 6000:,.0f} at 5 V")
    else:
        print("No --cal-factor given: gram-domain figures skipped.")
        print(f"At ~575 counts/g (3.3 V excitation) sigma would be "
              f"{sigma / 575 * 1000:.1f} mg.")
        print("Place a known weight to measure cal_factor.")

    return 0


if __name__ == "__main__":
    sys.exit(main())