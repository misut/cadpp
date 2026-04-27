package io.github.misut.cadpp

import com.google.androidgamesdk.GameActivity

// All app logic lives in cadpp_android_main.cpp via GameActivity's
// native_app_glue bridge, so the JVM side has nothing to do.
class MainActivity : GameActivity()
