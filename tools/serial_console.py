#!/usr/bin/env python3
"""Live serial console for the ESP32-Scale.

Why this exists
---------------
Three separate measurement attempts failed the same way: a script opened the
port, collected bytes into a Python string, and printed it only at the end. With
the output redirected to a file that means the file stays empty for the whole
run, so a two-minute measurement looks like a hang and nobody can tell whether
the device is working or the host script is stuck. Twice it also cost real time:
a measurement had already finished and sat in a buffer.

This tool prints every line as it arrives and flushes immediately, so the log
always shows the current state of the device.

Usage
-----
    tools/serial_console.py                 # passive monitor, no commands
    tools/serial_console.py --send 'm 73.03' --seconds 330
    tools/serial_console.py --send 'R' --seconds 120 --grep '#S|#RAW'
    tools/serial_console.py --reset          # toggle DTR/RTS like idf.py monitor

Options
-------
--port      serial device; defaults to the CH9102 by-id path, falling back to
            /dev/ttyUSB0 when that path is absent
--send      one command line to send after --settle seconds
--settle    seconds to wait before sending --send (default 0)
--seconds   total capture time (default 60)
--grep      only print lines matching this regular expression
--reset     toggle DTR/RTS at the start to reboot the board
--no-ansi   disable ANSI colour
"""

import argparse
import os
import re
import sys
import time

DEFAULT_PORT_CANDIDATES = [
    "/dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00",
    "/dev/ttyUSB0",
]

# The bootloader and the ROM both emit non-printable and high-byte noise when the
# baud rate is briefly wrong. Stripping it keeps the grep patterns honest instead
# of failing on a stray \x00 in the middle of an otherwise valid line.
NOISE = re.compile(r"[^\x09\x0a\x0d\x20-\x7e]")

GREEN = "\033[32m"
DIM = "\033[2m"
RESET = "\033[0m"


def pick_port():
    for path in DEFAULT_PORT_CANDIDATES:
        if os.path.exists(path):
            return path
    sys.exit("no serial port found; tried: %s" % ", ".join(DEFAULT_PORT_CANDIDATES))


def open_port(path):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing: pip install pyserial")
    return serial.Serial(path, 115200, timeout=1)


def reboot(port):
    """Same DTR/RTS dance idf.py monitor uses.

    Both lines are deasserted first: on the CH9102 an asserted RTS pulls EN low
    and an asserted DTR pulls GPIO0, so leaving them asserted across the switch
    holds the chip in reset instead of releasing it.
    """
    port.dtr = False
    port.rts = False
    time.sleep(0.2)
    port.dtr = True
    port.rts = True
    time.sleep(0.1)
    port.dtr = False
    port.rts = False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--send", action="append", default=[],
                    help="command to send, may be repeated")
    ap.add_argument("--settle", type=float, default=0.0,
                    help="seconds to wait before sending")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--grep")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--no-ansi", action="store_true")
    args = ap.parse_args()

    path = args.port or pick_port()
    colour = not args.no_ansi and sys.stdout.isatty()

    port = open_port(path)
    if args.reset:
        reboot(port)
    else:
        port.reset_input_buffer()

    pending = list(args.send)
    if args.settle and pending:
        deadline = time.time() + args.settle
        while time.time() < deadline:
            time.sleep(min(0.2, deadline - time.time()))

    for cmd in pending:
        sys.stderr.write("-> %s\n" % cmd)
        sys.stderr.flush()
        port.write(cmd.encode() + b"\n")
        port.flush()

    pattern = re.compile(args.grep) if args.grep else None

    sys.stderr.write("listening on %s for %.0f s\n" % (path, args.seconds))
    sys.stderr.flush()

    t0 = time.time()
    buf = b""
    try:
        while time.time() - t0 < args.seconds:
            chunk = port.read(port.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = NOISE.sub("", raw.decode("utf-8", "replace")).rstrip()
                if pattern and not pattern.search(line):
                    continue
                # Flushed per line on purpose: this is the whole point of the
                # tool. A measurement must be watchable while it runs.
                if colour and line.startswith("#"):
                    print(GREEN + line + RESET, flush=True)
                else:
                    print(line, flush=True)
    except KeyboardInterrupt:
        sys.stderr.write("\ninterrupted\n")
    finally:
        if buf:
            line = NOISE.sub("", buf.decode("utf-8", "replace")).rstrip()
            if line and (not pattern or pattern.search(line)):
                print(line, flush=True)
        port.close()


if __name__ == "__main__":
    main()