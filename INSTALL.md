# Install the Win64 release

1. Extract the release ZIP into your Forza Horizon 5 game folder.
2. Confirm the layout contains `version.dll` and `spotify-radio/ui/dist/index.html`.
3. Start Forza Horizon 5.
4. Open `http://127.0.0.1:8103` and set the source to QQ Music.
5. Install the routing helper:

```powershell
python -m pip install winappaudiorouter pycaw
```

6. While FH5 is running, route QQ Music to Steam Streaming Speakers:

```powershell
@'
import winappaudiorouter as router
print(router.set_app_output_device(
    process_name="QQMusic.exe", device="Steam Streaming Speakers"))
'@ | python -
```

Run `spotify-radio/qqmusic_router.py` in the background if you want QQ Music to
switch automatically between the virtual device while FH5 runs and your system
default device after FH5 exits.

If Windows or your browser warns about `version.dll`, verify the download came
from this repository's GitHub Release page. DLL proxy mods are commonly flagged
by reputation-based scanners.
