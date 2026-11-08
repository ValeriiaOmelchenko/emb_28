# Ціль "pio run -t sysview": знімає трасування SystemView з ESP32-S3 через
# вбудований USB-JTAG і зберігає trace.SVDat (обидва ядра в одному файлі).
#
# Повідомлення в консоль - англійською: кирилиця в консолі Windows ламається.
#
# Що відбувається:
#   1. запускаємо OpenOCD з board/esp32s3-builtin.cfg і скидаємо плату
#      (reset run), щоб запис почався з чистого буфера трасування;
#   2. через Tcl-порт OpenOCD (6666) даємо команду
#      "esp sysview_mcore start file://trace.SVDat" - офіційний мультиядерний
#      формат SystemView 3.60+;
#   3. чекаємо custom_sysview_seconds секунд, зупиняємо запис і OpenOCD.
#
# Версія OpenOCD важлива. PlatformIO ставить v0.11.0-esp32-20220706 - він не
# розуміє пакети SystemView з ESP-IDF 5.5 ("Unsupported predef event") і пише
# старий формат, який SystemView 4.x не відкриває (помилка 140). Тому беремо
# версію, яку рекомендує сам ESP-IDF 5.5.3 (tools/tools.json):
#   v0.12.0-esp32-20251215, шлях задає custom_sysview_openocd у platformio.ini.

Import("env")  # noqa: F821 (SCons)

import os
import socket
import subprocess
import time

TCL_PORT = 6666
TCL_END = b"\x1a"
TRACE_FILE = "trace.SVDat"
OPENOCD_URL = ("https://github.com/espressif/openocd-esp32/releases/download/"
               "v0.12.0-esp32-20251215/openocd-esp32-win64-0.12.0-esp32-20251215.zip")


def _tcl(sock, cmd):
    sock.sendall(cmd.encode() + TCL_END)
    reply = b""
    while not reply.endswith(TCL_END):
        chunk = sock.recv(4096)
        if not chunk:
            break
        reply += chunk
    return reply.rstrip(TCL_END).decode(errors="replace").strip()


def _connect(proc, timeout_s=15.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            return None
        try:
            return socket.create_connection(("127.0.0.1", TCL_PORT), timeout=5)
        except OSError:
            time.sleep(0.3)
    return None


def sysview(target, source, env):  # noqa: ARG001 (сигнатура SCons)
    seconds = int(env.GetProjectOption("custom_sysview_seconds", "10"))
    ocd_dir = os.path.expandvars(os.path.expanduser(
        env.GetProjectOption("custom_sysview_openocd", "")))
    ocd_exe = os.path.join(ocd_dir, "bin", "openocd.exe" if os.name == "nt" else "openocd")
    if not os.path.isfile(ocd_exe):
        print("OpenOCD not found: %s\n"
              "Download %s\n"
              "and unpack it so that the path above exists "
              "(or fix custom_sysview_openocd in platformio.ini)." % (ocd_exe, OPENOCD_URL))
        return 1

    project_dir = env.subst("$PROJECT_DIR")
    trace_path = os.path.join(project_dir, TRACE_FILE)
    if os.path.exists(trace_path):
        try:
            os.remove(trace_path)
        except OSError:
            print("Cannot overwrite %s: close it in SystemView first." % trace_path)
            return 1

    cmd = [
        ocd_exe,
        "-s", os.path.join(ocd_dir, "share", "openocd", "scripts"),
        "-f", "board/esp32s3-builtin.cfg",
    ]
    print("OpenOCD:", " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=project_dir)

    try:
        sock = _connect(proc)
        if sock is None:
            print("\nOpenOCD did not start. Check that the cable is in the \"USB\" port (not \"UART\")\n"
                  "and that \"USB JTAG/serial debug unit\" uses the WinUSB driver.")
            return 1

        with sock:
            # Скидаємо плату, щоб буфер трасування на ній був порожній: залишки
            # попередньої сесії дають "sysview: Invalid init seq" і порожній файл.
            _tcl(sock, "reset run")
            time.sleep(1.5)
            print(_tcl(sock, "esp sysview_mcore start file://" + TRACE_FILE))
            print("Recording %d s..." % seconds)
            time.sleep(seconds)
            print(_tcl(sock, "esp sysview_mcore stop"))
            _tcl(sock, "shutdown")
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    # OpenOCD пише проміжні файли по ядрах і склеює їх у trace.SVDat.
    for suffix in ("_core0", "_core1"):
        part = trace_path + suffix
        if os.path.exists(part):
            os.remove(part)

    if os.path.exists(trace_path) and os.path.getsize(trace_path) > 0:
        print("\nDone: %s (%d bytes). Open it in SystemView 3.60+: File > Load Data."
              % (trace_path, os.path.getsize(trace_path)))
        return 0
    print("\nTrace file was not created, see OpenOCD output above.")
    return 1


env.AddCustomTarget(  # noqa: F821
    name="sysview",
    dependencies=None,
    actions=sysview,
    title="SystemView trace",
    description="Record trace.SVDat via USB-JTAG (OpenOCD)",
    always_build=True,
)
