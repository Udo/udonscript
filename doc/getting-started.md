# Getting Started with UdonScript

This guide will help you get up and running with UdonScript quickly.

## Installation

1. Clone or download the UdonScript repository
2. Navigate to the project directory
3. Run the build script:

```bash
cd udonscript
./build
```

The build creates:
- Core library object files in `tmp/`
- Example executables in `bin/`

## Your First Script

### Step 1: Run the Hello World Example

```bash
./bin/helloworld
```

You should see output demonstrating various UdonScript features.

### Step 2: Create Your Own Script

Create a new file `src/programs/myscrip.cpp`:

```cpp
#include "core/udonscript.h"
#include <iostream>

int main(int argc, char* argv[])
{
    std::cout << "My UdonScript Program\n";
    std::cout << "=====================\n\n";

    UdonInterpreter interp;

    const char* script = R"(
    function main() {
        print("Welcome to UdonScript!")
        
        // Variables
        var name = "Learner"
        var score = 0
        
        // Loop
        for (var i = 1; i <= 5; i = i + 1) {
            score = score + i * 10
            print("Round " + to_string(i) + " score: " + to_string(score))
        }
        
        print("Final score for " + name + ": " + to_string(score))
    }
    )";

    // Compile
    std::cout << "Compiling script...\n";
    CodeLocation compile_result = interp.compile(script);
    
    if (compile_result.has_error) {
        std::cerr << "Compilation error at line " << compile_result.line 
                  << ", column " << compile_result.column << ":\n"
                  << compile_result.opt_error_message << std::endl;
        return 1;
    }
    
    std::cout << "Compilation successful!\n\n";
    
    // Run
    std::cout << "Running main()...\n";
    std::cout << "-------------------\n";
    
    UdonValue return_value;
    CodeLocation run_result = interp.run("main", {}, {}, return_value);
    
    if (run_result.has_error) {
        std::cerr << "Runtime error at line " << run_result.line 
                  << ", column " << run_result.column << ":\n"
                  << run_result.opt_error_message << std::endl;
        return 1;
    }
    
    std::cout << "-------------------\n";
    std::cout << "\nProgram completed successfully!\n";
    
    return 0;
}
```

### Step 3: Build and Run

```bash
./build
./bin/myscript
```

## Learning Path

### 1. Start with the Basics

Read through these sections in order:

1. **[Quick Reference](quick-reference.md)** - Get a feel for the syntax
2. **[Language Spec - Variables](language-spec.md#variables)** - Understand variables
3. **[Language Spec - Control Flow](language-spec.md#control-flow)** - Learn if/while/for

### 2. Practice with Examples

Try modifying the hello world example:

```javascript
function greet(name, times) {
    for (var i = 0; i < times; i = i + 1) {
        print("Hello, " + name + "!")
    }
}

function main() {
    greet("World", 3)
}
```

### 3. Explore Built-in Functions

Check out **[Built-in Functions Reference](builtins-reference.md)** and try:

- Math functions: `sin()`, `cos()`, `sqrt()`
- String functions: `to_upper()`, `substr()`, `trim()`
- Type conversions: `to_string()`, `to_s32()`
- Vector math: `vec3()`, vector operations

### 4. Work with Data Structures

Learn about arrays and maps:

```javascript
function main() {
    // Create a data structure
    var player = {}
    player.name = "Hero"
    player.health = 100
    player.level = 5
    
    // Create nested structures
    player.inventory = {}
    player.inventory.gold = 150
    player.inventory.potions = 3
    
    // Iterate
    foreach (key, value in player) {
        print(key + ": " + to_string(value))
    }
}
```

### 5. Build Something Real

Try creating a practical program:

**Example: Temperature Converter**

```javascript
function celsiusToFahrenheit(c) {
    return c * 9.0 / 5.0 + 32.0
}

function fahrenheitToCelsius(f) {
    return (f - 32.0) * 5.0 / 9.0
}

function main() {
    print("Temperature Conversion")
    print("=====================")
    
    var temps = {}
    temps.freezing = 0.0
    temps.room = 20.0
    temps.body = 37.0
    temps.boiling = 100.0
    
    foreach (name, celsius in temps) {
        var fahrenheit = celsiusToFahrenheit(celsius)
        print(name + ": " + to_string(celsius) + "°C = " + 
              to_string(fahrenheit) + "°F")
    }
}
```

## Common Tasks

### Reading User Input (from file)

Since UdonScript doesn't have direct console input, use files:

```javascript
function main() {
    var input = load_from_file("input.txt")
    var processed = to_upper(trim(input))
    save_to_file("output.txt", processed)
    print("Processed: " + processed)
}
```

### Working with Vectors

```javascript
function vectorDistance(v1, v2) {
    var dx = v2.x - v1.x
    var dy = v2.y - v1.y
    var dz = v2.z - v1.z
    return sqrt(dx * dx + dy * dy + dz * dz)
}

function main() {
    var pos1 = vec3(0.0, 0.0, 0.0)
    var pos2 = vec3(3.0, 4.0, 0.0)
    
    var dist = vectorDistance(pos1, pos2)
    print("Distance: " + to_string(dist))  // 5.0
}
```

### Error Handling Pattern

```javascript
function safeDivide(a, b) {
    if (b == 0) {
        print("Error: Division by zero")
        return 0
    }
    return a / b
}

function processData(data) {
    // Validate input
    if (len(data) == 0) {
        print("Error: No data provided")
        return false
    }
    
    // Process data
    // ...
    
    return true
}
```

## Next Steps

Once you're comfortable with the basics:

1. **Extend with C++**: Learn to add custom built-in functions
2. **Optimize**: Profile your scripts and optimize hot paths
3. **Build Tools**: Create domain-specific tools using UdonScript
4. **Share**: Contribute examples and improvements back to the project

## Tips for Success

1. **Start Small**: Write small, focused scripts first
2. **Test Frequently**: Compile and run often to catch errors early
3. **Use Comments**: Document your code, especially complex logic
4. **Read Examples**: Study the hello world and other examples
5. **Experiment**: Try things out - the best way to learn!

## Common Pitfalls

### Missing String Conversion

❌ Wrong:
```javascript
print("Score: " + 42)  // May not work as expected
```

✅ Correct:
```javascript
print("Score: " + to_string(42))
```

### Forgetting Return Type

❌ Wrong:
```javascript
function add(a, b) {
    a + b  // Result is lost
}
```

✅ Correct:
```javascript
function add(a, b) {
    return a + b
}
```

### Uninitialized Variables

❌ Wrong:
```javascript
var total
total = total + 10  // total is None
```

✅ Correct:
```javascript
var total = 0
total = total + 10
```

### Incorrect Loop Syntax

❌ Wrong:
```javascript
for (var i = 0; i < 10; i++) {  // No ++ operator
    print(i)
}
```

✅ Correct:
```javascript
for (var i = 0; i < 10; i = i + 1) {
    print(i)
}
```

## Getting Help

- **Language Questions**: Check [Language Specification](language-spec.md)
- **Function Reference**: See [Built-in Functions Reference](builtins-reference.md)
- **Quick Lookups**: Use [Quick Reference](quick-reference.md)
- **Code Examples**: Look at `src/programs/helloworld.cpp`

## Resources

- Full documentation in the `doc/` directory
- Example programs in `src/programs/`
- Source code in `src/core/` (well-commented)

---

**Happy Coding!** 🎉

Start experimenting, build something fun, and don't hesitate to explore the documentation when you need more details.
