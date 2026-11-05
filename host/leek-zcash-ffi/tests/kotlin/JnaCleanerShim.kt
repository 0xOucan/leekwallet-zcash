// Test-only: JNA's Cleaner (public since JNA 5.12), for running the smoke
// test with an older JNA such as the 5.6.0 in some Gradle caches. Backed by
// java.lang.ref.Cleaner. Apps ship JNA 5.12 or later and never see this;
// scripts/kotlin-smoke.sh adds it only when the JNA it found is older.
package com.sun.jna.internal

class Cleaner private constructor() {
    interface Cleanable {
        fun clean()
    }

    fun register(obj: Any, cleanupTask: Runnable): Cleanable {
        val c = jlr.register(obj, cleanupTask)
        return object : Cleanable {
            override fun clean() = c.clean()
        }
    }

    companion object {
        private val jlr = java.lang.ref.Cleaner.create()
        private val INSTANCE = Cleaner()

        @JvmStatic
        fun getCleaner(): Cleaner = INSTANCE
    }
}
