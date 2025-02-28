package net.rpcs3

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.AssetFileDescriptor
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.Message
import android.util.Log
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import net.rpcs3.databinding.ActivityMainBinding
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.net.URI
import java.util.concurrent.ConcurrentHashMap


private enum class PermRequestId(val id: Int, val key: String) {
    @SuppressLint("InlinedApi")
    PostNotifications(100, Manifest.permission.POST_NOTIFICATIONS),
}

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding

    private var progressHandlers = ConcurrentHashMap<Long, (Long, Long) -> Unit>();
    private var nextRequestId = 0L;

    fun onProgressEvent(requestId: Long, value: Long, max: Long): Boolean {
        val impl = progressHandlers[requestId] ?: return false;

        impl(value, max);

        if ((max != 0L && value == max) || value < 0) {
            cancelProgress(requestId)
        }

        return true;
    }

    private fun requestPermission(perm: PermRequestId): Boolean {
        if (checkSelfPermission(perm.key) == PackageManager.PERMISSION_GRANTED) {
            return true
        }

        ActivityCompat.requestPermissions(this,  arrayOf(perm.key), perm.id)
        return false
    }

    override fun onRequestPermissionsResult(requestCode: Int,
                                            permissions: Array<String>,
                                            grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
//        if (requestCode == PermRequestId.PostNotifications.id && grantResults[0] != PackageManager.PERMISSION_GRANTED) {
//            requestPermission(PermRequestId.PostNotifications)
//            return
//        }
    }

    private fun createProgress(title: String, handler: (Long, Long) -> Unit): Long {
        var requestId: Long;
        val noop = { _: Long, _: Long -> }
        while (true) {
            requestId = nextRequestId++;
            if (progressHandlers.put(requestId, noop) == null) {
                break;
            }
        }

        val hasPermission = checkSelfPermission(PermRequestId.PostNotifications.key) == PackageManager.PERMISSION_GRANTED;

        val builder = NotificationCompat.Builder(this, "rpcs3-progress").apply {
            setContentTitle(title)
            setSmallIcon(R.drawable.ic_launcher_foreground)
            setCategory(NotificationCompat.CATEGORY_SERVICE)
            setPriority(NotificationCompat.PRIORITY_DEFAULT)
            setProgress(0, 0, true)
            setSilent(true)
        }
        val notificationManager = NotificationManagerCompat.from(this);

        if (hasPermission) {
            with(notificationManager) {
                notify(requestId.toInt(), builder.build())
            }
        }

        val asyncHandler = Handler.createAsync(Looper.getMainLooper()) { message ->
            val value = message.data.getLong("value")
            val max = message.data.getLong("max")

            if (hasPermission) {
                if (value >= 0 && max > 0) {
                    if (value == max) {
                        with(notificationManager) {
                            cancel(requestId.toInt())
                        }
                    } else {
                        builder.setProgress(max.toInt(), value.toInt(), false)
                        with(notificationManager) {
                            notify(requestId.toInt(), builder.build())
                        }
                    }
                } else if (value < 0) {
                    builder.setContentText("Installation failed")
                    with(notificationManager) {
                        notify(requestId.toInt(), builder.build())
                    }
                } else {
                    builder.setProgress(max.toInt(), value.toInt(), true)
                    with(notificationManager) {
                        notify(requestId.toInt(), builder.build())
                    }
                }
            }

            handler(value, max)
            true
        }

        val wrapped = wrapped@{value: Long, max: Long ->
            val message = Message()
            val data = Bundle()
            data.putLong("value", value)
            data.putLong("max", max)
            message.data = data
            asyncHandler.sendMessage(message)
            return@wrapped
        }

        progressHandlers[requestId] = wrapped
        return requestId
    }

    private fun cancelProgress(id: Long) {
        progressHandlers.remove(id)
    }

    private fun openDescriptors(intent: Intent?): ArrayList<AssetFileDescriptor> {
        if (intent == null) {
            return arrayListOf()
        }

        val clipData = intent.clipData;
        val data = intent.data;
        val result = arrayListOf<AssetFileDescriptor>()

        if (clipData != null) {
            val count = clipData.itemCount
            var currentItem = 0
            while (currentItem < count) {
                val uri = clipData.getItemAt(currentItem).uri
                result += contentResolver.openAssetFileDescriptor(uri, "r")!!
                currentItem += 1
            }
        } else if (data != null) {
            result += contentResolver.openAssetFileDescriptor(data, "r")!!
        }

        return result
    }

    private fun saveFile(source: Uri, target: String) {
        var bis: BufferedInputStream? = null
        var bos: BufferedOutputStream? = null

        try {
            bis = BufferedInputStream(FileInputStream(contentResolver.openFileDescriptor(source, "r")!!.fileDescriptor))
            bos = BufferedOutputStream(FileOutputStream(target, false))
            val buf = ByteArray(1024)
            bis.read(buf)
            do {
                bos.write(buf)
            } while (bis.read(buf) != -1)
        } catch (e: IOException) {
            e.printStackTrace()
        } finally {
            bis?.close()
            bos?.close()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        requestPermission(PermRequestId.PostNotifications)

        val notificationManager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.deleteNotificationChannel("rpcs3-progress");
        val channel = NotificationChannel("rpcs3-progress", "Installation progress", NotificationManager.IMPORTANCE_DEFAULT)
        channel.setShowBadge(false)
        channel.lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        notificationManager.deleteNotificationChannel("rpcs3-progress")
        notificationManager.createNotificationChannel(channel)

        val apkDir = applicationContext.filesDir.toString()
        val extCacheDir = applicationContext.externalCacheDir
        val cacheDir = applicationContext.cacheDir
        val extFilesDir = applicationContext.getExternalFilesDir(null)

        RPCS3.instance.initialize(
            extFilesDir.toString(),
            Environment.getDataDirectory().toString() + "/$packageName",
            Environment.getStorageDirectory().toString() + "/$packageName",
            Environment.getExternalStorageDirectory().toString() + "/$packageName",
            Environment.getRootDirectory().toString() + "/$packageName");

        val installFwResultHandler = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                val descriptor = contentResolver.openAssetFileDescriptor(result.data!!.data!!, "r")
                val fd = descriptor?.parcelFileDescriptor?.fd;

                if (fd != null) {
                    val installProgress = createProgress("Firmware Installation") { value, max ->
                        if ((max != 0L && value == max) || value < 0) {
                            descriptor.close();
                        }
                    }

                    if (!RPCS3.instance.installFw(fd,  this, installProgress)) {
                        descriptor.close();
                        onProgressEvent(installProgress, -1, 0);
                    }
                } else {
                    descriptor?.close();
                }
            }
        }

        val installGameResultHandler = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                val descriptors = openDescriptors(result.data);
                if (descriptors.size == 1) {
                    val installProgress = createProgress("Package Installation") { value, max ->
                        if ((max != 0L && value == max) || value < 0) {
                            descriptors.forEach { d -> d.close() }
                        }
                    }

                    if (!RPCS3.instance.installPkgFile(descriptors[0].parcelFileDescriptor.fd, this, installProgress)) {
                        descriptors.forEach { d -> d.close() }
                        onProgressEvent(installProgress, -1, 0);
                    }
                } else {
                    // TODO
                }
            }
        }

        val runSampleResultHandler = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                saveFile(result.data!!.data!!, "$apkDir/sample.elf")
                val emulatorWindow = Intent(
                    this@MainActivity,
                    RPCS3Activity::class.java
                )
                emulatorWindow.putExtra("path", "$apkDir/sample.elf");
                startActivity(emulatorWindow)
            }
        }

        binding.bootGame
            .setOnClickListener {
                val emulatorWindow = Intent(
                    this@MainActivity,
                    RPCS3Activity::class.java
                )
                emulatorWindow.putExtra("path", "");
                startActivity(emulatorWindow)
            }

        binding.installFw
            .setOnClickListener {
                Log.e("Main", "Install fw")

                val intent = Intent()
                    .setType("*/*")
                    .setAction(Intent.ACTION_GET_CONTENT)
                    .addCategory(Intent.CATEGORY_OPENABLE)
                try {
                    installFwResultHandler.launch(Intent.createChooser(intent, "Select PS3UPDAT.PUP"))
                } catch (e: Exception) {
                    Toast.makeText(this, e.toString(), Toast.LENGTH_LONG).show()
                }
            }

        binding.installGame
            .setOnClickListener {
                Log.e("Main", "Install game")

                val intent = Intent()
                    .setType("*/*")
                    .setAction(Intent.ACTION_GET_CONTENT)
                    .addCategory(Intent.CATEGORY_OPENABLE)
//                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);

                try {
                    installGameResultHandler.launch(Intent.createChooser(intent, "Select PKGs"))
                } catch (e: Exception) {
                    Toast.makeText(this, e.toString(), Toast.LENGTH_LONG).show()
                }
            }

        binding.runSample
            .setOnClickListener {
                Log.e("Main", "Run sample")
                val intent = Intent()
                    .setType("*/*")
                    .setAction(Intent.ACTION_GET_CONTENT)
                    .addCategory(Intent.CATEGORY_OPENABLE)
                try {
                    runSampleResultHandler.launch(Intent.createChooser(intent, "Select sample"))
                } catch (e: Exception) {
                    Toast.makeText(this, e.toString(), Toast.LENGTH_LONG).show()
                }
            }
    }
}