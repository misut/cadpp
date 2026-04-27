// Root Gradle script. Plugin versions live here; subprojects only apply.
// Versions track phenotype/examples/android verbatim because the NDK and
// AGP need to agree across the linked static archives — drift between
// the two would silently change C++ ABI on the next build.
plugins {
    id("com.android.application")        version "8.7.3" apply false
    id("org.jetbrains.kotlin.android")    version "2.0.21" apply false
}
