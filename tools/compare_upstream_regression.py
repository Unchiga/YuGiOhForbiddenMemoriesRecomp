#!/usr/bin/env python3
"""Compare capture trees and traded scratch cards; emit reviewable JSON.

No files in either input tree are changed. Unmatched images are reported, not
silently counted as passes. Full-window comparisons intentionally include UI.
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def compare_images(baseline, candidate):
    def images(root):
        return {p.relative_to(root): p for p in root.rglob('*')
                if p.suffix.lower() in ('.png', '.ppm')}
    old, new = images(baseline), images(candidate)
    rows = []
    for name in sorted(old.keys() | new.keys()):
        row = {'image': str(name)}
        if name not in old or name not in new:
            row['result'] = 'missing-baseline' if name not in old else 'missing-candidate'
        else:
            a = Image.open(old[name]).convert('L').resize((80, 60))
            b = Image.open(new[name]).convert('L').resize((80, 60))
            row['mean_gray_difference'] = ImageStat.Stat(ImageChops.difference(a, b)).mean[0]
            row['result'] = 'pass' if row['mean_gray_difference'] < 18 else 'review'
        rows.append(row)
    return rows


def card_integrity(root):
    rows = []
    for path in sorted(root.rglob('card1.mcd')):
        if path.parent.name not in ('host', 'guest'):
            continue
        data = path.read_bytes()
        backups = sorted(path.parent.glob('card1.mcd.pre-netplay-*'))
        row = {'card': str(path.relative_to(root)), 'sha256': hashlib.sha256(data).hexdigest(),
               'backups': [p.name for p in backups]}
        if backups:
            before = backups[-1].read_bytes()
            row['same_size'] = len(data) == len(before)
            row['directory_identical'] = data[0x80:0x800] == before[0x80:0x800]
            ranges = []
            for i, (a, b) in enumerate(zip(before, data)):
                if a == b:
                    continue
                if ranges and ranges[-1][1] == i:
                    ranges[-1][1] = i + 1
                else:
                    ranges.append([i, i + 1])
            row['changed_ranges_exclusive'] = [[hex(a), hex(b)] for a, b in ranges]
            row['changed_bytes'] = sum(b - a for a, b in ranges)
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = {'baseline': str(args.baseline), 'candidate': str(args.candidate),
              'images': compare_images(args.baseline, args.candidate),
              'baseline_cards': card_integrity(args.baseline),
              'candidate_cards': card_integrity(args.candidate)}
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    return int(any(r['result'] != 'pass' for r in result['images']))


if __name__ == '__main__':
    raise SystemExit(main())
