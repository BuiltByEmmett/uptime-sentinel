# PlatformIO post-build step: after firmware.bin is built, also write
# firmware-full.bin, one file holding the bootloader, partition table and
# application at their flash offsets. It is flashed at offset 0 and is what
# the web flasher and a plain esptool install use. firmware.bin on its own is
# the application only, which is what the dashboard's update page takes.
#
# The flash mode, frequency and size are the same ones "pio run -t upload"
# passes to esptool, so both ways of flashing put identical bytes on the chip.
Import("env")  # noqa: F821  (provided by PlatformIO)


def merge(source, target, env):
    board = env.BoardConfig()
    images = []
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        images += [offset, '"%s"' % env.subst(path)]
    images += [env.subst("$ESP32_APP_OFFSET"), '"%s"' % env.subst("$BUILD_DIR/${PROGNAME}.bin")]
    command = [
        '"$PYTHONEXE"', '"$OBJCOPY"',
        "--chip", board.get("build.mcu", "esp32"),
        "merge_bin",
        "-o", '"%s"' % env.subst("$BUILD_DIR/${PROGNAME}-full.bin"),
        "--flash_mode", "${__get_board_flash_mode(__env__)}",
        "--flash_freq", "${__get_board_f_image(__env__)}",
        "--flash_size", board.get("upload.flash_size", "4MB"),
    ] + images
    return env.Execute(" ".join(command))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge)  # noqa: F821
