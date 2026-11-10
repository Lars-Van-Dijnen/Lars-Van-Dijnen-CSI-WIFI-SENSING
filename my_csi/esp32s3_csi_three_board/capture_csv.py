#!/usr/bin/env python3
"""Record raw CSI from one or more ESP32-S3 serial ports to one CSV file."""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import ExitStack
import csv
from dataclasses import dataclass, field
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time


BAUDRATE = 460800
MAX_LINE_BYTES = 4096
READ_CHUNK_BYTES = 8192
RAW_FIELDS = (
    "type", "rx_board", "tx_board", "kind", "session", "sequence", "rx_boot",
    "radio_time_us", "uptime_us", "rssi", "channel", "len",
    "first_word_invalid", "data",
)
IQ_FIELDS = tuple(name for index in range(64)
                  for name in (f"imag_{index:02d}", f"real_{index:02d}"))
CSV_FIELDS = ("host_time_utc", "label", "serial_port") + RAW_FIELDS[:-1] + IQ_FIELDS
LABELS = ("unlabeled", "absent", "present_still", "present_moving")


def parse_csi_line(line: str) -> list | None:
    """Ignore console chatter; validate CSI and expand its raw I/Q bytes.

    Raise ValueError for a damaged CSI row. No scaling, bin removal or signal
    processing is performed. Shorter even-length records retain their length
    and have empty cells for the missing pairs.
    """
    if not line.startswith("CSI,"):
        return None
    try:
        fields = next(csv.reader([line], strict=True))
        if len(fields) != len(RAW_FIELDS):
            raise ValueError("wrong field count")
        if (fields[1] not in ("A", "B", "C") or
                fields[2] not in ("A", "B", "C") or
                fields[1] == fields[2] or fields[3] != "PROBE"):
            raise ValueError("invalid board or message kind")
        for index in (4, 6):
            if not 0 <= int(fields[index], 16) <= 0xFFFFFFFF:
                raise ValueError("invalid session/boot ID")
        for index in (5, 7):
            if not 0 <= int(fields[index]) <= 0xFFFFFFFF:
                raise ValueError("invalid sequence/timestamp")
        if not 0 <= int(fields[8]) <= 0x7FFFFFFFFFFFFFFF:
            raise ValueError("invalid uptime")
        if not -128 <= int(fields[9]) <= 127 or not 1 <= int(fields[10]) <= 14:
            raise ValueError("invalid RSSI/channel")
        length = int(fields[11])
        if not 2 <= length <= 128 or length % 2 or fields[12] not in ("0", "1"):
            raise ValueError("invalid CSI length/flag")
        samples = json.loads(fields[13])
        if (not isinstance(samples, list) or len(samples) != length or
                any(type(value) is not int or not -128 <= value <= 127
                    for value in samples)):
            raise ValueError("invalid CSI bytes")
    except (csv.Error, json.JSONDecodeError, TypeError, StopIteration) as error:
        raise ValueError("malformed CSI row") from error
    return fields[:-1] + samples + [""] * (128 - length)


@dataclass
class CaptureResult:
    rows: Counter = field(default_factory=Counter)
    malformed: Counter = field(default_factory=Counter)
    last_status: dict = field(default_factory=dict)
    errors: list = field(default_factory=list)
    elapsed_seconds: float = 0.0
    stop_reason: str = ""

    @property
    def total(self) -> int:
        return sum(self.rows.values())


def _read_port(port, stream, events, stop):
    """Read available bytes in chunks and split complete console lines.

    pyserial 3.5 read_until() performs read(1) repeatedly. Avoid that per-byte
    driver overhead across three busy Windows serial ports. Preserve partial
    lines across reads and process every complete line in a chunk.
    """
    pending = bytearray()
    discarding = False

    def emit(kind, payload):
        event = (kind, port, datetime.now(timezone.utc).isoformat(
            timespec="microseconds"), payload)
        while not stop.is_set():
            try:
                events.put(event, timeout=0.2)
                return
            except queue.Full:
                continue

    try:
        while not stop.is_set():
            waiting = stream.in_waiting
            part = stream.read(min(waiting, READ_CHUNK_BYTES) if waiting else 1)
            if not part:
                continue
            pending.extend(part)
            while not stop.is_set():
                end = pending.find(b"\n")
                if end < 0:
                    if len(pending) > MAX_LINE_BYTES:
                        if not discarding:
                            emit("malformed", None)
                        pending.clear()
                        discarding = True
                    break
                raw = bytes(pending[:end + 1])
                del pending[:end + 1]
                if discarding:
                    discarding = False
                    continue
                if len(raw) > MAX_LINE_BYTES:
                    emit("malformed", None)
                    continue
                try:
                    line = raw.decode("ascii").strip()
                except UnicodeDecodeError:
                    if raw.startswith(b"CSI,"):
                        emit("malformed", None)
                    continue  # ROM boot output can use a different UART speed.
                if line.startswith("# STATUS"):
                    emit("status", line)
                    continue
                try:
                    row = parse_csi_line(line)
                except ValueError:
                    emit("malformed", None)
                    continue
                if row is not None:
                    emit("row", row)
    except Exception as error:
        if not stop.is_set():
            emit("error", f"{port}: {error}")


def capture(streams, output: Path, label="unlabeled", seconds=0.0,
            show_status=False, report=print) -> CaptureResult:
    """Capture already-open serial streams. All CSV writes use one thread.

    Streams must have a short read timeout (the CLI uses 0.2 seconds).
    Output is exclusively created, flushed each second, and synced on exit.
    """
    result = CaptureResult()
    events = queue.Queue(maxsize=2048)
    stop = threading.Event()
    threads = [threading.Thread(target=_read_port, args=(port, stream, events, stop),
                                name=f"csi-{port}", daemon=True)
               for port, stream in streams]
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(CSV_FIELDS)
        handle.flush()

        def save(event):
            kind, port, timestamp, payload = event
            if kind == "row":
                writer.writerow([timestamp, label, port] + payload)
                result.rows[port] += 1
            elif kind == "malformed":
                result.malformed[port] += 1
            elif kind == "status":
                result.last_status[port] = payload
                if show_status:
                    report(f"{port}: {payload}")
            elif kind == "error":
                result.errors.append(payload)

        started = time.monotonic()
        next_flush, next_report = started + 1, started + 2
        deadline = started + seconds if seconds else float("inf")
        try:
            for thread in threads:
                thread.start()
            while time.monotonic() < deadline and not result.errors:
                try:
                    save(events.get(timeout=min(0.2, max(0, deadline - time.monotonic()))))
                except queue.Empty:
                    pass
                now = time.monotonic()
                if now >= next_flush:
                    handle.flush()
                    next_flush = now + 1
                if now >= next_report:
                    counts = ", ".join(f"{port}: {result.rows[port]}"
                                       for port, _ in streams)
                    report(f"{result.total} rows saved ({counts}); "
                           f"malformed: {sum(result.malformed.values())}")
                    next_report = now + 2
        except KeyboardInterrupt:
            result.stop_reason = "Ctrl+C"
            report("Stopping; saving buffered rows...")
        finally:
            result.elapsed_seconds = time.monotonic() - started
            stop.set()
            for thread in threads:
                if thread.ident is not None:
                    thread.join(timeout=1)
            while True:
                try:
                    save(events.get_nowait())
                except queue.Empty:
                    break
            handle.flush()
            os.fsync(handle.fileno())
    if result.errors:
        result.stop_reason = "serial error"
    elif not result.stop_reason:
        result.stop_reason = "requested duration reached"
    return result


def nonnegative_seconds(value):
    number = float(value)
    if not 0 <= number < float("inf"):
        raise argparse.ArgumentTypeError("seconds must be finite and >= 0")
    return number


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs="+", metavar="PORT",
                        help="receiver ports, e.g. COM7 COM8 COM9")
    parser.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    parser.add_argument("--output", type=Path,
                        default=Path("recordings") / datetime.now().strftime("csi_%Y%m%d_%H%M%S.csv"))
    parser.add_argument("--label", choices=LABELS, default="unlabeled")
    parser.add_argument("--seconds", type=nonnegative_seconds, default=0,
                        help="recording duration; 0 (default) records until Ctrl+C")
    parser.add_argument("--baud", type=int, default=BAUDRATE,
                        help="UART baud rate; must match firmware (default: 460800)")
    parser.add_argument("--show-status", action="store_true", help="print live firmware status counters")
    args = parser.parse_args(argv)
    if not args.list_ports and not args.ports:
        parser.error("provide --ports COM7 COM8 COM9 or --list-ports")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.ports and len(set(args.ports)) != len(args.ports):
        parser.error("each port must be listed once")
    if not args.list_ports and args.output.exists():
        parser.error(f"output already exists: {args.output}; choose another filename")
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("Install the recorder dependency: python -m pip install -r requirements.txt",
              file=sys.stderr)
        return 1
    if args.list_ports:
        for port in list_ports.comports():
            print(f"{port.device}: {port.description}")
        return 0
    try:
        with ExitStack() as stack:
            streams = []
            for port in args.ports:
                stream = serial.Serial(port=None, baudrate=args.baud, timeout=0.2)
                stack.callback(stream.close)
                # Deassert modem control before opening to avoid deliberate resets.
                # Some USB drivers may still pulse these signals on open.
                stream.dtr = False
                stream.rts = False
                stream.port = port
                stream.open()
                streams.append((port, stream))
            print(f"Recording {args.label} to {args.output.resolve()} at {args.baud} baud.")
            print("Press Ctrl+C to finish and save. Close other monitors on these ports.")
            result = capture(streams, args.output, args.label, args.seconds, args.show_status)
    except (OSError, serial.SerialException) as error:
        print(f"Recording failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130  # Interrupted while opening ports, before capture started.
    print(f"Saved {result.total} CSI rows to {args.output.resolve()}")
    print(f"Recorded for {result.elapsed_seconds:.2f} seconds; stopped: {result.stop_reason}.")
    for port in args.ports:
        print(f"  {port}: {result.rows[port]} rows, {result.malformed[port]} malformed rows skipped")
        if port in result.last_status:
            print(f"  Last firmware status: {result.last_status[port]}")
    for error in result.errors:
        print(f"Serial error: {error}", file=sys.stderr)
    if result.total == 0:
        print("No CSI recorded: check power, ports, baud rate, and Board A.", file=sys.stderr)
    return 1 if result.errors or result.total == 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
