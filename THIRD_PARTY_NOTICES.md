# Third-party notices

Loudness Meter is built with JUCE 8.0.9 and includes the Inter SemiBold and JetBrains Mono Medium
font files. Its VST3 build uses the VST3 SDK shipped with that JUCE checkout; its Audio Unit build
also uses JUCE's bundled AudioUnitSDK. The build copies the applicable upstream notices into each
plug-in bundle under `Contents/Resources/Notices/`:

- `Inter-OFL.txt` — SIL Open Font License 1.1 for Inter SemiBold.
- `JetBrainsMono-OFL.txt` — SIL Open Font License 1.1 for JetBrains Mono Medium.
- `JUCE-LICENSE.md` — JUCE's own licence and dependency notice from the exact 8.0.9 checkout.
- `VST3-SDK-LICENSE.txt` and `VST3-License-Agreement.pdf` — the licence notice and agreement from
  the VST3 SDK in that checkout, present in the VST3 bundle.
- `AudioUnitSDK-LICENSE.txt` — the Apache 2.0 text for AudioUnitSDK, present only in the AU bundle.

Those copied upstream files are the licence texts; this summary does not replace or reinterpret
them. This repository's own source code is licensed under GPL-3.0; see `LICENSE`. Binary release
packages also carry the notices under `licenses/`.
