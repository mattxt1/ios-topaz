# UI candidates withheld from the default phone-tools build

`rounded-plates.patch` stages the small RGBA folder/Search fallback plates and
one-shot format diagnostics investigated on October28. The user confirmed the
folder improvement; revised Search is unverified. Applying it and respringing
also reproduced pink live apps, even without the earlier glyph experiment.
The root cause is not established. Do not automatically deploy this candidate.

The gray-alpha CALayer contents experiment was removed, and is not in this
patch. SF-symbol source assets were present with valid alpha in exported PNGs;
the GLES display path remains under investigation.

The default cgprobe.c has been restored to the exact source from before this
investigation. Device libcgprobe is likewise restored to its pre-turn binary.
Future work should isolate respring/scene/surface lifecycle on a controlled
reproducer before reintroducing these candidates to the system UI.
