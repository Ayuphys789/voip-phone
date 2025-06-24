# 📞 VoIP Phone - C Implementation

[日本語版 README](https://github.com/Ayuphys789/voip-phone/blob/main/README-ja.md)

This is a high-performance VoIP (Voice over IP) phone application built from the ground up in C, based on the GTK4, PortAudio, and SpeexDSP libraries. It provides low-latency, high-quality, bidirectional audio calls using a P2P (peer-to-peer) architecture.

This project implements the core technical elements required for real-time voice communication—including low-latency I/O, network packet handling, and acoustic signal processing—on a robust multi-threaded architecture.

---

## ✨ Technical Specifications

This application implements the following features to ensure a comfortable calling experience:

* **GUI (GTK4):**
  * Provides a basic user interface for entering peer connection details and controlling the call state.

* **Cross-Platform Audio I/O (PortAudio):**
  * Abstracts the OS audio subsystem to achieve low-latency audio input and output.
  * Employs a callback-based processing model, which forms the foundation for real-time audio processing.

* **Real-Time Signal Processing:**
  * **Acoustic Echo Cancellation (AEC):** Utilizes the SpeexDSP library (`libspeexdsp`) to suppress acoustic echo with an adaptive filter.
  * **Noise Gate:** Reduces steady-state background noise by calculating the RMS (Root Mean Square) of the microphone input signal and silencing signals below a configurable threshold.
  * **Gain Control:** Adjusts the volume of the outgoing audio by applying a linear gain factor, including saturation logic to prevent clipping.
  * **Level Meter:** Visualizes the RMS level of the microphone input via a GUI progress bar.

* **Network Protocol (UDP):**
  * Uses UDP as the transport layer protocol to achieve low-latency data transfer.
  * Transmitted audio data is encapsulated in a custom `AudioPacket` struct that includes a sequence number.

* **Communication Quality Assurance:**
  * **Jitter Buffer:** Implements a fixed-size jitter buffer to absorb network delay variations, reordering packets based on sequence numbers to smooth out playback timing.
  * **Packet Loss Concealment (PLC):** Implements basic PLC by inserting silence when an expected packet is missing in the jitter buffer, thus mitigating audio dropouts.

* **Auxiliary Features:**
  * **Call Timer:** Displays the elapsed call duration, triggered by the reception of the first packet from the peer.

---

## 📦 Dependencies & Build Environment

Building this application requires the development packages for the following libraries:

| Library | Package Name (Ubuntu/Debian) | Package Name (macOS/Homebrew) | Role | 
| :--- | :--- | :--- | :--- | 
| **GTK4** | `libgtk-4-dev` | `gtk4` | GUI Toolkit | 
| **PortAudio** | `portaudio19-dev` | `portaudio` | Audio I/O | 
| **SpeexDSP** | `libspeexdsp-dev` | `speexdsp` | AEC / Audio Processing | 
| **(Build Tools)** | `build-essential` | `pkg-config` | Compilation Tools | 

#### Example Installation Commands

**On Ubuntu / Debian-based systems:**
```bash
sudo apt-get update
sudo apt-get install build-essential libgtk-4-dev portaudio19-dev libspeexdsp-dev
```

**On macOS (using Homebrew):**
```bash
brew install gtk4 portaudio speexdsp pkg-config
```

---

## 🛠️ Build Instructions

With the source code (`src/voip_phone.c`) and all dependencies in place, you can build the application using the provided `Makefile`.
```bash
make
```
This command will generate an executable file at `bin/voip_phone`.

*(Alternatively, to compile manually, first ensure the `bin` directory exists and then run the command below.)*
```bash
mkdir -p bin
gcc src/voip_phone.c -o bin/voip_phone `pkg-config --cflags --libs gtk4 speexdsp` -pthread -lportaudio -lm
```
---

## 📂 Repository Structure

```
voip-phone/
├── .gitignore
├── LICENSE
├── Makefile
├── README.md
├── README-ja.md
│
├── bin/
│   └── voip_phone
│
└── src/
    └── voip_phone.c
```

---

## 💻 Architecture

This system employs a multi-threaded architecture to separate concerns and ensure real-time performance.

#### 5.1. Core Threads

* **Main Thread (GUI Thread):**
  Runs the GTK main loop, responsible solely for handling UI events and drawing. It performs no blocking operations to maintain responsiveness.

* **PortAudio Callback Thread:**
  A high-priority, real-time thread managed by the PortAudio library, invoked periodically by the audio device. It executes the audio processing pipeline (AEC, noise gate, gain) and interfaces with other threads via buffers. Heavy processing and blocking I/O are strictly forbidden in this thread.

* **Network I/O Threads:**
  Implemented as `sender_thread_func` and `receiver_thread_func`, these are low-priority background threads dedicated to UDP socket I/O.

#### 5.2. Data Flow and Buffering

* **Transmission Path:**
  `Microphone` → `PortAudio Callback` (AEC/DSP) → `Send Ring Buffer` (Thread-safe) → `Sender Thread` → `UDP Transmit`

* **Reception Path:**
  `UDP Receive` → `Receiver Thread` → `Jitter Buffer` (Reordering/Smoothing) → `PortAudio Callback` → `Speakers`

This asynchronous design decouples the responsive UI from the real-time audio I/O and the potentially blocking network I/O.

---

## 🔧 Key Functions and Data Structures

#### 6.1. Data Structures

* `AppState`: The central struct managing the application's global state. It holds all shared resources, including pointers to GUI widgets, socket descriptors, library handles, and shared flags, and is passed to callbacks and threads as `user_data`.

* `AudioPacket`: The basic unit of data transmitted over UDP, encapsulating a 32-bit sequence number and a fixed-size array of PCM samples.

* `JitterBuffer`: Used on the receiving end to reorder packets based on sequence numbers and regulate playback timing. Implements priming (waits for a minimum number of packets before starting playback) and basic packet loss concealment (inserts silence).

* `RingBuffer`: A simple circular buffer used on the sending end to decouple the PortAudio callback thread (producer) from the network sender thread (consumer).

#### 6.2. Key Functions

* `pa_callback()`: The core of the audio processing, invoked by PortAudio. It handles both the input buffer from the microphone and the output buffer to the speakers. All DSP tasks, such as AEC, noise gating, and gain adjustment, are performed here.

* `sender_thread_func()`: A loop that reads audio data from the send ring buffer, encapsulates it into an `AudioPacket`, and sends it via UDP.

* `receiver_thread_func()`: A loop that receives an `AudioPacket` from the UDP socket and places it into the jitter buffer.

* `on_call_button_clicked()`: The setup sequence for initiating a call. It creates sockets, initializes PortAudio and SpeexDSP, and spawns the network threads.

* `on_hangup_button_clicked()`: The shutdown sequence for ending a call. It triggers the thread termination flag (`is_running`) and safely releases all allocated resources (sockets, PortAudio, SpeexDSP, buffers).

---

## 📜 License

This project is released under the [MIT License](LICENSE).

---

## 👋 Author

Satoshi Miyake ([Ayuphys789](https://github.com/Ayuphys789)),
Masahiro Muranaka
