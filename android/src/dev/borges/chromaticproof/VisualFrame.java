package dev.borges.chromaticproof;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.CRC32;

public final class VisualFrame {
  public static final int TYPE_MANIFEST = 0;
  public static final int TYPE_DATA = 1;
  public static final long BLOCK_MANIFEST = 0xFFFFFFFFL;
  public static final int MAX_FILE_SIZE = 16 << 20;
  private static final int COLS = 40;
  private static final int ROWS = 36;

  public int version;
  public int blockBytes;
  public int type;
  public int length;
  public long fileCrc;
  public long fileSize;
  public long block;
  public byte[] payload;

  private VisualFrame() {}

  public boolean isManifest() { return type == TYPE_MANIFEST; }

  public String name() {
    return isManifest() ? new String(payload, StandardCharsets.US_ASCII) : null;
  }

  public static VisualFrame decode(byte[] yuv, int width, int height) {
    if (width < COLS || height < ROWS || yuv == null ||
        (long)width * height * 3 != yuv.length) {
      return null;
    }
    double cell = Math.min(width / 40.0, height / 36.0);
    double left = (width - COLS * cell) / 2;
    double top = (height - ROWS * cell) / 2;
    int[] samples = new int[COLS * ROWS * 3];
    for (int y = 0; y < ROWS; y++) {
      int py = (int)(top + (y + 0.5) * cell);
      for (int x = 0; x < COLS; x++) {
        int px = (int)(left + (x + 0.5) * cell);
        int source = (py * width + px) * 3;
        int dest = (y * COLS + x) * 3;
        for (int c = 0; c < 3; c++)
          samples[dest + c] = yuv[source + c] & 255;
      }
    }
    VisualFrame color = decodeCells(samples, true);
    return color != null ? color : decodeCells(samples, false);
  }

  private static boolean outer(int x, int y) {
    return x == 0 || x == COLS - 1 || y == 0 || y == ROWS - 1;
  }

  private static boolean payloadCell(int x, int y) {
    return x >= 2 && x < 38 && y >= 2 && y < 34;
  }

  private static int marker(int x, int y, boolean color) {
    if (!outer(x, y))
      return 0;
    return color ? (x + y) & 3 : ((x + y) & 1) == 0 ? 1 : 0;
  }

  private static int distance(int[] a, int offset, int[] b, int count) {
    int total = 0;
    for (int c = 0; c < count; c++) {
      int d = a[offset + c] - b[c];
      total += d * d;
    }
    return total;
  }

  private static VisualFrame decodeCells(int[] samples, boolean color) {
    int symbols = color ? 4 : 2;
    int channels = color ? 3 : 1;
    int[][] palette = new int[symbols][3];
    int[] counts = new int[symbols];
    for (int y = 0; y < ROWS; y++) {
      for (int x = 0; x < COLS; x++) {
        if (!outer(x, y))
          continue;
        int symbol = marker(x, y, color);
        counts[symbol]++;
        for (int c = 0; c < channels; c++) {
          palette[symbol][c] += samples[(y * COLS + x) * 3 + c];
        }
      }
    }
    for (int s = 0; s < symbols; s++) {
      for (int c = 0; c < channels; c++)
        palette[s][c] /= counts[s];
    }
    int separation = Integer.MAX_VALUE;
    for (int a = 0; a < symbols; a++) {
      for (int b = 0; b < a; b++) {
        separation =
            Math.min(separation, distance(palette[a], 0, palette[b], channels));
      }
    }
    if (separation < 2304)
      return null;
    byte[] wire = new byte[color ? 288 : 144];
    int bits = color ? 2 : 1;
    for (int y = 0; y < ROWS; y++) {
      for (int x = 0; x < COLS; x++) {
        int best = Integer.MAX_VALUE, next = Integer.MAX_VALUE, value = -1;
        for (int s = 0; s < symbols; s++) {
          int d = distance(samples, (y * COLS + x) * 3, palette[s], channels);
          if (d < best) {
            next = best;
            best = d;
            value = s;
          } else {
            next = Math.min(next, d);
          }
        }
        if (best * 9 > separation || best * 4 >= next)
          return null;
        if (payloadCell(x, y)) {
          int bit = ((y - 2) * 36 + x - 2) * bits;
          wire[bit / 8] |= (byte)(value << (8 - bits - bit % 8));
        } else if (value != marker(x, y, color)) {
          return null;
        }
      }
    }
    return parse(wire, color ? 2 : 1);
  }

  private static VisualFrame parse(byte[] f, int version) {
    int capacity = version == 2 ? 264 : 120;
    int crcOffset = f.length - 4;
    if (f[0] != 'X' || f[1] != '7' || f[2] != 'V' || f[3] != '0' + version)
      return null;
    int type = f[4] & 255;
    int length = (f[5] & 255) | (version == 2 ? (f[6] & 255) << 8 : 0);
    if (type > TYPE_DATA || length > capacity || f[7] != 0 ||
        (version == 1 && f[6] != 0))
      return null;
    CRC32 crc = new CRC32();
    crc.update(f, 0, crcOffset);
    if (readLE32(f, crcOffset) != crc.getValue())
      return null;
    for (int i = 20 + length; i < crcOffset; i++)
      if (f[i] != 0)
        return null;
    VisualFrame frame = new VisualFrame();
    frame.version = version;
    frame.blockBytes = capacity;
    frame.type = type;
    frame.length = length;
    frame.fileCrc = readLE32(f, 8);
    frame.fileSize = readLE32(f, 12);
    frame.block = readLE32(f, 16);
    if (frame.fileSize > MAX_FILE_SIZE)
      return null;
    if (type == TYPE_MANIFEST) {
      if (frame.block != BLOCK_MANIFEST || !isValidName(f, 20, length))
        return null;
    } else {
      long start = frame.block * capacity;
      if (frame.fileSize == 0 || start >= frame.fileSize ||
          length != Math.min(capacity, frame.fileSize - start))
        return null;
    }
    frame.payload = Arrays.copyOfRange(f, 20, 20 + length);
    return frame;
  }

  static boolean isValidName(byte[] b, int off, int len) {
    if (len < 1 || len > 12)
      return false;
    int dot = -1;
    for (int i = 0; i < len; i++) {
      int c = b[off + i] & 255;
      if (c == '.') {
        if (dot >= 0 || i == 0 || i > 8 || i == len - 1 || len - i - 1 > 3)
          return false;
        dot = i;
      } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   "!#$%&'()-@^_`{}~".indexOf(c) >= 0)) {
        return false;
      }
    }
    return dot >= 0 || len <= 8;
  }

  private static long readLE32(byte[] b, int off) {
    return (b[off] & 255L) | (b[off + 1] & 255L) << 8 |
        (b[off + 2] & 255L) << 16 | (b[off + 3] & 255L) << 24;
  }
}
