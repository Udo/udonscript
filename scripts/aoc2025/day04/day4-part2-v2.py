#!/usr/bin/env python3

from pathlib import Path


EXAMPLE_INPUT = (
    "..@@.@@@@.",
    "@@@.@.@.@@",
    "@@@@@.@.@@",
    "@.@@@@..@.",
    "@@.@@@@.@@",
    ".@@@@@@@.@",
    ".@.@.@.@@@",
    "@.@@@.@@@@",
    ".@@@@@@@@.",
    "@.@.@@@.@.",
)

INPUT_PATH = Path(__file__).with_name("input.txt")
OFFSETS = tuple(
    (dr, dc)
    for dr in (-1, 0, 1)
    for dc in (-1, 0, 1)
    if not (dr == 0 and dc == 0)
)


def parse_rolls(lines):
    rolls = {}
    width = 0
    height = 0
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if width == 0:
            width = len(line)
        rowset = set()
        for c, ch in enumerate(line):
            if ch == "@":
                rowset.add(c)
        rolls[height] = rowset
        height += 1
    return rolls, width, height


def has_roll(rolls, width, height, r, c):
    if r < 0 or r >= height or c < 0 or c >= width:
        return False
    return c in rolls.get(r, ())


def adjacent_rolls(rolls, width, height, r, c):
    return sum(1 for dr, dc in OFFSETS if has_roll(rolls, width, height, r + dr, c + dc))


def is_accessible(rolls, width, height, r, c):
    return adjacent_rolls(rolls, width, height, r, c) < 4


def count_accessible(parsed):
    rolls, width, height = parsed
    return sum(
        1
        for r, rowset in rolls.items()
        for c in rowset
        if is_accessible(rolls, width, height, r, c)
    )


def total_removed(lines):
    rolls, width, height = parse_rolls(lines)
    removed = 0
    while True:
        to_remove = {}
        for r, rowset in rolls.items():
            for c in rowset:
                if is_accessible(rolls, width, height, r, c):
                    to_remove.setdefault(r, []).append(c)
        if not to_remove:
            break
        for r, cols in to_remove.items():
            rowset = rolls.get(r, set())
            rowset.difference_update(cols)
            if rowset:
                rolls[r] = rowset
            else:
                rolls.pop(r, None)
            removed += len(cols)
    return removed


def run(label, lines):
    parsed = parse_rolls(lines)
    print(f"{label} accessible:", count_accessible(parsed))
    print(f"{label} total removed:", total_removed(lines))


def main():
    print("=== Day 4.2: Complex-plane solution (Python) ===")
    print("Example accessible (13):", count_accessible(parse_rolls(EXAMPLE_INPUT)))
    print("Example total removed (43):", total_removed(EXAMPLE_INPUT))
    run("Input", INPUT_PATH.read_text().splitlines())


if __name__ == "__main__":
    main()
