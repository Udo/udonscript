# UdonScript JetBrains Plugin (skeleton)

## Requirements
- JDK 17+
- Gradle

## Build & Install
```bash
cd editor-plugins/jetbrains
gradle buildPlugin
# Install the resulting ZIP from build/distributions via
# Settings → Plugins → Install plugin from disk...
```

## Notes
- This is a minimal lexer-based highlighter (comments, strings with escapes, numbers, identifiers, `$template` tokens). It does not include a real parser or brace matching yet.
- You can extend the Flex lexer and PSI if you want richer features.
