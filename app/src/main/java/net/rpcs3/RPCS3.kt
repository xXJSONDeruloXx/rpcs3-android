package net.rpcs3

import android.view.Surface

class RPCS3 {
    external fun initialize(rootDir: String): Boolean
    external fun installFw(fd: Int, progressId: Long): Boolean
    external fun installPkgFile(fd: Int, progressId: Long): Boolean
    external fun boot(path: String): Boolean
    external fun surfaceEvent(surface: Surface, event: Int): Boolean

    companion object {
        val instance = RPCS3()
        var rootDirectory: String = ""

        init {
            System.loadLibrary("rpcs3-android")
        }
    }
}
