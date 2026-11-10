# Three ESP32-S3 boards alternating transmitter and receiver

This is a separate three-board version of the working two-board CSI example.
It targets **ESP-IDF 6.1**. **Flash the new firmware onto all three boards**:
the earlier two-board firmware cannot participate in this protocol.

Each board takes a turn broadcasting one measurement probe. The other two
boards receive that same probe and measure their own CSI:

| Turn | Measurement transmitter | CSI receivers |
|---|---|---|
| 1 | A | B and C |
| 2 | B | A and C |
| 3 | C | A and B |

Then A starts the next round. **A also coordinates the turn order**, by sending
short control grants to B and C. Those extra control packets are excluded from
the printed CSI data. A, B, and C all transmit and receive measurement probes.

No router, Wi-Fi password, IP address, ping, or wires between boards are needed.
All three radios use 2.4 GHz channel 6, 20 MHz bandwidth, and HT20 MCS0 probes.
Use a USB connection to power/read each board, or a suitable separate power
supply if you only need to monitor the other boards.

## Timing: five times more rounds

The new default is **20 ms per transmitting turn**, so a complete A+B+C round
targets **60 ms**, compared with 300 ms in the uploaded version. The reply
delay is now **2 ms** and UART console output is **460800 baud**. The CSI
queue holds 64 records instead of 24 to absorb short output stalls.

| Quantity | Approximate rate, all three boards running |
|---|---|
| Complete A+B+C rounds | 16.67 per second |
| A broadcasts a measurement probe | 16.67 per second |
| B broadcasts a measurement probe | 16.67 per second |
| C broadcasts a measurement probe | 16.67 per second |
| Each directed link, such as A to B | 16.67 CSI measurements per second |
| Each receiver, combining its two incoming links | 33.33 CSI rows per second |
| Across all three receiver streams | 100 CSI rows per second |

These are targets, not guaranteed rates. Radio delays, packet loss and task
scheduling can reduce them. Turns are extended after late radio completions.
The faster UART setting carries the extra raw CSI output; open the recorder
or a monitor at **460800 baud**. It is a software turn-taking schedule, not hard
real-time TDMA or clock synchronization. The timing guard is retained: a late
send extends the turn rather than deliberately overlapping the next one.

## 1. Identify the boards and COM ports

Extract the ZIP **into a fresh folder** and open `esp32s3_csi_three_board`. It has
three independent projects: `board_a`, `board_b`, and `board_c`.

Existing generated `sdkconfig` files override `sdkconfig.defaults`. If reusing
an old project directory, set the turn to 20 ms, reply delay to 2 ms, FreeRTOS
tick frequency to 1000 Hz, and UART console baud rate to 460800 using
`idf.py menuconfig`, on **each** board project. Extracting into a fresh folder
avoids carrying over the old 100 ms / 115200-baud configuration.

Use your **ESP-IDF terminal**. Connect the boards one at a time and run:

```powershell
python -m serial.tools.list_ports
```

The new COM port belongs to the board you just connected. You can also use
Windows Device Manager > Ports (COM & LPT). Label each physical board A/B/C.

**COM7, COM8, and COM9 below are examples. Replace them with your actual ports.**
Make sure each project is flashed to a different physical board.

## 2. Flash Board A

Start with a terminal in the extracted `esp32s3_csi_three_board` folder:

```powershell
cd .\board_a
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 -b 460800 flash monitor
```

Look for `READY board=A`. Until B and C are running, failed grant sends are
expected. Press **Ctrl+]** to exit the monitor, then keep A powered.

## 3. Flash Board B

From the same terminal, currently inside `board_a`:

```powershell
cd ..\board_b
idf.py set-target esp32s3
idf.py build
idf.py -p COM8 -b 460800 flash monitor
```

Look for `READY board=B`. Press Ctrl+] and keep B powered.

## 4. Flash Board C

From the same terminal, currently inside `board_b`:

```powershell
cd ..\board_c
idf.py set-target esp32s3
idf.py build
idf.py -p COM9 -b 460800 flash monitor
```

Look for `READY board=C` and CSI rows. Start with boards about one metre apart.
They can be powered in any order, but **A must stay powered to coordinate**.
If B or C is absent, A keeps moving through the turn order and the remaining
boards can continue measuring each other. If A is absent, B/C wait for grants.

To view another board, open another ESP-IDF terminal inside its project folder
and run `idf.py -p COM7 -b 460800 monitor`, using that board's actual COM port. Use one
monitor per port. Exiting a monitor does not stop the firmware; removing power
does. To collect all six directions, read all three receiver streams.

## 5. Record an actual CSV file on your computer

The firmware sends CSI over USB/serial. The included **`capture_csv.py` writes
the CSV to your computer**; no SD card or ESP32 flash logging is required.
To collect all six directed links, connect all three boards' console ports.
You can also record one or two ports, which saves only those receivers' data.

Exit every `idf.py monitor` with **Ctrl+]** before recording. A serial port
can only be opened by one program at a time. Leave the boards powered.

Open a terminal in the top-level `esp32s3_csi_three_board` folder, the folder
containing `capture_csv.py`, rather than inside a `board_*` folder. For example,
if the previous terminal is still in `board_c`, first run `cd ..`.
Python 3.10 or newer is required.

```powershell
python -m pip install -r requirements.txt
python capture_csv.py --list-ports
python capture_csv.py --ports COM7 COM8 COM9 --output recordings\csi.csv
```

Replace the example COM ports with your ports. Press **Ctrl+C** to stop and
save. To stop automatically after 60 seconds, add `--seconds 60`. The default
baud rate is 460800. On Linux/macOS, use your actual `/dev/...` port names.

For your three activity classes, make separate recordings:

```powershell
python capture_csv.py --ports COM7 COM8 COM9 --output recordings\absent.csv --label absent --seconds 60
python capture_csv.py --ports COM7 COM8 COM9 --output recordings\still.csv --label present_still --seconds 60
python capture_csv.py --ports COM7 COM8 COM9 --output recordings\moving.csv --label present_moving --seconds 60
```

Set the room/activity condition before starting each recording and keep it
consistent during that recording. Without `--label`, rows are `unlabeled`.
Existing output files are never overwritten; use a new filename for each run.
If `--output` is omitted, the recorder chooses a timestamped name in `recordings`.

Each saved CSV row contains:

- `host_time_utc`, `label`, and `serial_port`.
- The firmware metadata from `type` through `first_word_invalid`, including
  the transmitter, receiver, session, sequence, local timestamps and RSSI.
- **128 numeric columns**: `imag_00,real_00,...,imag_63,real_63`, in the original
  raw-byte order. The serial `data` list is expanded into these columns.

All raw bins are retained. The active pair indices are `1..26` and `38..63`;
if `first_word_invalid` is 1, also exclude pairs 0 and 1 during analysis.
No normalization, phase correction, amplitude conversion or classification is
applied. A shorter even-length record retains its `len`, with empty cells for
missing pairs. The host timestamp records serial arrival, not synchronized
radio reception. Use session/sequence/transmitter to match probes across boards.
Rows are written in arrival order, which need not be radio transmission order.

The recorder filters out boot messages, headers and status lines. It counts
malformed CSI rows, prints progress every two seconds, flushes the file every
second, and syncs it when stopped. A USB disconnect stops the capture, preserves
queued rows and reports an error. A hard power loss may lose buffered data.

Add `--show-status` to print the firmware's counters while recording. The last
status from each port is also printed on exit. Check `csi_dropped` and
`turn_overruns`; growing counters mean output or radio timing is struggling.
Packet losses can still occur without malformed rows: inspect missing
session/sequence/tx/rx combinations in the CSV to measure capture completeness.
To measure the actual link rate, divide each link's row count by its recording
duration, or use its receiver's local `uptime_us` deltas.

## Reading firmware serial rows

The CSV header keeps the same field order as the two-board version:

```text
type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,uptime_us,rssi,channel,len,first_word_invalid,data
```

The message kind is now `PROBE`. Depending on which board's monitor is open,
you should see these prefixes (the dots below indicate the remaining fields):

| Monitor | Rows from first peer | Rows from second peer |
|---|---|---|
| A | `CSI,A,B,PROBE,...` | `CSI,A,C,PROBE,...` |
| B | `CSI,B,A,PROBE,...` | `CSI,B,C,PROBE,...` |
| C | `CSI,C,A,PROBE,...` | `CSI,C,B,PROBE,...` |

Each row describes **one received measurement probe**. Board labels identify
the receiver and transmitter. These are six directed links along three
physical board-to-board paths; reverse directions are not independent spatial
viewpoints.

`session` identifies A's current coordination session. `sequence` counts a
complete A+B+C round, so all six expected rows in a successful round share
that session and sequence. To match the same broadcast on its two receivers,
also match `tx_board`. Missing packets leave incomplete groups. The receiver's
`rx_boot` identifies restarts of that receiver.

The clocks on the three boards are independent. `radio_time_us` is a local
32-bit receive timestamp, wrapping about every 71.6 minutes. `uptime_us` is the
local 64-bit callback timestamp. Matching session/sequence aligns the probes;
it does not synchronize the boards' clocks.

`data` contains 128 signed bytes with the LLTF-only setting: 64
`[imaginary, real]` pairs, of which 52 are active subcarrier positions. Count
pairs from zero: active indices are `1..26` and `38..63`. Pair 0 is DC;
27..37 are guard bins. If `first_word_invalid` is 1, the first four bytes
(first two pairs) must additionally be excluded from analysis. `rssi` is dBm.
This is raw CSI, without gain/phase calibration or activity classification.

## Status counters and troubleshooting

`# STATUS` lines appear every two seconds. The supplied recorder filters these
out of the CSV and reports the last status on exit. The fields are cumulative:

- `probes_tx`: broadcast probes completed by this board's radio.
- `probes_rx`: peer probes received by ESP-NOW's application callback.
- `csi_A`, `csi_B`, `csi_C`: captured CSI records from each transmitter;
  a board's own counter stays zero.
- `tx_ok`, `tx_fail`: send callback results for probes and grants combined.
  **Broadcast send success does not prove either receiver got the packet.**
- `grants_rx`: grants received by B/C. A issues grants, so its counter is zero.
- `grants_rejected`: duplicate, older, or late grants rejected before transmission.
- `csi_dropped`, `grants_dropped`: full software queues.
- `unlabelled`: matching broadcast CSI whose probe payload could not be decoded;
  these records are excluded so control/unidentified frames cannot inflate rates.
- `oversize`: CSI longer than the selected 128-byte LLTF format.
- `rounds`, `turn_overruns`: A's round count and turns extended for radio delays.

If there are no rows, check all three `READY` identities and their channels,
then check that A is powered and B/C's grant counters increase. A high
`tx_fail` count can indicate an absent peer, wrong firmware or channel. If
`probes_rx` rises without CSI, check CSI configuration and report the status
including `unlabelled`. Packet and grant losses are visible as missing rows;
the next rounds continue rather than waiting for a token from a missing peer.

## Changing settings and reading the code

The application's source is `main/esp32s3_csi_three_board.c` inside each board
project. The top-level C file is the same source supplied separately. The
preconfigured projects choose identities through `sdkconfig.defaults`; a
standalone copy defaults to A, or can define `CSI_NODE_ID` as 2 or 3 above the
settings block.

`idf.py menuconfig` > **CSI three-board settings** changes board identity,
channel, turn duration and reply delay. Match channel and timing settings on
all three boards. The packaged configuration is 20 ms per turn and a 2 ms
reply delay, targeting a 60 ms full round. For example, a 33 ms turn targets
roughly a 100 ms full round, or about 10 Hz per directed link. A 100 ms turn
restores the earlier conservative 300 ms full round. Faster rates require
sufficient serial/USB throughput and radio testing; 20 ms is the minimum
supported turn in this package.

The fixed demo MAC addresses are `02:43:53:49:03:01`, `...:02`, and `...:03`.
Change the shared prefix on all three if running multiple independent groups.
Probes/grants contain public demo identifiers and counters, without ESP-NOW
encryption or authentication.

The default UART console is **460800 baud**; the monitor and recorder must
match it. For native USB, if necessary choose
**Component config > ESP-STDIO > Channel for console output > USB Serial/JTAG
Controller** in menuconfig, then rebuild and reflash. Native USB Serial/JTAG
does not depend on the physical UART baud divider. Use your board's matching
USB connection/console setting. ROM boot messages on a UART may still use
115200 baud; the recorder ignores them.

For a code walkthrough, begin at `app_main()`: create queues, start the radio,
then start the exchange and output tasks. Driver callbacks pass data to queues.
B/C act only on fresh grants from A and discard duplicate/older rounds.

## Verification

Host tests are in `tests/test_three_board.c`. They cover packet parsing,
malformed/truncated input, board MAC identities, stale/duplicate grants,
sequence wrap, nominal and extended turn timing, the six directed links,
loss/missing-peer protocol scenarios, CSV bounds and the 20 ms turn deadline.
`tests/test_capture_csv.py` compiles that same formatter and feeds its real
output into the recorder, checking all six links, I/Q order, three concurrent
ports, split serial reads, labels, malformed rows, line bounds, disconnects,
Ctrl+C saving, and refusing to overwrite a file. These protocol-level
simulations do not replace RF or on-board testing.

```sh
gcc -std=c11 -Wall -Wextra -Werror -pedantic tests/test_three_board.c -o test_three_board
./test_three_board
python -m unittest discover -s tests -p "test_*.py" -v
```

See `VALIDATION.txt` for the current test results and build limitations. No
physical boards were available to verify radio reception, CSI and actual
timing. This edited version has been host-tested; rebuild all three ESP-IDF
projects locally before flashing.

References: [ESP-IDF 6.1 ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_now.html)
and [ESP32-S3 CSI](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-vendor-features.html).
