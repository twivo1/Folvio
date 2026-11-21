# Installing Folvio on Linux

This guide is for people who just want to use Folvio — no programming tools are needed.
Short version: [README → Installation](../README.md#installation).

## 1. Which file do I need?

| Your system | Recommended | Alternative |
|---|---|---|
| Linux Mint 22.x (Wilma, Xia, Zara) | `folvio_amd64.deb` | AppImage |
| Ubuntu 24.04 LTS, Ubuntu 26.04 LTS | `folvio_amd64.deb` | AppImage |
| Fedora 40 or newer and other x86_64 distributions with glibc 2.39+ | AppImage | — |
| Linux Mint 21.x, Ubuntu 22.04 and older | **not supported** (see §6) | — |

Not sure which version you have? Open a terminal and run `cat /etc/os-release` (or, on Mint,
*Menu → System Reports → System Information*).

All downloads: <https://github.com/twivo1/Folvio/releases>. The file names with a version number
(`Folvio-0.10.1-Ubuntu-amd64.deb`, `Folvio-0.10.1-Linux-x86_64.AppImage`) are the same files as
`folvio_amd64.deb` and `Folvio-x86_64.AppImage`; `SHA256SUMS.txt` lists their checksums.

## 2. Install the .deb package (Linux Mint, Ubuntu)

### With the mouse

1. Download `folvio_amd64.deb` from the [latest release](https://github.com/twivo1/Folvio/releases/latest).
2. Double-click the downloaded file. Linux Mint opens it in its package installer; on Ubuntu use right-click →
   *Open With* → *App Center*. (Which program handles `.deb` files depends on the desktop. If nothing offers
   to install it, use the terminal command below.)
3. Click **Install** and enter your password.
4. Start **Folvio** from the application menu (category *Office*).

### With one terminal command

```bash
wget -O /tmp/folvio.deb https://github.com/twivo1/Folvio/releases/latest/download/folvio_amd64.deb && sudo apt install /tmp/folvio.deb ffmpeg tesseract-ocr libreoffice-writer libreoffice-calc libreoffice-impress
```

What it does: downloads the official package from GitHub, then `apt` installs it with all the libraries it
needs and the three conversion engines (FFmpeg, Tesseract, LibreOffice). Answer `Y` when `apt` asks.

* `N: Download is performed unsandboxed as root…` is only a notice from `apt` and can be ignored.
* Folvio is installed system-wide but always runs as your normal user — never start it with `sudo`.
* Leave out the engine names (`ffmpeg tesseract-ocr libreoffice-…`) if you only need PDF editing and Combine.

### Remove

```bash
sudo apt remove folvio
```

Your settings, history and backups in `~/.local/share/Folvio` are kept (delete that folder yourself if you
want them gone).

## 3. Run the AppImage (no installation)

```bash
wget -O ~/Folvio.AppImage https://github.com/twivo1/Folvio/releases/latest/download/Folvio-x86_64.AppImage && chmod +x ~/Folvio.AppImage && ~/Folvio.AppImage
```

Without a terminal: download `Folvio-x86_64.AppImage`, right-click it → *Properties* → *Permissions* → tick
*Allow executing file as program* (wording differs slightly between desktops), then double-click it.

* The AppImage needs **FUSE 3** (`fusermount3`, package `fuse3`), which Linux Mint 22 and Ubuntu 24.04 install by
  default. It does **not** need `libfuse2` / `libfuse2t64`.
* Everything Folvio needs is inside the file except the base libraries every desktop has (graphics, fonts,
  GLib, D-Bus, libtiff, X11/Wayland).
* The conversion engines are not inside — install them as in §4.
* To "uninstall", delete the file. Settings are in `~/.local/share/Folvio`.

## 4. Conversion engines

| Works right after installation | Needs |
|---|---|
| PDF text editing (Auto Change), Combine (photos → PDF, perspective, annotations), PDF ↔ PNG/JPG, image formats, Markdown/HTML/CSV/JSON, Polish and English OCR data | — |
| Audio and video (MP4 → MP3, …), HEIC and AVIF photos | **FFmpeg** |
| DOCX, ODT, RTF, XLSX, PPTX, PDF → DOCX | **LibreOffice** |
| OCR (searchable text for scanned PDFs) | **Tesseract** |

```bash
sudo apt install ffmpeg tesseract-ocr libreoffice-writer libreoffice-calc libreoffice-impress   # Mint, Ubuntu
sudo dnf install ffmpeg-free tesseract libreoffice-writer libreoffice-calc libreoffice-impress  # Fedora
```

`tesseract-ocr-pol` and `tesseract-ocr-eng` are optional: Folvio ships its own Polish and English data.
After installing an engine, open *Settings → Engines → Detect again* in Folvio (or restart it).
*Settings → Install missing engines…* shows the exact command for your distribution.

## 5. Check that everything works

```bash
folvio --selftest                    # .deb
~/Folvio.AppImage --selftest         # AppImage
```

It runs real operations (Combine 3 images → PDF, PDF edit and save, conversions, and — if installed — MP4 → MP3,
DOCX ↔ PDF and OCR) in a temporary folder and prints `PASS`/`SKIP`/`FAIL` for each. `SKIP … is not installed`
means only that the engine is missing. Your own files and settings are not touched.

## 6. Troubleshooting

**`apt` says: `folvio : Depends: libc6 (>= 2.38) but 2.35… is to be installed`**
or the AppImage says: ``version `GLIBC_2.38' not found``
Your system is older than Folvio supports (this is what Linux Mint 21.3 and Ubuntu 22.04 report). Nothing was
installed. Upgrade to Linux Mint 22 / Ubuntu 24.04. A build for the older systems is not available yet.

**AppImage: `fuse: device not found, try 'modprobe fuse' first` / `Cannot mount AppImage, please check your FUSE setup.`**
FUSE is missing or not available (common in containers and some minimal installations). Either run
`sudo apt install fuse3`, or start Folvio without FUSE:

```bash
~/Folvio.AppImage --appimage-extract-and-run
```

**A conversion is missing or greyed out.** The engine for it is not installed — see §4, then
*Settings → Detect again*.

**OCR does not work.** Install `tesseract-ocr` (§4). The language data are already in Folvio.

**Folvio does not appear in the menu after installing the .deb.** Start it with `folvio` in a terminal; the
menu usually refreshes after logging out and back in.

**`Could not load the Qt platform plugin "xcb"` or `"wayland"`.** Please run the self-test (§5) and open an
[issue](https://github.com/twivo1/Folvio/issues) with its output and your `cat /etc/os-release`. As a
workaround on Wayland desktops, `QT_QPA_PLATFORM=xcb folvio` uses the X11 path through XWayland.

**The download command fails with `404`.** The release may still be in progress; download the files from the
[Releases page](https://github.com/twivo1/Folvio/releases) instead.

## 7. What was tested

Folvio 0.10.1 packages were installed and self-tested before the release (all tests run as a normal user):

| System | Package | Result |
|---|---|---|
| Linux Mint 22.2 — official `linuxmintd/mint22.2-amd64` container (Ubuntu 24.04 base + Mint 22.2 repositories) | .deb with engines, .deb without engines, AppImage | installed, menu entry and icon, self-test passed, window rendered, `apt remove` kept user data |
| Linux Mint 22.1 — official container | .deb with engines | installed, self-test passed |
| Linux Mint 21.3 — official container | .deb, AppImage | refused with the messages above (not supported) |
| Ubuntu 24.04, Ubuntu 26.04, Fedora 43 — clean containers | .deb / AppImage | self-test passed |
| Ubuntu 26.04 on WSLg (Wayland and X11) | AppImage (with FUSE) | started directly, self-test passed |

These are containers with an X server, not full desktop installations; the double-click installation in the
Mint and Ubuntu file managers was not tested automatically.
