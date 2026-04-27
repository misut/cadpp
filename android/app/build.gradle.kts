plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.github.misut.cadpp"
    compileSdk = 35
    // Aligns with the NDK exon uses to produce libcadpp.a /
    // libphenotype-modules.a. Override with -PcadppNdkVersion=... if your
    // environment pins a different revision.
    ndkVersion = (project.findProperty("cadppNdkVersion") as String?)
        ?: "30.0.14904198-beta1"

    defaultConfig {
        applicationId = "io.github.misut.cadpp"
        minSdk = 33
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.1"

        ndk {
            // M6c ships aarch64 only — that's the one target exon's
            // toolchain currently emits cadpp / phenotype archives for.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++23")
                // exon vendors phenotype + cppx + txn + jsoncpp + the
                // generated std module into cadpp's own Android build
                // directory because cadpp path-deps phenotype, so a
                // single CADPP_LIB_DIR carries everything except the
                // libredwg static archive (which lives in _deps).
                // Override either with -PcadppLibDir=/x -PlibredwgLib=/y.
                val cadppLibDir = (project.findProperty("cadppLibDir") as String?)
                    ?: "${rootDir}/../.exon/aarch64-linux-android/debug"
                val libredwgLib = (project.findProperty("libredwgLib") as String?)
                    ?: "$cadppLibDir/_deps/libredwg-build/libredwg.a"
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DCADPP_LIB_DIR=$cadppLibDir",
                    "-DLIBREDWG_LIB=$libredwgLib"
                )
            }
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.28.0+"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

dependencies {
    implementation("androidx.games:games-activity:3.0.5")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.core:core:1.16.0")
}
