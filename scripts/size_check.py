Import("env")

def size_check(source, target, env):
    import os, sys
    elf = str(target[0])
    # Read .map file produced by linker
    map_file = elf.replace(".elf", ".map")
    if not os.path.exists(map_file):
        return

    # Parse BSS + DATA section sizes from map
    bss = 0; data = 0
    with open(map_file) as f:
        for line in f:
            if ".bss " in line and "0x" in line:
                parts = line.split()
                if len(parts) >= 3:
                    try: bss += int(parts[2], 16)
                    except: pass
            if ".data " in line and "0x" in line:
                parts = line.split()
                if len(parts) >= 3:
                    try: data += int(parts[2], 16)
                    except: pass

    RAM_TOTAL = 327680  # ESP32 DRAM: 320 KB
    used_pct  = ((bss + data) / RAM_TOTAL) * 100

    print(f"\n[LumaRoK] BSS={bss:,}  DATA={data:,}  "
          f"Total RAM static={bss+data:,} ({used_pct:.1f}%)")

    THRESHOLD = 75.0
    if used_pct > THRESHOLD:
        print(f"[LumaRoK] ERROR: Static RAM {used_pct:.1f}% > {THRESHOLD}% limit")
        env.Exit(1)
    else:
        print(f"[LumaRoK] RAM check OK ({used_pct:.1f}% < {THRESHOLD}%)")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", size_check)
