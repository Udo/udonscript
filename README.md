# UdonScript

A toy scripting language designed for embedding in C++ applications.

## Features

- **Easy to Embed** - Simple C++ API for integration
- **Dynamic Typing** - Flexible type system with runtime type checking
- **Vector Math** - Built-in 2D, 3D, and 4D vector types and operations
- **Named Arguments** - Call functions with arguments by name
- **Flexible Arrays** - Unified array/map data structure
- **Extensible** - Easy to add custom built-in functions
- **Clear Errors** - Detailed error messages with line/column information

## Quick Start

### Building

```bash
./build
```

This compiles the core library and all programs in `src/programs/`:
- `helloworld` - Comprehensive feature demo
- `us` - Command-line script executor
- `repl` - Interactive Read-Eval-Print Loop
- `testrunner` - Automated test suite runner
- `dump` - Compile-only tool that prints decoded bytecode for debugging

Use `./build --release` for an optimized release build; it defaults to a debug build.

### Useful Directories

- `src/core/` - Interpreter core
- `src/programs/` - Host integration examples
- `scripts/testsuite/` - Automated tests
- `scripts/demo/` - Small demo scripts (FFI, IO, closures, math)

### Hello World

```javascript
function main() {
    print("Hello, World!")
}
```
