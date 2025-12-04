# UdonScript Language Specification

## Overview

UdonScript is a dynamically-typed scripting language designed for embedding in C++ applications. It features a simple, C-like syntax with support for functions, variables, control flow, and built-in types including vectors.

## Table of Contents

1. [Lexical Elements](#lexical-elements)
2. [Data Types](#data-types)
3. [Variables](#variables)
4. [Operators](#operators)
5. [Control Flow](#control-flow)
6. [Functions](#functions)
7. [Arrays and Maps](#arrays-and-maps)
8. [Comments](#comments)
9. [Type System](#type-system)

---

## Lexical Elements

### Keywords

The following are reserved keywords in UdonScript:

- `function` - Function definition
- `var` - Variable declaration
- `if` / `else` - Conditional statements
- `while` - While loop
- `for` - For loop
- `foreach` - Foreach loop
- `switch` / `case` / `default` - Switch statement
- `return` - Return from function
- `on` - Event handler definition

### Identifiers

Identifiers must start with a letter or underscore, followed by any combination of letters, numbers, or underscores.

```
Valid:   myVar, _private, count123, getUserName
Invalid: 123abc, my-var, function
```

### Literals

#### Number Literals
```javascript
42          // Integer
3.14159     // Floating point
-10         // Negative integer
2.5e3       // Scientific notation (2500.0)
```

#### String Literals
```javascript
"Hello, World!"
"Line with \"quotes\""
'Single quotes also work'
```

#### Boolean Literals
```javascript
true
false
```

### Statement Boundaries

The parser is expression-based and does not rely on automatic semicolon insertion. Semicolons are treated like whitespace everywhere except inside `for (...)` headers, where they still separate the init/condition/increment parts.

You can freely break lines or place multiple statements on one line without semicolons:

```javascript
var x = 10 var y = 20 print("Sum: " + to_string(x + y))

var total = add(
    foo,
    bar)

return(total) // return values are written with parentheses

for (var i = 0; i < 10; i = i + 1) {
    // ...
}
```

---

## Data Types

UdonScript supports the following data types:

### Primitive Types

- **`None`** - Represents absence of value (similar to null/nil)
- **`Bool`** - Boolean value (`true` or `false`)
- **`Int`** - 64-bit signed integer
- **`Float`** - 64-bit floating point
- **`String`** - Text string

### Vector Types

- **`Vector2`** - 2D vector with `x`, `y` components
- **`Vector3`** - 3D vector with `x`, `y`, `z` components
- **`Vector4`** - 4D vector with `x`, `y`, `z`, `w` components

### Complex Types

- **`Array`** - Dynamic array/map structure (associative array)
- **`Function`** - Callable value produced by function declarations or anonymous function expressions
- **`VariableReference`** - Internal type for variable references

---

## Variables

### Declaration

Variables are declared using the `var` keyword:

```javascript
var x = 10
var name = "Alice"
var position = vec3(0.0, 1.0, 0.0)
```

### Type Annotations (Optional)

While UdonScript is dynamically typed, you can optionally specify types for documentation:

```javascript
var count: Int = 0
var ratio: Float = 0.5
var message: String = "Hello"
```

Multiple variables can be declared and assigned from a multi-value expression (commas return arrays):

```javascript
var x, y = pair(5)   // pair returns (5, 6)
var _, only = pair(3) // '_' ignores a slot
x, y = pair(10)      // destructuring assignment
```

### Global Variables

Variables declared at the top level (outside functions) are global:

```javascript
var globalCounter = 0

function increment() {
    globalCounter = globalCounter + 1
}
```

### Local Variables

Variables declared inside functions are local to that function:

```javascript
function example() {
    var local = 10  // Only visible inside this function
}
```

---

## Operators

### Arithmetic Operators

```javascript
x + y   // Addition
x - y   // Subtraction
x * y   // Multiplication
x / y   // Division
x % y   // Modulo (remainder)
-x      // Negation
```

### Comparison Operators

```javascript
x == y  // Equal to
x != y  // Not equal to
x < y   // Less than
x <= y  // Less than or equal to
x > y   // Greater than
x >= y  // Greater than or equal to
```

### Logical Operators

```javascript
!x      // Logical NOT
x && y  // Logical AND (short-circuit)
x || y  // Logical OR (short-circuit)
```

### String Concatenation

```javascript
"Hello, " + "World!"  // Result: "Hello, World!"
"Count: " + to_string(42)  // Result: "Count: 42"
```

### Property Access

```javascript
obj:prop        // Array/map key access (safe)
obj:prop = 10   // Set key
value.method()  // Call built-in or user-defined method (dot is for methods)
```

Colon access `obj:foo:bar` is equivalent to `obj["foo"]["bar"]` and returns `none` instead of raising errors when an intermediate value is missing or not an array/map.

Square-bracket access is also safe: `obj["missing"]` and `obj[5]` yield `none` when the target cannot be indexed (mirroring PHP-style nullish access). Dot (`.`) is reserved for method calls: `"text".to_lower()`, `{x:1}.len()`.

---

## Control Flow

### If Statement

```javascript
if (condition) {
    // code
}

if (condition) {
    // code
} else {
    // code
}

if (condition1) {
    // code
} else if (condition2) {
    // code
} else {
    // code
}
```

### While Loop

```javascript
while (condition) {
    // code
}
```

Example:
```javascript
var i = 0
while (i < 10) {
    print(i)
    i = i + 1
}
```

### For Loop

```javascript
for (initialization; condition; increment) {
    // code
}
```

Example:
```javascript
for (var i = 0; i < 10; i = i + 1) {
    print(i)
}
```

### Foreach Loop

Iterate over array/map entries:

```javascript
foreach (key, value in collection) {
    // code
}
```

Example:
```javascript
var data = {}
data.name = "Alice"
data.age = 30

foreach (key, value in data) {
    print(key + ": " + to_string(value))
}
```

### Switch Statement

```javascript
switch (expression) {
    case value1:
        // code
    case value2:
        // code
    default:
        // code
}
```

Example:
```javascript
switch (day) {
    case "Monday":
        print("Start of week")
    case "Friday":
        print("End of week")
    default:
        print("Middle of week")
}
```

---

## Functions

### Function Definition

```javascript
function functionName(param1, param2) {
    // code
    return(value)
}
```

### Parameters

Functions can have zero or more parameters:

```javascript
function noParams() {
    print("No parameters")
}

function oneParam(x) {
    return(x * 2)
}

function multipleParams(a, b, c) {
    return(a + b + c)
}
```

### Type Annotations (Optional)

Parameters and return types can be annotated:

```javascript
function add(a: Int, b: Int) -> Int {
    return(a + b)
}
```

### Context Info

The `context` global contains meta information about the script.

`context:comment_lines` contains all comments in the script.

### Anonymous Functions (Closures)

Anonymous functions can be created as expressions using the same syntax as a normal function, but without a name. They capture visible locals by reference (mutations after creation are observed):

```javascript
var bias = 3
var add_bias = function(x) {
    return(x + bias)
}
bias = 100
print(add_bias(4)) // prints 104
```

Closures are first-class values stored in variables, array/map properties, or returned from other functions. Any callable value can be invoked by name like a normal function call.

### Named Arguments

Functions can be called with named arguments:

```javascript
function greet(firstName, lastName, title) {
    print(title + " " + firstName + " " + lastName)
}

// Call with named arguments
greet(lastName="Smith", firstName="John", title="Dr.")
```

### Return Values

Use `return(expr)` to return a value from a function. Comma-separated expressions return an array that can be destructured:

```javascript
function square(x) {
    return(x * x)
}

function pair(x) {
    return(x, x + 1)
}

var result = square(5)  // result = 25
```

Functions without a return statement implicitly return `None`.

### Main Function

By convention, the entry point is the `main()` function:

```javascript
function main() {
    print("Hello, World!")
}

### Method Call Sugar

You can call functions and built-ins using method syntax: `receiver.method(arg1, arg2)`. The receiver is automatically passed as the first positional argument to the target function. Examples:

```javascript
"Hello".to_lower()       // same as to_lower("Hello")
{a: 1, b: 2}.len()       // same as len({a:1, b:2})
```

For arrays/maps, if a property with the same name exists and is a callable value, that property is invoked before any built-in or global function of the same name. If the property exists but is not callable, an error is raised.
```

---

## Arrays and Maps

UdonScript uses a unified Array/Map type that can function as both a dynamic array and an associative array (hash map).

### Creating Arrays/Maps

```javascript
var arr = {}        // Empty array/map
var data = {}
data.x = 10         // Set by key
data.y = 20
data.name = "Point"
```

### Accessing Elements

```javascript
var value = arr.key     // Get value by key
arr.key = 42           // Set value by key
```

### Array Functions

Use the built-in `len()` function to get the size:

```javascript
var count = len(arr)
```

Use `keys(arr)` or `arr.keys()` to get an array of key names:

```javascript
var names = keys(arr)
```

`sort(arr, options?)` returns a sorted copy of `arr` (does not mutate the input). Useful options: `reverse` (bool), `keep_keys` (preserve original keys instead of reindexing), `by` (`"value"` or `"key"`), and `key` (function to compute a sort key).
`ksort(arr, options?)` returns a copy ordered by keys (numeric keys first, ascending); option `reverse` flips the order.

### Iteration

Use `foreach` to iterate:

```javascript
foreach (key, value in arr) {
    print(key + " = " + to_string(value))
}
```

---

## Comments

### Single-line Comments

```javascript
// This is a single-line comment
var x = 10  // Comment after code
```

### Multi-line Comments

```javascript
/*
 * This is a multi-line comment
 * It can span multiple lines
 */
```

---

## Type System

### Dynamic Typing

UdonScript is dynamically typed. Variables can hold values of any type:

```javascript
var x = 10      // x is now an integer
x = "hello"     // x is now a string
x = vec3(1, 2, 3)  // x is now a vector
```

### Type Checking

Use the type conversion functions to check and convert types:

```javascript
var value = to_int("42")     // Convert to integer
var text = to_string(123)    // Convert to string
var flag = to_bool("true")   // Convert to boolean
```

### Truthy and Falsy Values

The following values are considered "falsy" in boolean contexts:
- `false` (boolean false)
- `0` (integer zero)
- `0.0` (float zero)
- `""` (empty string)
- `None`
- Zero vectors: `vec2(0,0)`, `vec3(0,0,0)`, `vec4(0,0,0,0)`

All other values are "truthy".

---

## Event Handlers

Functions can be registered as event handlers using the `on` keyword:

```javascript
function on update myUpdateHandler() {
    // Called when "update" event fires
}

// Alternative syntax
function myHandler() on update {
    // Called when "update" event fires
}
```

Event handlers are triggered from the host application using the interpreter's `run_eventhandlers()` method.

---

## Complete Example

```javascript
// Global variables
var score = 0
var playerName = "Player"

// Helper function
function calculateBonus(basePoints, multiplier) {
    return(basePoints * multiplier)
}

// Main game loop function
function main() {
    print("Welcome, " + playerName + "!")
    
    // Game logic
    var basePoints = 100
    var multiplier = 1.5
    score = calculateBonus(basePoints, multiplier)
    
    print("Final score: " + to_string(score))
    
    // Vector math
    var position = vec3(10.0, 0.0, 5.0)
    var velocity = vec3(1.0, 0.0, 0.0)
    
    // Conditionals
    if (score > 100) {
        print("High score achieved!")
    } else {
        print("Try again!")
    }
    
    // Loops
    for (var i = 0; i < 3; i = i + 1) {
        print("Iteration: " + to_string(i))
    }
}
```

---

## Integration with C++

UdonScript is designed to be embedded in C++ applications:

```cpp
#include "core/udonscript.h"

UdonInterpreter interp;

// Compile script
CodeLocation result = interp.compile(scriptSource);

// Run main function
UdonValue returnValue;
interp.run("main", {}, {}, returnValue);

// Register custom built-in functions
interp.register_function("myFunction", "x:number", "number",
    [](UdonInterpreter*, const std::vector<UdonValue>& args,
       const std::unordered_map<std::string, UdonValue>&,
       UdonValue& out, CodeLocation& err) {
        // Implementation
        return true;
    });
```

---

## Best Practices

1. **Use descriptive variable names**: `playerHealth` instead of `h`
2. **Keep functions focused**: Each function should do one thing well
3. **Comment complex logic**: Help future maintainers understand your code
4. **Initialize variables**: Always give variables an initial value
5. **Check return values**: Especially from built-in functions that may fail
6. **Use type annotations**: Optional but helpful for documentation
7. **Consistent naming**: Choose a naming convention and stick to it

---

## Grammar Summary

```
program         → declaration* EOF
declaration     → functionDecl | varDecl | statement
functionDecl    → "function" IDENTIFIER "(" parameters? ")" "{" statement* "}"
varDecl         → "var" IDENTIFIER ( ":" type )? ( "=" expression )? 
statement       → exprStmt | ifStmt | whileStmt | forStmt | 
                  foreachStmt | switchStmt | returnStmt | block
block           → "{" statement* "}"
exprStmt        → expression
ifStmt          → "if" "(" expression ")" statement ( "else" statement )?
whileStmt       → "while" "(" expression ")" statement
forStmt         → "for" "(" (varDecl | exprStmt)? ";" expression? ";" expression? ")" statement
foreachStmt     → "foreach" "(" IDENTIFIER "," IDENTIFIER "in" expression ")" statement
switchStmt      → "switch" "(" expression ")" "{" caseStmt* defaultStmt? "}"
caseStmt        → "case" expression ":" statement*
defaultStmt     → "default" ":" statement*
returnStmt      → "return" "(" expression? ")"
expression      → assignment
assignment      → ( property "=" )? logicalOr
logicalOr       → logicalAnd ( "||" logicalAnd )*
logicalAnd      → equality ( "&&" equality )*
equality        → comparison ( ( "!=" | "==" ) comparison )*
comparison      → term ( ( ">" | ">=" | "<" | "<=" ) term )*
term            → factor ( ( "-" | "+" ) factor )*
factor          → unary ( ( "/" | "*" ) unary )*
unary           → ( "!" | "-" ) unary | call
call            → primary ( "(" arguments? ")" | "." IDENTIFIER )*
primary         → NUMBER | STRING | "true" | "false" | "none" 
                | IDENTIFIER | "(" expression ")"
arguments       → expression ( "," expression )*
parameters      → IDENTIFIER ( ":" type )? ( "," IDENTIFIER ( ":" type )? )*
type            → "Int" | "Float" | "Bool" | "String" | "vec2" | "vec3" | "vec4" | "any"
```
