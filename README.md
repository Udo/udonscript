# UdonScript

A lightweight, dynamically-typed scripting language designed for embedding in C++ applications.

## Features

- 🚀 **Easy to Embed** - Simple C++ API for integration
- 💡 **Dynamic Typing** - Flexible type system with runtime type checking
- 📐 **Vector Math** - Built-in 2D, 3D, and 4D vector types and operations
- 🎯 **Named Arguments** - Call functions with arguments by name
- 📦 **Flexible Arrays** - Unified array/map data structure
- 🔧 **Extensible** - Easy to add custom built-in functions
- 📝 **Clear Errors** - Detailed error messages with line/column information

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

### Hello World

```javascript
function main() {
    print("Hello, World!")
}
```

### Running Your First Script

```cpp
#include "core/udonscript.h"
#include <iostream>

int main() {
    UdonInterpreter interp;
    
    const char* script = R"(
        function greet(name) {
            print("Hello, " + name + "!")
        }
        
        function main() {
            greet("World")
        }
    )";
    
    // Compile
    CodeLocation result = interp.compile(script);
    if (result.has_error) {
        std::cerr << "Compilation error: " << result.opt_error_message << "\n";
        return 1;
    }
    
    // Run main function
    UdonValue returnValue;
    interp.run("main", {}, {}, returnValue);
    
    return 0;
}
```

## Interactive REPL

Try UdonScript interactively:

```bash
./bin/repl
```

```
UdonScript REPL - Interactive Interpreter
Type 'help' for commands, 'exit' to quit

> var x = 10
> var y = 20
> print("Sum: " + to_string(x + y))
Sum: 30
> exit
```

## Running Scripts

Execute `.udon` scripts directly:

```bash
./bin/us scripts/demo/fibonacci.udon
```

## Running Tests

The test suite validates all language features:

```bash
./bin/testrunner
```

This runs 25 test cases covering:
- Arithmetic, comparison, logical operations
- String handling and type conversion
- Control flow (if/else, while, for, foreach, switch)
- Functions and recursion
- Object literals and array operations
- File I/O and shell execution
- Math functions and vectors
- Type hints and type inspection

If all tests pass, the command exits with code 0. Failed tests are reported to `tmp/testsuite.report`.

## Language Examples

### Variables and Types

```javascript
// Semicolons are optional!
var x = 42                    // Integer
var pi = 3.14159              // Float
var name = "Alice"            // String
var position = vec3(0, 1, 0)  // 3D Vector
var flag = true               // Boolean

// Optional type annotations
var count: s32 = 0
var ratio: f32 = 0.5
```

### Functions

```javascript
function add(a, b) {
    return a + b
}

// With type annotations (optional)
function multiply(a: f32, b: f32) -> f32 {
    return a * b
}

// Named arguments
function greet(firstName, lastName, title) {
    print(title + " " + firstName + " " + lastName)
}

greet(lastName="Smith", firstName="John", title="Dr.")
```

### Control Flow

```javascript
// If-else
if (score >= 90) {
    print("Grade: A")
} else if (score >= 80) {
    print("Grade: B")
} else {
    print("Grade: C")
}

// Loops
for (var i = 0; i < 10; i = i + 1) {
    print(i)
}

while (count < 100) {
    count = count + 1
}

// Foreach
foreach (key, value in data) {
    print(key + ": " + to_string(value))
}
```

### Arrays and Maps

```javascript
var data = {}
data.name = "Alice"
data.age = 30
data.scores = {}
data.scores.math = 95
data.scores.science = 88

print(data.name)        // "Alice"
print(len(data))        // 2
```

### Vector Math

```javascript
var pos = vec3(10.0, 0.0, 5.0)
var vel = vec3(1.0, 0.0, 0.0)
var newPos = pos + vel * 2.0

var distance = sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z)
```

## Built-in Functions

### Console & I/O
- `print(values...)` - Print to console
- `load_from_file(path)` - Read file contents (line by line)
- `save_to_file(path, data)` - Write to file (line by line)
- `read_entire_file(path)` - Read entire file as string
- `write_entire_file(path, data)` - Write entire string to file
- `file_size(path)` - Get file size in bytes
- `file_time(path)` - Get file modification time (Unix timestamp)

### Shell & Escaping
- `shell(command, args...)` - Execute shell command and capture output
- `to_shellarg(s)` - Escape string for safe shell usage
- `to_htmlsafe(s)` - Escape HTML entities
- `to_sqlarg(s)` - Escape string for SQL queries

### Math
- `sin(x)`, `cos(x)`, `tan(x)` - Trigonometry
- `sqrt(x)`, `pow(base, exp)` - Power and roots
- `abs(x)`, `floor(x)`, `ceil(x)`, `round(x)` - Rounding
- `min(a, b)`, `max(a, b)` - Min/max

### Strings
- `len(s)` - String length or array size
- `substr(s, start, count)` - Extract substring
- `to_upper(s)`, `to_lower(s)` - Case conversion
- `trim(s)` - Remove whitespace

### Type Conversion & Inspection
- `to_s32(value)` - Convert to integer
- `to_f32(value)` - Convert to float
- `to_string(value)` - Convert to string
- `to_bool(value)` - Convert to boolean
- `typeof(value)` - Get type name as string

### Vectors
- `vec2(x, y)` - Create 2D vector
- `vec3(x, y, z)` - Create 3D vector
- `vec4(x, y, z, w)` - Create 4D vector

## Documentation

Comprehensive documentation is available in the `doc/` directory:

- **[Language Specification](doc/language-spec.md)** - Complete syntax and grammar reference
- **[Built-in Functions Reference](doc/builtins-reference.md)** - Detailed function documentation
- **[Quick Reference](doc/quick-reference.md)** - Cheat sheet and common patterns
- **[Documentation Index](doc/README.md)** - Documentation overview

## Project Structure

```
udonscript/
├── src/
│   ├── core/              # Core scripting engine
│   │   ├── types.h/cpp    # Type definitions
│   │   ├── udonscript.h/cpp             # Interpreter and VM
│   │   ├── udonscript-builtins.cpp      # Built-in functions
│   │   ├── udonscript-helpers.cpp       # Helper functions
│   │   └── udonscript_internal.h        # Internal headers
│   └── programs/          # Programs and tools
│       ├── helloworld.cpp # Comprehensive feature demo
│       ├── us.cpp         # Command-line executor (./bin/us script.udon)
│       ├── repl.cpp       # Interactive REPL (./bin/repl)
│       └── testrunner.cpp # Test suite runner (./bin/testrunner)
├── scripts/
│   ├── demo/              # Demo scripts
│   │   └── fibonacci.udon # Fibonacci example
│   └── testsuite/         # Test cases (25 tests)
│       ├── *.udon         # Test scripts
│       └── *.expected     # Expected output
├── bin/                   # Compiled executables
├── tmp/                   # Intermediate build files
├── doc/                   # Documentation
├── build                  # Build script
└── README.md
```

## Extending UdonScript

### Adding Custom Built-in Functions

You can easily add custom functions from your C++ code:

```cpp
interp.register_function("my_function", "x:number, y:number", "number",
    [](UdonInterpreter* interp,
       const std::vector<UdonValue>& args,
       const std::unordered_map<std::string, UdonValue>& named,
       UdonValue& out,
       CodeLocation& err) {
        
        if (args.size() != 2) {
            err.has_error = true;
            err.opt_error_message = "Expected 2 arguments";
            return true;
        }
        
        double x = as_number(args[0]);
        double y = as_number(args[1]);
        out = make_float(x + y);
        
        return true;
    });
```

Then use it in your scripts:

```javascript
var result = my_function(10, 20)
print(result)  // 30
```

## Use Cases

UdonScript is perfect for:

- **Game Scripting** - Character behaviors, game logic, quests
- **Configuration** - Complex configuration files with logic
- **Tool Automation** - Scripting for tools and editors
- **Data Processing** - Transform and analyze data
- **Prototyping** - Rapid algorithm development
- **User Customization** - Let users script custom behaviors

## Building Your Own Programs

Create a new file in `src/programs/`:

```cpp
#include "core/udonscript.h"
#include <iostream>

int main() {
    UdonInterpreter interp;
    
    // Your script here
    const char* script = R"(
        function main() {
            print("My custom program")
        }
    )";
    
    CodeLocation result = interp.compile(script);
    if (result.has_error) {
        std::cerr << "Error: " << result.opt_error_message << "\n";
        return 1;
    }
    
    UdonValue ret;
    interp.run("main", {}, {}, ret);
    
    return 0;
}
```

Run `./build` and your program will be compiled to `bin/`.

## Requirements

- C++11 or later
- Standard C++ library
- No external dependencies

## License

See license information in the source files.

## Contributing

Contributions are welcome! Areas for improvement:

- Additional built-in functions
- Performance optimizations
- Better error messages
- More example programs
- Documentation improvements

---

**UdonScript** - Simple, powerful scripting for C++ applications.
