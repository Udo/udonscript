# UdonScript Quick Reference

## Syntax at a Glance

### Variables
```javascript
var x = 10
var name = "Alice"
var position = vec3(0, 1, 0)
```

### Functions
```javascript
function add(a, b) {
    return a + b
}

// With type annotations
function multiply(a: f32, b: f32) -> f32 {
    return a * b
}
```

### Control Flow
```javascript
// If statement
if (x > 10) {
    print("Large")
} else if (x > 5) {
    print("Medium")
} else {
    print("Small")
}

// While loop
while (count < 10) {
    count = count + 1
}

// For loop
for (var i = 0; i < 10; i = i + 1) {
    print(i)
}

// Foreach loop
foreach (key, value in data) {
    print(key, value)
}

// Switch statement
switch (value) {
    case 1:
        print("One")
    case 2:
        print("Two")
    default:
        print("Other")
}
```

### Operators
```javascript
// Arithmetic
x + y    x - y    x * y    x / y    -x

// Comparison
x == y   x != y   x < y    x <= y   x > y    x >= y

// Logical
!x       x && y   x || y

// String concatenation
"Hello" + " " + "World"
```

### Arrays/Maps
```javascript
var data = {}
data:name = "Alice"
data:age = 30

var name = data:name
var size = len(data)

foreach (key, value in data) {
    print(key + ":", value)
}
```

## Built-in Functions Quick List

### Console
- `print(values...)`

### Math
- `abs(x)` `sqrt(x)` `pow(base, exp)`
- `sin(x)` `cos(x)` `tan(x)` `asin(x)` `acos(x)` `atan(x)` `atan2(y, x)`
- `floor(x)` `ceil(x)` `round(x)`
- `min(a, b)` `max(a, b)`
- `exp(x)` `log(x)` `log10(x)`

### Strings
- `len(s)` - Length of string or array
- `substr(s, start, count)` - Extract substring
- `to_upper(s)` - Convert to uppercase
- `to_lower(s)` - Convert to lowercase
- `trim(s)` - Remove whitespace

### Type Conversion
- `to_s32(value)` - Convert to integer
- `to_f32(value)` - Convert to float
- `to_string(value)` - Convert to string
- `to_bool(value)` - Convert to boolean

### Vectors
- `vec2(x, y)` - Create 2D vector
- `vec3(x, y, z)` - Create 3D vector
- `vec4(x, y, z, w)` - Create 4D vector

### File I/O
- `load_from_file(path)` - Read file contents
- `save_to_file(path, data)` - Write file contents
- `read_entire_file(path)` - Read entire file
- `write_entire_file(path, data)` - Write entire file
- `file_size(path)` - Get file size in bytes
- `file_time(path)` - Get file modification time

### Shell & Escaping
- `shell(command, ...args)` - Execute shell command
- `to_shellarg(s)` - Escape for shell
- `to_htmlsafe(s)` - Escape for HTML
- `to_sqlarg(s)` - Escape for SQL

### Type Inspection
- `typeof(value)` - Get type name as string

## Data Types
- **None** - No value
- **Bool** - `true` or `false`
- **S32** - 32-bit integer
- **F32** - 32-bit float
- **String** - Text string
- **Vector2** - 2D vector
- **Vector3** - 3D vector
- **Vector4** - 4D vector
- **Array** - Dynamic array/map

## Common Patterns

### Distance Between Points
```javascript
function distance(x1, y1, x2, y2) {
    var dx = x2 - x1
    var dy = y2 - y1
    return sqrt(dx * dx + dy * dy)
}
```

### Clamp Value to Range
```javascript
function clamp(value, minVal, maxVal) {
    return max(minVal, min(maxVal, value))
}
```

### Degrees to Radians
```javascript
function toRadians(degrees) {
    return degrees * 3.14159 / 180.0
}
```

### Check if String Contains
```javascript
function contains(haystack, needle) {
    var haystackLower = to_lower(haystack)
    var needleLower = to_lower(needle)
    // Note: Need to implement manual search
    // UdonScript doesn't have built-in string search
    return false  // Placeholder
}
```

### Array Iteration
```javascript
var items = {}
items.apple = 1.50
items.banana = 0.75
items.orange = 1.25

var total = 0.0
foreach (name, price in items) {
    total = total + price
    print(name + ": $" + to_string(price))
}
print("Total: $" + to_string(total))
```

### Safe Division
```javascript
function safeDivide(a, b) {
    if (b == 0) {
        return 0
    }
    return a / b
}
```

### Vector Normalization
```javascript
function normalize(v) {
    var length = sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
    if (length == 0) {
        return vec3(0, 0, 0)
    }
    return vec3(v.x / length, v.y / length, v.z / length)
}
```

## Comments
```javascript
// Single line comment

/*
 * Multi-line
 * comment
 */
```

## Best Practices

1. **Initialize variables**: Always give variables a starting value
2. **Use descriptive names**: `playerHealth` not `ph`
3. **Keep functions small**: Do one thing well
4. **Check for zero before division**: Avoid division by zero errors
5. **Use constants**: Define magic numbers as named variables
6. **Comment complex logic**: Help future readers understand
7. **Type annotations optional**: Use for clarity when helpful
8. **Consistent style**: Pick naming conventions and stick with them

## Common Gotchas

1. **No implicit type coercion**: Use `to_string()` when concatenating numbers with strings
2. **Integer division**: `5 / 2` yields 2, not 2.5 (if both are integers)
3. **Angles in radians**: Trig functions use radians, not degrees
4. **Assignment vs comparison**: `=` assigns, `==` compares
5. **Array/map access**: Use colon notation (`data:key`); dot is for methods
6. **No string interpolation**: Must concatenate with `+`
7. **Case statements don't auto-break**: Each case executes until return or end of switch

## Integration with C++

### Basic Usage
```cpp
#include "core/udonscript.h"

UdonInterpreter interp;

// Compile script
CodeLocation result = interp.compile(scriptSource);
if (result.has_error) {
    std::cerr << "Error: " << result.opt_error_message << std::endl;
}

// Run function
UdonValue returnValue;
interp.run("main", {}, {}, returnValue);
```

### Custom Built-in Function
```cpp
interp.register_function("myFunc", "x:number", "number",
    [](UdonInterpreter* interp, 
       const std::vector<UdonValue>& args,
       const std::unordered_map<std::string, UdonValue>& named,
       UdonValue& out, 
       CodeLocation& err) {
        if (args.size() != 1) {
            err.has_error = true;
            err.opt_error_message = "Expected 1 argument";
            return true;
        }
        // Implementation
        out = make_int(42);
        return true;
    });
```

## Error Messages

When errors occur, check:
- Line and column numbers in error message
- Missing semicolons (not required but can cause issues)
- Matching braces `{}`
- Function names and parameters
- Type mismatches in operations

## Resources

- **Language Specification**: `language-spec.md` - Complete syntax and grammar
- **Built-ins Reference**: `builtins-reference.md` - Detailed function documentation
- **Examples**: Check `src/programs/` for example programs
