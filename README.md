# OS-Max : an HFT Signal Engine

OS-Max is the idea of making the most out of my current setup, more than aware that has no competition with production-scale real-time low-latency, High-Frequency Trading (HFT) Signal Engines but serves as a toy project to brush up my C++20 meanwhile making few tweaks for OS optimization. This engine evaluates real-time market data to generate trading signals and features a decoupled architecture using lock-free Single-Producer Single-Consumer (SPSC) queues for inter-thread communication.

Telemetry is broadcasted via ZeroMQ to a real-time Python Terminal User Interface (TUI) dashboard.

## Features

- **Lock-Free Concurrency**: Strict acquire/release memory semantics for thread-safe `SPSCQueue` communication without mutex overhead.
- **CPU Core Pinning**: Dedicates specific CPU cores to the Feed and Signal threads to prevent context-switching latency.
- **ZeroMQ Telemetry**: Non-blocking `PUB` socket for streaming ticks, bars, signals, and engine statistics.
- **Dynamic Risk Configuration**: Pluggable strategies with configurable minimum signal strengths, stop-loss, and take-profit multipliers.
- **Hardware/OS Tuning script**: Provided `run.sh` script handles CPU performance governors, C-states, IRQ affinity, and NUMA node bindings.
- **Python Rich Dashboard**: Live terminal UI to monitor market data, latency metrics, and recent trade signals.

## Architecture

The engine is composed of three primary threads:

1. **Feed Thread (Pinned to Core 2)**
   - Simulates/ingests market data (Ticks) and updates the internal OrderBook.
   - Pushes tick data to the Signal Thread via a lock-free queue.

2. **Signal Thread (Pinned to Core 3)**
   - Consumes ticks and aggregates them into OHLCV Bars based on time/volume.
   - Calculates mathematical indicators (EMA, RSI, VWAP, ATR) using `IndicatorEngine`.
   - Evaluates the strategy logic (e.g., `SwingMomentumStrategy`).
   - Pushes generated `Signal` objects and telemetry events downstream.

3. **Telemetry Thread**
   - Consumes telemetry events (Ticks, Bars, Signals, Stats).
   - Formats them as JSON and broadcasts over a `tcp://*:5555` ZeroMQ Publisher socket.

## Prerequisites

- **C++ Compiler**: `g++` (supports C++20)
- **ZeroMQ**: C++ library (`libzmq`) installed via Homebrew (`brew install zmq`) or apt (`apt install libzmq3-dev`).
- **Python**: Python 3.8+ with `pyzmq` and `rich` libraries.

## Building the Engine

The project uses a `Makefile` with targets optimized for different scenarios.

```bash
# Build the optimized release binary (default)
make release

# Build with debug symbols and AddressSanitizer (ASan)
make debug

# Build for profiling with gprof
make profile

# Check system compatibilities and specs
make check
```

## Running the Engine

The engine includes a `run.sh` launcher script that safely builds the project and attempts to apply Linux/macOS specific optimizations (e.g., disabling C-states, setting CPU frequency scaling governors, and setting thread affinities).

```bash
# Run with automatic optimizations
sudo ./run.sh

# Run without root privileges (skips hardware optimizations)
./run.sh --no-root
```

## Telemetry Dashboard

Once the engine is running, open a separate terminal to view the real-time telemetry dashboard.

First, ensure you have the required Python dependencies:

```bash
pip install pyzmq rich
```

Then launch the dashboard:

```bash
python3 dashboard.py
```

## Project Structure

- `src/` - Source code for specific strategies.
  - `strategies/my_strategy.hpp` - Custom EMA Crossover & Momentum strategy implementation.
- `include/` - Core engine headers.
  - `feed.hpp` - Market data feed interfaces.
  - `indicators.hpp` - Bar building and mathematical indicator calculations.
  - `spsc_queue.hpp` - Lock-free single-producer single-consumer ring buffer.
  - `strategy.hpp` - Strategy base class (CRTP for zero-overhead polymorphism).
  - `types.hpp` - Core struct definitions (Tick, Bar, Signal).
- `hft_engine.cpp` - Engine orchestration, threading, and ZeroMQ publisher setup.
- `dashboard.py` - Python TUI for monitoring the engine.
- `run.sh` - Bash launcher script with kernel/CPU optimizations.
- `Makefile` - Build configuration.

## Latency Realities & Design Philosophy

While this project utilizes several modern C++ concurrency practices, such as lock-free SPSC queues, CRTP for zero-overhead polymorphism, and explicit cache-line alignment to avoid false sharing, it is important to recognize that it serves as an educational/hobbyist project rather than an institutional-grade HFT engine.

To achieve true sub-microsecond "Tick-to-Trade" latencies at an institutional level, you would need to fundamentally change several architectural choices:

- **Operating System**: macOS is inherently unsuitable due to network stack jitter and its non-real-time kernel scheduler. Modern HFT requires heavily tuned Linux environments with `isolcpus` and kernel bypass, I just have my macbook and I like it so that's why I wanted to play a bit on it.
- **Kernel Bypass**: Passing 64-byte structs by value and relying on standard POSIX sockets is too slow. Real engines use zero-copy mechanisms like DPDK or Solarflare OpenOnload, where NIC drivers write directly into pre-allocated ring-buffers.
- **Floating Point Math**: This engine uses floats and doubles for indicators and prices, which induce FPU overhead and can suffer from edge cases (e.g. subnormals). Real systems utilize scaled integers (fixed-point math).
- **Telemetry in Hot Path**: ZeroMQ and JSON string formatting (`snprintf`) are far too slow for real-time publishing within the execution critical path. Top-tier engines dump raw binary structs straight into `mmap`'d files.

Compared to institutional trading firms, this system's latency sits in the space of microseconds to milliseconds rather than nanoseconds.

have fun, peace.

## License

MIT
