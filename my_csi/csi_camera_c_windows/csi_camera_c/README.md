# CSI and laptop webcam recorder in C — v1.1.0

Acquisition recorder for your three ESP32-S3 boards and a laptop webcam.
It records raw CSI, H.264 video and timing information. It prepares the recordings
for automatic 2D pose labelling afterward; the pose detector is a separate next step.

Run the included **capture.exe in native Windows PowerShell**. Python, OpenCV,
Camo and FFmpeg are not needed for this recorder. The source files are plain C.

## Update for the shutdown timeout

This version addresses the unsafe shutdown sequence in v1.0.0. The earlier code
shut down the camera source before its recording thread had finished, and passed
camera-owned frame buffers directly to the encoder. In v1.1.0:

- Frame data is copied into encoder-owned buffers.
- Software H.264 encoding is used while validating the recording pipeline.
- The worker stops capturing and finalizes the MP4 while the camera source remains alive.
- The camera source is released only after the worker has exited.
- Frame and CSI CSV logs are flushed approximately once a second.
- Finalization progress and the camera worker phase are printed explicitly.

Replace the old `capture.exe` with the one from this ZIP. Confirm the version:

```powershell
.\capture.exe --version
```

It must print `1.1.0`. Retry in a **new** output folder, for example:

```powershell
.\capture.exe --camera 0 --camera-only --seconds 10 --countdown 3 --output camera_test_003
```

The progress update at approximately eight seconds is the last periodic update
before a ten-second test ends. That is expected. Successful closing messages must
include `MP4 finalized successfully.` and a final `Session complete` message.
If the worker still times out, the message now identifies its phase and metadata
is marked `shutdown_timeout`. Do not treat that session as a valid finished video.

The revised sequence has been compiled and inspected; it still requires a
hardware test on the HP HD Camera that exposed the first timeout.

## First: test only the laptop camera

Extract the ZIP, open PowerShell in this folder, and run:

```powershell
.\capture.exe --list-cameras
```

Use the index for the laptop's colour webcam. An infrared / Windows Hello camera
may also appear; select the colour camera.

```powershell
.\capture.exe --camera 0 --camera-only --seconds 10 --countdown 3 --output camera_test_001
```

It prints the selected camera, resolution and frame rate, warms up the camera,
counts down, then records ten seconds. Open `camera_test_001\video.mp4` afterward.
Check that your whole body, hands and feet are visible and the image is upright.
There is no live preview window in this first version. Close other programs using
the camera before testing.

Each output folder must have a **new name**. Use `camera_test_002` on the next run.
The parent folder must already exist. Existing folders are never overwritten.

## Then: record the camera and all three boards

Connect the boards and close the Arduino / ESP-IDF serial monitors and the old
Python recorder so they release the COM ports.

```powershell
.\capture.exe --camera 0 --ports COM3 COM5 COM7 --seconds 30 --countdown 10 --output session_001
```

The default baud rate is **460800**, matching your working recorder. It waits for
at least one valid CSI row on **every** requested port and a camera frame before
starting the countdown. Streams are drained during warmup and the countdown.

Your existing mapping was COM3=A, COM5=B, COM7=C. A different laptop or USB port
can have different COM numbers; use the ports that Windows actually assigns.

After this short test works, record five minutes:

```powershell
.\capture.exe --camera 0 --ports COM3 COM5 COM7 --seconds 300 --countdown 10 --output session_002
```

Press **Ctrl+C** to stop early and finalize the MP4. Wait for the final session
message before closing the terminal. A session with an error is marked `failed`.
A program crash can leave `warming_up` or `recording` metadata and an incomplete MP4.

## Camera modes

```powershell
.\capture.exe --camera 0 --list-modes
.\capture.exe --camera 0 --mode 12 --camera-only --seconds 10 --output camera_test_003
```

Replace `12` with a mode listed by your own camera. Without `--mode`, it selects a
native mode closest to 1280x720 at 30 fps. It converts pixels to NV12 for encoding
without intentionally generating or resampling camera frames. If encoding cannot
keep up, test a listed 640x480/30 mode. The selected size and frame rate are printed
and recorded in metadata. Keep the original video even if you later label fewer frames.

## Saved files

| File | Contents |
|---|---|
| `video.mp4` | Camera video, H.264, with video timestamps derived from source timestamps |
| `frames.csv` | One timing row for every frame successfully submitted to the encoder |
| `csi_COM3.csv`, etc. | Validated raw CSI rows plus PC receipt timestamps |
| `COM3_console.log`, etc. | Firmware status, boot messages and malformed CSI lines |
| `metadata.json` | Configuration, timing origin, frame/row totals and completion status |

CSI header:

```text
host_time_utc,host_qpc_ticks,host_elapsed_s,serial_port,type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,uptime_us,rssi,channel,len,first_word_invalid,data
```

The firmware fields are preserved, including the quoted `data` array of signed
I/Q bytes, with imaginary then real values. This version keeps that array in one
CSV cell. Your earlier Python recorder expanded it into `imag_00,real_00,...`
columns. No signal filtering, averaging or bin removal happens during capture.
Retain `len` and `first_word_invalid` for later preprocessing.

Frame header:

```text
frame_index,host_time_utc,host_qpc_ticks,host_elapsed_s,source_pts_100ns,video_pts_100ns,duration_100ns
```

## What the timestamps mean

- **host_qpc_ticks:** Windows QueryPerformanceCounter read on the recording PC.
  All recording threads use the same clock and `metadata.json` gives its frequency.
- **host_elapsed_s:** `(host_qpc_ticks - start_qpc_ticks) / qpc_frequency_hz`.
  This is the shared session timeline for camera and CSI **receipt**, independent
  of adjustments to the PC's wall clock.
- **host_time_utc:** UTC read alongside that receipt timestamp, for human-readable
  timing and compatibility with your existing recordings.
- **source_pts_100ns:** Timestamp provided through Media Foundation for a camera
  sample. Its clock origin is not assumed to be the same as QPC or the ESP32 clocks.
- **video_pts_100ns:** Source timestamp minus the first saved source timestamp.
  The MP4 is written using this timeline. It starts at zero for the first saved frame.
- **radio_time_us / uptime_us:** The board-supplied firmware fields. They are
  retained, not assumed to be synchronized across boards or with the camera.

**Video time zero is not necessarily session time zero.** The first saved video
frame might arrive, for example, 0.020 seconds after the shared session start.
Use its row in `frames.csv` to map the video frame to the CSI receipt timeline.
For offline joins, use each video's presentation timestamp and the matching
`video_pts_100ns`, rather than guessing time from `frame_index / nominal_fps`.
Check the decoded video frame count against `frames.csv` in the first hardware test.

The host timestamps are taken immediately after `ReadSample` / `ReadFile` returns.
All complete serial lines obtained from the same chunk share its receipt timestamp.
They are **not exposure or RF capture times**. Camera buffering, USB transmission,
serial buffering and encoder backpressure can cause different delays. A shared
clock and countdown do not establish hardware synchronization. Metadata therefore
explicitly reports `uncalibrated_host_receipt_timestamps`.

Before training on continuous movement, measure the remaining timing offset and
its variability. Distinct repeated movements at the start and end can provide an
approximate visual/CSI alignment check. A visible hardware-timed marker is a stronger
calibration approach. For an initial held-pose test, leave transitions out and use
only the stable middle portions of each hold.

This program reads packets that the firmware already sends. Switching to C does
not change the boards' sampling rate or establish a 20,000-packets/second rate.

## Build from source

The prebuilt executable targets **64-bit Windows**. No extra runtime DLLs are
bundled. It uses Win32 serial APIs, Windows Media Foundation and QPC. The camera
and all serial streams run on separate native threads.

### Microsoft C compiler

With Visual Studio Build Tools and the Windows SDK installed:

```powershell
.\build.cmd
```

The installer calls the workload "Desktop development with C++"; `build.cmd`
uses `/TC /std:c11`, so the source is compiled as **C**.

### Native MinGW-w64 GCC

```text
gcc -std=c11 -O2 -Wall -Wextra capture_windows.c csi_parse.c -o capture.exe -lmfplat -lmfreadwrite -lmfuuid -lmf -lole32
```

### Cross-compile with Zig

```text
zig cc -target x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror capture_windows.c csi_parse.c -o capture.exe -lmfplat -lmfreadwrite -lmfuuid -lmf -lole32
```

A Linux/WSL `gcc` command without a Windows target produces a Linux program and
cannot directly use these Win32 APIs. Start with the prebuilt `.exe` in PowerShell.

## Portable parser checks

`test_csi_parse.c` checks the real firmware CSV layout, signed I/Q boundaries,
full 128-byte rows, truncated/malformed rows, oversized records, identifiers,
timestamp bounds and elapsed-time arithmetic. It runs without Windows:

```text
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror csi_parse.c test_csi_parse.c -lm -o test_csi_parse
./test_csi_parse
```

The hardware acquisition test must be run on your own laptop and boards.

## API references

- [Windows camera capture](https://learn.microsoft.com/en-us/windows/win32/medfound/audio-video-capture-in-media-foundation)
- [Media Foundation Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader)
- [Media Foundation Sink Writer](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video)
- [Windows high-resolution timestamps](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)
