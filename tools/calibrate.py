#!/usr/bin/env python3
"""2-point scale calibration from captured HX711 logs.

Reads two capture logs from main/stage1_main.c:
  * a tare log      - platform in its working configuration, nothing on top
  * a reference log - known mass on the platform

and produces the two numbers the firmware needs:
  tare_offset  raw count with the platform empty
  cal_factor   counts per gram

Statistics matter here. The raw sample sigma is ~46 counts, so a short capture
is dominated by noise, and load-cell drift (~0.6-1.0 counts/s) can exceed the
noise in the mean if the capture runs long. Both are therefore estimated
explicitly and propagated into the calibration uncertainty, rather than
reporting a mean as if it were exact.

Usage:
    ./tools/calibrate.py --tare logs/tare.log \\
                         --reference logs/ref.log --mass 1000.0
"""

import argparse
import math
import statistics
import sys


def parse(path):
    samples = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                _, micros, raw = line.split(",")
                samples.append((int(micros), int(raw)))
            except ValueError:
                continue
    if not samples:
        raise SystemExit(f"no samples in {path}")
    return samples


def analyse(samples, warmup_s=5.0, trim_sigma=4.0):
    """Robust statistics for one capture.

    Returns mean, sigma of the raw samples, drift in counts/s, and the standard
    error of the mean including drift.
    """
    t0 = samples[0][0]
    kept = [((t - t0) / 1e6, raw) for t, raw in samples if (t - t0) / 1e6 > warmup_s]
    if len(kept) < 20:
        raise SystemExit(f"only {len(kept)} samples after warm-up, need >= 20")

    times = [t for t, _ in kept]
    raw = [r for _, r in kept]

    # Trim gross outliers first so they cannot drag the mean or inflate sigma.
    median = statistics.median(raw)
    mad = statistics.median([abs(r - median) for r in raw])
    robust_sigma = 1.4826 * mad
    if robust_sigma <= 0:
        raise SystemExit("zero spread in capture")
    cutoff = trim_sigma * robust_sigma
    clean = [(t, r) for t, r in kept if abs(r - median) <= cutoff]
    trimmed = len(kept) - len(clean)

    ct = [t for t, _ in clean]
    cv = [r for _, r in clean]
    mean = statistics.fmean(cv)
    sigma = statistics.stdev(cv)

    # Drift: slope over the capture, and the residual scatter once removed.
    n = len(ct)
    t_mean = statistics.fmean(ct)
    num = sum((t - t_mean) * (v - mean) for t, v in zip(ct, cv))
    den = sum((t - t_mean) ** 2 for t in ct)
    slope = num / den if den else 0.0

    residuals = [v - (mean + slope * (t - t_mean)) for t, v in zip(ct, cv)]
    resid_sigma = statistics.stdev(residuals)
    drift_total = slope * (ct[-1] - ct[0])

    # Uncertainty of the mean: noise averaging plus drift contribution.
    sem_noise = resid_sigma / math.sqrt(n)
    sem_drift = abs(slope) * math.sqrt((ct[-1] - ct[0]) ** 2 / 12.0)
    sem = math.hypot(sem_noise, sem_drift)

    return {
        "mean": mean,
        "sigma": sigma,
        "n": n,
        "seconds": ct[-1] - ct[0],
        "slope": slope,
        "drift_total": drift_total,
        "resid_sigma": resid_sigma,
        "sem": sem,
        "trimmed": trimmed,
        "trimmed_pct": 100.0 * trimmed / len(kept),
    }


def report(name, s):
    print(f"--- {name} ---")
    print(f"  samples          : {s['n']} over {s['seconds']:.1f} s")
    print(f"  mean             : {s['mean']:,.1f} counts")
    print(f"  raw sigma        : {s['sigma']:.1f} counts")
    print(f"  residual sigma   : {s['resid_sigma']:.1f} counts (drift removed)")
    print(f"  drift            : {s['slope']:+.2f} counts/s "
          f"({s['drift_total']:+.1f} total)")
    print(f"  SEM of mean      : +/-{s['sem']:.1f} counts")
    if s["trimmed"]:
        print(f"  outliers trimmed : {s['trimmed']} ({s['trimmed_pct']:.2f}%)")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tare", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--mass", type=float, required=True,
                    help="known reference mass in grams")
    ap.add_argument("--warmup", type=float, default=5.0)
    args = ap.parse_args()

    print()
    tare = analyse(parse(args.tare), args.warmup)
    ref = analyse(parse(args.reference), args.warmup)
    report("tare", tare)
    report("reference", ref)

    delta = ref["mean"] - tare["mean"]
    cal = delta / args.mass

    # Uncertainty: reference mean, tare mean and the mass itself.
    u_delta = math.hypot(ref["sem"], tare["sem"])
    u_cal = u_delta / args.mass
    print(f"=== CALIBRATION ===")
    print(f"  reference mass   : {args.mass:.1f} g")
    print(f"  delta            : {delta:,.1f} counts +/-{u_delta:.1f}")
    print()
    print(f"  tare_offset      : {round(tare['mean'])}")
    print(f"  cal_factor       : {cal:.2f} counts/g  (+/-{u_cal:.2f}, "
          f"{100.0 * u_cal / cal:.2f}%)")
    print()
    print(f"  1 count          : {1000.0 / cal:.4f} mg")
    print(f"  resolution 0.1 g : {0.1 * cal:.1f} counts")
    print(f"  resolution 0.01 g: {0.01 * cal:.2f} counts")
    print(f"  sigma in grams   : {ref['sigma'] / cal * 1000:.1f} mg")
    print()

    # Sanity: where does full scale land?
    fs_counts = 2 ** 23
    print(f"  6 kg would be    : {6000 * cal / 1e6:.2f}M counts "
          f"({100.0 * 6000 * cal / fs_counts:.0f}% of HX711 full scale)")
    return 0


if __name__ == "__main__":
    sys.exit(main())