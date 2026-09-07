# Chromatic Visual Backup

Export files from an EverDrive GB X-series microSD card through a ModRetro Chromatic's **stock USB video output**, then reconstruct and CRC-verify them on Android. No custom Chromatic firmware or FPGA compiler is required.

A Game Boy ROM reads a selected file and displays a repeating black-and-white data grid. The Android app captures the normal UVC video stream, assembles numbered blocks, and verifies the received file against a source CRC32 sent through that same video stream.

## Status

- The original demo transfer has been confirmed working on physical Chromatic/Android hardware by the project owner.
- Current ROMs include an SD-card directory browser and faster rendering/CRC calculation. The current demo measures 12.5 distinct frames/s in PyBoy at normal CGB CPU speed, versus a target cap of about 15 frames/s. Physical SD reads can lower the rate.
- The updated directory browser was exercised using the compiled ROM and a local FAT32 replay image: root paging, nested folders, selected-file checksum, transmission, cancel and parent navigation.
- Android includes a dark theme and system-bar/display-cutout insets.
- The updated ROM/app combination still needs hardware confirmation. CRC verification detects transfer corruption; it does not establish that a source save was valid before export.

## Requirements

- ModRetro Chromatic with its standard USB video firmware.
- EverDrive GB X-series cartridge. Development and hardware confirmation have used an X7; other models are not independently verified.
- SDv2 card formatted FAT32. The browser uses FAT **8.3 short names**, such as `TETR~001.SAV`, not long filenames.
- Android 7.0+ device with USB host support and a data-capable USB-C connection. The APK contains arm64-v8a and x86_64 native libraries.

## Use

1. Flush any pending EverDrive save using its normal menu before powering off. Keep an independent backup of important saves.
2. Copy `build/x7-visual.gb` onto the microSD using a card reader. Install `android/build/ChromaticVisual.apk` on Android.
3. Launch the ROM on the X7. **START** mounts the card; no file is automatically transmitted.
4. Browse from the SD root:
   - **Up/Down:** select an entry; moving past a page edge changes pages.
   - **Left/Right:** previous/next page (ten entries per page).
   - **A:** enter a directory or calculate a selected file's CRC.
   - **B:** parent directory; at the root, remain at the root.
   - `+` marks a directory. Hidden/system entries may also be listed.
5. Connect the Chromatic directly to the phone. Close other apps using its USB video interface. Tap **Receive stock USB video** and grant permissions. Android requires camera permission to open a USB video device; this app does not capture from the phone's camera.
6. At the ROM's CRC screen, press **A** to broadcast. The complete sequence repeats. **B** returns to the browser.
7. Wait for **CRC VERIFIED** in the app. Stop the ROM manually, then tap **Save verified file**. The app reads the destination back and checks its length and CRC again.
8. Start a new receive operation before broadcasting a different file. A conflicting manifest is an error, not an automatic switch.

For a first connection check, `build/visual-demo.gb` broadcasts a deterministic 1,021-byte `DEMO.BIN` after **A**. Its CRC32 is `19C82313`. The demo performs no SD file access.

**Decode video recording** imports a saved recording through Android's file picker. It expects the source image to fill a centered 10:9 rectangle, optionally letterboxed. Sampling is best-effort; a recording missing required blocks cannot complete. Live stock USB capture is preferred, especially at the faster sender cadence. Arbitrary camera perspective, cropping and rotation are not supported.

## Safety and limitations

- Export only. Restore uses a separate SD reader; the ROM does not implement SD data writes.
- The X7 reader does write control registers to unlock/select its SD interface and issue read commands. This is not a claim of zero cartridge-bus writes.
- The app uses standard video negotiation and capture. It does not select the experimental CDC mailbox, toggle DTR/RTS, load firmware, or issue cartridge commands.
- Maximum exported file: **16 MiB**. Directory paths: **255 bytes** in short-name form. Oversized selections/path entries report a message rather than silently truncate.
- CRC32 is accidental-corruption detection, not authentication, encryption, or a collision-proof guarantee. The source CRC is computed afresh before each selected-file transfer. Anyone who captures the video can decode its contents.
- Missing frames are recovered through repetition. No completion or throughput guarantee: missing blocks, conflicting duplicates, wrong length, or CRC mismatch prevent acceptance.

## Build

### ROMs

Install [GBDK-2020 4.5.0](https://github.com/gbdk-2020/gbdk-2020/releases). Its compiler tools are not vendored.

```sh
make GBDK_HOME=/path/to/gbdk
```

Outputs:

- `build/x7-visual.gb` — SD browser and file sender (64 KiB).
- `build/visual-demo.gb` — no-SD transfer demo (32 KiB).

An older read-only checksum diagnostic remains available:

```sh
make GBDK_HOME=/path/to/gbdk SNAP_DIRECTORY=GBCSYS/SNAP build/x7-read-diagnostic.gb
```

`SNAP_DIRECTORY` affects only that diagnostic. The visual sender always browses from the root.

### Android

Install Python 3, JDK 11+ (built with JDK 21), and these Android SDK packages:

```sh
sdkmanager 'platforms;android-35' 'build-tools;35.0.0' 'ndk;28.2.13676358' 'platform-tools'
export ANDROID_HOME=/path/to/android-sdk
python3 android/build.py
```

macOS and Linux build hosts are supported. The standalone build invokes `javac`, `aapt2`, `d8`, NDK clang, `zipalign`, and `apksigner`; no Gradle is required. A local debug signing key is generated in `android/build/` and excluded from Git. Keep that key locally to install updates over an existing APK signed with it.

```sh
adb install -r android/build/ChromaticVisual.apk
```

No command above flashes the Chromatic. Install commands address the currently selected Android device; use `adb -s SERIAL` when multiple devices are connected.

## Source map

- `src/visual_sender.c` — browser, source checksum and repeating file transmission.
- `src/visual_demo.c` — deterministic transport demo.
- `src/visual_grid.c` — 4×4-pixel cells, packed tile generation, VBlank map swaps and pacing.
- `src/x7_disk.c` — SD read path and X7 control-register access.
- `src/checksum.c` — CRC-32/ISO-HDLC, table-driven.
- `android/src/dev/borges/chromaticproof/` — activity, grid decoding, staged-file verification and USB capture ownership.
- `android/native/uvc_capture.c` — Linux usbdevfs isochronous UVC/YUY2 capture through Android's permission-granted descriptor.
- [`visual-protocol.json`](visual-protocol.json) — wire layout and receiver rules.

The screen has 40×36 cells. A 36×32 inner rectangle carries a 144-byte frame: 20 header bytes, 120 payload bytes, and a 4-byte frame CRC. The manifest carries filename, length, and the source file CRC. Data frames identify their file by size plus CRC, assuming a single selected file is broadcasting. They are not collision-resistant session identifiers.

## Third-party notices

Petit FatFs is copyright © 2019 ChaN; its redistribution terms remain in the source headers. The X7 SD driver derives from [untoxa/VGM_player](https://github.com/untoxa/VGM_player), copyright © 2024 Toxa, under the [included MIT license](vendor/petitfatfs/DRIVER-LICENSE). Exact provenance and local changes are recorded in [`vendor/petitfatfs/provenance.json`](vendor/petitfatfs/provenance.json).

This project is not affiliated with ModRetro or Krikzz. Game ROMs, cartridge dumps, save files, toolchains, debug keys, and abandoned custom-FPGA experiments are deliberately excluded from the repository.
