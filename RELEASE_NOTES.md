# Loudness Meter release notes

## v0.1.0

Loudness Meter is a transparent mono/stereo loudness meter: Momentary, Short-term, Integrated,
loudness range (LRA), sample peak and true peak, with a 10 minute history graph. Audio passes
through sample for sample, with zero added latency. It follows ITU-R BS.1770-5 and EBU Tech
3341/3342 but is not certified. It is an independent plug-in identity
(`com.c0dypeng.loudnessmeter`, `Cdpg` / `Ldmt`).

- Controls: Capture (Restart / Continuous, the only automated parameter) and the Reset, Pause and
  Start commands.
- Sample rates: 32, 44.1, 48, 88.2, 96, 176.4, 192, 352.8 and 384 kHz, mono or stereo. Other rates
  pass audio unchanged and show an UNSUPPORTED RATE fault.
- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built natively with MSVC (Visual Studio 2022) on windows-2022, where CTest
  passed 6/6. Requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- Checked before release: CTest 6/6 (two independent references and their cross-check, the
  loudness and true-peak cores, and the JUCE processor); editor snapshot rendered, with 18 hover
  help texts checked and none overflowing; AU and VST3 slices arm64 + x86_64; ad-hoc signatures,
  ZIP integrity and SHA-256 verified by the packaging script.

Known limits: not certified by EBU or ITU. macOS runtime has been checked on Apple Silicon only
and no DAW session has been run; the Windows VST3 has been built and tested but not loaded in a
host. True-peak accuracy is tested over a finite band (DC to the lower
of 20 kHz and 0.45 times the sample rate), not for every waveform. A host that gives no stop,
release or end callback leaves the final true-peak tail unmeasured. Capture stops at 24 hours.

Each ZIP includes installation instructions, dependency notices and build provenance, with a
SHA-256 checksum companion.
