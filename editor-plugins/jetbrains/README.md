# UdonScript JetBrains Plugin (skeleton)

This folder contains a minimal starting point for a JetBrains plugin. It registers the UdonScript file type and a very simple lexer-based syntax highlighter.

## Structure
- `build.gradle` / `settings.gradle`: Gradle IntelliJ Plugin setup
- `src/main/java/udon/` : plugin source (file type, language, highlighter)

## Building & Installing
1. Install JDK 17+ and Gradle.
2. From this folder: `gradle buildPlugin`
3. Install the generated ZIP from `build/distributions/udonscript-*.zip` via *Settings → Plugins → Install plugin from disk...*

This is intentionally minimal and not published; extend as needed (lexer, parser) for richer support.
