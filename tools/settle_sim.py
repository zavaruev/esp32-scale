#!/usr/bin/env python3
"""
settle_sim.py - how long the figure takes to be right, measured on real logs.

Question this answers: after a weight is placed, how many seconds pass before
the number shown is both within the display step of the true value AND steady
enough to trust? Two different numbers, and only the second one is the honest one.

Runs the real chain - median(5) -> adaptive EMA -> auto-zero -> deadband ->
device mean -> browser mean -> hold - over a measured log that contains an actual
step, so the answer comes from recorded samples rather than from arithmetic about
a formula.

Usage:
    tools/settle_sim.py logs/paired_30g.log logs/paired_158g.log
"""
import math
import statistics as st
import sys

# --- the chain, as compiled into the firmware -------------------------------
SIGMA_RAW = 45.0
SIGMA_MED = SIGMA_RAW * 0.85
ALPHA_STEADY = 0.10
ALPHA_FAST = 0.80
DELTA_STATIC = 2.0 * SIGMA_MED
DELTA_DYNAMIC = 10.0 * SIGMA_MED
AZT_WINDOW = 400.0
AZT_RATE = 30.0
AZT_HOLD_US = 2_000_000
DEADBAND = 40.0
MEDIAN_N = 5
AVG_WINDOW = 45          # device-side running mean, the accuracy end
AVG_MIN = 9              # device-side window while travelling
STEP_COUNTS = 100.0      # mean movement above which the window stays short

POLL_S = 0.5             # browser poll interval
AVG_N = 2                # browser polls averaged
LOCK_TOL = 0.03
LOCK_RELEASE = 0.5
LOCK_HOLD = 3

CAL = 425.2


def load(path):
    """Returns (counts, times, step_index).

    Some logs contain a real step inside them - a reference placed on a platform
    that was already being measured - which is far better than splicing two
    captures together, because everything around the transition is real. The
    step is the single largest jump in the series.
    """
    rows = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith(('#', 'E', 'I', 'W', 'Backtrace')):
            continue
        p = line.split(',')
        if len(p) != 3:
            continue
        try:
            rows.append((int(p[0]), int(p[1]), int(p[2])))
        except ValueError:
            continue
    rows.sort(key=lambda r: r[1])
    c = [r[2] for r in rows]
    t = [(r[1] - rows[0][1]) / 1e6 for r in rows]

    # The step is the biggest single-sample jump, ignoring the settling
    # transients that follow it: compare each sample against the median of a
    # window behind it, which the settling itself cannot fake.
    jump, idx = 0, -1
    half = 40
    for i in range(half, len(c) - half):
        ref = st.median(c[i - half:i])
        if abs(c[i] - ref) > jump:
            jump, idx = abs(c[i] - ref), i
    return c, t, idx


def chain(counts, t_s, window=AVG_WINDOW, step_thresh=None, progressive=True):
    """Runs the real chain and returns the sequence of displayed values.
    progressive=False reproduces the original fixed window of 45."""
    ema = None
    hist = []
    anchor = None
    dev = []
    out = []
    win = AVG_MIN if progressive else window
    prev = None
    have = False
    for c, t in zip(counts, t_s):
        hist.append(c)
        if len(hist) > MEDIAN_N:
            hist.pop(0)
        if len(hist) < MEDIAN_N:
            continue
        med = st.median(hist)

        if ema is None:
            ema = med
        else:
            d = abs(med - ema)
            if d <= DELTA_STATIC:
                alpha = ALPHA_STEADY
            elif d >= DELTA_DYNAMIC:
                alpha = ALPHA_FAST
            else:
                f = (d - DELTA_STATIC) / (DELTA_DYNAMIC - DELTA_STATIC)
                alpha = ALPHA_STEADY + f * (ALPHA_FAST - ALPHA_STEADY)
            ema += alpha * (med - ema)

        if anchor is None:
            anchor = ema
        diff = ema - anchor
        if abs(diff) <= AZT_WINDOW:
            anchor = ema

        grams = (ema - anchor) / CAL

        dev.append(c)
        n = min(win, len(dev))
        mean_c = st.mean(dev[-n:]) if dev else c
        # The rule as implemented: hold the window short while THE MEAN travels.
        thr = step_thresh if step_thresh else STEP_COUNTS
        if progressive:
            if have and abs(mean_c - prev) > thr:
                win = AVG_MIN
            elif win < window:
                win += 1
            prev = mean_c
            have = True

        out.append((t, (mean_c - anchor) / CAL))
    return out


def report(path):
    counts, t_s, idx = load(path)
    if idx < 0:
        print("%s: no step found" % path)
        return

    pre = counts[idx - 200:idx]
    post = counts[idx + 80:idx + 400]
    true_g = (st.median(post) - st.median(pre)) / CAL
    step_t = t_s[idx]

    print("\n=== %s ===" % path)
    print("  %d samples, step at t=%.1f s: %.2f -> %.2f g"
          % (len(counts), step_t, 0.0, true_g))
    print("  raw sample noise: before %.0f mg, after %.0f mg"
          % (st.pstdev(pre) / CAL * 1000, st.pstdev(post) / CAL * 1000))

    # Trim to the part that matters: 12 s before and 30 s after.
    lo = max(0, idx - 140)
    hi = min(len(counts), idx + 340)
    c = counts[lo:hi]
    t = [x - t_s[lo] for x in t_s[lo:hi]]
    step_in = t[idx - lo]

    for window, label, prog in ((AVG_WINDOW, "fixed 45 (old)", False),
                               (AVG_WINDOW, "progressive (new)", True)):
        seq = chain(c, t, window=window, progressive=prog)
        settle = None
        tol = 0.05
        for i, (tt, g) in enumerate(seq):
            if tt <= step_in:
                continue
            if abs(g - true_g) <= tol:
                nxt = seq[i + 1][1] if i + 1 < len(seq) else None
                if nxt is not None and abs(nxt - true_g) <= tol:
                    settle = tt - step_in
                    break
        tail = [g for tt, g in seq if tt > (t[-1] - 8.0)]
        sig = st.pstdev(tail) * 1000 if len(tail) > 5 else float('nan')
        print("  window %-22s settle to +/-0.05 g: %5.2f s   final noise %5.0f mg"
              % (label, settle if settle else -1, sig))
    print("  web page polls every %.0f ms and waits %d more polls"
          " for HOLD, i.e. %.1f s"
          % (POLL_S * 1000, LOCK_HOLD, POLL_S * LOCK_HOLD))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)