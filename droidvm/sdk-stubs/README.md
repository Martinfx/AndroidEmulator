# sdk-stubs

Minimal compile-time stand-ins for `android.jar`, the same idea as the real SDK's
`android.jar` (every method body is `throw new RuntimeException("Stub!")`).
They are used only by `javac` to compile the samples; at run time droidvm
provides the real (native) implementations. If you have the Android SDK,
you can compile against its `platforms/android-XX/android.jar` instead.
