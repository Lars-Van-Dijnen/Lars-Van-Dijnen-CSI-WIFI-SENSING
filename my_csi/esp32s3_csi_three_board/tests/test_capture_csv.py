"""Test real C-formatted rows, multiport capture, noise and disconnects."""

import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import ModuleType
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import capture_csv as recorder


class FakeSerial:
    def __init__(self, chunks, failure=None):
        self.chunks = list(chunks)
        self.failure = failure

    def read_until(self, expected, size):
        if self.chunks:
            return self.chunks.pop(0)
        if self.failure:
            raise OSError(self.failure)
        time.sleep(0.005)
        return b""


class RecorderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "test_three_board"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        str(ROOT / "tests/test_three_board.c"), "-o", str(cls.binary)],
                       check=True, capture_output=True)
        cls.lines = subprocess.check_output([str(cls.binary), "--emit-csv"], text=True).splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_host_protocol_suite(self):
        self.assertIn("PASS:", subprocess.check_output([str(self.binary)], text=True))

    def test_all_six_real_firmware_directions_and_iq_order(self):
        links = set()
        for line in self.lines:
            raw = next(csv.reader([line]))
            row = recorder.parse_csi_line(line)
            self.assertEqual(row[:13], raw[:13])
            self.assertEqual(row[13:], json.loads(raw[13]))
            self.assertEqual(len(row), len(recorder.CSV_FIELDS) - 3)
            links.add(tuple(row[1:3]))
        self.assertEqual(len(links), 6)
        self.assertEqual(recorder.IQ_FIELDS[:4], ("imag_00", "real_00", "imag_01", "real_01"))
        self.assertEqual(recorder.IQ_FIELDS[-2:], ("imag_63", "real_63"))

    def test_reject_damaged_records_and_preserve_short_records(self):
        raw = next(csv.reader([self.lines[0]]))

        def encode(values):
            import io
            stream = io.StringIO()
            csv.writer(stream).writerow(values)
            return stream.getvalue().strip()

        for index, value in ((1, "D"), (2, raw[1]), (3, "GRANT"), (4, "xyz"),
                             (5, "-1"), (6, "0x100000000"), (7, "4294967296"),
                             (8, "-2"), (9, "128"), (10, "0"), (11, "129"),
                             (11, "127"), (12, "2"), (13, "[true]"),
                             (13, "[999]"), (13, "{}"), (13, "not JSON")):
            changed = raw.copy()
            changed[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                recorder.parse_csi_line(encode(changed))
        self.assertIsNone(recorder.parse_csi_line("# STATUS A: rounds=1"))
        self.assertIsNone(recorder.parse_csi_line(",".join(recorder.RAW_FIELDS)))
        with self.assertRaises(ValueError):
            recorder.parse_csi_line("CSI,partial")
        raw[11], raw[13] = "2", "[-128,127]"
        row = recorder.parse_csi_line(encode(raw))
        self.assertEqual(row[13:15], [-128, 127])
        self.assertEqual(row[15:], [""] * 126)

    def test_three_ports_split_lines_noise_labels_and_csv_roundtrip(self):
        streams = []
        for index, board in enumerate("ABC"):
            chunks = [b"boot noise\xff\n", (",".join(recorder.RAW_FIELDS) + "\n").encode(),
                      f"# STATUS {board}: csi_dropped=0 turn_overruns=0\n".encode()]
            for line in self.lines[index * 2:index * 2 + 2]:
                raw = (line + "\r\n").encode()
                chunks += [raw[:75], b"", raw[75:]]
            chunks += [b"CSI,broken\n"]
            streams.append((f"COM{index + 7}", FakeSerial(chunks)))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "nested/moving.csv"
            result = recorder.capture(streams, output, "present_moving", 0.08, report=lambda _: None)
            with output.open(newline="") as handle:
                reader = csv.DictReader(handle)
                rows = list(reader)
                self.assertEqual(tuple(reader.fieldnames), recorder.CSV_FIELDS)
            self.assertEqual(result.total, 6)
            self.assertEqual(len(rows), 6)
            self.assertEqual(sum(result.malformed.values()), 3)
            self.assertEqual(len(result.last_status), 3)
            self.assertEqual(set(result.rows.values()), {2})
            for row in rows:
                self.assertEqual(row["label"], "present_moving")
                self.assertTrue(row["host_time_utc"].endswith("+00:00"))
                self.assertEqual(row["session"], "0xffffffff")
                self.assertEqual(row["uptime_us"], "5000000000")
                rx, tx = ord(row["rx_board"]) - 64, ord(row["tx_board"]) - 64
                self.assertEqual(int(row["imag_00"]), rx + tx - 128)
                self.assertEqual(int(row["real_00"]), rx + tx - 127)
                self.assertEqual(int(row["real_63"]), rx + tx - 1)
            with self.assertRaises(FileExistsError):
                recorder.capture([], output, seconds=0.01, report=lambda _: None)

    def test_overlong_line_recovers_at_next_newline(self):
        stream = FakeSerial([b"x" * 4096, b"bad" * 5, b"tail\n",
                             (self.lines[0] + "\n").encode()])
        with tempfile.TemporaryDirectory() as directory:
            result = recorder.capture([("COM7", stream)], Path(directory) / "csi.csv",
                                      seconds=0.05, report=lambda _: None)
        self.assertEqual(result.total, 1)
        self.assertEqual(result.malformed["COM7"], 1)

    def test_disconnect_preserves_written_rows_and_reports_error(self):
        stream = FakeSerial([(self.lines[0] + "\n").encode()], failure="USB unplugged")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "csi.csv"
            result = recorder.capture([("COM7", stream)], output, seconds=0.2, report=lambda _: None)
            with output.open(newline="") as handle:
                self.assertEqual(len(list(csv.reader(handle))), 2)
        self.assertEqual(result.total, 1)
        self.assertEqual(result.errors, ["COM7: USB unplugged"])

    def test_ctrl_c_saves_queued_rows(self):
        real_get = recorder.queue.Queue.get

        def interrupt_get(events, block=True, timeout=None):
            if timeout is not None:
                time.sleep(0.01)  # Allow the reader to enqueue its first record.
                raise KeyboardInterrupt
            return real_get(events, block, timeout)

        stream = FakeSerial([(self.lines[0] + "\n").encode()])
        with tempfile.TemporaryDirectory() as directory, patch.object(
                recorder.queue.Queue, "get", interrupt_get):
            output = Path(directory) / "csi.csv"
            result = recorder.capture([("COM7", stream)], output, report=lambda _: None)
            with output.open(newline="") as handle:
                self.assertEqual(len(list(csv.reader(handle))), 2)
        self.assertEqual(result.total, 1)

    def test_firmware_copies_and_fast_defaults_match(self):
        source = (ROOT / "esp32s3_csi_three_board.c").read_bytes()
        for board in "abc":
            project = ROOT / f"board_{board}"
            self.assertEqual(source, (project / "main/esp32s3_csi_three_board.c").read_bytes())
            config = (project / "sdkconfig.defaults").read_text()
            self.assertIn("CONFIG_CSI_TURN_MS=20\n", config)
            self.assertIn("CONFIG_CSI_REPLY_DELAY_MS=2\n", config)
            self.assertIn(f"CONFIG_ESP_CONSOLE_UART_BAUDRATE={recorder.BAUDRATE}\n", config)

    def test_cli_opens_three_ports_at_correct_baud_and_closes_them(self):
        devices = []

        class Device(FakeSerial):
            def __init__(self, port, baudrate, timeout):
                super().__init__([])
                self.port, self.baudrate, self.timeout = port, baudrate, timeout
                self.closed = False
                devices.append(self)

            def open(self):
                index = int(self.port[-1]) - 7
                self.chunks = [(line + "\n").encode()
                               for line in RecorderTests.lines[index * 2:index * 2 + 2]]

            def close(self):
                self.closed = True

        serial = ModuleType("serial")
        serial.Serial, serial.SerialException = Device, OSError
        tools = ModuleType("serial.tools")
        tools.list_ports = ModuleType("serial.tools.list_ports")
        tools.list_ports.comports = lambda: []
        with tempfile.TemporaryDirectory() as directory, patch.dict(
                sys.modules, {"serial": serial, "serial.tools": tools}), patch(
                "sys.stdout", new_callable=io.StringIO):
            output = Path(directory) / "cli.csv"
            code = recorder.main(["--ports", "COM7", "COM8", "COM9", "--seconds", "0.05",
                                  "--output", str(output), "--label", "absent"])
            self.assertEqual(code, 0)
            with output.open(newline="") as handle:
                self.assertEqual(len(list(csv.reader(handle))), 7)
        self.assertEqual(len(devices), 3)
        for device in devices:
            self.assertEqual(device.baudrate, 460800)
            self.assertEqual(device.timeout, 0.2)
            self.assertFalse(device.dtr)
            self.assertFalse(device.rts)
            self.assertTrue(device.closed)


if __name__ == "__main__":
    unittest.main()
