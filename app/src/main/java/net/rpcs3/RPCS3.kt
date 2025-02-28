package net.rpcs3

import android.view.Surface

class RPCS3 {
    external fun initialize(apkDir: String, dataDir: String, storageDir: String, externalStorageDir: String, rootDir: String): Boolean;

    external fun installFw(fd: Int, eventHandler: Any, requestId: Long): Boolean;
    external fun installPkgFile(fd: Int, eventHandler: Any, requestId: Long): Boolean;
    external fun boot(surface: Surface, path: String): Boolean;
    //    external fun shutdown(): Void;

    companion object {
        var instance = RPCS3();

        init {
            System.loadLibrary("rpcs3-android")
        }
    }
}
