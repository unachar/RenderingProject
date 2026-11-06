# Audio authoring implementation plan

**Spec** [Audio assets and playback engine](../specs/2026-11-04-audio-authoring-design.md)

- [ ] Replace the live music voice with a shared miniaudio device and streaming playback. Use the same float PCM decoder for the waveform worker and Audio Spectrum. Build the Windows target, then verify old scene transport and sample-clock synchronization.
- [ ] Add the persisted audio asset, dedicated waveform editor, preview and mixer controls. Connect Asset Browser import/open and verify save, reopen and audible parameter changes.
- [ ] Add persistent Transform-based scene emitters, Inspector editing and runtime play/stop. Verify placement, listener motion and scene/Prefab reload.
- [ ] Add persisted Sequencer lanes and audio clips. Use PCM-frame reservations, invalidate them on transport changes, and verify overlap, seeking, loops and session transitions in the Windows application.

Use the existing clock and timeline tests. Do not add test code. Review UTF-8 asset paths, invalid media, preview/runtime ownership and teardown, older scene data, and timing across discontinuities.
