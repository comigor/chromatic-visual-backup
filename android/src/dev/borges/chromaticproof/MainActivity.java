package dev.borges.chromaticproof;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.CRC32;

public class MainActivity extends Activity {
  private static final String USB_PERMISSION =
      "dev.borges.chromaticproof.USB_PERMISSION";
  private static final int IMPORT_VIDEO = 1;
  private static final int EXPORT_FILE = 2;
  private UsbManager usb;
  private UsbDevice pendingDevice;
  private TextView status;
  private TextView details;
  private Button receive;
  private Button importVideo;
  private Button save;
  private Button stop;
  private Job active;
  private File verified;
  private String verifiedName;
  private long verifiedCrc;
  private boolean exporting;
  private boolean destroyed;

  private final BroadcastReceiver receiver = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
      if (USB_PERMISSION.equals(intent.getAction())) {
        UsbDevice device = pendingDevice;
        pendingDevice = null;
        if (device != null &&
            intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED,
                                   false) &&
            usb.hasPermission(device)) {
          start(device, null);
        } else {
          message("Permission denied", "No capture started.");
          buttons();
        }
      } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(
                     intent.getAction())) {
        if (active != null && active.device != null &&
            !usb.getDeviceList().containsKey(active.device.getDeviceName())) {
          cancel("USB disconnected. Incomplete transfer discarded; no backup "
                 + "accepted.");
        }
      }
    }
  };

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    usb = (UsbManager)getSystemService(USB_SERVICE);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    int padding = (int)(20 * getResources().getDisplayMetrics().density);
    ScrollView viewport = new ScrollView(this);
    viewport.setFillViewport(true);
    root.setPadding(padding, padding, padding, padding);
    if (Build.VERSION.SDK_INT >= 30) {
      viewport.setOnApplyWindowInsetsListener((view, insets) -> {
        android.graphics.Insets safe = insets.getInsets(
            WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
        return WindowInsets.CONSUMED;
      });
    } else {
      viewport.setFitsSystemWindows(true);
    }
    TextView title = new TextView(this);
    title.setText("Chromatic Visual Backup");
    title.setTextSize(24);
    root.addView(title);
    TextView safety = new TextView(this);
    safety.setText("Stock USB video only. No firmware loading, cartridge "
                   + "commands or SD writes.\n"
                   + "Run the visual ROM and leave its grid looping until "
                   + "verification finishes.");
    root.addView(safety);
    receive = new Button(this);
    receive.setText("Receive stock USB video");
    receive.setOnClickListener(v -> requestCapture());
    root.addView(receive);
    importVideo = new Button(this);
    importVideo.setText("Decode video recording");
    importVideo.setOnClickListener(v -> {
      Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                          .setType("video/*")
                          .addCategory(Intent.CATEGORY_OPENABLE);
      startActivityForResult(intent, IMPORT_VIDEO);
    });
    root.addView(importVideo);
    stop = new Button(this);
    stop.setText("Cancel transfer");
    stop.setOnClickListener(
        v -> cancel("Cancelled. Incomplete transfer discarded."));
    root.addView(stop);
    save = new Button(this);
    save.setText("Save verified file…");
    save.setOnClickListener(v -> {
      Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                          .addCategory(Intent.CATEGORY_OPENABLE)
                          .setType("application/octet-stream")
                          .putExtra(Intent.EXTRA_TITLE, verifiedName);
      startActivityForResult(intent, EXPORT_FILE);
    });
    root.addView(save);
    status = new TextView(this);
    status.setTextSize(20);
    root.addView(status);
    details = new TextView(this);
    root.addView(details);
    viewport.addView(root);
    setContentView(viewport);
    if (Build.VERSION.SDK_INT >= 30) {
      getWindow().getInsetsController().setSystemBarsAppearance(
          0, android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS |
                 android.view.WindowInsetsController
                     .APPEARANCE_LIGHT_NAVIGATION_BARS);
    }
    viewport.requestApplyInsets();
    IntentFilter filter = new IntentFilter(USB_PERMISSION);
    filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
    if (Build.VERSION.SDK_INT >= 33) {
      registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
    } else {
      registerReceiver(receiver, filter);
    }
    message("Ready",
            "Both the file and its expected CRC32 arrive through the video.\n"
                + "A file is accepted only after every block, exact length "
                + "and readback CRC match.");
    buttons();
  }

  private void requestCapture() {
    if (checkSelfPermission(Manifest.permission.CAMERA) !=
        PackageManager.PERMISSION_GRANTED) {
      requestPermissions(new String[] {Manifest.permission.CAMERA}, 3);
      return;
    }
    UsbDevice device = StockVideoCapture.findDevice(usb);
    if (device == null) {
      message("No stock video device",
              "Connect the powered-on Chromatic directly to this phone. "
                  + "Close other USB video apps, then try again. No USB "
                  + "commands were sent.");
      return;
    }
    if (usb.hasPermission(device)) {
      start(device, null);
    } else {
      pendingDevice = device;
      int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
      PendingIntent permission = PendingIntent.getBroadcast(
          this, 0, new Intent(USB_PERMISSION).setPackage(getPackageName()),
          flags);
      usb.requestPermission(device, permission);
      message("USB permission required",
              "Only the video-streaming interface will be used.");
      buttons();
    }
  }

  @Override
  public void onRequestPermissionsResult(int request, String[] permissions,
                                         int[] grants) {
    super.onRequestPermissionsResult(request, permissions, grants);
    if (request == 3) {
      if (grants.length == 1 &&
          grants[0] == PackageManager.PERMISSION_GRANTED) {
        requestCapture();
      } else {
        message("Camera permission denied",
                "Android requires camera permission for USB video access.");
      }
    }
  }

  private void start(UsbDevice device, Uri recording) {
    if (active != null || exporting) {
      return;
    }
    verified = null;
    verifiedName = null;
    Job job = new Job(device, recording);
    active = job;
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    message("Waiting for manifest",
            "Receiving visual frames. Keep the ROM broadcasting.");
    buttons();
    new Thread(job, "visual-receive").start();
  }

  private void cancel(String reason) {
    if (active != null) {
      active.cancelled.set(true);
      StockVideoCapture capture = active.capture;
      if (capture != null) {
        capture.close();
      }
      message("Stopping", reason);
    }
  }

  private void buttons() {
    boolean idle = active == null && pendingDevice == null && !exporting;
    receive.setEnabled(idle);
    importVideo.setEnabled(idle);
    stop.setEnabled(active != null && !active.cancelled.get());
    save.setEnabled(idle && verified != null);
  }

  private void message(String heading, String body) {
    status.setText(heading);
    details.setText(body);
  }

  private final class Job implements Runnable {
    final UsbDevice device;
    final Uri recording;
    final AtomicBoolean cancelled = new AtomicBoolean();
    volatile StockVideoCapture capture;
    VisualTransfer transfer;
    long lastProgress;
    int frames;
    int validFrames;
    long sourceCrc;

    Job(UsbDevice device, Uri recording) {
      this.device = device;
      this.recording = recording;
    }

    void frame(byte[] yuv, int width, int height) {
      if (cancelled.get() || transfer.verifiedFile() != null) {
        return;
      }
      frames++;
      VisualFrame decoded = VisualFrame.decode(yuv, width, height);
      if (decoded != null) {
        validFrames++;
        try {
          transfer.accept(decoded);
        } catch (IOException e) {
          throw new UncheckedIOException(e);
        }
        if (decoded.type == 0) {
          sourceCrc = decoded.fileCrc;
        }
      }
      if (transfer.verifiedFile() != null) {
        if (capture != null) {
          capture.close();
        }
        return;
      }
      long now = android.os.SystemClock.elapsedRealtime();
      if (now - lastProgress >= 250) {
        lastProgress = now;
        String progress =
            transfer.name() == null ? "Waiting for manifest" : transfer.name();
        String counts =
            transfer.receivedBlocks() + "/" + transfer.totalBlocks() +
            " blocks · " + validFrames + "/" + frames + " valid frames\n"
            + "Missing or damaged frames are recovered on the next loop.";
        runOnUiThread(() -> {
          if (active == this && !cancelled.get()) {
            message(progress, counts);
          }
        });
      }
    }

    @Override
    public void run() {
      File complete = null;
      String name = null;
      String failure = null;
      try (VisualTransfer incoming = new VisualTransfer(getCacheDir())) {
        transfer = incoming;
        if (device != null) {
          try (StockVideoCapture video = new StockVideoCapture(usb, device)) {
            capture = video;
            if (!cancelled.get()) {
              video.run(this::frame);
            }
          }
        } else {
          decodeRecording(recording, this);
        }
        if (!cancelled.get()) {
          complete = transfer.verifiedFile();
          name = transfer.name();
          if (complete == null) {
            failure = "Input ended with " + transfer.receivedBlocks() + "/" +
                      transfer.totalBlocks() + " blocks (" + validFrames +
                      (" valid frames). No verified backup. Record another "
                       + "full loop.");
          }
        }
      } catch (IOException | RuntimeException | LinkageError e) {
        failure = e.toString();
      } finally {
        capture = null;
      }
      File result = complete;
      String resultName = name;
      String error = failure;
      runOnUiThread(() -> {
        if (destroyed || active != this) {
          return;
        }
        active = null;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (cancelled.get()) {
          message("Cancelled",
                  "Incomplete transfer discarded. No backup accepted.");
        } else if (error != null) {
          message("Not verified", error);
        } else {
          verified = result;
          verifiedName = resultName;
          verifiedCrc = sourceCrc;
          message("CRC VERIFIED",
                  resultName + " · " + result.length() + " bytes\nCRC32 " +
                      String.format(Locale.ROOT, "%08X", sourceCrc) +
                      ("\nAll blocks present. Stored bytes match the "
                       + "transmitted source CRC.\n") +
                      "Stop the ROM manually, then save the file.");
        }
        buttons();
      });
    }
  }

  private void decodeRecording(Uri uri, Job job) throws IOException {
    MediaMetadataRetriever video = new MediaMetadataRetriever();
    try {
      video.setDataSource(this, uri);
      String value =
          video.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
      if (value == null) {
        throw new IOException("Recording has no duration.");
      }
      long durationUs = Math.multiplyExact(Long.parseLong(value), 1000L);
      int[] pixels = null;
      byte[] yuv = null;
      for (long time = 0; time < durationUs && !job.cancelled.get() &&
                          job.transfer.verifiedFile() == null;
           time += 100000) {
        Bitmap frame =
            video.getFrameAtTime(time, MediaMetadataRetriever.OPTION_CLOSEST);
        if (frame == null) {
          continue;
        }
        try {
          int size = Math.multiplyExact(frame.getWidth(), frame.getHeight());
          if (pixels == null || pixels.length != size) {
            pixels = new int[size];
            yuv = new byte[Math.multiplyExact(size, 3)];
          }
          frame.getPixels(pixels, 0, frame.getWidth(), 0, 0, frame.getWidth(),
                          frame.getHeight());
          for (int i = 0; i < size; i++) {
            int pixel = pixels[i];
            int r = pixel >> 16 & 255, g = pixel >> 8 & 255, b = pixel & 255;
            yuv[i * 3] = (byte)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            yuv[i * 3 + 1] =
                (byte)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            yuv[i * 3 + 2] =
                (byte)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
          }
          job.frame(yuv, frame.getWidth(), frame.getHeight());
        } finally {
          frame.recycle();
        }
      }
    } finally {
      video.release();
    }
  }

  @Override
  protected void onActivityResult(int request, int result, Intent data) {
    super.onActivityResult(request, result, data);
    if (result != RESULT_OK || data == null || data.getData() == null) {
      return;
    }
    if (request == IMPORT_VIDEO) {
      start(null, data.getData());
    } else if (request == EXPORT_FILE && verified != null && !exporting) {
      export(data.getData());
    }
  }

  private void export(Uri destination) {
    File source = verified;
    long expected = verifiedCrc;
    exporting = true;
    message("Saving",
            "Writing and reading back the destination to verify CRC32.");
    buttons();
    new Thread(() -> {
      String failure = null;
      try {
        byte[] buffer = new byte[8192];
        try (InputStream input = new FileInputStream(source);
             OutputStream output =
                 getContentResolver().openOutputStream(destination, "wt")) {
          if (output == null) {
            throw new IOException("Cannot open destination.");
          }
          int count;
          while ((count = input.read(buffer)) != -1) {
            output.write(buffer, 0, count);
          }
        }
        CRC32 crc = new CRC32();
        long length = 0;
        try (InputStream input =
                 getContentResolver().openInputStream(destination)) {
          if (input == null) {
            throw new IOException("Cannot read back destination.");
          }
          int count;
          while ((count = input.read(buffer)) != -1) {
            crc.update(buffer, 0, count);
            length += count;
          }
        }
        if (length != source.length() || crc.getValue() != expected) {
          throw new IOException("Destination length or CRC32 mismatch.");
        }
      } catch (IOException | RuntimeException e) {
        failure = e.toString();
        try {
          if (!DocumentsContract.deleteDocument(getContentResolver(),
                                                destination)) {
            failure += " Remove the unverified destination manually.";
          }
        } catch (IOException | RuntimeException cleanup) {
          failure += " Remove the unverified destination manually.";
        }
      }
      String error = failure;
      runOnUiThread(() -> {
        exporting = false;
        if (!destroyed) {
          message(error == null ? "Saved and verified" : "Export failed",
                  error == null
                      ? destination + "\nCRC32 " +
                            String.format(Locale.ROOT, "%08X", expected)
                      : error);
          buttons();
        }
      });
    }, "visual-export").start();
  }

  @Override
  protected void onDestroy() {
    destroyed = true;
    pendingDevice = null;
    cancel("Activity closed.");
    unregisterReceiver(receiver);
    super.onDestroy();
  }
}
