"""Read the monitor's serial log for a fixed time.

usage: serial_read.py PORT SECONDS [--reset] [--send "cmd@delay" ...]
  --reset          pulse EN (via RTS) so the log starts from boot
  --send "x@5"     send line "x" five seconds after start (repeatable)
"""
import sys
import time

import serial


def main():
    port, seconds = sys.argv[1], float(sys.argv[2])
    args = sys.argv[3:]
    sends = []
    i = 0
    while i < len(args):
        if args[i] == "--send":
            text, _, delay = args[i + 1].rpartition("@")
            sends.append((float(delay), text))
            i += 1
        i += 1
    sends.sort()

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if "--reset" in args:
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False

    start = time.time()
    while time.time() - start < seconds:
        while sends and time.time() - start >= sends[0][0]:
            ser.write((sends.pop(0)[1] + "\n").encode())
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
