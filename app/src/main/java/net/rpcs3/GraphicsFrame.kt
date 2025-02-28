package net.rpcs3

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import kotlin.system.exitProcess

class GraphicsFrame : SurfaceView, SurfaceHolder.Callback {
    private var lateInit: String = "";
    private var hasSurface = false;
    private val handler = Handler(Looper.getMainLooper())

    constructor(context: Context) : super(context) {
        holder.addCallback(this)
    }
    constructor(context: Context?, attrs: AttributeSet?) : super(context, attrs) {
        holder.addCallback(this)
    }
    constructor(context: Context?, attrs: AttributeSet?, defStyleAttr: Int) : super(context, attrs, defStyleAttr) {
        holder.addCallback(this)
    }
    constructor(context: Context?, attrs: AttributeSet?, defStyleAttr: Int, defStyleRes: Int): super(context, attrs, defStyleAttr, defStyleRes) {
        holder.addCallback(this)
    }

    fun boot(path: String) {
        if (!hasSurface) {
            lateInit = path
        } else {
            RPCS3.instance.boot(holder.surface, path)
        }
    }

    override fun surfaceCreated(p0: SurfaceHolder) {
        handler.post {
            hasSurface = true
            val path = lateInit

            if (path.isNotEmpty()) {
                lateInit = ""
                RPCS3.instance.boot(p0.surface, path)
            }
        }
    }

    override fun surfaceChanged(p0: SurfaceHolder, p1: Int, p2: Int, p3: Int) {
        Log.e("Main", "Surface changed")
//        TODO("Not yet implemented")
    }

    override fun surfaceDestroyed(p0: SurfaceHolder) {
//        TODO("Not yet implemented")
        hasSurface = false
    }

}