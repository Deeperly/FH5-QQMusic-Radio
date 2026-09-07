import os
import time
import traceback
from pathlib import Path

import psutil
import winappaudiorouter as router

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
    return any(
        (process.info.get("name") or "").lower() == GAME_PROCESS.lower()
        for process in psutil.process_iter(["name"])
    )


def apply_route(running: bool) -> None:
    if running:
        result = router.set_app_output_device(
            process_name=QQ_PROCESS, device=VIRTUAL_DEVICE
        )
        log(f"FH5 running; QQ Music -> {VIRTUAL_DEVICE}: {result}")
    else:
        result = router.clear_app_output_device(process_name=QQ_PROCESS)
        log(f"FH5 exited; QQ Music -> default device: {result}")


def main() -> None:
    last_state = None
    log("router started")
    while True:
        try:
            running = game_is_running()
            if running != last_state:
                apply_route(running)
                last_state = running
        except Exception:
            log(f"router error:\n{traceback.format_exc()}")
            last_state = None
            time.sleep(5)
            continue
        time.sleep(2)


if __name__ == "__main__":
    main()
