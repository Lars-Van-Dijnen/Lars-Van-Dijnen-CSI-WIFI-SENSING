# Two ESP32-S3 boards: CSI in both directions

Use **ESP-IDF 6.1** and two ESP32-S3 boards. Both boards transmit and receive.
Board A sends a request; Board B records its CSI and sends a reply; Board A
records the reply's CSI. The cycle repeats automatically.

The default is approximately **10 CSI measurements per second on each board**
(one request/reply cycle every 100 ms). Packet loss and scheduling can reduce
that rate. This is a direct ESP-NOW link on 2.4 GHz channel 6: no router,
Wi-Fi password, IP address, or ping is needed. No wires connect the two boards.
Each board needs power; USB also lets you read that board's measurements.

## 1. Extract and find the COM ports

Extract the ZIP. Inside `esp32s3_csi_duplex` are two ready-to-build projects:

| Folder | Flash onto | Behavior |
|---|---|---|
| `board_a` | First ESP32-S3 | Sends requests and measures received replies |
| `board_b` | Second ESP32-S3 | Measures received requests and sends replies |

Open the **ESP-IDF terminal**, as you did for your working project. Plug in
one board at a time and run:

```powershell
python -m serial.tools.list_ports
```

The port that appears is that board's COM port. You can also look under
Windows Device Manager > Ports (COM & LPT). Label the boards A and B.

**COM7 and COM8 below are examples. Replace them with your actual ports.**
Do not flash both projects to the same board.

## 2. Build and flash Board A

In the ESP-IDF terminal, change directory to the extracted `board_a` folder.
For example, if you extracted the project directly inside Downloads:

```powershell
cd "$env:USERPROFILE\Downloads\esp32s3_csi_duplex\board_a"
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

You should see `READY board=A` and `CSI enabled`. Until Board B is running,
failed sends are expected. **Ctrl+]** exits the monitor; the board keeps running.

## 3. Build and flash Board B

Keep A powered. Open a second ESP-IDF terminal and use the `board_b` folder:

```powershell
cd "$env:USERPROFILE\Downloads\esp32s3_csi_duplex\board_b"
idf.py set-target esp32s3
idf.py build
idf.py -p COM8 flash monitor
```

Now B should print `READY board=B` followed by CSI rows. To see A's stream,
run this in a terminal inside `board_a`:

```powershell
idf.py -p COM7 monitor
```

Use one monitor per COM port. Both boards must stay powered. They can start in
either order, and A keeps trying if B is temporarily absent. Begin with the
boards about one metre apart. Unplugging power stops a board's program.

## 4. Read the output

The header is:

```text
type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,uptime_us,rssi,channel,len,first_word_invalid,data
```

On B, a row starts with `CSI,B,A,REQUEST,...`. On A, it starts with
`CSI,A,B,REPLY,...`. **Each row is one received packet's CSI measurement.**

| Field | Meaning |
|---|---|
| `rx_board`, `tx_board` | Receiver and transmitter, A or B |
| `kind` | `REQUEST`, `REPLY`, or `OTHER` if the frame could not be labelled |
| `session`, `sequence` | Match a request on B with its reply on A; a new A boot starts a new session |
| `rx_boot` | Identifies the receiving board's current boot |
| `radio_time_us` | Local Wi-Fi receive timestamp in microseconds; wraps about every 71.6 minutes |
| `uptime_us` | Local 64-bit microsecond timestamp taken in the CSI callback |
| `rssi` | Received signal strength in dBm |
| `channel` | Wi-Fi channel, default 6 |
| `len` | Number of signed bytes in `data`, normally 128 with this configuration |
| `first_word_invalid` | If 1, discard the first four bytes when processing CSI |
| `data` | Quoted array of signed bytes: imaginary, real, imaginary, real, ... |

128 bytes occupy 64 complex slots; these include unused/null subcarriers.
For a valid pair `[imaginary, real]`, amplitude is
`sqrt(real*real + imaginary*imaginary)`. Data values along one row describe
frequency subcarriers, not 128 successive times.

The clocks on the two boards are independent. Match directions by session
and sequence, not by directly subtracting timestamps from different boards.
The request and reply also occur at different times. Their CSI values can
differ because of noise, motion, and radio hardware/gain/phase differences.
This example does not calibrate those differences or synchronize the clocks.

`OTHER` rows retain the raw CSI but have empty session/sequence fields; do not
assume a cycle number for them. A lost request or reply may leave an unpaired
measurement. Driver retransmissions can also yield repeated measurements.

Lines beginning `# STATUS` report cumulative counters every two seconds.
For analysis, retain only lines beginning `CSI,` and save the header once.
`csi_dropped` indicates that the serial output task could not keep up;
`unlabelled` counts CSI frames without a decoded probe identity.

## Settings and source code

Both project folders contain `main/esp32s3_csi_duplex.c`. Edit the appropriate
copy, rebuild, and reflash that board. The top-level copy of the C file is
also supplied separately for reading or use in your own project. By itself,
the source defaults to Board A; defining `CSI_NODE_ID` as `2` above the settings
block selects B. Your own project must enable Wi-Fi CSI and include the same
component dependencies as the supplied `main/CMakeLists.txt`.

Run `idf.py menuconfig` inside a project and open **CSI pair settings** to
change its identity, channel, cycle period, or reply timing. Change the channel
on both boards. The ZIP already enables Wi-Fi CSI and selects different
identities for A and B, so no setup changes are needed for the first run.

The source sets fixed locally administered demo MAC addresses:
`02:43:53:49:00:01` (A) and `02:43:53:49:00:02` (B). For multiple independent
pairs, change the shared MAC prefix on both members of each pair.

ESP-NOW packets use HT20 MCS0 so they can produce CSI. Callbacks copy data
into queues; a separate task prints it, keeping serial work out of the Wi-Fi
task. The radio uses LLTF-only CSI and disables power saving. Probes contain
only public demo identifiers and counters and use no ESP-NOW encryption.

The default UART monitor speed is 115200. Faster sample rates may need faster
console output. For a native USB connection, choose **Component config >
ESP-STDIO > Channel for console output > USB Serial/JTAG Controller** in
`menuconfig` if necessary, then rebuild and reflash. Use the USB connection
that matches your board and its console setting.

## If there are no CSI rows

- Check the startup lines: one board must say A, the other B, with matching channels.
- Keep both powered and check `tx_ok` and `messages_rx` in the status lines.
- `tx_fail` rising usually means the peer is absent, on a different channel,
  or running the wrong firmware. Check the flashed project and move them closer.
- If packets arrive but `csi_rx` stays zero, check that CSI is enabled and both
  boards run this source with its HT20 rate configuration. The ordinary
  ESP-NOW default rate is not suitable for this CSI example.
- If there is no `READY` output, check COM port, cable, console selection,
  monitor baud rate, and whether another program has the serial port open.
- This is a new project. Build in `board_a` or `board_b`, rather than reusing
  your previous `my_csi` build directory.

## Validation and references

Host protocol tests are in `tests/test_protocol.c`. They exercise encoding,
direction checks, truncated frames, malformed vendor elements, and random
invalid inputs. To run on a machine with GCC:

```sh
gcc -std=c11 -Wall -Wextra -Werror -pedantic tests/test_protocol.c -o test_protocol
./test_protocol
```

This project needs testing on your actual two boards; radio performance and
console configuration depend on the hardware. See `VALIDATION.txt` for the
checks performed before packaging.

Official references:

- [ESP-IDF 6.1 ESP-NOW API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_now.html)
- [ESP32-S3 CSI guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-vendor-features.html)
- [Espressif CSI receiver example](https://github.com/espressif/esp-csi/blob/master/examples/get-started/csi_recv/main/app_main.c)
