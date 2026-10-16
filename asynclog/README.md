# Async Logger

A logger whose hot path never formats, never allocates, and never blocks.

![Async Logger preview](preview.png)

*Real output of `./asynclog`.*

## Features

- A log call **copies a small fixed-size record** into a **lock-free ring**.
- A **background thread** does the formatting and the `write()`.
- Costs about **30 ns per call** instead of the 1-2 µs of snprintf plus a locked stream.
- Under a burst, **drops records with a counted gap** instead of stalling the caller.

## How it works

Each record holds a level, timestamp, format pointer and a few plain arguments. All the expensive work moves off the thread doing real work.

## Usage

```bash
g++ -std=c++23 -O2 -pthread asynclog.cpp -o asynclog && ./asynclog
```

## Run it

```bash
g++ -std=c++23 -O2 -pthread asynclog.cpp -o asynclog
./asynclog
```

Needs a C++23 compiler (GCC 13+ or Clang 17+).

## Files

| File | Purpose |
| --- | --- |
| [`asynclog.cpp`](asynclog.cpp) | The complete program |
| [`preview.png`](preview.png) | Captured output of a real run |
