<div align="center">

# Marlin

**Write it once. Run it on every CPU core and the GPU.**

A fast, compiled language with a calm, readable syntax, built on MLIR and LLVM.

[Website](https://marlin-lang.org) · [Documentation](https://marlin-lang.org/docs.html) · [Download](https://marlin-lang.org/downloads.html) · [Releases](https://github.com/MehmetEminKaymaz/marlin/releases)

[![CI](https://github.com/MehmetEminKaymaz/marlin/actions/workflows/ci.yml/badge.svg)](https://github.com/MehmetEminKaymaz/marlin/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/MehmetEminKaymaz/marlin)](https://github.com/MehmetEminKaymaz/marlin/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

</div>

---

Marlin makes parallelism part of the language. Tasks, race-checked parallel loops and GPU functions are built in, so the same source runs on your CPU and your graphics card, and falls back to the CPU when there is no GPU. Programs compile to a single native executable for Linux, macOS or Windows.

```
@GPU
func relax(matrix<float> a, matrix<float> b) -> matrix<float> {
    return matmul(a, b) * 0.5 + 1.0;       // runs on the GPU if there is one
}

func main() -> void {
    matrix<float> a = fill(2048, 2048, 0.5);
    matrix<float> b = fill(2048, 2048, 0.25);
    print(sum(relax(a, b)));
}
```

## Features

- **GPU with one word.** Put `@GPU` above a function. Matrix maths and `parallel for` loops inside it become GPU kernels: CUDA on NVIDIA, Vulkan on AMD and Intel, Metal (through MoltenVK) on Apple. Every `@GPU` function also gets a CPU copy, so the program runs everywhere and gives the same result.
- **Parallel loops without data races.** `parallel for` spreads iterations over every CPU core. The compiler rejects loops that could race, so every loop it accepts is correct.
- **Lightweight tasks.** `marl f(x)` starts a task, `await(t)` takes its result. Waiting never blocks a thread, so thousands of tasks can be in flight.
- **Matrices as a built-in type.** `matrix<T, N>` with element-wise operators, `matmul`, `transpose` and `sum`. Expressions like `a + b * 2 - c` are fused into one loop, and `matmul` uses tiled kernels on the CPU and the GPU.
- **Memory you don't manage.** Strings and matrices are freed automatically; list and matrix indices are checked at run time with clear error messages.
- **A practical standard library.** HTTP/1.1 server and client, typed JSON, PostgreSQL and SQLite (embedded), files, strings, time, random numbers.
- **Native, self-contained programs.** One executable, no VM and no interpreter. Copy it to another machine and run it.
- **Tooling.** A VS Code extension with live errors from the real compiler (built-in language server), and `marlin doctor`, which checks your setup and tells you how to fix it.

## Install

**Linux and macOS**

```sh
curl -fsSL https://github.com/MehmetEminKaymaz/marlin/releases/latest/download/install.sh | sh
```

**Windows** (PowerShell)

```powershell
irm https://github.com/MehmetEminKaymaz/marlin/releases/latest/download/install.ps1 | iex
```

The script downloads the compiler, verifies its checksum, installs it in your home folder and adds `marlin` to your PATH. Run it again to update. Archives for every platform, the VS Code extension and checksums are on the [download page](https://marlin-lang.org/downloads.html).

Marlin links programs with your system's C toolchain:

| System | You also need |
|---|---|
| Linux (x86_64, arm64) | A C compiler: `sudo apt install build-essential` |
| macOS 11+ (Apple silicon, Intel) | Command Line Tools: `xcode-select --install` |
| Windows 10/11 (x86_64) | Visual Studio Build Tools with the C++ and Clang components |

Then check your setup:

```sh
marlin doctor
```

## Hello, Marlin

```
func main() -> void {
    print("hello, ocean");
}
```

```sh
marlin hello.marl -O2 -o hello
./hello
```

## A closer look

**Parallel loops and tasks**

```
func fib(int n) -> int64 {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

func main() -> void {
    task<int64> t = marl fib(35);          // runs in the background

    list<float64> squares = [];
    for i in 0 .. 1000 { push(squares, 0.0); }
    parallel for i in 0 .. 1000 {          // spread over every core
        squares[i] = sqrt(i) * i;
    }

    print(await(t), squares[999]);
}
```

**A web service in a few lines**

```
import std.http as http;
import std.json as json;

struct User {
    string name;
    int age;
}

func create(http.Request req) -> http.Response {
    json.Doc d = json.parse(req.body);
    if !d.ok { return http.text(400, "invalid JSON: " + d.error); }
    User u = from_json(d.root);
    json.free(d);
    return http.json(201, to_json(u));
}

func main() -> void {
    list<http.Route> routes = [];
    push(routes, http.get("/", (http.Request r) -> http.Response | http.text(200, "hello\n")));
    push(routes, http.post("/users", create));
    http.serve_concurrent(8080, routes);   // each request runs as a task
}
```

## How it works

```
.marl  →  AST + checks  →  MLIR (scf, async, gpu, linalg)  →  LLVM IR / PTX / SPIR-V  →  native executable
```

The front end is an ANTLR grammar and a semantic checker that enforces the parallel-loop and task rules. Code is lowered through MLIR: `parallel for` and tasks go through the `async` dialect onto a thread pool (tasks that wait are compiled as coroutines, so they never hold a thread), and `@GPU` functions are outlined into `gpu` modules that become PTX for CUDA and SPIR-V for Vulkan. The CPU side is compiled by LLVM and linked with a small C runtime.

## Performance

Measured on an RTX 4060 Laptop GPU:

| Workload | Result |
|---|---|
| 2048 × 2048 `float` `matmul`, five times: CPU (20 threads) vs. GPU (CUDA), same source | 350 ms → 30 ms |
| 4096 × 4096 `float` on the GPU: hand-written `parallel for` vs. built-in tiled `matmul` | 270 ms → 35 ms |

Single-threaded code compiles through LLVM, like Clang, and runs at C-like speed.

## Editor support

Download [`marlin-vscode.vsix`](https://github.com/MehmetEminKaymaz/marlin/releases/latest/download/marlin-vscode.vsix) and install it in VS Code with **Extensions → … → Install from VSIX**. You get syntax colouring, snippets, live errors and a one-key **Compile & Run**.

## Building from source

Requirements: CMake 3.25+, a C++20 compiler, LLVM/MLIR 19.1.7 and Java 17 (for ANTLR).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMARLIN_ANTLR_FROM_SOURCE=ON
cmake --build build -j
bash tests/run.sh                 # the test suite
cd build && cpack                 # optional: build an installable archive
```

The [CI workflow](.github/workflows/ci.yml) builds and tests the compiler on Linux (x86_64, arm64), macOS (arm64, x86_64) and Windows, and is the most complete reference for setting up a build environment. A detailed developer guide, in Turkish, is in [docs/README.tr.md](docs/README.tr.md).

## Status

Marlin is a research language developed as part of a PhD thesis on generating CPU and GPU code from a single codebase. It is usable today, but the language and its library are still evolving and breaking changes are possible before 1.0. Compiler messages are currently in Turkish.

## License

[MIT](LICENSE)
