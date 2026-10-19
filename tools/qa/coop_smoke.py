"""Co-op smoke test (task 0.5.8): two test copies of the game connect to each other without a person.

  python tools/qa/coop_smoke.py [--scenario together|diverged|routes|commander|hangar] [--no-build]

1. Builds the mod and the server, installs them into .testinstall/slot1 (host) and slot2 (client).
2. Both copies load a save through the test bot (together: both the "Test" save at the Bunker;
   diverged: the host the "Test" save, the client the finished save at the Resistance Camp;
   routes: the host the "Test" save (route A), the client Chapter Select as 9S (route B);
   commander: both the "Test" save, then the host talks to the Commander, TASKS 3.0.1;
   hangar: as commander, then the host goes on to the hangar until the phase changes, TASKS 3.0.1 / 3.2.1).
3. slot1 presses "Host Game", slot2 "Join Game" to 127.0.0.1:6969.
4. Asserts: both are in the session, slot1 leads the story and slot2 doesn't, each sees the other
   player's position close to where the other game says its player is, the phase and route are known
   to the other side; per scenario: same phase and roles / different phases / different routes.
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
    parser.add_argument("--scenario", choices=["together", "diverged", "routes", "commander", "hangar"], default="together",
                        help="together: both load the same save; diverged: the games are at different story points; "
                             "routes: the games are on routes A and B; commander: the host talks to the Commander; "
                             "hangar: then the host flies off from the hangar (natural phase change)")
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
        {"together": together, "diverged": diverged, "routes": routes, "commander": commander, "hangar": hangar}[args.scenario](expect)
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
    A save is its index (0 = top) or a function (slot) -> bool that gets the game into the world.
    Returns (host_coop, client_coop, host_state, client_state) or None."""
    # One copy after the other: two copies loading at once reach the title screen late.
    for slot, save in ((HOST, host_save), (CLIENT, client_save)):
        loaded = False
        for attempt in range(2):
            if attempt > 0:
                print(f"{slot}: retrying with a fresh start")
                bot.stop(slot)
            if bot.start(slot) and (save(slot) if callable(save) else bot.load(slot, save)):
                loaded = True
                break
        expect(loaded, f"{slot} loaded save {save.__doc__ if callable(save) else save}")
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
    expect(parse(host_coop, "other_route") == parse(client_state, "route")
           and parse(client_coop, "other_route") == parse(host_state, "route"), "each knows the other's route")
    return host_coop, client_coop, host_state, client_state


def together(expect):
    """Both load the "Test" save (Bunker, route A): the client takes over 9S, positions match."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result
    # Route from the scene state flags Route/A..C (sdk::get_route).
    expect(parse(host_state, "route") == "A" and parse(client_state, "route") == "A",
           f"both games are on route A ({parse(host_state, 'route')} / {parse(client_state, 'route')})")

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


def routes(expect):
    """The host on route A ("Test" save), the client on route B (Chapter Select on the finished save as 9S):
    both must see that the routes differ (concept 5.6, TASKS 3.1.1)."""
    def chapter_as_9s(slot):
        """finished save, Chapter Select as 9S"""
        return bot.chapter(slot, 0, 2, 0, "9S")

    result = connect(expect, 1, chapter_as_9s)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result

    expect(parse(host_state, "route") == "A" and parse(client_state, "route") == "B",
           f"the host is on route A, the client on route B ({parse(host_state, 'route')} / {parse(client_state, 'route')})")
    expect(parse(client_state, "hero") == "9S", f"the client's game has 9S as the hero ({parse(client_state, 'hero')})")
    expect("different routes" in log_text(HOST) and "different routes" in log_text(CLIENT),
           "both logs say the routes differ")


# The Commander on the "Test" save: west of her, facing east, the talk prompt shows (found 2026-10-18).
COMMANDER_SPOT = "goto -114.06 138.37 -22.72 90"


def commander(expect):
    """TASKS 3.0.1, 3.6.1: the host talks to the Commander (a story step inside the phase 16_AB_bunker_Order),
    the client stands still. Asserts the host's step goes 130 -> 140 and that the client's game follows: the
    host sends the finished actions of its story graph (ID_STORY_ACTION), the client's game finishes the same
    ones and its step becomes 140 too (before 3.6.1 it stayed at 130, observed 2026-10-18)."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    talk_to_commander(expect, result[2], result[3])


def talk_to_commander(expect, host_state, client_state):
    expect(parse(host_state, "step") == "130" and parse(client_state, "step") == "130",
           f"both start at step 130 ({parse(host_state, 'step')} / {parse(client_state, 'step')})")

    host_step = "130"
    for attempt in range(2):
        # Hold B next to her; the first try in a fresh session sometimes comes before the talk prompt.
        bot.cmd(HOST, [COMMANDER_SPOT, "wait 3000", "pad B 1500", "wait 2500", f"shot commander_host_talk{attempt}"])
        if attempt == 0:
            bot.cmd(CLIENT, ["shot commander_client_during"])
        # A advances the lines. Stop as soon as the step changes: after the talk A jumps, and 2B can fall off.
        for _ in range(12):
            host_step = parse(bot.cmd(HOST, ["pad A 120", "wait 2300"] * 3 + ["state"], quiet=True)[-1], "step")
            if host_step != "130":
                break
        if host_step != "130":
            break

    bot.cmd(HOST, ["shot commander_host_after"])
    expect(host_step == "140", f"the host's step is 140 after the talk ({host_step})")

    client_step = None
    for _ in range(10):
        client_step = parse(bot.cmd(CLIENT, ["wait 1000", "state"], quiet=True)[-1], "step")
        if client_step == "140":
            break
    bot.cmd(CLIENT, ["wait 2000", "shot commander_client_after"])
    for slot in (HOST, CLIENT):
        lines = [l for l in log_text(slot).splitlines() if "[StoryAction]" in l and "Hooked" not in l]
        print(f"  {slot} story actions: {len(lines)}")
        for line in lines[:40]:
            print("    " + line.split("] ", 2)[-1] if "] " in line else line)
    expect(client_step == "140", f"the client's step follows the host's to 140 ({client_step}); screenshot commander_client_after")

# After the Commander: the phase graph waits for an area (AreaAction a9864814) that holds the spot where the
# "Test" save starts (the hangar entrance), then near the host's flight unit (model 10010) the launch cutscene ev0200
# starts; after about 40 s of flight the player character asks for the next phase (+0x81a440) and the game
# loads 20_AB_buker_Aerial_battle (found 2026-10-19; walking the hangar floor reached the area only by chance).
FLIGHT_UNIT = "10010"


def hangar(expect):
    """TASKS 3.0.1 / 3.2.1 / 3.6.1: the host talks to the Commander, goes to the hangar and flies off (a natural
    phase change), the client stands still. Asserts that the host's phase changes and that the client's game follows
    through the story actions the host sends: the launch cutscene ev0200 and the same next phase (before 3.6.1 the
    client's game stayed at 16_AB_bunker_Order, step 130, observed 2026-10-19)."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_start = parse(result[2], "player")
    talk_to_commander(expect, result[2], result[3])

    # Back to where the save started: the area the graph waits for.
    bot.cmd(HOST, [f"goto {host_start.replace(',', ' ')} 0", "wait 3000"], quiet=True)
    expect(bot.goto_entity(HOST, FLIGHT_UNIT, 2.2, 0.6, 0, 90), "the host is next to the flight unit")

    host_phase, client_phase, client_cutscene, client_lines = "16_AB_bunker_Order", "", False, []
    for _ in range(30):
        host = bot.cmd(HOST, ["wait 3000", "state"], quiet=True)[-1]
        client = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
        host_phase, client_phase = parse(host, "phase"), parse(client, "phase")
        client_cutscene = client_cutscene or parse(client, "event") == "ev0200"
        line = f"host {host_phase} step {parse(host, 'step')} event {parse(host, 'event')} loading {parse(host, 'loading')} | "                f"client {parse(client, 'phase')} step {parse(client, 'step')} event {parse(client, 'event')} loading {parse(client, 'loading')}"
        if not client_lines or client_lines[-1] != line:
            client_lines.append(line)
            print("  " + line)
        if host_phase != "16_AB_bunker_Order" and parse(host, "loading") == "0" and client_phase != "16_AB_bunker_Order":
            break

    bot.cmd(HOST, ["shot hangar_host_after"])
    bot.cmd(CLIENT, ["shot hangar_client_after"])
    expect(host_phase == "20_AB_buker_Aerial_battle", f"the host's phase changed to 20_AB_buker_Aerial_battle ({host_phase})")
    expect(client_cutscene, "the client's game played the launch cutscene ev0200 too")
    expect(client_phase == "20_AB_buker_Aerial_battle", f"the client's phase followed to 20_AB_buker_Aerial_battle ({client_phase})")
    client_coop = bot.cmd(CLIENT, ["wait 6000", "coop"], quiet=True)[-1]
    print(f"  observed: client coop: {client_coop}")
    for line in log_text(CLIENT).splitlines():
        if any(k in line for k in ("[StoryAction] Finished", "skipped", "[Story] Phase")):
            print("    client log: " + line.split("] ", 2)[-1])


if __name__ == "__main__":
    main()
