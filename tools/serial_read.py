"""Read the monitor's serial log for a fixed time.

usage: serial_read.py PORT SECONDS [--reset] [--hold N] [--send "cmd@delay" ...] [--until TEXT N]
  --reset          pulse EN (via RTS) so the log starts from boot
  --hold N         keep the board in reset for N seconds first (fakes a power cut)
  --send "x@5"     send line "x" five seconds after start (repeatable)
  --until TEXT N   stop early once TEXT has appeared N times
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
    until_text, until_count, seen = None, 0, 0
    if "--until" in args:
        k = args.index("--until")
        until_text, until_count = args[k + 1], int(args[k + 2])
    sends.sort()

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if "--hold" in args:
        ser.rts = True
        time.sleep(float(args[args.index("--hold") + 1]))
        ser.rts = False
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
            if until_text:
                seen += data.decode("utf-8", "replace").count(until_text)
                if seen >= until_count:
                    break
    ser.close()


if __name__ == "__main__":
    main()
