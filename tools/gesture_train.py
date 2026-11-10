#!/usr/bin/env python3
"""Train the raise-to-mouth classifier from labelled IMU windows (SPEC §7.3-7.4).

Data (gateway/received/, gitignored):
  gesture_feedback/*_correct.csv   positives - the user tapped green
  gesture_feedback/*_wrong.csv     negatives - the user tapped red
  imu/*.csv                        tap-to-capture repetitions of the gesture (positives)
  imu_negative/*.csv               tap-to-capture of other movements (negatives)

Every window is aligned on the moment the watch ENTERS the mouth pose; features
use the 1.5 s before entry. Feedback CSVs from firmware >= gesture v3 also hold
1 s after entry (their .txt sidecar has "entry=<row>"), kept for learning the
dwell later.

Model: logistic regression on 4 features that separate the user's gesture from
other movements in the first labelled session (2026-11-09):
  rot_x, rot_y, rot_z  integrated rotation (deg) about each axis
  t_peak               seconds from the rotation peak to pose entry
Writes firmware/main/gesture_model.h and prints leave-one-out results.

Usage: tools/gesture_train.py [--threshold 0.6]
"""
import argparse
import csv
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECEIVED = ROOT / "gateway" / "received"
HEADER = ROOT / "firmware" / "main" / "gesture_model.h"

# Must match firmware/main/imu.c
SAMPLE_S = 0.02
WIN = 75                       # samples before entry
POSE_PITCH = (-65.0, -5.0)
POSE_ROLL_ABS_MIN = 140.0
FEATURES = ("rot_x", "rot_y", "rot_z", "t_peak")


def load(path):
    with open(path) as f:
        return [[float(r[k]) for k in ("ax", "ay", "az", "gx", "gy", "gz")] for r in csv.DictReader(f)]


def tilt(s):
    return (math.degrees(math.atan2(-s[0], math.sqrt(s[1] ** 2 + s[2] ** 2))),
            math.degrees(math.atan2(s[1], s[2])))


LYING_PITCH = (-70.0, -30.0)     # second mouth box: lying down / reclined
LYING_ROLL = (80.0, 135.0)


def region(s):
    """0 none, 1 upright mouth box, 2 lying-down mouth box (mirrors imu.c)."""
    pitch, roll = tilt(s)
    if POSE_PITCH[0] <= pitch <= POSE_PITCH[1] and abs(roll) >= POSE_ROLL_ABS_MIN:
        return 1
    if LYING_PITCH[0] <= pitch <= LYING_PITCH[1] and LYING_ROLL[0] <= abs(roll) <= LYING_ROLL[1]:
        return 2
    return 0


def entry_index(rows):
    """First move into a mouth box (incl. upright -> lying), as the firmware detects it."""
    prev = 0
    for i, s in enumerate(rows):
        cur = region(s)
        if cur and cur != prev and i >= 5:
            return i
        prev = cur
    return None


def features(pre):
    """pre = samples up to and including pose entry (mirrors imu.c)."""
    rot = [sum(s[3 + k] for s in pre) * SAMPLE_S for k in range(3)]
    mags = [math.sqrt(s[3] ** 2 + s[4] ** 2 + s[5] ** 2) for s in pre]
    peak_i = max(range(len(mags)), key=mags.__getitem__)
    t_peak = (len(pre) - 1 - peak_i) * SAMPLE_S
    return rot + [t_peak]


def feedback_entry(csv_path, rows):
    side = csv_path.with_suffix(".txt")
    if side.exists():
        for line in side.read_text().splitlines():
            if line.startswith("entry="):
                return int(line.split("=", 1)[1])
            if line.startswith("detail:") and "entry=" in line:
                return int(line.split("entry=", 1)[1].split(";")[0])
    return len(rows) - 1       # v2 windows end at entry


def dataset():
    data = []
    for label, pattern in ((1, "*_correct.csv"), (0, "*_wrong.csv")):
        for p in sorted((RECEIVED / "gesture_feedback").glob(pattern)):
            rows = load(p)
            e = feedback_entry(p, rows)
            data.append((label, "feedback", p.name, features(rows[max(0, e - WIN):e + 1])))
    for label, folder in ((1, "imu"), (0, "imu_negative")):
        for p in sorted((RECEIVED / folder).glob("*.csv")):
            rows = load(p)
            e = entry_index(rows)       # movements that never reach the pose can't trigger
            if e is not None:
                data.append((label, "capture", p.name, features(rows[max(0, e - WIN):e + 1])))
    return data


def standardize(xs):
    cols = list(zip(*xs))
    mean = [sum(c) / len(c) for c in cols]
    std = [math.sqrt(sum((v - m) ** 2 for v in c) / len(c)) or 1.0 for c, m in zip(cols, mean)]
    return mean, std


def fit(xs, ys, l2=1.0, iters=3000, lr=0.1):
    mean, std = standardize(xs)
    z = [[(v - m) / s for v, m, s in zip(x, mean, std)] for x in xs]
    w, b, n = [0.0] * len(mean), 0.0, len(ys)
    for _ in range(iters):
        gw, gb = [0.0] * len(w), 0.0
        for zi, yi in zip(z, ys):
            p = 1 / (1 + math.exp(-(sum(a * c for a, c in zip(w, zi)) + b)))
            for k in range(len(w)):
                gw[k] += (p - yi) * zi[k]
            gb += p - yi
        w = [wk - lr * (g / n + l2 * wk / n) for wk, g in zip(w, gw)]
        b -= lr * gb / n
    return mean, std, w, b


def prob(model, x):
    mean, std, w, b = model
    return 1 / (1 + math.exp(-(sum(wk * (v - m) / s for wk, v, m, s in zip(w, x, mean, std)) + b)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=float, default=0.6)
    args = ap.parse_args()

    data = dataset()
    pos = sum(1 for d in data if d[0] == 1)
    print(f"{len(data)} windows: {pos} positive, {len(data) - pos} negative")
    if pos < 5 or len(data) - pos < 5:
        raise SystemExit("need at least 5 positives and 5 negatives")

    # Leave-one-out on the real-world (feedback) windows only.
    tp = fp = tn = fn = 0
    for i, (y, src, _, x) in enumerate(data):
        if src != "feedback":
            continue
        rest = [d for j, d in enumerate(data) if j != i]
        model = fit([d[3] for d in rest], [d[0] for d in rest])
        v = prob(model, x) >= args.threshold
        tp += v and y == 1
        fp += v and y == 0
        tn += (not v) and y == 0
        fn += (not v) and y == 1
    print(f"leave-one-out @ {args.threshold}: correct kept {tp}/{tp + fn}, wrong blocked {tn}/{tn + fp}, "
          f"precision {tp / max(1, tp + fp):.0%}")

    mean, std, w, b = fit([d[3] for d in data], [d[0] for d in data])
    fmt = lambda xs: ", ".join(f"{v:.6f}f" for v in xs)
    HEADER.write_text("\n".join([
        "/* Generated by tools/gesture_train.py - do not edit by hand. */",
        "#pragma once",
        "",
        f"/* Features: {', '.join(FEATURES)} (see imu.c). Trained on {pos} positive /"
        f" {len(data) - pos} negative windows. */",
        f"#define GESTURE_N_FEATURES {len(FEATURES)}",
        f"static const float gesture_mean[GESTURE_N_FEATURES] = {{{fmt(mean)}}};",
        f"static const float gesture_std[GESTURE_N_FEATURES] = {{{fmt(std)}}};",
        f"static const float gesture_weight[GESTURE_N_FEATURES] = {{{fmt(w)}}};",
        f"static const float gesture_bias = {b:.6f}f;",
        f"static const float gesture_threshold = {args.threshold:.3f}f;",
        "",
    ]))
    print(f"wrote {HEADER.relative_to(ROOT)}: weights {[round(v, 2) for v in w]}, bias {b:.2f}")


if __name__ == "__main__":
    main()
