import subprocess
import time
import os
from pathlib import Path

import winappaudiorouter as router

from pycaw.pycaw import AudioUtilities


GAME_PROCESS = "ForzaHorizon5.exe"
QQ_PROCESS = "QQMusic.exe"
VIRTUAL_DEVICE = "Steam Streaming Speakers"
LOG_PATH = Path(
    os.getenv(
        "FH5_QQMUSIC_ROUTER_LOG",
        Path.home() / "AppData" / "Local" / "FH5-QQMusic-Radio" / "router.log",
    )
)


def log(message: str) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("a", encoding="utf-8") as output:
        output.write(f"[{timestamp}] {message}\n")


def game_is_running() -> bool:
    result = subprocess.run(
        ["tasklist", "/FI", f"IMAGENAME eq {GAME_PROCESS}", "/FO", "CSV", "/NH"],
        capture_output=True,
        text=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
        check=False,
    )
    return GAME_PROCESS.lower() in result.stdout.lower()


def apply_route(running: bool) -> None:
    if running:
        set_qq_volume_to_100_percent()
        result = router.set_app_output_device(
            process_name=QQ_PROCESS, device=VIRTUAL_DEVICE
        )
        log(f"FH5 running; QQ Music -> {VIRTUAL_DEVICE}: {result}")
    else:
        result = router.clear_app_output_device(process_name=QQ_PROCESS)
        set_qq_volume_to_100_percent()
        log(f"FH5 exited; QQ Music -> default device: {result}")


def set_qq_volume_to_100_percent() -> None:
    for session in AudioUtilities.GetAllSessions():
        if session.Process and session.Process.name().lower() == QQ_PROCESS.lower():
            session.SimpleAudioVolume.SetMasterVolume(1.0, None)


def main() -> None:
    last_state = None
    log("router started")
    while True:
        running = game_is_running()
        if running != last_state:
            apply_route(running)
            last_state = running
        time.sleep(2)


if __name__ == "__main__":
    main()
