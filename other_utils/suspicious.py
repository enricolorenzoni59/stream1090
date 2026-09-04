# suspicious.py — detect suspicious ADS-B / Mode-S messages
# Note: it is about finding candidates, so this is an heuristic
import sys
import pyModeS as pms

# pms.decode() arrived in pyModeS 3. On an older one every call below raises,
# the except swallows it, and the script ends with '0 suspicious out of 0' --
# which reads exactly like a clean stream. Say what is missing instead.
if not hasattr(pms, "decode"):
    sys.exit(
        "suspicious.py needs pyModeS 3 or newer: this one has no pms.decode(). "
        "Installed version: %s" % getattr(pms, "__version__", "unknown"))

# Track ICAO appearances
icao_seen = set()
icao_df17_seen = set()

# Assigned ICAO ranges which most likely is not 100% correct
# Malta and San Marino for example had to be adjusted
ASSIGNED_ICAO_RANGES = [
    (0x004000, 0x0043FF, "Zimbabwe"),
    (0x006000, 0x006FFF, "Mozambique"),
    (0x008000, 0x00FFFF, "South Africa"),
    (0x010000, 0x017FFF, "Egypt"),
    (0x018000, 0x01FFFF, "Libya"),
    (0x020000, 0x027FFF, "Morocco"),
    (0x028000, 0x02FFFF, "Tunisia"),
    (0x030000, 0x0303FF, "Botswana"),
    (0x032000, 0x032FFF, "Burundi"),
    (0x034000, 0x034FFF, "Cameroon"),
    (0x035000, 0x0353FF, "Comoros"),
    (0x036000, 0x036FFF, "Congo"),
    (0x038000, 0x038FFF, "Coate d Ivoire"),
    (0x03E000, 0x03EFFF, "Gabon"),
    (0x040000, 0x040FFF, "Ethiopia"),
    (0x042000, 0x042FFF, "Equatorial Guinea"),
    (0x044000, 0x044FFF, "Ghana"),
    (0x046000, 0x046FFF, "Guinea"),
    (0x048000, 0x0483FF, "Guinea-Bissau"),
    (0x04A000, 0x04A3FF, "Lesotho"),
    (0x04C000, 0x04CFFF, "Kenya"),
    (0x050000, 0x050FFF, "Liberia"),
    (0x054000, 0x054FFF, "Madagascar"),
    (0x058000, 0x058FFF, "Malawi"),
    (0x05A000, 0x05A3FF, "Maldives"),
    (0x05C000, 0x05CFFF, "Mali"),
    (0x05E000, 0x05E3FF, "Mauritania"),
    (0x060000, 0x0603FF, "Mauritius"),
    (0x062000, 0x062FFF, "Niger"),
    (0x064000, 0x064FFF, "Nigeria"),
    (0x068000, 0x068FFF, "Uganda"),
    (0x06A000, 0x06A3FF, "Qatar"),
    (0x06C000, 0x06CFFF, "Central African Republic"),
    (0x06E000, 0x06EFFF, "Rwanda"),
    (0x070000, 0x070FFF, "Senegal"),
    (0x074000, 0x0743FF, "Seychelles"),
    (0x076000, 0x0763FF, "Sierra Leone"),
    (0x078000, 0x078FFF, "Somalia"),
    (0x07A000, 0x07A3FF, "Swaziland"),
    (0x07C000, 0x07CFFF, "Sudan"),
    (0x080000, 0x080FFF, "Tanzania"),
    (0x084000, 0x084FFF, "Chad"),
    (0x088000, 0x088FFF, "Togo"),
    (0x08A000, 0x08AFFF, "Zambia"),
    (0x08C000, 0x08CFFF, "D R Congo"),
    (0x090000, 0x090FFF, "Angola"),
    (0x094000, 0x0943FF, "Benin"),
    (0x096000, 0x0963FF, "Cape Verde"),
    (0x098000, 0x0983FF, "Djibouti"),
    (0x09A000, 0x09AFFF, "Gambia"),
    (0x09C000, 0x09CFFF, "Burkina Faso"),
    (0x09E000, 0x09E3FF, "Sao Tome"),
    (0x0A0000, 0x0A7FFF, "Algeria"),
    (0x0A8000, 0x0A8FFF, "Bahamas"),
    (0x0AA000, 0x0AA3FF, "Barbados"),
    (0x0AB000, 0x0AB3FF, "Belize"),
    (0x0AC000, 0x0ACFFF, "Colombia"),
    (0x0AE000, 0x0AEFFF, "Costa Rica"),
    (0x0B0000, 0x0B0FFF, "Cuba"),
    (0x0B2000, 0x0B2FFF, "El Salvador"),
    (0x0B4000, 0x0B4FFF, "Guatemala"),
    (0x0B6000, 0x0B6FFF, "Guyana"),
    (0x0B8000, 0x0B8FFF, "Haiti"),
    (0x0BA000, 0x0BAFFF, "Honduras"),
    (0x0BC000, 0x0BC3FF, "St.Vincent + Grenadines"),
    (0x0BE000, 0x0BEFFF, "Jamaica"),
    (0x0C0000, 0x0C0FFF, "Nicaragua"),
    (0x0C2000, 0x0C2FFF, "Panama"),
    (0x0C4000, 0x0C4FFF, "Dominican Republic"),
    (0x0C6000, 0x0C6FFF, "Trinidad and Tobago"),
    (0x0C8000, 0x0C8FFF, "Suriname"),
    (0x0CA000, 0x0CA3FF, "Antigua & Barbuda"),
    (0x0CC000, 0x0CC3FF, "Grenada"),
    (0x0D0000, 0x0D7FFF, "Mexico"),
    (0x0D8000, 0x0DFFFF, "Venezuela"),
    (0x100000, 0x1FFFFF, "Russia"),
    (0x201000, 0x2013FF, "Namibia"),
    (0x202000, 0x2023FF, "Eritrea"),
    (0x300000, 0x33FFFF, "Italy"),
    (0x340000, 0x37FFFF, "Spain"),
    (0x380000, 0x3BFFFF, "France"),
    (0x3C0000, 0x3FFFFF, "Germany"),
    (0x400000, 0x43FFFF, "United Kingdom"),
    (0x440000, 0x447FFF, "Austria"),
    (0x448000, 0x44FFFF, "Belgium"),
    (0x450000, 0x457FFF, "Bulgaria"),
    (0x458000, 0x45FFFF, "Denmark"),
    (0x460000, 0x467FFF, "Finland"),
    (0x468000, 0x46FFFF, "Greece"),
    (0x470000, 0x477FFF, "Hungary"),
    (0x478000, 0x47FFFF, "Norway"),
    (0x480000, 0x487FFF, "Netherlands"),
    (0x488000, 0x48FFFF, "Poland"),
    (0x490000, 0x497FFF, "Portugal"),
    (0x498000, 0x49FFFF, "Czech Republic"),
    (0x4A0000, 0x4A7FFF, "Romania"),
    (0x4A8000, 0x4AFFFF, "Sweden"),
    (0x4B0000, 0x4B7FFF, "Switzerland"),
    (0x4B8000, 0x4BFFFF, "Turkey"),
    (0x4C0000, 0x4C7FFF, "Yugoslavia"),
    (0x4C8000, 0x4C83FF, "Cyprus"),
    (0x4CA000, 0x4CAFFF, "Ireland"),
    (0x4CC000, 0x4CCFFF, "Iceland"),
    (0x4D0000, 0x4D03FF, "Luxembourg"),
    (0x4D2000, 0x4D3FFF, "Malta"),
    (0x4D4000, 0x4D43FF, "Monaco"),
    (0x500000, 0x5005FF, "San Marino"),
    (0x501000, 0x5013FF, "Albania"),
    (0x501C00, 0x501FFF, "Croatia"),
    (0x502C00, 0x502FFF, "Latvia"),
    (0x503C00, 0x503FFF, "Lithuania"),
    (0x504C00, 0x504FFF, "Moldova"),
    (0x505C00, 0x505FFF, "Slovakia"),
    (0x506C00, 0x506FFF, "Slovenia"),
    (0x507C00, 0x507FFF, "Uzbekistan"),
    (0x508000, 0x50FFFF, "Ukraine"),
    (0x510000, 0x5103FF, "Belarus"),
    (0x511000, 0x5113FF, "Estonia"),
    (0x512000, 0x5123FF, "Macedonia"),
    (0x513000, 0x5133FF, "Bosnia & Herzegovina"),
    (0x514000, 0x5143FF, "Georgia"),
    (0x515000, 0x5153FF, "Tajikistan"),
    (0x600000, 0x6003FF, "Armenia"),
    (0x600800, 0x600BFF, "Azerbaijan"),
    (0x601000, 0x6013FF, "Kyrgyzstan"),
    (0x601800, 0x601BFF, "Turkmenistan"),
    (0x680000, 0x6803FF, "Bhutan"),
    (0x681000, 0x6813FF, "Micronesia"),
    (0x682000, 0x6823FF, "Mongolia"),
    (0x683000, 0x6833FF, "Kazakhstan"),
    (0x684000, 0x6843FF, "Palau"),
    (0x700000, 0x700FFF, "Afghanistan"),
    (0x702000, 0x702FFF, "Bangladesh"),
    (0x704000, 0x704FFF, "Myanmar"),
    (0x706000, 0x706FFF, "Kuwait"),
    (0x708000, 0x708FFF, "Laos"),
    (0x70A000, 0x70AFFF, "Nepal"),
    (0x70C000, 0x70C3FF, "Oman"),
    (0x70E000, 0x70EFFF, "Cambodia"),
    (0x710000, 0x717FFF, "Saudi Arabia"),
    (0x718000, 0x71FFFF, "South Korea"),
    (0x720000, 0x727FFF, "North Korea"),
    (0x728000, 0x72FFFF, "Iraq"),
    (0x730000, 0x737FFF, "Iran"),
    (0x738000, 0x73FFFF, "Israel"),
    (0x740000, 0x747FFF, "Jordan"),
    (0x748000, 0x74FFFF, "Lebanon"),
    (0x750000, 0x757FFF, "Malaysia"),
    (0x758000, 0x75FFFF, "Philippines"),
    (0x760000, 0x767FFF, "Pakistan"),
    (0x768000, 0x76FFFF, "Singapore"),
    (0x770000, 0x777FFF, "Sri Lanka"),
    (0x778000, 0x77FFFF, "Syria"),
    (0x780000, 0x7BFFFF, "China"),
    (0x7C0000, 0x7FFFFF, "Australia"),
    (0x800000, 0x83FFFF, "India"),
    (0x840000, 0x87FFFF, "Japan"),
    (0x880000, 0x887FFF, "Thailand"),
    (0x888000, 0x88FFFF, "Viet Nam"),
    (0x890000, 0x890FFF, "Yemen"),
    (0x894000, 0x894FFF, "Bahrain"),
    (0x895000, 0x8953FF, "Brunei"),
    (0x896000, 0x896FFF, "United Arab Emirates"),
    (0x897000, 0x8973FF, "Solomon Islands"),
    (0x898000, 0x898FFF, "Papua New Guinea"),
    (0x899000, 0x8993FF, "Taiwan (unofficial)"),
    (0x8A0000, 0x8A7FFF, "Indonesia"),
    (0x900000, 0x9003FF, "Marshall Islands"),
    (0x901000, 0x9013FF, "Cook Islands"),
    (0x902000, 0x9023FF, "Samoa"),
    (0xA00000, 0xAFFFFF, "United States"),
    (0xC00000, 0xC3FFFF, "Canada"),
    (0xC80000, 0xC87FFF, "New Zealand"),
    (0xC88000, 0xC88FFF, "Fiji"),
    (0xC8A000, 0xC8A3FF, "Nauru"),
    (0xC8C000, 0xC8C3FF, "Saint Lucia"),
    (0xC8D000, 0xC8D3FF, "Tonga"),
    (0xC8E000, 0xC8E3FF, "Kiribati"),
    (0xC90000, 0xC903FF, "Vanuatu"),
    (0xE00000, 0xE3FFFF, "Argentina"),
    (0xE40000, 0xE7FFFF, "Brazil"),
    (0xE80000, 0xE80FFF, "Chile"),
    (0xE84000, 0xE84FFF, "Ecuador"),
    (0xE88000, 0xE88FFF, "Paraguay"),
    (0xE8C000, 0xE8CFFF, "Peru"),
    (0xE90000, 0xE90FFF, "Uruguay"),
    (0xE94000, 0xE94FFF, "Bolivia"),
]

def merge_intervals(ranges):
    # we merge adjacent intervals
    merged = []
    current_lo, current_hi, current_country = ranges[0]

    for lo, hi, country in ranges[1:]:
        if lo <= current_hi + 1:
            current_hi = max(current_hi, hi)
        else:
            merged.append((current_lo, current_hi))
            current_lo, current_hi = lo, hi

    merged.append((current_lo, current_hi))
    return merged


# Merge the above table for less work
merged_ranges = merge_intervals(ASSIGNED_ICAO_RANGES)

def icao_assigned(icao_str):
    # Check whether ICAO hex string falls into any assigned range (merged).
    try:
        icao = int(icao_str, 16)
    except Exception:
        return False

    for lo, hi in merged_ranges:
        if lo <= icao <= hi:
            return True

    return False


def is_suspicious(mlat, rssi, frame_hex, decoded):
    reasons = []
    df = decoded["df"]
    icao = decoded.get("icao")

    # 1) ICAO not assigned in ICAOHexRange.csv
    if icao is not None and not icao_assigned(icao):
        reasons.append(f"ICAO {icao} not assigned")

    # 2) DF17/DF18: CA 1/2/3 is invalid (reserved)
    if df in (17, 18) and decoded["capability"] in (1, 2, 3):
        reasons.append(f"Invalid ADS-B capability CA={decoded['capability']} for DF{df}")

    # 3) DF19: 1-bit flip away from DF17 → suspicious
    if df == 19:
        reasons.append("DF19 is a 1-bit flip away from DF17")

    # 4) DF17: Typecodes 23–27 are reserved / invalid
    if df == 17 and 23 <= decoded["typecode"] <= 27:
        reasons.append(f"Reserved DF17 typecode {decoded['typecode']}")

    # 5) DF4/DF5: flight status > 5 is invalid
    if df in (4, 5) and decoded["flight_status"] > 5:
        reasons.append(f"Invalid DF{df} flight status {decoded['flight_status']}")

    # 6) DF4/DF5: downlink request must be 0,1,4,5
    if df in (4, 5) and decoded["downlink_request"] not in (0, 1, 4, 5):
        reasons.append(f"Invalid DF{df} downlink request {decoded['downlink_request']}")

    # 7) RSSI anomalies
    if rssi < 1 or rssi > 200:
        reasons.append(f"RSSI anomaly {rssi}")

    return reasons if reasons else None


def parse_stream1090_line(line: str):
    """Parse MLAT ASCII format from stream1090."""
    line = line.strip()
    if not line.startswith("<") or not line.endswith(";"):
        return None

    payload = line[1:-1]
    if len(payload) < 12 + 2 + 14:
        return None

    mlat_hex = payload[:12]
    rssi_hex = payload[12:14]
    frame_hex = payload[14:]

    if len(frame_hex) not in (14, 28):
        return None

    mlat_int = int(mlat_hex, 16)
    rssi_int = int(rssi_hex, 16)

    return mlat_int, rssi_int, frame_hex

total_messages = 0
suspicious_messages = 0
undecodable = 0

for raw in sys.stdin:
    parsed = parse_stream1090_line(raw)
    if parsed is None:
        continue

    mlat_int, rssi_int, frame_hex = parsed

    try:
        decoded = pms.decode(frame_hex)
    except Exception:
        undecodable = undecodable + 1
        continue

    if decoded is None:
        undecodable = undecodable + 1
        continue

    df = decoded["df"]

    # DF-17/18: PyModeS does NOT extract capability → do it manually
    if df in {17, 18}:
        decoded["capability"] = int(frame_hex[0:2], 16) & 0x7

    # Track DF17 appearances
    if df == 17:
        icao_df17_seen.add(decoded.get("icao"))

    total_messages = total_messages + 1
    # Check suspiciousness
    reasons = is_suspicious(mlat_int, rssi_int, frame_hex, decoded)
    if reasons is not None:
        suspicious_messages = suspicious_messages + 1
        print(frame_hex)
        print(decoded)
        for r in reasons:
            print(r)
        print("")
    
    
print(f"{suspicious_messages} suspicious out of {total_messages}")
if undecodable:
    # Not necessarily a problem -- a stream full of fabricated frames has
    # plenty that decode to nothing -- but a run where everything was skipped
    # is a broken run, not a clean stream, and the two used to look identical.
    print(f"{undecodable} messages could not be decoded and were skipped")