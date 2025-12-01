# UdonScript Built-in Functions Reference

## Table of Contents

1. [Console Output](#console-output)
2. [File I/O](#file-io)
3. [Shell Execution](#shell-execution)
4. [Mathematical Functions](#mathematical-functions)
5. [String Functions](#string-functions)
6. [String Escaping](#string-escaping)
7. [Type Conversion](#type-conversion)
8. [Type Inspection](#type-inspection)
9. [Vector Functions](#vector-functions)
10. [Array Functions](#array-functions)
11. [Utility Functions](#utility-functions)

---

## Console Output

### `print(values...)`

Prints values to standard output.

**Parameters:**
- `values...` - Variable number of arguments of any type

**Returns:** `None`

**Description:**
Prints all arguments to the console, separated by spaces, followed by a newline.

**Examples:**
```javascript
print("Hello, World!")
// Output: Hello, World!

print("The answer is:", 42)
// Output: The answer is: 42

var x = 10
var y = 20
print("x =", x, "y =", y)
// Output: x = 10 y = 20

print(vec3(1.0, 2.0, 3.0))
// Output: vec3(1,2,3)
```

---

## File I/O

### `load_from_file(path)`

Loads the contents of a file as a string.

**Parameters:**
- `path: string` - Path to the file to read

**Returns:** `string` - Contents of the file

**Errors:**
- Triggers error if file cannot be read

**Example:**
```javascript
var content = load_from_file("data.txt")
print("File contents:", content)
```

### `save_to_file(path, data)`

Saves data to a file.

**Parameters:**
- `path: string` - Path to the file to write
- `data: any` - Data to write (will be converted to string)

**Returns:** `None`

**Errors:**
- Triggers error if file cannot be written

**Example:**
```javascript
save_to_file("output.txt", "Hello, World!")

var data = "Score: " + to_string(100)
save_to_file("score.txt", data)
```

### `read_entire_file(path)`

Reads the entire contents of a file as a string.

**Parameters:**
- `path: string` - Path to the file to read

**Returns:** `string` - Contents of the file

**Errors:**
- Triggers error if file cannot be read

**Example:**
```javascript
var content = read_entire_file("data.txt")
print("File contents:", content)
```

### `write_entire_file(path, data)`

Writes data to a file, replacing existing content.

**Parameters:**
- `path: string` - Path to the file to write
- `data: any` - Data to write (will be converted to string)

**Returns:** `None`

**Errors:**
- Triggers error if file cannot be written

**Example:**
```javascript
write_entire_file("output.txt", "New content")
```

### `file_size(path)`

Returns the size of a file in bytes.

**Parameters:**
- `path: string` - Path to the file

**Returns:** `s32` - File size in bytes

**Errors:**
- Triggers error if file cannot be accessed

**Example:**
```javascript
var size = file_size("data.txt")
print("File size:", size, "bytes")
```

### `file_time(path)`

Returns the last modification time of a file as a Unix timestamp.

**Parameters:**
- `path: string` - Path to the file

**Returns:** `s32` - Unix timestamp of last modification

**Errors:**
- Triggers error if file cannot be accessed

**Example:**
```javascript
var mtime = file_time("data.txt")
print("Last modified:", mtime)
```

---

## Shell Execution

### `shell(command, ...args)`

Executes a shell command and returns its output.

**Parameters:**
- `command: string` - Command to execute
- `...args: any` - Additional arguments (concatenated with spaces)

**Returns:** `string` - Command output (trailing newline removed)

**Errors:**
- Triggers error if command cannot be executed

**Example:**
```javascript
// Single argument
var result = shell("ls -l")
print(result)

// Multiple arguments (concatenated with spaces)
var date = shell("date", "+%Y-%m-%d")
print("Today:", date)

// Using with escaping
var filename = "test file.txt"
shell("touch", to_shellarg(filename))
```

---

## Mathematical Functions

### Trigonometric Functions

#### `sin(x)`
Computes the sine of `x` (in radians).

**Parameters:**
- `x: number` - Angle in radians

**Returns:** `number` - Sine of x

**Example:**
```javascript
var angle = 3.14159 / 2  // 90 degrees
var result = sin(angle)   // result ≈ 1.0
```

#### `cos(x)`
Computes the cosine of `x` (in radians).

**Parameters:**
- `x: number` - Angle in radians

**Returns:** `number` - Cosine of x

**Example:**
```javascript
var angle = 0.0
var result = cos(angle)  // result = 1.0
```

#### `tan(x)`
Computes the tangent of `x` (in radians).

**Parameters:**
- `x: number` - Angle in radians

**Returns:** `number` - Tangent of x

**Example:**
```javascript
var angle = 3.14159 / 4  // 45 degrees
var result = tan(angle)   // result ≈ 1.0
```

#### `asin(x)`
Computes the arc sine of `x` (result in radians).

**Parameters:**
- `x: number` - Value between -1 and 1

**Returns:** `number` - Arc sine of x in radians

**Example:**
```javascript
var result = asin(1.0)  // result ≈ π/2
```

#### `acos(x)`
Computes the arc cosine of `x` (result in radians).

**Parameters:**
- `x: number` - Value between -1 and 1

**Returns:** `number` - Arc cosine of x in radians

**Example:**
```javascript
var result = acos(0.0)  // result ≈ π/2
```

#### `atan(x)`
Computes the arc tangent of `x` (result in radians).

**Parameters:**
- `x: number` - Any number

**Returns:** `number` - Arc tangent of x in radians

**Example:**
```javascript
var result = atan(1.0)  // result ≈ π/4
```

#### `atan2(y, x)`
Computes the arc tangent of `y/x`, using the signs of both to determine the quadrant.

**Parameters:**
- `y: number` - Y coordinate
- `x: number` - X coordinate

**Returns:** `number` - Angle in radians

**Example:**
```javascript
var angle = atan2(1.0, 1.0)  // result ≈ π/4 (45 degrees)
```

### Power and Root Functions

#### `sqrt(x)`
Computes the square root of `x`.

**Parameters:**
- `x: number` - Non-negative number

**Returns:** `number` - Square root of x

**Example:**
```javascript
var result = sqrt(16.0)  // result = 4.0
var hypotenuse = sqrt(3.0 * 3.0 + 4.0 * 4.0)  // result = 5.0
```

#### `pow(base, exponent)`
Raises `base` to the power of `exponent`.

**Parameters:**
- `base: number` - Base value
- `exponent: number` - Exponent value

**Returns:** `number` - base raised to exponent

**Example:**
```javascript
var result = pow(2.0, 3.0)   // result = 8.0
var result2 = pow(9.0, 0.5)  // result2 = 3.0 (square root)
```

#### `exp(x)`
Computes e raised to the power of `x`.

**Parameters:**
- `x: number` - Exponent

**Returns:** `number` - e^x

**Example:**
```javascript
var result = exp(1.0)  // result ≈ 2.71828 (e)
```

#### `log(x)`
Computes the natural logarithm of `x`.

**Parameters:**
- `x: number` - Positive number

**Returns:** `number` - Natural log of x

**Example:**
```javascript
var result = log(2.71828)  // result ≈ 1.0
```

#### `log10(x)`
Computes the base-10 logarithm of `x`.

**Parameters:**
- `x: number` - Positive number

**Returns:** `number` - Base-10 log of x

**Example:**
```javascript
var result = log10(100.0)  // result = 2.0
```

### Rounding and Absolute Value

#### `abs(x)`
Computes the absolute value of `x`.

**Parameters:**
- `x: number` - Any number

**Returns:** `number` - Absolute value of x

**Example:**
```javascript
var result1 = abs(-5.0)  // result1 = 5.0
var result2 = abs(3.0)   // result2 = 3.0
```

#### `floor(x)`
Rounds `x` down to the nearest integer.

**Parameters:**
- `x: number` - Any number

**Returns:** `number` - Floor of x

**Example:**
```javascript
var result1 = floor(3.7)   // result1 = 3.0
var result2 = floor(-2.3)  // result2 = -3.0
```

#### `ceil(x)`
Rounds `x` up to the nearest integer.

**Parameters:**
- `x: number` - Any number

**Returns:** `number` - Ceiling of x

**Example:**
```javascript
var result1 = ceil(3.2)   // result1 = 4.0
var result2 = ceil(-2.7)  // result2 = -2.0
```

#### `round(x)`
Rounds `x` to the nearest integer.

**Parameters:**
- `x: number` - Any number

**Returns:** `number` - x rounded to nearest integer

**Example:**
```javascript
var result1 = round(3.4)  // result1 = 3.0
var result2 = round(3.6)  // result2 = 4.0
```

### Min/Max Functions

#### `min(a, b)`
Returns the smaller of two numbers.

**Parameters:**
- `a: number` - First number
- `b: number` - Second number

**Returns:** `number` - Minimum of a and b

**Example:**
```javascript
var result = min(10, 20)  // result = 10
var lowest = min(-5, 3)   // lowest = -5
```

#### `max(a, b)`
Returns the larger of two numbers.

**Parameters:**
- `a: number` - First number
- `b: number` - Second number

**Returns:** `number` - Maximum of a and b

**Example:**
```javascript
var result = max(10, 20)   // result = 20
var highest = max(-5, 3)   // highest = 3
```

---

## String Functions

### `len(value)`

Returns the length of a string or the number of elements in an array.

**Parameters:**
- `value: any` - String or array

**Returns:** `s32` - Length of string or size of array

**Example:**
```javascript
var length = len("Hello")     // length = 5
var size = len({a=1, b=2})    // size = 2
```

### `substr(s, start, count)`

Extracts a substring from a string.

**Parameters:**
- `s: string` - Source string
- `start: s32` - Starting index (0-based)
- `count: s32` - Number of characters (optional, defaults to rest of string)

**Returns:** `string` - Substring

**Example:**
```javascript
var s = "Hello, World!"
var sub1 = substr(s, 0, 5)   // sub1 = "Hello"
var sub2 = substr(s, 7, 5)   // sub2 = "World"
var sub3 = substr(s, 7)      // sub3 = "World!" (to end)
```

### `to_upper(s)`

Converts a string to uppercase.

**Parameters:**
- `s: string` - String to convert

**Returns:** `string` - Uppercase version

**Example:**
```javascript
var upper = to_upper("hello")  // upper = "HELLO"
```

### `to_lower(s)`

Converts a string to lowercase.

**Parameters:**
- `s: string` - String to convert

**Returns:** `string` - Lowercase version

**Example:**
```javascript
var lower = to_lower("HELLO")  // lower = "hello"
```

### `trim(s)`

Trims whitespace from both ends of a string.

**Parameters:**
- `s: string` - String to trim

**Returns:** `string` - Trimmed string

**Example:**
```javascript
var trimmed = trim("  hello  ")  // trimmed = "hello"
```

---

## String Escaping

### `to_shellarg(s)`

Escapes a string for safe use in shell commands.

**Parameters:**
- `s: string` - String to escape

**Returns:** `string` - Shell-safe string wrapped in single quotes

**Description:**
Wraps the string in single quotes and escapes any single quotes within the string using `'\''`.

**Example:**
```javascript
var filename = "test's file.txt"
var safe = to_shellarg(filename)
// safe = 'test'\''s file.txt'

shell("cat", safe)  // Safely passes filename to shell
```

### `to_htmlsafe(s)`

Escapes HTML special characters for safe use in HTML.

**Parameters:**
- `s: string` - String to escape

**Returns:** `string` - HTML-safe string

**Description:**
Escapes the following characters:
- `&` → `&amp;`
- `<` → `&lt;`
- `>` → `&gt;`
- `"` → `&quot;`
- `'` → `&#39;`

**Example:**
```javascript
var html = "<script>alert('XSS')</script>"
var safe = to_htmlsafe(html)
// safe = "&lt;script&gt;alert(&#39;XSS&#39;)&lt;/script&gt;"
print(safe)
```

### `to_sqlarg(s)`

Escapes single quotes for safe use in SQL queries.

**Parameters:**
- `s: string` - String to escape

**Returns:** `string` - SQL-safe string

**Description:**
Escapes single quotes by doubling them (`'` → `''`).

**Example:**
```javascript
var name = "O'Reilly"
var safe = to_sqlarg(name)
// safe = "O''Reilly"

var query = "SELECT * FROM books WHERE author = '" + safe + "'"
// Safe to use in SQL query
```

---

## Type Conversion

### `to_s32(value)`

Converts a value to a 32-bit signed integer.

**Parameters:**
- `value: any` - Value to convert

**Returns:** `s32` - Integer value

**Description:**
- Numeric values are truncated to integer
- Strings are parsed as numbers (returns 0 if invalid)
- Other types return 0

**Example:**
```javascript
var i1 = to_s32(3.7)      // i1 = 3
var i2 = to_s32("42")     // i2 = 42
var i3 = to_s32("hello")  // i3 = 0
```

### `to_f32(value)`

Converts a value to a 32-bit floating point number.

**Parameters:**
- `value: any` - Value to convert

**Returns:** `f32` - Float value

**Description:**
- Numeric values are converted to float
- Strings are parsed as numbers (returns 0.0 if invalid)
- Other types return 0.0

**Example:**
```javascript
var f1 = to_f32(42)       // f1 = 42.0
var f2 = to_f32("3.14")   // f2 = 3.14
var f3 = to_f32("hello")  // f3 = 0.0
```

### `to_string(value)`

Converts any value to a string representation.

**Parameters:**
- `value: any` - Value to convert

**Returns:** `string` - String representation

**Description:**
- Numbers are converted to their string representation
- Vectors are formatted as "vec2(x,y)", "vec3(x,y,z)", etc.
- Booleans become "true" or "false"
- Arrays show element count

**Example:**
```javascript
var s1 = to_string(42)              // s1 = "42"
var s2 = to_string(3.14)            // s2 = "3.14"
var s3 = to_string(true)            // s3 = "true"
var s4 = to_string(vec3(1, 2, 3))   // s4 = "vec3(1,2,3)"
```

### `to_bool(value)`

Converts a value to a boolean.

**Parameters:**
- `value: any` - Value to convert

**Returns:** `bool` - Boolean value

**Description:**
- String values: "true", "yes", "on", "1" → true; "false", "no", "off", "0" → false
- Numbers: 0 is false, non-zero is true
- Empty strings are false
- None is false
- Vectors: zero vectors are false, non-zero are true

**Example:**
```javascript
var b1 = to_bool("true")   // b1 = true
var b2 = to_bool(0)        // b2 = false
var b3 = to_bool(42)       // b3 = true
var b4 = to_bool("")       // b4 = false
```

---

## Type Inspection

### `typeof(value)`

Returns the type name of a value as a string.

**Parameters:**
- `value: any` - Value to inspect

**Returns:** `string` - Type name

**Possible return values:**
- `"S32"` - 32-bit signed integer
- `"F32"` - 32-bit floating point
- `"String"` - String
- `"Bool"` - Boolean
- `"Array"` - Array/object/map
- `"Vector2"` - 2D vector
- `"Vector3"` - 3D vector
- `"Vector4"` - 4D vector
- `"None"` - None/null value

**Example:**
```javascript
print(typeof(42))           // S32
print(typeof(3.14))         // F32
print(typeof("hello"))      // String
print(typeof(true))         // Bool
print(typeof({x: 10}))      // Array
print(typeof(vec3(1,2,3)))  // Vector3
```

---

## Vector Functions

### `vec2(x, y)`

Creates a 2D vector.

**Parameters:**
- `x: number` - X component
- `y: number` - Y component

**Returns:** `vec2` - 2D vector

**Example:**
```javascript
var position = vec2(10.0, 20.0)
var velocity = vec2(1.5, -2.0)
```

### `vec3(x, y, z)`

Creates a 3D vector.

**Parameters:**
- `x: number` - X component
- `y: number` - Y component
- `z: number` - Z component

**Returns:** `vec3` - 3D vector

**Example:**
```javascript
var position = vec3(10.0, 0.0, 5.0)
var color = vec3(1.0, 0.5, 0.0)  // RGB color
```

### `vec4(x, y, z, w)`

Creates a 4D vector.

**Parameters:**
- `x: number` - X component
- `y: number` - Y component
- `z: number` - Z component
- `w: number` - W component

**Returns:** `vec4` - 4D vector

**Example:**
```javascript
var color = vec4(1.0, 0.5, 0.0, 1.0)  // RGBA color
var quaternion = vec4(0.0, 0.0, 0.0, 1.0)  // Identity quaternion
```

### Vector Operations

Vectors support arithmetic operations:

```javascript
var v1 = vec3(1.0, 2.0, 3.0)
var v2 = vec3(4.0, 5.0, 6.0)

// Addition
var sum = v1 + v2  // vec3(5.0, 7.0, 9.0)

// Subtraction
var diff = v2 - v1  // vec3(3.0, 3.0, 3.0)

// Scalar multiplication
var scaled = v1 * 2.0  // vec3(2.0, 4.0, 6.0)

// Negation
var neg = -v1  // vec3(-1.0, -2.0, -3.0)

// Comparison
var equal = (v1 == v2)     // false
var notEqual = (v1 != v2)  // true
```

---

## Array Functions

### Object Literal Syntax

UdonScript supports JavaScript-style object literal syntax with PHP array semantics:

```javascript
// Create object with properties
var person = {name: "Alice", age: 30, active: true}

// Access properties
print(person.name)   // Alice
print(person.age)    // 30

// Modify properties
person.age = 31

// Arrays with numeric keys
var colors = {0: "red", 1: "green", 2: "blue"}
print(colors.0)  // red

// Mixed keys
var data = {x: 10, y: 20, label: "point"}
```

### `array_keys(arr)`

Returns an array of all keys in an array/object (internal function used by foreach).

**Parameters:**
- `arr: array` - Array or object

**Returns:** `array` - Array of keys as strings

**Example:**
```javascript
var data = {name: "Alice", age: 30, city: "NYC"}
var keys = array_keys(data)
// keys contains: ["name", "age", "city"]
```

### `array_len(arr)`

Returns the number of elements in an array/object (internal function used by foreach).

**Parameters:**
- `arr: array` - Array or object

**Returns:** `s32` - Number of elements

**Example:**
```javascript
var data = {x: 10, y: 20, z: 30}
var count = array_len(data)  // count = 3
```

### `array_get(arr, key)`

Gets a value from an array by key (internal function used by foreach).

**Parameters:**
- `arr: array` - Array or object
- `key: any` - Key to look up

**Returns:** `any` - Value at key, or `none` if not found

**Example:**
```javascript
var data = {name: "Alice", age: 30}
var name = array_get(data, "name")  // "Alice"
var missing = array_get(data, "city")  // none
```

### Foreach Iteration

UdonScript supports foreach loops with both key-only and key-value syntax:

```javascript
var data = {x: 10, y: 20, z: 30}

// Key and value
foreach (var key, value in data) {
    print(key, "=", value)
}

// Key only
foreach (var key in data) {
    print(key)
}
```

---

## Utility Functions

### Array/Map Access

Arrays and maps use property access syntax:

```javascript
var data = {}

// Set values
data.name = "Alice"
data.age = 30
data.score = 100

// Get values
var name = data.name      // "Alice"
var age = data.age        // 30

// Check existence
var hasName = data.name != none

// Get size
var size = len(data)      // 3

// Iterate
foreach (key, value in data) {
    print(key + " = " + to_string(value))
}
```

---

## Constants

UdonScript doesn't have built-in constants, but you can define them as global variables:

```javascript
var PI = 3.14159265359
var E = 2.71828182846
var GRAVITY = 9.81

function circleArea(radius) {
    return PI * radius * radius
}
```

---

## Examples

### Example 1: Distance Calculation

```javascript
function distance(x1, y1, x2, y2) {
    var dx = x2 - x1
    var dy = y2 - y1
    return sqrt(dx * dx + dy * dy)
}

function main() {
    var dist = distance(0.0, 0.0, 3.0, 4.0)
    print("Distance: " + to_string(dist))  // Distance: 5.0
}
```

### Example 2: String Processing

```javascript
function processText(input) {
    var trimmed = trim(input)
    var upper = to_upper(trimmed)
    var length = len(upper)
    
    print("Original: '" + input + "'")
    print("Processed: '" + upper + "'")
    print("Length: " + to_string(length))
}

function main() {
    processText("  hello world  ")
    // Output:
    // Original: '  hello world  '
    // Processed: 'HELLO WORLD'
    // Length: 11
}
```

### Example 3: Vector Math

```javascript
function vectorDemo() {
    var forward = vec3(0.0, 0.0, 1.0)
    var right = vec3(1.0, 0.0, 0.0)
    var up = vec3(0.0, 1.0, 0.0)
    
    var diagonal = forward + right + up
    print("Diagonal: " + to_string(diagonal))
    
    var scaled = diagonal * 2.0
    print("Scaled: " + to_string(scaled))
}
```

### Example 4: Data Processing

```javascript
function analyzeScore(score) {
    var grade = ""
    
    if (score >= 90) {
        grade = "A"
    } else if (score >= 80) {
        grade = "B"
    } else if (score >= 70) {
        grade = "C"
    } else if (score >= 60) {
        grade = "D"
    } else {
        grade = "F"
    }
    
    print("Score: " + to_string(score) + " -> Grade: " + grade)
    return grade
}

function main() {
    analyzeScore(95)  // Score: 95 -> Grade: A
    analyzeScore(82)  // Score: 82 -> Grade: B
    analyzeScore(58)  // Score: 58 -> Grade: F
}
```

### Example 5: File I/O

```javascript
function saveGameData(playerName, score) {
    var data = "Player: " + playerName
    data = data + "\nScore: " + to_string(score)
    save_to_file("savegame.txt", data)
    print("Game saved!")
}

function loadGameData() {
    var content = load_from_file("savegame.txt")
    print("Loaded data:")
    print(content)
}
```

---

## Error Handling

Many built-in functions will trigger errors if:
- Invalid arguments are provided
- File operations fail
- Mathematical operations are invalid (e.g., sqrt of negative number)

When an error occurs, the script execution stops and an error message is returned to the host application.

---

## Performance Notes

1. **String concatenation**: Creating large strings with many concatenations can be slow. Consider building arrays and joining them if performance is critical.

2. **Array/Map access**: Property access is efficient, but avoid unnecessary lookups in tight loops.

3. **Type conversions**: Minimize unnecessary type conversions (`to_string`, `to_s32`, etc.) in performance-critical code.

4. **Function calls**: Function calls have overhead. Very tight inner loops might benefit from inlining logic.

5. **Vector operations**: Vector arithmetic is efficient and should be preferred over component-wise operations when possible.
