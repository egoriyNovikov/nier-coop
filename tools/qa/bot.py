"""Drives the test bot of the mod (src/mods/TestBot.hpp) in a test copy of the game.

  python tools/qa/bot.py start [slot]            launch the test copy, wait for the bot
  python tools/qa/bot.py cmd [slot] "pad A 100" "wait 2000" "shot title" ...
  python tools/qa/bot.py load [slot] [save]      from the title screen into the world of save N (0 = top)
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
    while time.time() < end:
        lines = out_lines(root)
        if len(lines) >= count:
            return lines
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
    print("\n".join(lines) if lines else "bot did not start in 90 s")


def cmd(slot, commands):
    root = slot_dir(slot)
    before = len(out_lines(root))
    tmp = root / "coop_test_cmd.tmp"
    tmp.write_text("\n".join(commands) + "\n")
    tmp.replace(root / "coop_test_cmd.txt")

    timeout = 30 + sum(int(c.split()[-1]) / 1000 for c in commands
                       if c.split()[0] in ("pad", "stick", "wait") and c.split()[-1].isdigit())
    lines = wait_for(root, before + len(commands), timeout)[before:]
    for line in lines:
        if line.startswith("shot "):
            line = "shot " + str(to_png(Path(line[5:])))
        print(line)
    if len(lines) < len(commands):
        print(f"timeout: {len(lines)} of {len(commands)} commands finished")


def load(slot, save_index):
    """Title screen -> Continue -> save N (0 = top) -> Yes -> Start Game, then wait for the player."""
    root = slot_dir(slot)
    cmd(slot, ["wait 15000", "pad A 150", "wait 3000", "pad A 150", "wait 3000"]
        + ["pad UP 120"] * 3 + ["pad DOWN 120"] * save_index
        + ["pad A 150", "wait 1500", "pad LEFT 120", "pad A 150", "wait 20000", "pad A 150"])
    for _ in range(30):
        before = len(out_lines(root))
        cmd(slot, ["wait 3000", "state"])
        state = out_lines(root)[before:][-1]
        if "player=" in state and "player=none" not in state:
            print("RESULT: PASS loaded")
            return
    cmd(slot, ["shot load_failed"])
    print("RESULT: FAIL the player did not appear")


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
    print("RESULT: PASS" if ok else "RESULT: FAIL")


def stop(slot):
    root = slot_dir(slot)
    exe = str(root / "NieRAutomata.exe")
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Get-Process NieRAutomata -ErrorAction SilentlyContinue | Where-Object Path -eq '{exe}' | Stop-Process -Force"])
    time.sleep(5)
    check(slot)


if __name__ == "__main__":
    action = sys.argv[1]
    rest = sys.argv[2:]
    slot = rest.pop(0) if rest and rest[0].startswith("slot") else "slot1"
    {"start": lambda: start(slot), "cmd": lambda: cmd(slot, rest),
     "load": lambda: load(slot, int(rest[0]) if rest else 0),
     "stop": lambda: stop(slot), "check": lambda: check(slot)}[action]()
