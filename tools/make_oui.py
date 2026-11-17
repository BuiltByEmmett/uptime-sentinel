"""Write src/oui_table.cpp: MAC address prefixes of common device makers,
for the device list's "manufacturer" column.

The prefixes come from the IEEE's public registry of 24-bit assignments.
Download https://standards-oui.ieee.org/oui/oui.csv and pass its path:

    python tools/make_oui.py path/to/oui.csv

Only the makers listed in VENDORS are kept, which covers most of what turns
up on a home network in about 60 KB. Anything else shows as "Unknown".
"""
import csv
import re
import sys
from pathlib import Path

# Label shown on the dashboard, and a pattern for the registered names.
VENDORS = [
    ("Apple", r"^Apple, Inc"),
    ("Samsung", r"^Samsung Electr"),
    ("Google", r"^Google(, Inc| LLC)|^Nest Labs"),
    ("Amazon", r"^Amazon (Technologies|\.com)|^Amazon\.com"),
    ("Microsoft", r"^Microsoft"),
    ("Intel", r"^Intel Corporat"),
    ("Espressif", r"^Espressif"),
    ("Raspberry Pi", r"^Raspberry Pi"),
    ("TP-Link", r"^TP-LINK|^TP-Link"),
    ("Netgear", r"^NETGEAR"),
    ("ASUS", r"^ASUSTek"),
    ("Ubiquiti", r"^Ubiquiti"),
    ("Cisco", r"^Cisco Systems|^Cisco Meraki|^Cisco SPVTG"),
    ("Linksys", r"^Cisco-Linksys|^Linksys|^Belkin International"),
    ("D-Link", r"^D-Link"),
    ("Huawei", r"^HUAWEI TECHNOLOGIES|^Huawei Device"),
    ("Xiaomi", r"^Xiaomi Communications|^Beijing Xiaomi"),
    ("Sony", r"^Sony (Corporation|Interactive|Mobile|Group)"),
    ("Nintendo", r"^Nintendo"),
    ("LG", r"^LG Electronics|^LG Innotek"),
    ("Sonos", r"^Sonos"),
    ("Roku", r"^Roku"),
    ("Philips Hue", r"^Philips Lighting|^Signify"),
    ("Dell", r"^Dell (Inc|Technologies)"),
    ("HP", r"^Hewlett Packard|^HP Inc"),
    ("Lenovo", r"^Lenovo|^LCFC\(HeFei\)"),
    ("Motorola", r"^Motorola Mobility"),
    ("OnePlus", r"^OnePlus"),
    ("OPPO", r"^GUANGDONG OPPO"),
    ("vivo", r"^vivo Mobile"),
    ("Realtek", r"^REALTEK|^Realtek"),
    ("MediaTek", r"^MediaTek"),
    ("Murata", r"^Murata Manufacturing"),
    ("AzureWave", r"^AzureWave"),
    ("Liteon", r"^Liteon|^LITE-ON"),
    ("Foxconn", r"^Hon Hai Precision"),
    ("Tuya", r"^Tuya Smart"),
    ("Synology", r"^Synology"),
    ("QNAP", r"^QNAP"),
    ("Western Digital", r"^Western Digital"),
    ("Brother", r"^Brother Industries"),
    ("Canon", r"^CANON INC|^Canon Inc"),
    ("Epson", r"^Seiko Epson"),
    ("Ring", r"^Ring LLC"),
    ("eero", r"^eero"),
    ("Arris", r"^ARRIS|^Arris|^CommScope"),
    ("Technicolor", r"^Technicolor|^Vantiva"),
    ("Sagemcom", r"^Sagemcom"),
    ("AVM", r"^AVM "),
    ("ZTE", r"^zte corporation|^ZTE"),
    ("MikroTik", r"^Routerboard\.com|^Mikrotikls"),
    ("Texas Instruments", r"^Texas Instruments"),
    ("Garmin", r"^Garmin"),
    ("Tesla", r"^Tesla"),
    ("Bose", r"^Bose Corporation"),
    ("Wyze", r"^Wyze Labs"),
    ("ecobee", r"^ecobee"),
    ("Vizio", r"^Vizio"),
    ("TCL", r"^TCL "),
    ("Hisense", r"^Hisense"),
    ("Panasonic", r"^Panasonic"),
    ("Nvidia", r"^NVIDIA"),
    ("Meta", r"^Facebook|^Meta Platforms|^Oculus"),
    ("VMware", r"^VMware"),
    ("Shelly", r"^Allterco|^Shelly"),
    ("Belkin", r"^Belkin"),
    ("MSI", r"^Micro-Star"),
    ("Gigabyte", r"^GIGA-BYTE"),
    ("ASRock", r"^ASRock"),
    ("Acer", r"^Acer"),
    ("Toshiba", r"^Toshiba"),
    ("Nokia", r"^Nokia"),
    ("Hikvision", r"^Hangzhou Hikvision|^Hikvision"),
    ("Ezviz", r"^Hangzhou Ezviz|^Ezviz"),
    ("Dahua", r"^Zhejiang Dahua|^Dahua"),
    ("Arlo", r"^Arlo"),
    ("iRobot", r"^iRobot"),
    ("Roborock", r"^Beijing Roborock|^Roborock"),
    ("Dyson", r"^Dyson"),
    ("Fitbit", r"^Fitbit"),
    ("GoPro", r"^GoPro"),
    ("DJI", r"^SZ DJI|^DJI"),
    ("Chamberlain", r"^Chamberlain"),
    ("Honeywell", r"^Honeywell|^Resideo"),
    ("Skyworth", r"^Shenzhen Skyworth|^Skyworth"),
    ("Zyxel", r"^Zyxel"),
    ("Tenda", r"^Tenda|^Shenzhen Tenda"),
    ("Arcadyan", r"^Arcadyan"),
    ("Vodafone", r"^Vodafone"),
    ("Sky", r"^SKY UK|^Sky CP"),
    ("Supermicro", r"^Super Micro"),
    ("Quectel", r"^Quectel"),
    ("Wistron", r"^Wistron"),
    ("Compal", r"^Compal"),
    ("Quanta", r"^Quanta"),
    ("USI", r"^Universal Global Scientific"),
    ("Gaoshengda", r"^Hui Zhou Gaoshengda"),
    ("AI-Link", r"^Sichuan AI-Link"),
    ("CE Link", r"^CE LINK"),
]

OUT = Path(__file__).resolve().parent.parent / "src" / "oui_table.cpp"


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    patterns = [(label, re.compile(pattern, re.I)) for label, pattern in VENDORS]
    entries = {}
    with open(sys.argv[1], encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row["Registry"] != "MA-L":
                continue
            name = row["Organization Name"].strip()
            for index, (_, pattern) in enumerate(patterns):
                if pattern.search(name):
                    entries.setdefault(int(row["Assignment"], 16), index)
                    break
    used = sorted({index for index in entries.values()})
    remap = {index: i for i, index in enumerate(used)}
    lines = [
        "// Generated by tools/make_oui.py from the IEEE registry - do not edit.",
        f"// {len(entries)} MAC address prefixes of {len(used)} common device makers.",
        '#include "oui_table.h"',
        "",
        "const char *const OUI_VENDORS[] = {",
    ]
    for index in used:
        lines.append(f'    "{VENDORS[index][0]}",')
    lines += ["};", "", "// Sorted by prefix: three bytes of prefix, then the index of the maker.",
              "const uint8_t OUI_TABLE[] = {"]
    row = []
    for prefix in sorted(entries):
        row.append(f"{prefix >> 16},{(prefix >> 8) & 255},{prefix & 255},{remap[entries[prefix]]}")
        if len(row) == 6:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines += ["};", "", f"const size_t OUI_COUNT = {len(entries)};"]
    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    counts = {}
    for index in entries.values():
        counts[VENDORS[index][0]] = counts.get(VENDORS[index][0], 0) + 1
    print(f"wrote {len(entries)} prefixes ({len(entries) * 4} bytes) of {len(used)} makers to {OUT}")
    print(", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))
    missing = [VENDORS[i][0] for i in range(len(VENDORS)) if i not in remap]
    if missing:
        print("no prefixes matched for:", ", ".join(missing))


if __name__ == "__main__":
    main()
