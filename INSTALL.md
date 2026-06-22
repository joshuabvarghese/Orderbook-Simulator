# Installation & Quick Start

## macOS — Apple Silicon (M1 / M2 / M3)  ← you are here

### 1. Install the compiler (one-time)

You need Xcode Command Line Tools. If you haven't already:

```bash
xcode-select --install
```

Verify:

```bash
clang++ --version
# Apple clang version 15.x ...  Target: arm64-apple-darwin
```

That's it — no Homebrew, no extra packages needed.

### 2. Clone and build

```bash
git clone https://github.com/<you>/orderbook.git
cd orderbook
make
```

Expected output:

```
  ✓ Build successful  →  ./orderbook
  Run:  make run        (5M messages)
  Run:  make bench      (1M messages — quick)
  Test: make test
```

### 3. Run the benchmark

```bash
make bench          # 1M messages, ~3 seconds
make run            # 5M messages, ~15 seconds
./orderbook 500000  # custom count
```

### 4. Run the tests

```bash
make test           # 68 unit tests, release build
make sanitize       # same tests + AddressSanitizer + UBSan
```

---

## macOS — Intel Mac

Same steps as above — the Makefile auto-detects the platform.

---

## Linux (Ubuntu / Debian)

```bash
sudo apt-get install -y g++ make
git clone https://github.com/<you>/orderbook.git
cd orderbook
make
```

---

## What the Makefile does per platform

| Step               | macOS (Apple Silicon)       | Linux (x86-64)                        |
|--------------------|-----------------------------|---------------------------------------|
| Compiler           | `clang++` (auto-detected)   | `g++` (auto-detected)                 |
| Optimisation flags | `-O3 -march=native`         | `-O3 -march=native -funroll-loops`    |
| Link flags         | *(none — POSIX in libc)*    | `-lpthread`                           |
| Sanitizer          | `-fsanitize=address,undefined` | `-fsanitize=address,undefined`     |

---

## Troubleshooting

**`clang++: command not found`**
→ Run `xcode-select --install`

**`make: command not found`**
→ Run `xcode-select --install` (includes make)

**Benchmark numbers look slow**
→ Make sure you ran `make` not `make sanitize` — sanitizers add ~5× overhead.
→ Close other apps during the run; the M1's efficiency cores will affect results.

**`ld: library not found for -lpthread`**
→ You're on macOS. Make sure you're using the provided Makefile — it omits `-lpthread` on Darwin automatically.
