# Pico Modem File Transfer

Move files between a modern PC and a retro laptop (an IBM ThinkPad 365XD, in
the original setup) over a serial cable, using a Raspberry Pi Pico W as a
Wi-Fi modem.

```
 [Retro laptop]                                   [Modern PC]
  XFER.BAS  --RS-232-->  MAX3232  -->  Pico W  --Wi-Fi/TCP-->  xfer_server.py
  (QBasic)               (3.3V<->12V)  (modem firmware)         (Python 3)
```

The laptop "dials" the PC with a Hayes `ATDT ip:port` command. The Pico turns
that into a TCP connection. After that, the laptop and PC exchange text
commands and then transfer files using **XMODEM-CRC**, a 1977 protocol that
nearly every terminal program understands.

## What's in this folder

| File | Runs on | Purpose |
|---|---|---|
| `xfer_server.py` | PC | File server. Serves one folder, accepts uploads. |
| `XFER.BAS` | Laptop | Full client with menu, CRC checking, and retries. |
| `BOOT.BAS` | Laptop | Tiny download-only client, short enough to type by hand. Used to fetch `XFER.BAS` the first time. |

The Pico firmware lives in `../picomodem/`. It needs no changes for file
transfer: it just passes bytes between serial and TCP.

## Requirements

- **PC:** Python 3.6 or newer. No extra packages. It needs to be on the same
  network as the Pico.
- **Pico W** running the `picomodem` firmware, wired to the laptop's serial
  port through a MAX3232 level shifter (never connect RS-232 voltages directly
  to a Pico).
- **Laptop:** QBasic (`QBASIC.EXE`, found on DOS 5/6 disks and on the Windows 95
  CD under `\OTHER\OLDMSDOS`) and a working COM1.

## Quick start

1. **Start the server on the PC:**
   ```
   python3 xfer_server.py
   ```
   It creates a `share` folder next to the script and prints something like
   `On the laptop, dial:  ATDT 192.168.1.50:2323`.
   Put anything you want to download into `share`.

2. **Get the client onto the laptop.** The first time, you need one way to move
   a file that isn't the serial link: a floppy, a CF card, or a CD-R. If you have
   none, type in `BOOT.BAS` by hand, then use it in step 3 to fetch `XFER.BAS`.
   Copy `XFER.BAS` into `share` first.

3. **(Bootstrap only)** Run `QBASIC /RUN BOOT.BAS`. Enter the server address,
   remote file `XFER.BAS`, and a local name. When it prints
   `Wrote N of N bytes`, you're done with it.

4. **Run the full client:**
   ```
   QBASIC /RUN XFER.BAS
   ```
   Enter the server address (`192.168.1.50:2323`). The program sets raw mode,
   dials, and shows a menu.

## Using XFER.BAS

| Key | Action |
|---|---|
| **L** | List files in the server's `share` folder, with sizes. |
| **G** | Get (download). Asks for the remote name and a local name to save as. Refuses to overwrite a local file. |
| **P** | Put (upload). Asks for the local file and the name to use on the server. |
| **Q** | Quit and hang up. |

Each transferred block prints a dot, so you can watch progress.

**Round-trip test:** make a small text file on the laptop, upload it with **P**,
confirm it appears in `share` on the PC, download it back with **G** under a new
name, and compare the two.

## Configuration

**Server** (environment variables):

| Variable | Default | Meaning |
|---|---|---|
| `XFER_DIR` | `share` | Folder to serve. |
| `XFER_PORT` | `2323` | TCP port to listen on. |

Example: `XFER_DIR=/home/me/retro XFER_PORT=4000 python3 xfer_server.py`

The upload size limit (16 MB) is `MAX_PUT` near the top of `xfer_server.py`.

**Client:** the serial settings are in the `OPEN "COM1:..."` line of
`XFER.BAS` and `BOOT.BAS`. They must match `MODEM_BAUD` in the Pico firmware
(19200 by default).

## Limits and rules

- **No overwriting on upload.** If the name already exists on the server it
  answers `ERR file exists`.
- **Plain file names only.** No folders, slashes, colons, or names starting
  with a dot. Anything else is refused.
- **DOS 8.3 names on the laptop.** The server can hold long names, but the
  laptop must save them under names like `NOTES.TXT`. That's why G asks for a
  local name.
- **Speed:** about 1.5 KB/s at 19200 baud, so 1 MB takes roughly 11 minutes.
  QBasic is an interpreter, so going past 38400 is not realistic. A compiled
  client could go faster.
- **One client at a time** is the intended use.
- **No password or encryption.** Run it on a trusted home network only. Anyone
  who can reach the port can list, download, and upload files in the share folder.

## Troubleshooting

| Symptom | What to try |
|---|---|
| `Bad file name` on the `OPEN "COM1..."` line | Delete options from the right end of the string one at a time (`RB2048`, then `CD0`, and so on) until it opens. |
| `Modem did not answer` | Check the cable, MAX3232 wiring, and that laptop and Pico baud rates match. Try typing `AT` in a terminal program first. |
| `No connection.` | Wrong IP or port, PC firewall blocking the port, or the server isn't running. The Pico and the PC must be on the same network. |
| Windows 95 blocks COM1 in a DOS window | Restart in MS-DOS mode and run QBasic from there. |
| `Server never started sending` | The server didn't see the laptop's `C` requests. Re-run and retry. |
| `Too many errors` or `Block failed too many times` | Line noise, a loose cable, or a baud mismatch. Quit and restart so both sides resync. |
| Garbage or missing bytes in files | Make sure raw mode is on (XFER.BAS sends `ATNET0` for you). With a terminal program you must send it yourself. |
| After any failed transfer | Quit and restart the client. A half-finished transfer leaves the two sides out of step. |

## Protocol reference

Commands are text lines ending in a carriage return. The server's replies end
in CR LF.

| Client sends | Server replies |
|---|---|
| (connect) | `XFER 1 READY` |
| `LIST` | One `name size` line per file, then a line containing only `.` |
| `GET name` | `OK size`, then XMODEM-CRC send. Or `ERR no such file`. |
| `PUT name size` | `OK`, then XMODEM-CRC receive, then `OK saved N bytes`. Or `ERR ...`. |
| `QUIT` | Server closes the connection. |

Errors: `ERR no such file`, `ERR bad name or size`, `ERR file exists`,
`ERR short file`, `ERR unknown command`.

**XMODEM-CRC in short:** the receiver sends `C` to start. Each block is
`SOH, block#, 255-block#, 128 data bytes, CRC16 (high byte first)`. The receiver
answers `ACK` or `NAK`, and the sender ends with `EOT`. Block numbers start at 1
and wrap from 255 to 0. The last block is padded with Ctrl-Z (0x1A), which is why
the size is sent up front: both sides trim the padding to the exact length. The
CRC is CRC-16/XMODEM (polynomial 0x1021, initial value 0).

The server also works with any terminal program that supports XMODEM-CRC: type
the command yourself and start the transfer from the program's menu.

## Status

- **Server and protocol:** tested against a Python client that follows the same
  steps as `XFER.BAS`, covering empty, 1-byte, exact-block, and odd-sized files,
  binary data containing `+++`, Ctrl-Z, and 0xFF, and a 40 KB file that wraps the
  block counter. Overwrite and path-traversal protections were also checked.
- **QBasic programs:** written carefully but **not run in QBasic**. Expect to fix
  a typo or two on first use. The CRC table logic was checked against a
  reference implementation.
- **Not tested on real hardware.**

## Ideas for later

- A compiled client (Open Watcom for Win95 or DOS) with interrupt-driven serial
  for higher speed.
- Directory support and a progress percentage.
- A password on the server.
- XMODEM-1K (1024-byte blocks) to cut the per-block overhead.
