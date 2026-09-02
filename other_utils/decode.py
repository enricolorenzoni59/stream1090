# This script reads stream1090 output from stdin and outputs the decoded results to stdout
# requires pyModeS
import sys
import pyModeS as pms

def parse_stream1090_line(line: str):
    """
    Stream1090 MLAT ASCII format:
      <MLAT(12 hex) RSSI(2 hex) FRAMEHEX...;
    Returns (mlat_int, rssi_int, frame_hex) or None.
    """
    line = line.strip()
    if not line.startswith("<") or not line.endswith(";"):
        return None

    payload = line[1:-1]  # strip < and ;

    # Must contain MLAT(12) + RSSI(2) + at least 14 hex frame
    if len(payload) < 12 + 2 + 14:
        return None

    # Extract MLAT + RSSI + frame
    mlat_hex = payload[:12]
    rssi_hex = payload[12:14]
    frame_hex = payload[14:]

    # Exact Mode-S frame lengths: 14 or 28 hex chars
    if len(frame_hex) not in (14, 28):
        print(f"BAD FRAME LENGTH {len(frame_hex)} HEX={frame_hex}")
        return None

    # Convert MLAT + RSSI to integers
    mlat_int = int(mlat_hex, 16)
    rssi_int = int(rssi_hex, 16)

    return mlat_int, rssi_int, frame_hex


for raw in sys.stdin:
    parsed = parse_stream1090_line(raw)
    if parsed is None:
        continue

    mlat_int, rssi_int, frame_hex = parsed

    try:
        decoded = pms.decode(frame_hex)
    except Exception as e:
        print(f"Decode error: {e} HEX={frame_hex}")
        continue

    if decoded is None:
        continue

    print({
        "mlat": mlat_int,
        "rssi": rssi_int,
        "frame": frame_hex,
        "decoded": decoded
    })
