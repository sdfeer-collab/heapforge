# HeapForge

**A pluggable memory-strategy engine for C/C++**

English | [中文文档](README.md)

HeapForge provides a set of classic memory allocation strategies, two tiers of memory-safety instrumentation, heap fragmentation analysis with visualization, and a crash-consistent persistent memory pool. All components implement a common `IAllocator` interface and can be freely composed or swapped per scenario.

## Features

- **Header-only**: no pre-compilation or linking required; adding the include path is sufficient
- **C++17**, zero third-party dependencies
- **Cross-platform**: full support for macOS and Linux; the Windows adaptation layer (`VirtualAlloc` / VEH) is implemented but not yet verified on real hardware
- **Thoroughly tested**: 4,118 assertions covering randomized stress, child-process crash verification, and power-loss recovery replay

## Architecture Overview

| Layer | Component | Responsibility |
|---|---|---|
| Platform abstraction | `hf::VM`, `hf::page_size()` | Unified wrappers for `mmap` / `VirtualAlloc` / `mprotect` / `mlock` / page prefaulting |
| Allocation strategies | `FreeListAllocator` | Free list with First-Fit / Best-Fit / Next-Fit; boundary tags with immediate coalescing |
| | `BuddyAllocator` | Buddy system; power-of-two blocks, buddy located via address XOR, O(1) merging |
| | `SlabAllocator` | Fixed-size object pool with a thread-local cache to reduce lock contention |
| | `StackAllocator` | Linear (bump-pointer) allocation with Marker-based bulk rewind |
| | `BlockPool` | Bitmap-managed fixed-size block pool with 64-bit word scanning |
| Memory safety | `DebugHeap` | Free-time checks: canary words, poison-on-free, leak tracking with automatic reclamation |
| | `GuardHeap` | Instruction-level detection: `PROT_NONE` guard pages; overflows and use-after-free fault at the offending instruction |
| Analysis | `HeapAnalyzer` | External fragmentation ratio, free-block histograms; JSON / CSV / HTML heatmap export |
| Persistence | `PersistentPool` | File-mapped memory pool; write-ahead log and dual-buffered metadata slots for crash consistency |
| Infrastructure | `hf::Log` | Thread-safe structured logging with `[bug]` / `[debug]` event channels |

## Requirements

| Item | Requirement |
|---|---|
| Compiler | Clang / GCC with C++17 support (`-std=c++17`); MSVC is expected to work but unverified |
| Operating system | macOS, Linux; Windows pending verification |
| Build tooling | None required; CMake ≥ 3.16 optional |

## Building and Testing

### With the compiler directly

```bash
git clone https://github.com/sdfeer-collab/heapforge.git && cd heapforge

# Unit tests
clang++ -std=c++17 -Wall -Wextra -fno-omit-frame-pointer -g \
    -Iinclude tests/test_heapforge.cpp -o build/heapforge_tests
./build/heapforge_tests
# Expected output: === HeapForge tests: 4118 passed, 0 failed ===

# Demo (performance comparison, detection showcase, report export)
clang++ -std=c++17 -O2 -Iinclude examples/demo.cpp -o build/heapforge_demo
cd build && ./heapforge_demo
```

### With CMake

```bash
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

**Note**: `[bug]` events (heap overflow, double-free, memory leak, etc.) printed to
standard error during the test run are constructed deliberately by the test cases to
verify the detection machinery. Judge the test result by the final summary line and
the process exit code.

## Integration

### Option 1: CMake subdirectory

```cmake
add_subdirectory(third_party/heapforge)
target_link_libraries(your_target PRIVATE heapforge::heapforge)
```

When consumed as a subproject, HeapForge's own tests and demo are excluded from the
build by default (controlled by the `HEAPFORGE_BUILD_TESTS` option).

### Option 2: System install and find_package

```bash
cmake -B build && cmake --install build --prefix /usr/local
```

```cmake
find_package(heapforge REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE heapforge::heapforge)
```

### Option 3: Direct header inclusion

```bash
c++ -std=c++17 -I/path/to/heapforge/include your_source.cpp
```

For Xcode projects, add `heapforge/include` under *Build Settings → Header Search
Paths* and set *C++ Language Dialect* to C++17 or later.

**Recommendation**: enable `-fno-omit-frame-pointer` in Debug builds to keep call
stacks in detection reports complete.

## Quick Start

```cpp
#include "heapforge.h"

int main() {
    // Optional: write detection events to a log file
    hf::Log::instance().set_file("heapforge.log");

    // Compose: free-list allocator + debug instrumentation layer
    hf::FreeListAllocator heap(1 << 20, hf::FitPolicy::BestFit);
    hf::DebugHeap dbg(heap);

    void* p = dbg.allocate(256);
    dbg.deallocate(p);

    // Heap snapshot analysis and report export
    auto snapshot = hf::HeapAnalyzer::snapshot(heap);
    hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_html(snapshot), "heap_report.html");
    return 0;
}
```

## Memory-Safety Instrumentation

HeapForge offers two detection tiers; choose based on overhead and detection timing:

| | `DebugHeap` | `GuardHeap` |
|---|---|---|
| Detection timing | Validated on free | The instant the violating instruction executes |
| Mechanism | Canary words, 0xCD poisoning, metadata hash table | `PROT_NONE` hardware page protection, UAF quarantine |
| Space overhead | ~32 bytes per allocation | At least 3 memory pages per allocation |
| Intended use | Always-on Debug builds, routine CI checks | Targeted investigation of elusive memory bugs |
| Violation response | Log and reclaim safely | `Abort` (terminate with an on-the-spot call stack) or `Continue` (allow and count) |

All detection events are logged in a uniform format:

```
[2026-07-26 20:14:59.285] [bug]   [heap-overflow] canary smashed: block 0x…, user size 64 B
[2026-07-26 20:14:59.285] [debug] [repaired]      block 0x… poisoned (0xCD) and reclaimed safely
```

## Persistent Memory Pool

```cpp
hf::PersistentPool pool("data.pool", /*block_size=*/256, /*block_count=*/4096);

std::size_t index = 0;
auto* p = static_cast<char*>(pool.allocate(&index));
std::strcpy(p, "persistent data");
pool.flush(index);
// After a restart, recover the data via pool.at(index).
// If the previous session did not exit cleanly, the WAL is replayed on open
// to restore metadata consistency.
```

Crash-consistency design highlights:

1. **Three-step commit protocol**: WAL record persisted first → bitmap change applied → commit watermark advanced; a power loss between any two steps is recovered by idempotent replay
2. **Dual-buffered metadata slots**: alternating A/B writes with CRC checksums guard against torn writes to the header
3. **Configurable durability**: `Durability::Fast` (`msync`, filesystem level) and `Durability::Full` (additionally `F_FULLFSYNC`, disk-controller level)

## Performance Reference

Average cost of allocate + deallocate for 64-byte objects (Apple Silicon, `-O2`, single-threaded):

| Allocator | Average (µs/op) |
|---|---|
| BlockPool | 0.050 |
| StackAllocator | 0.055 |
| SlabAllocator | 0.062 |
| FreeListAllocator (First-Fit) | 0.064 |
| BuddyAllocator | 0.089 |

Reproduce on your target machine by running `heapforge_demo`.

## Directory Layout

```
├── include/
│   ├── heapforge.h            # Umbrella header
│   └── heapforge/             # Module implementations
├── tests/                     # Unit tests (zero-dependency assertion framework)
├── examples/                  # Demo program
├── sample_project/            # Standalone downstream integration example (see its README)
└── CMakeLists.txt             # Library definition, tests, install/export rules
```

## Known Limitations

- The Windows adaptation layer has not been verified on real hardware
- The `PersistentPool` WAL protects allocation metadata only; transactional data-area updates (shadow paging) are not yet implemented
- Output from the `GuardHeap` signal handler is restricted by async-signal-safety and goes to standard error only, not to the log file
- The `SlabAllocator` thread-local cache is a simplified design, not a strict lock-free per-CPU cache

## License

This project is released under the MIT License. See the [LICENSE](LICENSE) file for details.
