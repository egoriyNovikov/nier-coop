"""Co-op smoke test (task 0.5.8): two test copies of the game connect to each other without a person.

  python tools/qa/coop_smoke.py [--save N] [--no-build]

1. Builds the mod and the server, installs them into .testinstall/slot1 (host) and slot2 (client).
2. Both copies load save N (default 1, the "Test" save at the Bunker) through the test bot.
3. slot1 presses "Host Game", slot2 "Join Game" to 127.0.0.1:6969.
4. Asserts: both are in the session, slot1 leads the story and slot2 doesn't, each sees the other
   player's position close to where the other game says its player is, the phase is the same.
5. Closes both and checks isolation (the owner's saves and config unchanged, nothing blocked).
Prints RESULT: PASS or RESULT: FAIL; exit code 0 on PASS.
"""
import argparse
import re
import subprocess
import sys

import bot

HOST, CLIENT = "slot1", "slot2"
PORT = "6969"


def run(command, cwd=bot.REPO):
    print("$ " + " ".join(command))
    if subprocess.run(command, cwd=cwd).returncode != 0:
        sys.exit("RESULT: FAIL build step failed")


def install():
    run(["cmake", "--build", "build", "--config", "Release", "--target", "automatamp"])
    run(["go", "build", "-o", str(bot.REPO / "build" / "bin" / "server.exe"), "."], cwd=bot.REPO / "server")
    for slot in (HOST, CLIENT):
        if not (bot.slot_dir(slot) / "coop_test.ini").exists():
            run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                 str(bot.REPO / "tools" / "qa" / "make_test_install.ps1"), "-Slot", slot])
        (bot.slot_dir(slot) / "dinput8.dll").write_bytes((bot.REPO / "build" / "bin" / "automatamp" / "dinput8.dll").read_bytes())
    server_dir = bot.slot_dir(HOST) / "automatamp_server"
    server_dir.mkdir(exist_ok=True)
    (server_dir / "server.exe").write_bytes((bot.REPO / "build" / "bin" / "server.exe").read_bytes())


def parse(line, key):
    match = re.search(rf"\b{key}=(\S+)", line)
    return match.group(1) if match else None


def position(text):
    return [float(v) for v in text.split(",")] if text and text != "none" else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--save", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    if not args.no_build:
        install()

    failures = []

    def expect(condition, message):
        print(("  ok   " if condition else "  FAIL ") + message)
        if not condition:
            failures.append(message)

    try:
        scenario(args, expect)
    finally:
        isolated = [bot.stop(slot) for slot in (HOST, CLIENT)]
        server = bot.slot_dir(HOST) / "automatamp_server" / "server.exe"
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"Get-Process server -ErrorAction SilentlyContinue | Where-Object Path -eq '{server}' | Stop-Process -Force"])
        if not all(isolated):
            failures.append("isolation")

    print("RESULT: PASS" if not failures else f"RESULT: FAIL ({len(failures)}): " + "; ".join(failures))
    sys.exit(0 if not failures else 1)


def scenario(args, expect):
    # One copy after the other: menu input is blind (timed button presses), and two copies loading at once
    # reach the title screen late, which shifts the presses (seen 2026-10-18: slot2 ended up in License).
    for slot in (HOST, CLIENT):
        loaded = False
        for attempt in range(2):
            if attempt > 0:
                print(f"{slot}: retrying with a fresh start")
                bot.stop(slot)
            if bot.start(slot) and bot.load(slot, args.save):
                loaded = True
                break
        expect(loaded, f"{slot} loaded save {args.save}")
        if not loaded:
            return

    expect(bot.cmd(HOST, [f"host {PORT}"])[-1].startswith("ok"), "Host Game started the server")
    expect(bot.cmd(CLIENT, [f"join 127.0.0.1:{PORT}"])[-1].startswith("ok"), "Join Game accepted the address")

    host_coop = client_coop = ""
    for _ in range(30):
        host_coop = bot.cmd(HOST, ["wait 2000", "coop"], quiet=True)[-1]
        client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
        if "data=1" in host_coop and "data=1" in client_coop:
            break
    print(f"{HOST}: {host_coop}\n{CLIENT}: {client_coop}")

    expect(parse(host_coop, "session") == "1" and parse(client_coop, "session") == "1", "both are in the session")
    expect(parse(host_coop, "master") == "1", "the host leads the story")
    expect(parse(client_coop, "master") == "0", "the client doesn't lead the story")
    expect("data=1" in host_coop and "data=1" in client_coop, "both receive the other player's data")

    # Each game's view of the other player against where the other game says its player is.
    host_state = bot.cmd(HOST, ["state"], quiet=True)[-1]
    client_state = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
    print(f"{HOST}: {host_state}\n{CLIENT}: {client_state}")
    for viewer, view, other, other_state in ((HOST, host_coop, CLIENT, client_state), (CLIENT, client_coop, HOST, host_state)):
        seen, actual = position(parse(view, "pos")), position(parse(other_state, "player"))
        distance = sum((a - b) ** 2 for a, b in zip(seen, actual)) ** 0.5 if seen and actual else None
        expect(distance is not None and distance < 3.0,
               f"{viewer} sees {other} where {other} is (distance {distance if distance is None else round(distance, 2)} m)")
    expect(parse(host_state, "phase") == parse(client_state, "phase"), "both games are in the same phase")
    # Concept 5.1, route A: the host plays 2B (10000), the client takes over the story buddy 9S (10200).
    expect(parse(host_state, "model") == "10000", f"the host plays 2B (model {parse(host_state, 'model')})")
    expect(parse(client_state, "model") == "10200", f"the client plays 9S (model {parse(client_state, 'model')})")

    bot.cmd(HOST, ["shot coop_host"])
    bot.cmd(CLIENT, ["shot coop_client"])


if __name__ == "__main__":
    main()
