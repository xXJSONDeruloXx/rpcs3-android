package net.rpcs3

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager

class UsbDeviceRepository {
    companion object {
        private val devices = HashMap<UsbDevice, UsbDeviceConnection>()

        fun attach(device: UsbDevice, usbManager: UsbManager) {
            if (devices[device] != null) {
                return
            }

            val connection = usbManager.openDevice(device)
            devices[device] = connection
            RPCS3.instance.usbDeviceEvent(connection.fileDescriptor, 0);
        }

        fun detach(device: UsbDevice) {
            val connection = devices[device]
            if (connection != null) {
                RPCS3.instance.usbDeviceEvent(connection.fileDescriptor, 1);
                connection.close();

                devices.remove(device)
            }
        }
    }
}