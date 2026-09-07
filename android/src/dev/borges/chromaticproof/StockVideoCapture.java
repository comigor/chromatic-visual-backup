package dev.borges.chromaticproof;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import java.io.IOException;

/**
 * Stock Chromatic USB video capture. Streams the stock UVC isochronous YUY2
 * video interface and delivers interleaved Y,U,V samples for each pixel to a
 * listener. Only the video streaming interface is claimed and alt-set; the
 * CDC cartridge link is never touched, and no firmware or FPGA changes are
 * involved. All device IO runs through native code on a dup() of the
 * permission granted USB descriptor, so no root privileges are needed.
 */
public final class StockVideoCapture implements AutoCloseable {
  private static final int VENDOR_ID_CHROMATIC = 0x374E;
  private static final int USB_CLASS_VIDEO = 0x0E;
  private static final int SUBCLASS_VIDEO_STREAMING = 2;

  static { System.loadLibrary("chromatic_uvc"); }

  /** Receives packed YUV444 samples on the thread that called run. */
  public interface Listener {
    void frame(byte[] yuv, int width, int height);
  }

  /** A Chromatic exposing a stock USB video streaming interface, or null. */
  public static UsbDevice findDevice(UsbManager manager) {
    for (UsbDevice device : manager.getDeviceList().values()) {
      if (device.getVendorId() == VENDOR_ID_CHROMATIC &&
          hasVideoInterface(device)) {
        return device;
      }
    }
    return null;
  }

  private static boolean hasVideoInterface(UsbDevice device) {
    for (int i = 0; i < device.getInterfaceCount(); i++) {
      UsbInterface iface = device.getInterface(i);
      if (iface.getInterfaceClass() == USB_CLASS_VIDEO &&
          iface.getInterfaceSubclass() == SUBCLASS_VIDEO_STREAMING) {
        for (int e = 0; e < iface.getEndpointCount(); e++) {
          UsbEndpoint endpoint = iface.getEndpoint(e);
          if (endpoint.getDirection() == UsbConstants.USB_DIR_IN &&
              endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_ISOC) {
            return true;
          }
        }
      }
    }
    return false;
  }

  private final long handle;
  private boolean closed;
  private boolean started;
  private boolean running;

  /**
   * Opens the stock video interface of a permission granted Chromatic.
   *
   * @throws IOException if the device is not a supported Chromatic video
   *         device or its streaming interface cannot be claimed
   */
  public StockVideoCapture(UsbManager manager, UsbDevice device)
      throws IOException {
    if (device.getVendorId() != VENDOR_ID_CHROMATIC ||
        !hasVideoInterface(device)) {
      throw new IOException(
          "Device is not a supported Chromatic video device.");
    }
    UsbDeviceConnection opened = manager.openDevice(device);
    if (opened == null) {
      throw new IOException(
          "Opening the USB device failed (missing USB permission?).");
    }
    long nativeHandle;
    try {
      nativeHandle = nOpen(opened.getFileDescriptor());
    } catch (IOException | RuntimeException e) {
      opened.close();
      throw e;
    }
    opened.close();
    handle = nativeHandle;
  }

  /**
   * Blocks receiving frames until close is called (from any thread, also
   * from inside the listener), or the device fails.
   *
   * @throws IOException on any streaming failure; never after cancellation
   */
  public void run(Listener listener) throws IOException {
    synchronized (this) {
      if (closed) {
        return;
      }
      if (started) {
        throw new IOException("Capture has already been started.");
      }
      started = true;
      running = true;
    }
    try {
      nRun(handle, listener);
    } finally {
      synchronized (this) {
        running = false;
        closed = true;
        nDestroy(handle);
      }
    }
  }

  /** Cancels a running capture and releases every resource. Idempotent. */
  @Override
  public synchronized void close() {
    if (!closed) {
      closed = true;
      nCancel(handle);
      if (!running) {
        nDestroy(handle);
      }
    }
  }

  private static native long nOpen(int fd) throws IOException;

  private static native void nRun(long handle, Object listener)
      throws IOException;

  private static native void nCancel(long handle);

  private static native void nDestroy(long handle);
}
