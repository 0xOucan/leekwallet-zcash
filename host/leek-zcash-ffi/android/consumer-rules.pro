# R8 / ProGuard keep rules for the leek-zcash-ffi Kotlin bindings
# (io.leekwallet.zcash.sdk) and JNA. Shipped as the AAR's consumer rules;
# with the drop-in layout, add this file to the app's proguardFiles.
#
# Why: the bindings call into libleek_zcash_ffi.so through JNA, and JNA works
# by reflection. It builds java.lang.reflect.Proxy objects over the
# `UniffiLib` / `IntegrityCheckingUniffiLib` interfaces and looks native
# functions up by the Kotlin method names; it reads Structure fields by the
# names in @Structure.FieldOrder; and the Rust side calls back into Kotlin
# through com.sun.jna.Callback objects (the UniffiCallbackInterface*Method*
# vtables behind FrameTransport, KeyWrapper, PairingListener and
# SignListener) whose single method JNA finds by reflection. Nothing in
# Kotlin calls those members directly, so R8 would rename or remove them and
# the first board call fails at runtime (as 3.17.0-leek.1 did with the JNI
# callbacks).

# --- JNA (https://github.com/java-native-access/jna/blob/master/www/FrequentlyAskedQuestions.md)
-dontwarn java.awt.**
-dontwarn com.sun.jna.**
-keep class com.sun.jna.** { *; }
-keep class * implements com.sun.jna.** { *; }
-keepclassmembers class * extends com.sun.jna.** { public *; }

# Structure.FieldOrder and other annotations are read at runtime.
-keepattributes RuntimeVisibleAnnotations,AnnotationDefault,Signature,InnerClasses,EnclosingMethod

# --- UniFFI bindings: the generated package, names and all.
-keep class io.leekwallet.zcash.sdk.** { *; }
-keep interface io.leekwallet.zcash.sdk.** { *; }
-keepnames class io.leekwallet.zcash.sdk.** { *; }

# --- Callback interfaces the app implements. The Rust side reaches these
# through the bindings' vtables; keep every implementation's methods so
# R8 cannot inline, merge or drop them in the app's own classes.
-keep interface io.leekwallet.zcash.sdk.FrameTransport { *; }
-keep interface io.leekwallet.zcash.sdk.KeyWrapper { *; }
-keep interface io.leekwallet.zcash.sdk.PairingListener { *; }
-keep interface io.leekwallet.zcash.sdk.SignListener { *; }
-keep class * implements io.leekwallet.zcash.sdk.FrameTransport { *; }
-keep class * implements io.leekwallet.zcash.sdk.KeyWrapper { *; }
-keep class * implements io.leekwallet.zcash.sdk.PairingListener { *; }
-keep class * implements io.leekwallet.zcash.sdk.SignListener { *; }

# Errors thrown across the boundary (KeyWrapperFailure, TransportFailure,
# LeekException) are matched by class; keep subclasses an app defines too.
-keep class * extends io.leekwallet.zcash.sdk.** { *; }
