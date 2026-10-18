"""Co-op smoke test (task 0.5.8): two test copies of the game connect to each other without a person.

  python tools/qa/coop_smoke.py [--scenario together|diverged] [--no-build]

1. Builds the mod and the server, installs them into .testinstall/slot1 (host) and slot2 (client).
2. Both copies load a save through the test bot (together: both the "Test" save at the Bunker;
   diverged: the host the "Test" save, the client the finished save at the Resistance Camp).
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
    parser.add_argument("--scenario", choices=["together", "diverged"], default="together",
                        help="together: both load the same save; diverged: the games are at different story points")
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
        if args.scenario == "together":
            together(expect)
        else:
            diverged(expect)
    finally:
        isolated = [bot.stop(slot) for slot in (HOST, CLIENT)]
        server = bot.slot_dir(HOST) / "automatamp_server" / "server.exe"
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"Get-Process server -ErrorAction SilentlyContinue | Where-Object Path -eq '{server}' | Stop-Process -Force"])
        if not all(isolated):
            failures.append("isolation")

    print("RESULT: PASS" if not failures else f"RESULT: FAIL ({len(failures)}): " + "; ".join(failures))
    sys.exit(0 if not failures else 1)


def connect(expect, host_save, client_save):
    """Loads the saves, Host Game + Join Game, waits for both to have the other's data.
    Returns (host_coop, client_coop, host_state, client_state) or None."""
    # One copy after the other: two copies loading at once reach the title screen late.
    for slot, save in ((HOST, host_save), (CLIENT, client_save)):
        loaded = False
        for attempt in range(2):
            if attempt > 0:
                print(f"{slot}: retrying with a fresh start")
                bot.stop(slot)
            if bot.start(slot) and bot.load(slot, save):
                loaded = True
                break
        expect(loaded, f"{slot} loaded save {save}")
        if not loaded:
            return None

    expect(bot.cmd(HOST, [f"host {PORT}"])[-1].startswith("ok"), "Host Game started the server")
    expect(bot.cmd(CLIENT, [f"join 127.0.0.1:{PORT}"])[-1].startswith("ok"), "Join Game accepted the address")

    host_coop = client_coop = ""
    for _ in range(30):
        host_coop = bot.cmd(HOST, ["wait 2000", "coop"], quiet=True)[-1]
        client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
        if "data=1" in host_coop and "data=1" in client_coop:
            break

    expect(parse(host_coop, "session") == "1" and parse(client_coop, "session") == "1", "both are in the session")
    expect(parse(host_coop, "master") == "1", "the host leads the story")
    expect(parse(client_coop, "master") == "0", "the client doesn't lead the story")
    expect("data=1" in host_coop and "data=1" in client_coop, "both receive the other player's data")

    # ID_STORY_STATE is sent every 5 s: ask again after that.
    host_coop = bot.cmd(HOST, ["wait 6000", "coop"], quiet=True)[-1]
    client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
    host_state = bot.cmd(HOST, ["state"], quiet=True)[-1]
    client_state = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
    for slot, line in ((HOST, host_coop), (CLIENT, client_coop), (HOST, host_state), (CLIENT, client_state)):
        print(f"{slot}: {line}")

    expect(parse(host_coop, "other_story") == parse(client_state, "phase"),
           f"the host knows the client's phase ({parse(host_coop, 'other_story')})")
    expect(parse(client_coop, "other_story") == parse(host_state, "phase"),
           f"the client knows the host's phase ({parse(client_coop, 'other_story')})")
    return host_coop, client_coop, host_state, client_state


def together(expect):
    """Both load the "Test" save (Bunker, route A): the client takes over 9S, positions match."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result

    # Each game's view of the other player against where the other game says its player is.
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


def log_text(slot):
    return "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in bot.slot_dir(slot).glob("automatamp_log*.txt"))


def diverged(expect):
    """The host is in the Bunker ("Test" save), the client at the Resistance Camp (finished save).
    The mod must notice the different phases, and (concept, section 4) not pull the story buddy out of
    either scene: the other player is far from where the game put the buddy."""
    result = connect(expect, 1, 0)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result

    expect(parse(host_state, "phase") != parse(client_state, "phase"),
           f"the games are in different phases ({parse(host_state, 'phase')} / {parse(client_state, 'phase')})")
    expect("different phases" in log_text(HOST) and "different phases" in log_text(CLIENT),
           "both logs say the phases differ")
    # handle=0: no puppet bound for the other player (the 50 m rule in NierClient::bind_puppet).
    expect(parse(host_coop, "handle") == "0", f"the host's buddy is not taken over (handle {parse(host_coop, 'handle')})")
    expect(parse(client_coop, "handle") == "0", f"the client's buddy is not taken over (handle {parse(client_coop, 'handle')})")

    bot.cmd(HOST, ["shot diverged_host"])
    bot.cmd(CLIENT, ["shot diverged_client"])


if __name__ == "__main__":
    main()
