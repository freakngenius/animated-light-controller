#!/usr/bin/env python3
# cyd_log.py - restart the CYD from the Mac (no button press) and print what it says on the Serial port.
# Usage: python3 cyd_log.py [seconds]      (default 60)
import fcntl, glob, os, select, sys, termios, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
ports = sorted(glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/cu.wchusbserial*') + glob.glob('/dev/cu.usbmodem*'))
if not ports:
    sys.exit('No CYD port found. Check the cable.')
port = ports[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

attrs = termios.tcgetattr(fd)
attrs[0] = 0                                   # iflag
attrs[1] = 0                                   # oflag
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[3] = 0                                   # lflag: raw
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)

def lines(set_bits=0, clear_bits=0):
    if clear_bits:
        fcntl.ioctl(fd, termios.TIOCMBIC, clear_bits.to_bytes(4, 'little'))
    if set_bits:
        fcntl.ioctl(fd, termios.TIOCMBIS, set_bits.to_bytes(4, 'little'))

# Reset pulse: keep DTR (boot pin) released, pull RTS (reset pin) for a moment.
lines(clear_bits=termios.TIOCM_DTR)
lines(set_bits=termios.TIOCM_RTS)
time.sleep(0.15)
lines(clear_bits=termios.TIOCM_RTS)

print(f'[cyd_log] restarted the board on {port}, listening for {secs:.0f} s', flush=True)
end = time.time() + secs
buf = b''
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.25)
    if r:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        buf += chunk.replace(b'\r', b'')
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            print(line.decode('utf-8', 'replace'), flush=True)
if buf:
    print(buf.decode('utf-8', 'replace'), flush=True)
os.close(fd)
