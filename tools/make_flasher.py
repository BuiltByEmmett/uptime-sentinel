"""Collect release files and build the web flasher site.

usage:
  make_flasher.py collect BUILD_DIR DIST_DIR [ENV ...]
      Copy each environment's images out of PlatformIO's build folder as
      uptime-sentinel-<env>-full.bin    bootloader + partitions + app, flash at 0
      uptime-sentinel-<env>-update.bin  app only, for the dashboard's update page
      With no ENV given, every board in flasher/boards.json is collected.

  make_flasher.py site DIST_DIR SITE_DIR WEBTOOLS_DIR
      Write the GitHub Pages site: index.html, one ESP Web Tools manifest per
      board, and the -full.bin files they point to. WEBTOOLS_DIR is the
      unpacked esp-web-tools npm package; its browser build is copied into
      the site, so the page loads no script from anywhere else.

  make_flasher.py version
      Print the firmware version from src/config.h.
"""
import html
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "https://github.com/TradeSolana/uptime-sentinel"


def boards():
    with open(os.path.join(ROOT, "flasher", "boards.json"), encoding="utf-8") as f:
        return json.load(f)


def version():
    with open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8") as f:
        return re.search(r'VERSION\[\] = "([^"]+)"', f.read()).group(1)


def full_name(env):
    return "uptime-sentinel-%s-full.bin" % env


def update_name(env):
    return "uptime-sentinel-%s-update.bin" % env


def collect(build_dir, dist_dir, envs):
    os.makedirs(dist_dir, exist_ok=True)
    for env in envs or [b["env"] for b in boards()]:
        folder = os.path.join(build_dir, env)
        shutil.copyfile(os.path.join(folder, "firmware-full.bin"), os.path.join(dist_dir, full_name(env)))
        shutil.copyfile(os.path.join(folder, "firmware.bin"), os.path.join(dist_dir, update_name(env)))
        print("collected", env)


def site(dist_dir, site_dir, webtools_dir):
    os.makedirs(site_dir, exist_ok=True)
    served = os.path.join(site_dir, "esp-web-tools")
    shutil.rmtree(served, ignore_errors=True)
    shutil.copytree(os.path.join(webtools_dir, "dist", "web"), served)
    for name in ("LICENSE", "LICENSE.md"):
        if os.path.exists(os.path.join(webtools_dir, name)):
            shutil.copyfile(os.path.join(webtools_dir, name), os.path.join(served, name))
    ver = version()
    rows = []
    first = None
    for index, board in enumerate(boards()):
        env = board["env"]
        source = os.path.join(dist_dir, full_name(env))
        if not os.path.exists(source):
            sys.exit("missing " + source)
        shutil.copyfile(source, os.path.join(site_dir, full_name(env)))
        manifest = "manifest-%s.json" % env
        with open(os.path.join(site_dir, manifest), "w", encoding="utf-8", newline="\n") as f:
            json.dump({
                "name": "Uptime Sentinel",
                "version": ver,
                "new_install_prompt_erase": True,
                "builds": [{
                    "chipFamily": board["chipFamily"],
                    "parts": [{"path": full_name(env), "offset": 0}],
                }],
            }, f, indent=2)
            f.write("\n")
        first = first or manifest
        rows.append(
            '<label class="board"><input type="radio" name="board" value="%s"%s>'
            '<div><span>%s</span><small>%s</small><code>%s</code></div></label>'
            % (manifest, " checked" if index == 0 else "", html.escape(board["name"]),
               html.escape(board["detail"]), html.escape(env)))
    with open(os.path.join(ROOT, "flasher", "index.html"), encoding="utf-8") as f:
        page = f.read()
    page = (page.replace("{{BOARDS}}", "\n".join(rows)).replace("{{VERSION}}", html.escape(ver))
            .replace("{{FIRST_MANIFEST}}", first).replace("{{REPO}}", REPO))
    with open(os.path.join(site_dir, "index.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(page)
    # Pages would otherwise run the files through Jekyll.
    open(os.path.join(site_dir, ".nojekyll"), "w").close()
    print("site for version", ver, "with", len(rows), "boards in", site_dir)


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "collect":
        collect(args[1], args[2], args[3:])
    elif len(args) == 4 and args[0] == "site":
        site(args[1], args[2], args[3])
    elif args == ["version"]:
        print(version())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
