"""Compare matched, steady gameplay intervals in AC II trace CSVs."""
import argparse
import csv
from pathlib import Path
from statistics import median


def summarize(path, first, last):
    rates, cadence = [], []
    with path.open(encoding="utf-8", newline="") as stream:
        for event in csv.DictReader(stream):
            frame = int(event["frame"])
            if event["event"] == "present_rate":
                window = int(float(event["arg0"]))
                if frame - window >= first and frame <= last:
                    rates.append((window, float(event["arg1"])))
            elif event["event"] == "frame_cadence":
                window = int(event["width"])
                if frame - window >= first and frame <= last:
                    cadence.append((float(event["arg0"]), float(event["arg1"]), float(event["height"])))
    if not rates or not cadence:
        raise ValueError(f"{path.name} has no full measurement windows in the selected interval.")
    frames = sum(count for count, _ in rates)
    seconds = sum(us for _, us in rates) * 1e-6
    return frames, seconds, frames / seconds, median(x[0] for x in cadence) / 1000, \
        median(x[1] for x in cadence) / 1000, max(x[2] for x in cadence) / 1000


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--baseline-frames", type=int, nargs=2, required=True, metavar=("FIRST", "LAST"))
    parser.add_argument("--candidate-frames", type=int, nargs=2, required=True, metavar=("FIRST", "LAST"))
    options = parser.parse_args()
    print("Run           Frames   Seconds       FPS    Median p95    Median p99      Worst")
    for label, path, frames in (("Baseline", options.baseline, options.baseline_frames),
                                ("Candidate", options.candidate, options.candidate_frames)):
        try:
            count, seconds, fps, p95, p99, worst = summarize(path, *frames)
        except ValueError as error:
            parser.error(str(error))
        print(f"{label:<10} {count:>8} {seconds:>9.2f} {fps:>9.2f} {p95:>11.2f} ms "
              f"{p99:>11.2f} ms {worst:>8.2f} ms")
