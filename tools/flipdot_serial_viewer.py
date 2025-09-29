#!/usr/bin/env python3

"""Live flip-dot display viewer that mirrors ESP32 log output.

The ESP32 firmware emits lines formatted as::

    DISPLAY_FRAME:<frame_id>:<hex_payload>

This utility opens the serial console, forwards all regular log lines to
STDOUT, and renders the most recent frame in a small Tkinter window.
"""

from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from dataclasses import dataclass

try:
    import tkinter as tk
except ImportError as exc:  # pragma: no cover - Tk is expected to exist on host
    print("Tkinter is required for the flip-dot viewer: {}".format(exc), file=sys.stderr)
    sys.exit(1)

try:
    import serial
    from serial import SerialException
except ImportError as exc:  # pragma: no cover - pyserial must be installed by user
    print("pyserial is required for the flip-dot viewer: {}".format(exc), file=sys.stderr)
    sys.exit(1)


FRAME_PREFIX = "DISPLAY_FRAME:"
PIXEL_ROWS = 14
PIXEL_COLS = 28
HEX_PAYLOAD_LENGTH = (PIXEL_COLS * 2) * 2  # 56 bytes -> 112 hex chars


@dataclass
class Frame:
    frame_id: int
    pixels: list[list[int]]
    received_at: float


def parse_frame_payload(payload: str) -> Frame | None:
    """Decode a DISPLAY_FRAME payload into a bitmap."""

    parts = payload.split(":", 2)
    if len(parts) != 3:
        return None

    header, frame_id_str, hex_data = parts
    if header != "DISPLAY_FRAME":
        return None

    hex_data = hex_data.strip()
    if len(hex_data) != HEX_PAYLOAD_LENGTH:
        return None

    try:
        frame_id = int(frame_id_str, 10)
    except ValueError:
        return None

    try:
        raw_bytes = bytes.fromhex(hex_data)
    except ValueError:
        return None

    if len(raw_bytes) != HEX_PAYLOAD_LENGTH // 2:
        return None

    pixels: list[list[int]] = [[0 for _ in range(PIXEL_COLS)] for _ in range(PIXEL_ROWS)]
    for col in range(PIXEL_COLS):
        upper = raw_bytes[col]
        lower = raw_bytes[PIXEL_COLS + col]
        for row in range(7):
            pixels[row][col] = 1 if (upper >> row) & 0x01 else 0
            pixels[row + 7][col] = 1 if (lower >> row) & 0x01 else 0

    return Frame(frame_id=frame_id, pixels=pixels, received_at=time.time())


def stream_serial_lines(ser: serial.Serial, frame_queue: "queue.Queue[Frame]", stop_event: threading.Event) -> None:
    """Read the serial port until stopped, forwarding frames and logs."""

    while not stop_event.is_set():
        try:
            raw_line = ser.readline()
        except SerialException as exc:
            print(f"Serial error: {exc}", file=sys.stderr, flush=True)
            break

        if not raw_line:
            continue

        line = raw_line.decode("utf-8", errors="replace")
        stripped = line.rstrip("\r\n")
        marker_pos = stripped.find(FRAME_PREFIX)

        if marker_pos == -1:
            print(stripped, flush=True)
            continue

        prefix = stripped[:marker_pos].rstrip()
        payload = stripped[marker_pos:]

        frame = parse_frame_payload(payload)
        if frame is None:
            # Something went wrong, keep the original log line so nothing is lost.
            print(stripped, flush=True)
            continue

        if prefix:
            print(prefix, flush=True)

        frame_queue.put(frame)

    stop_event.set()


class FlipDotViewer:
    """Tkinter canvas that mirrors the pixels of the flip-dot display."""

    def __init__(self, root: tk.Tk, pixel_size: int) -> None:
        self.root = root
        self.pixel_size = pixel_size
        self.canvas = tk.Canvas(
            root,
            width=PIXEL_COLS * pixel_size,
            height=PIXEL_ROWS * pixel_size,
            bg="#101010",
            highlightthickness=0,
        )
        self.canvas.pack(padx=12, pady=12)

        self.status_var = tk.StringVar(value="Waiting for frames…")
        status_label = tk.Label(root, textvariable=self.status_var)
        status_label.pack(padx=12, pady=(0, 12))

        self._dot_padding = max(1, pixel_size // 10)
        self._rectangles: list[list[int]] = []
        for row in range(PIXEL_ROWS):
            row_items: list[int] = []
            for col in range(PIXEL_COLS):
                x0 = col * pixel_size + self._dot_padding
                y0 = row * pixel_size + self._dot_padding
                x1 = (col + 1) * pixel_size - self._dot_padding
                y1 = (row + 1) * pixel_size - self._dot_padding
                rect = self.canvas.create_oval(
                    x0,
                    y0,
                    x1,
                    y1,
                    fill="#1a1a1a",
                    outline="#202020",
                )
                row_items.append(rect)
            self._rectangles.append(row_items)

    def update_pixels(self, frame: Frame) -> None:
        on_colour = "#ffd000"
        off_colour = "#1a1a1a"

        for row in range(PIXEL_ROWS):
            for col in range(PIXEL_COLS):
                target_colour = on_colour if frame.pixels[row][col] else off_colour
                current_colour = self.canvas.itemcget(self._rectangles[row][col], "fill")
                if current_colour != target_colour:
                    self.canvas.itemconfigure(self._rectangles[row][col], fill=target_colour)

        timestamp = time.strftime("%H:%M:%S", time.localtime(frame.received_at))
        self.status_var.set(f"Frame {frame.frame_id} @ {timestamp}")


def schedule_queue_pump(root: tk.Tk, frame_queue: "queue.Queue[Frame]", viewer: FlipDotViewer) -> None:
    """Drain the frame queue on the Tkinter main loop."""

    try:
        while True:
            frame = frame_queue.get_nowait()
            viewer.update_pixels(frame)
    except queue.Empty:
        pass

    root.after(50, schedule_queue_pump, root, frame_queue, viewer)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flip-dot serial log viewer with live preview")
    parser.add_argument("--port", required=True, help="Serial port device, e.g. /dev/ttyUSB0 or COM4")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument(
        "--pixel-size",
        type=int,
        default=35,
        help="Pixel size in the viewer window (default: 18)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except SerialException as exc:
        print(f"Failed to open serial port {args.port}: {exc}", file=sys.stderr)
        return 1

    stop_event = threading.Event()
    frame_queue: "queue.Queue[Frame]" = queue.Queue()

    reader_thread = threading.Thread(
        target=stream_serial_lines,
        args=(ser, frame_queue, stop_event),
        name="serial-reader",
        daemon=True,
    )
    reader_thread.start()

    root = tk.Tk()
    root.title("Flip-Dot Display Preview")

    viewer = FlipDotViewer(root, pixel_size=max(6, args.pixel_size))
    schedule_queue_pump(root, frame_queue, viewer)

    def on_close() -> None:
        stop_event.set()
        try:
            ser.close()
        except SerialException:
            pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)

    try:
        root.mainloop()
    except KeyboardInterrupt:
        on_close()

    stop_event.set()
    reader_thread.join(timeout=1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
