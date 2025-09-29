#!/usr/bin/env python3
"""
Minimal CLI helper to trigger an OTA update on flip.local.

Usage examples:

  # Serve ./build/flipdot.bin locally and ask the device to fetch it
  python tools/ota_update_cli.py build/flipdot.bin

  # If the firmware is already hosted somewhere
  python tools/ota_update_cli.py --url http://files.example.com/app.bin
"""

import argparse
import contextlib
import functools
import os
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

DEFAULT_DEVICE = "http://flip.local"
DEFAULT_SERVE_TIME_S = 30


def normalize_base_url(url: Optional[str]) -> str:
    if not url:
        return DEFAULT_DEVICE
    parsed = urllib.parse.urlparse(url)
    if not parsed.scheme:
        url = f"http://{url}"
    return url.rstrip('/')


def auto_host_ip(target: str = "flip.local") -> Optional[str]:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(1)
            sock.connect((target, 80))
            return sock.getsockname()[0]
    except OSError:
        return None


def post_update(device_base: str, firmware_url: str) -> None:
    endpoint = f"{device_base}/fw_update"
    data = firmware_url.encode("utf-8")
    request = urllib.request.Request(endpoint, data=data, method="POST")
    request.add_header("Content-Type", "text/plain")
    with urllib.request.urlopen(request, timeout=5) as response:
        body = response.read().decode("utf-8", errors="ignore")
        print(f"Device replied: {response.status} {response.reason}\n{body}")


@contextlib.contextmanager
def serve_firmware(file_path: str, listen: str, port: int):
    directory = os.path.dirname(os.path.abspath(file_path))
    handler = functools.partial(SimpleHTTPRequestHandler, directory=directory)
    httpd = ThreadingHTTPServer((listen, port), handler)
    thread = threading.Thread(target=httpd.serve_forever, name="firmware-httpd", daemon=True)
    thread.start()
    try:
        yield
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Trigger an OTA update on flip.local")
    parser.add_argument("firmware", nargs="?", help="Path to the firmware binary")
    parser.add_argument("--device", default=DEFAULT_DEVICE, help="Device base URL (default: %(default)s)")
    parser.add_argument("--url", help="If supplied, skip hosting and send this firmware URL to the device")
    parser.add_argument("--listen", default="0.0.0.0", help="Address to bind the local HTTP server")
    parser.add_argument("--port", type=int, default=8000, help="Port for the local HTTP server")
    parser.add_argument("--serve-seconds", type=int, default=DEFAULT_SERVE_TIME_S, help="Duration to keep the HTTP server alive; 0 to keep running until Ctrl+C")
    parser.add_argument("--host-ip", help="Public IP/hostname the device can reach (defaults to autodetected outbound IP)")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    device_base = normalize_base_url(args.device)

    if args.url:
        firmware_url = args.url
        print(f"Using provided firmware URL: {firmware_url}")
        post_update(device_base, firmware_url)
        return

    if not args.firmware:
        raise SystemExit("Firmware path is required when --url is omitted")

    firmware_path = os.path.abspath(args.firmware)
    if not os.path.exists(firmware_path):
        raise SystemExit(f"Firmware not found: {firmware_path}")

    host_ip = args.host_ip or auto_host_ip()
    if host_ip is None:
        raise SystemExit("Unable to detect host IP; provide --host-ip")

    filename = os.path.basename(firmware_path)
    listen_addr = args.listen
    port = args.port

    print(f"Hosting {firmware_path} on http://{listen_addr}:{port}/")
    with serve_firmware(firmware_path, listen_addr, port):
        download_host = host_ip if listen_addr == "0.0.0.0" else listen_addr
        firmware_url = f"http://{download_host}:{port}/{urllib.parse.quote(filename)}"
        print(f"Triggering OTA with URL: {firmware_url}")
        try:
            post_update(device_base, firmware_url)
        except (urllib.error.URLError, urllib.error.HTTPError) as err:
            raise SystemExit(f"Failed to trigger OTA: {err}")

        serve_seconds = args.serve_seconds
        if serve_seconds == 0:
            print("Serving indefinitely; press Ctrl+C once the device finishes downloading.")
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                print("Stopping server")
        else:
            print(f"Serving firmware for {serve_seconds} seconds...")
            try:
                time.sleep(serve_seconds)
            except KeyboardInterrupt:
                print("Stopping server early")


if __name__ == "__main__":
    main()
