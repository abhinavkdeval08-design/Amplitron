# Amplitron Processing Agents & System Architecture

This document outlines the **Agent Architecture** within the Amplitron Guitar Amp Simulator. In the context of this high-performance DSP (Digital Signal Processing) pipeline, an "Agent" is defined as an autonomous, encapsulated component responsible for specific processing, coordination, or state management tasks. 

These agents operate concurrently across high-priority audio threads and UI threads, ensuring ultra-low latency (~1.3ms) and glitch-free real-time performance.

---

## 1. System-Level Coordination Agents

These are the core manager agents that oversee the lifecycle, data flow, and hardware interaction of the Amplitron ecosystem.

### 1.1 The Audio Engine Agent (`audio_engine.cpp`)
The master coordinator of the system. Operating at the highest system priority via PortAudio, this agent is responsible for the critical real-time audio callback loop.
* **Role:** Fetches raw mono float32 samples from the hardware input (USB interface/guitar cable), routes them sequentially through the active DSP agents, and pushes the processed frames to the output hardware.
* **Responsibilities:** * Auto-detecting input/output hardware.
  * Buffer size negotiation (32 to 512 samples) and sample rate enforcement (44.1kHz - 96kHz).
  * Enforcing safety clamps (hard limiting to ±1.0) to prevent hardware or auditory damage.
  * Handling lock-free or `try_lock` mutex polling to ensure the GUI thread never blocks the audio thread.

### 1.2 The GUI Manager Agent (`gui_manager.cpp`)
The user-facing orchestration agent built on SDL2 and Dear ImGui.
* **Role:** Translates human interaction into system state changes without interrupting the DSP pipeline.
* **Responsibilities:** Rendering the application window, parsing hardware input (mouse/keyboard), painting the pedal board visually, and updating the global state tree that the Audio Engine Agent consumes.

### 1.3 The Pedal Board Agent (`pedal_board.cpp`)
The state-manager agent for the signal chain.
* **Role:** Acts as a dynamic registry that maintains the ordered list of active DSP Pedal Agents.
* **Responsibilities:** Handling the insertion, deletion, reordering, and bypass toggling of effects. It safely mutates the chain state while communicating with the Audio Engine.

### 1.4 The Preset Manager Agent (`preset_manager.cpp`)
The persistence agent for signal chain configurations.
* **Role:** Serializes and deserializes the complete pedal chain state (effect types, parameter values, enabled states, ordering) to and from JSON files.
* **Responsibilities:** Saving presets, loading presets, creating effect instances by name during deserialization, and managing the presets directory. Integrates with the file dialog system for save/load UI.

### 1.5 The Command History Agent (`command_history.cpp`)
The undo/redo state-tracking agent.
* **Role:** Maintains a stack-based history of all user actions (parameter changes, pedal additions/removals, reordering) to enable full undo/redo functionality.
* **Responsibilities:** Recording commands, executing undo/redo, managing the history stack with proper cleanup when new actions branch off from a past state.

### 1.6 The Spectrum Analyzer Agent (`spectrum_analyzer.cpp`)
The frequency-domain visualization agent.
* **Role:** Performs real-time frequency analysis of the audio signal and renders a visual spectrum display in the GUI.
* **Responsibilities:** Accepting audio data from the lock-free SPSC queue, computing frequency bins, and rendering the spectrum graph.

### 1.7 The Recorder Agent (`recorder.cpp`)
The WAV recording agent.
* **Role:** Captures processed audio output and writes it to WAV files on disk.
* **Responsibilities:** Managing recording state (start/stop), writing WAV headers, buffering audio data, and flushing to disk.

---

## 2. DSP Node Agents (The Pedal Board)

Each effect pedal in Amplitron acts as an independent DSP processing agent. They receive an input buffer, apply mathematical transformations based on their internal state (knob values), and yield an output buffer.

### 2.1 Dynamic Range Agents
* **`NoiseGate` (The Gatekeeper Agent):** Uses envelope following to monitor signal amplitude. If the signal falls below a user-defined threshold, it silences the output to eliminate background hum, strictly respecting configured attack and release times to prevent unnatural audio chopping.
* **`Compressor` (The Dynamics Agent):** Monitors amplitude and applies gain reduction when the signal exceeds a threshold. It relies on internal calculation of ratios, attack/release ballistics, and makeup gain to squash dynamic peaks and sustain notes artificially.

### 2.2 Saturation & Harmonics Agents
* **`Overdrive` (The Tube-Sim Agent):** Simulates the soft, asymmetric clipping of analog vacuum tubes. It utilizes mathematical waveshaping (such as `tanh()` or polynomial functions) to introduce warm, even-order harmonic distortion.
* **`Distortion` (The Hard-Clip Agent):** Applies aggressive, hard-clipping algorithms to the waveform, shearing off the peaks of the audio signal to create dense, heavy harmonic saturation suitable for high-gain modern genres.

### 2.3 Frequency Shaping Agents
* **`Equalizer` (The Tone-Shaping Agent):** A 3-band parametric EQ utilizing active Biquad filters. This agent splits the signal into Low Shelf, Peaking (Mid), and High Shelf bands, allowing precise amplification or attenuation of specific frequency domains.
* **`AmpSimulator` (The Preamp Agent):** A full preamp model simulator with 4 selectable amp models (Clean American / Fender Twin, British Crunch / Marshall JCM800, High Gain Modern / Mesa Rectifier, Jazz Warm / Roland JC-120). Each model packages a characteristic tone-stack EQ curve (3 biquad filters), saturation transfer function (soft/hard clipping blend with asymmetry), and dynamic response (envelope follower with power sag simulation). Exposed parameters include Model, Gain, Bass/Mid/Treble trim, and Level.
* **`CabinetSim` (The IR/Speaker Agent):** Replicates the physical acoustic properties of a guitar speaker cabinet. It applies convolution or specialized one-pole filtering to strip away the harsh, "fizzy" high-end frequencies that raw distortion produces, making the signal sound as though it was recorded by a microphone in a physical room.

### 2.4 Time & Spatial Agents
* **`Chorus` (The Modulation Agent):** Duplicates the incoming signal and applies a low-frequency oscillator (LFO) to modulate the delay time of the duplicate. By using linear interpolation for fractional delay reads, it creates a thick, multi-instrument illusion.
* **`Delay` (The Echo Agent):** A digital ring-buffer agent that captures the signal and repeats it at specific time intervals. It manages an internal feedback loop, feeding a percentage of the output back into its input to create decaying echoes.
* **`Reverb` (The Spatial Agent):** Utilizes Schroeder reverb architecture. This complex agent runs 4 parallel comb filters feeding into 2 series allpass filters to simulate the thousands of overlapping acoustic reflections found in physical spaces (rooms, halls, caves).

### 2.5 Utility Agents
* **`TunerPedal` (The Pitch Detection Agent):** A chromatic tuner using the YIN pitch detection algorithm. Operates on a 4096-sample circular buffer (~85ms window at 48kHz), providing accurate fundamental frequency detection down to E2 (82.41Hz). Reports detected note name, octave, cent offset, and signal presence via atomic variables for thread-safe GUI display. Updates at ~15Hz to balance responsiveness and CPU usage.

---

## 3. Agent Communication & Concurrency Protocol

Because the UI Agent and the DSP Agents operate on entirely different threads (with vastly different priority levels), they must communicate carefully to avoid "Dropouts" (audio clicking/stuttering).

* **The `try_lock` Paradigm:** When the UI Agent attempts to modify a DSP Agent's state (e.g., turning a knob), the Audio Engine uses a non-blocking `try_lock` on the data mutex. If the UI is currently writing, the Audio Engine simply processes the buffer using the *previous* state rather than waiting. 
* **Parameter Smoothing:** DSP Agents utilize one-pole filters internally on their parameter inputs. If the UI Agent jumps a parameter from `0.1` to `0.9` instantly, the DSP Agent interpolates the value over several samples to prevent audible "zipper" noise or clicking.

---
**Maintained by:** [@sudip-mondal-2002](https://github.com/sudip-mondal-2002)
**Architecture Reference** — 11 DSP effects, 7 system agents