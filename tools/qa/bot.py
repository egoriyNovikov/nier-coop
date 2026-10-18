"""Drives the test bot of the mod (src/mods/TestBot.hpp) in a test copy of the game.

  python tools/qa/bot.py start [slot]            launch the test copy, wait for the bot
  python tools/qa/bot.py cmd [slot] "pad A 100" "wait 2000" "shot title" ...
  python tools/qa/bot.py load [slot] [save]      from the title screen into the world of save N (0 = top)
  python tools/qa/bot.py chapter [slot] <save> <chapter> <segment> [2B|9S]   Chapter Select (0-based), into the world
  python tools/qa/bot.py stop [slot]             kill the test copy, then check isolation
  python tools/qa/bot.py check [slot]            isolation check only

Isolation check (coop-anything rule 2): the owner's saves and config keep the hashes recorded at
`start`, and the write guard blocked nothing (coop_test_guard.txt). Prints RESULT: PASS / FAIL.
"""
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OWNER_DIRS = [
    Path(os.environ["USERPROFILE"]) / "Documents" / "My Games" / "NieR_Automata",
    Path(os.environ["APPDATA"]) / "PlatinumGames" / "NierAutomata",
]


def slot_dir(slot):
    return REPO / ".testinstall" / slot


def owner_hashes():
    result = {}
    for d in OWNER_DIRS:
        for f in sorted(d.glob("*")):
            if f.is_file():
                result[str(f)] = hashlib.sha256(f.read_bytes()).hexdigest()
    return result


def out_lines(root):
    path = root / "coop_test_out.txt"
    return path.read_text(encoding="utf-8", errors="replace").splitlines() if path.exists() else []


def wait_for(root, count, timeout):
    end = time.time() + timeout
    next_alive_check = time.time() + 5
    while time.time() < end:
        lines = out_lines(root)
        if len(lines) >= count:
            return lines
        # Don't wait for answers from a game that has exited.
        if time.time() > next_alive_check:
            if not game_running(root):
                break
            next_alive_check = time.time() + 5
        time.sleep(0.25)
    return out_lines(root)


def to_png(bmp):
    from PIL import Image
    png = bmp.with_suffix(".png")
    image = Image.open(bmp)
    image.thumbnail((1280, 1280))
    image.save(png)
    return png


def game_running(root):
    exe = str(root / "NieRAutomata.exe").lower()
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process NieRAutomata -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }"],
                         capture_output=True, text=True).stdout
    return any(line.strip().lower() == exe for line in out.splitlines())


def start(slot):
    root = slot_dir(slot)
    if game_running(root):
        sys.exit(f"{slot} is already running")
    (root / "owner_hashes.json").write_text(json.dumps(owner_hashes(), indent=1))
    for name in ["coop_test_out.txt", "coop_test_cmd.txt"]:
        (root / name).unlink(missing_ok=True)
    subprocess.Popen([str(root / "NieRAutomata.exe")], cwd=root)
    lines = wait_for(root, 1, 90)
    print(f"{slot}: " + ("\n".join(lines) if lines else "the bot did not start"))
    return bool(lines)


def cmd(slot, commands, quiet=False):
    root = slot_dir(slot)
    before = len(out_lines(root))
    tmp = root / "coop_test_cmd.tmp"
    tmp.write_text("\n".join(commands) + "\n")
    tmp.replace(root / "coop_test_cmd.txt")

    timeout = 30 + sum(int(c.split()[-1]) / 1000 for c in commands
                       if c.split()[0] in ("pad", "stick", "wait") and c.split()[-1].isdigit())
    lines = wait_for(root, before + len(commands), timeout)[before:]
    result = []
    for line in lines:
        if line.startswith("shot "):
            line = "shot " + str(to_png(Path(line[5:])))
        result.append(line)
        if not quiet:
            print(f"{slot}: {line}")
    if len(lines) < len(commands):
        print(f"{slot}: timeout: {len(lines)} of {len(commands)} commands finished")
    return result


# Menu screens are recognised by comparing parts of a screenshot with reference screenshots of the same
# screens in .testinstall/reference/<screen>.png (local only: they are images of the game, not for git).
# Make them once with `cmd shot <name>` on each screen. Blind timed presses were not enough: a press lost
# on the title screen shifted the rest and once ended in License, once in Exit Game -> Yes.
REFERENCE = REPO / ".testinstall" / "reference"
SCREENS = ["title", "main_menu", "load_game", "confirm_load", "start_menu", "name_entry"]
# (x0, y0, x1, y1, size): screen header ("LOAD GAME" / "START MENU" text), centre (main menu items vs
# "Press Any Button"), save thumbnail (only on Load Game). Measured 2026-10-18: closest pair of different
# screens differs by 10 (title vs main menu, centre), 22+ elsewhere.
REGIONS = [(0.03, 0.08, 0.30, 0.14, (240, 24)), (0.33, 0.55, 0.67, 0.86, (96, 24)), (0.06, 0.18, 0.20, 0.33, (48, 32))]
UNKNOWN_DISTANCE = 25


def _features(path):
    from PIL import Image
    image = Image.open(path).convert("L")
    w, h = image.size
    return [image.crop((int(a * w), int(b * h), int(c * w), int(d * h))).resize(size) for a, b, c, d, size in REGIONS]


def _distance(a, b):
    from PIL import ImageChops, ImageStat
    return sum(ImageStat.Stat(ImageChops.difference(x, y)).mean[0] for x, y in zip(a, b))


def screen(slot):
    """Name of the menu screen the game shows, or "unknown" (logos, loading, the world)."""
    shot = cmd(slot, ["shot screen"], quiet=True)
    if not shot or not shot[0].startswith("shot "):
        return "unknown"
    features = _features(shot[0][5:])
    scores = sorted((_distance(features, _features(REFERENCE / f"{name}.png")), name) for name in SCREENS)
    best_distance, best = scores[0]
    return best if best_distance <= UNKNOWN_DISTANCE else "unknown"


def player_state(slot):
    state = cmd(slot, ["state"], quiet=True)[-1:]
    return state[0] if state and "player=" in state[0] and "player=none" not in state[0] else None


def load(slot, save_index, timeout=240):
    """From wherever the menus are into the world of save N (0 = top), looking at the screen before each press."""
    root = slot_dir(slot)
    actions = {
        "title": ["pad A 150", "wait 2500"],
        "main_menu": ["pad A 150", "wait 2500"],  # the cursor starts on Continue
        "load_game": ["pad UP 120"] * 3 + ["pad DOWN 120"] * save_index + ["pad A 150", "wait 1500"],
        "confirm_load": ["pad LEFT 120", "pad A 150", "wait 5000"],
        "start_menu": ["pad A 150", "wait 5000"],  # Start Game is the first item
        "name_entry": ["pad B 150", "wait 2000"],  # a new game was started by mistake: back out
    }
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        if not game_running(root):
            print(f"{slot}: RESULT: FAIL the game exited (screens: {' > '.join(seen)})")
            return False
        state = player_state(slot)
        if state:
            print(f"{slot}: {state}")
            print(f"{slot}: RESULT: PASS loaded (screens: {' > '.join(seen)})")
            return True
        name = screen(slot)
        if not seen or seen[-1] != name:
            seen.append(name)
        cmd(slot, actions.get(name, ["wait 2000"]), quiet=True)
    cmd(slot, ["shot load_failed"])
    print(f"{slot}: RESULT: FAIL the player did not appear in {timeout} s (screens: {' > '.join(seen)})")
    return False


def to_start_menu(slot, save_index, timeout=180):
    """From the title screen to the Start Menu of save N (Start Game / Chapter Select)."""
    actions = {
        "title": ["pad A 150", "wait 2500"],
        "main_menu": ["pad A 150", "wait 2500"],
        "load_game": ["pad UP 120"] * 3 + ["pad DOWN 120"] * save_index + ["pad A 150", "wait 1500"],
        "confirm_load": ["pad LEFT 120", "pad A 150", "wait 5000"],
    }
    end = time.time() + timeout
    while time.time() < end:
        name = screen(slot)
        if name == "start_menu":
            return True
        cmd(slot, actions.get(name, ["wait 2000"]), quiet=True)
    return False


def chapter(slot, save_index, chapter_index, segment_index, character="2B", timeout=180):
    """Chapter Select on a finished save: chapter N (0 = Prologue), segment M (0 = first), as 2B or 9S,
    then wait until the player is in the world. Works on a fresh start (the list cursor is at the top)."""
    if not to_start_menu(slot, save_index):
        print(f"{slot}: RESULT: FAIL no Start Menu")
        return False
    cmd(slot, ["wait 1500", "pad DOWN 120", "wait 500", "pad A 150", "wait 2500"]  # Chapter Select (the menu needs a moment)
        + ["pad DOWN 120"] * chapter_index + ["pad A 150", "wait 1000"]  # expand the chapter
        + ["pad DOWN 120"] * (segment_index + 1) + ["pad A 150", "wait 1000"]  # the segment: 2B / 9S popup
        + (["pad DOWN 120"] if character == "9S" else []) + ["pad A 150", "wait 1000"]
        + ["pad LEFT 120", "pad A 150", "wait 5000"], quiet=True)  # Begin? Yes
    end = time.time() + timeout
    while time.time() < end:
        state = player_state(slot)
        if state:
            print(f"{slot}: {state}")
            return True
        cmd(slot, ["wait 3000"], quiet=True)
    cmd(slot, ["shot chapter_failed"])
    print(f"{slot}: RESULT: FAIL the player did not appear after Chapter Select")
    return False


def check(slot):
    root = slot_dir(slot)
    ok = True
    recorded = root / "owner_hashes.json"
    if recorded.exists():
        before, now = json.loads(recorded.read_text()), owner_hashes()
        changed = [f for f in set(before) | set(now) if before.get(f) != now.get(f)]
        for f in changed:
            print(f"owner file changed: {f}")
        ok &= not changed
    else:
        print("no owner_hashes.json (run start first)")
        ok = False
    guard = root / "coop_test_guard.txt"
    blocked = [l for l in guard.read_text(encoding="utf-8", errors="replace").splitlines()
               if l.startswith("BLOCKED")] if guard.exists() else ["no coop_test_guard.txt"]
    for line in blocked:
        print(line)
    ok &= not blocked
    print(f"{slot}: isolation " + ("RESULT: PASS" if ok else "RESULT: FAIL"))
    return ok


def stop(slot):
    root = slot_dir(slot)
    exe = str(root / "NieRAutomata.exe")
    # Steam Auto-Cloud uploads the owner's save folder when the game exits: a changed owner file must be
    # dealt with (by the owner) while the game still runs, or the change reaches the cloud.
    if game_running(root) and not check(slot):
        print(f"{slot}: NOT closing the game: an owner file changed or a write was blocked. "
              "Restore the owner's file first, then close the game.")
        return False
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Get-Process NieRAutomata -ErrorAction SilentlyContinue | Where-Object Path -eq '{exe}' | Stop-Process -Force"])
    time.sleep(5)
    return check(slot)


if __name__ == "__main__":
    action = sys.argv[1]
    rest = sys.argv[2:]
    slot = rest.pop(0) if rest and rest[0].startswith("slot") else "slot1"
    {"start": lambda: start(slot), "cmd": lambda: cmd(slot, rest),
     "load": lambda: load(slot, int(rest[0]) if rest else 0),
     "chapter": lambda: chapter(slot, int(rest[0]), int(rest[1]), int(rest[2]), rest[3] if len(rest) > 3 else "2B"),
     "stop": lambda: stop(slot), "check": lambda: check(slot)}[action]()
