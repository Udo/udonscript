# UdonScript Documentation

UdonScript is a toy scripting language designed for embedding in C++ applications.

## Hello World Example

```javascript
function main() {
    print("Hello, World!")
}
```

### Running Your First Script

```cpp
#include "core/udonscript.h"

int main() {
    UdonInterpreter interp;
    
    const char* script = R"(
    function main() {
        print("Hello, World!")
    }
    )";
    
    // Compile
    CodeLocation result = interp.compile(script);
    if (result.has_error) {
        std::cerr << "Error: " << result.opt_error_message << std::endl;
        return 1;
    }
    
    // Run
    UdonValue returnValue;
    interp.run("main", {}, {}, returnValue);
    
    return 0;
}
```

## Language Features

### ✨ Dynamic Typing
Variables can hold any type of value and change types at runtime.

```javascript
var x = 42          // Integer
x = "hello"         // Now a string
x = vec3(1, 2, 3)   // Now a vector
```

### 🎯 Functions with Named Arguments
Call functions with arguments by name for clarity.

```javascript
function greet(firstName, lastName, title) {
    print(title + " " + firstName + " " + lastName)
}

greet(lastName="Smith", firstName="John", title="Dr.")
```

### 📦 Flexible Arrays/Maps
A single data structure that works as both array and associative array.

```javascript
var data = {}
data.name = "Alice"
data.age = 30
data.scores = {}
data.scores.math = 95
data.scores.english = 88
```

