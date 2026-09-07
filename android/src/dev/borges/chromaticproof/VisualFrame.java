package dev.borges.chromaticproof;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.CRC32;

/**
 * Wire-level codec for the stock-firmware visual snapshot protocol.
 *
 * <p>A frame is the 160x144 monochrome source image divided into a 40x36 grid
 * of 4x4-pixel cells. Cells x=2..37, y=2..33 carry 1152 payload bits
 * (row-major, MSB first, bit set = black). Every cell outside the payload is
 * a calibration marker: the outer edge is a checkerboard (black where (x+y) is
 * even), and the one-cell ring around the payload is solid white.
 *
 * <p>The 144 cell bytes are laid out as: 0..3 ASCII "X7V1"; 4 type (0 manifest,
 * 1 data); 5 payload length; 6..7 reserved zero; 8..11 whole-file CRC32 LE;
 * 12..15 file size LE; 16..19 block number LE (manifest uses 0xffffffff);
 * 20..139 payload zero padded; 140..143 CRC32/IEEE of bytes 0..139 LE.
 *
 * <p>Manifest payloads carry a 1..12 byte 8.3 filename; data block payloads
 * carry up to 120 bytes with the exact remaining length on the last block.
 * File size is capped at {@link #MAX_FILE_SIZE}.
 */
public final class VisualFrame {

    public static final int FRAME_BYTES = 144;
    public static final int PAYLOAD_MAX = 120;
    public static final int TYPE_MANIFEST = 0;
    public static final int TYPE_DATA = 1;
    public static final long BLOCK_MANIFEST = 0xFFFFFFFFL;
    public static final int MAX_FILE_SIZE = 16 << 20;

    static final int COLS = 40;
    static final int ROWS = 36;
    static final int PAY_X0 = 2;
    static final int PAY_Y0 = 2;
    static final int PAY_COLS = COLS - 2 * PAY_X0;
    static final int PAY_ROWS = ROWS - 2 * PAY_Y0;
    private static final int MIN_CONTRAST = 48;

    public int type;
    public int length;
    public long fileCrc;
    public long fileSize;
    public long block;
    public byte[] payload;

    private VisualFrame() {
    }

    public boolean isManifest() {
        return type == TYPE_MANIFEST;
    }

    /** Source filename; only meaningful for a manifest frame that passed validation. */
    public String name() {
        if (type != TYPE_MANIFEST || payload == null || payload.length < length || length < 0) {
            return null;
        }
        return new String(payload, 0, length, StandardCharsets.US_ASCII);
    }

    /**
     * Decodes one frame from a grayscale image. The 160x144 frame is assumed
     * to be scaled to fit (letterboxed, centered) inside the given image.
     * Returns null for any invalid frame: bad geometry, low contrast, uncertain
     * cells, marker mismatch, magic/reserved/padding/metadata violations, or a
     * CRC mismatch. Not thread-safe.
     */
    public static VisualFrame decode(byte[] luminance, int width, int height) {
        if (luminance == null || width <= 0 || height <= 0
                || (long) width * (long) height > luminance.length) {
            return null;
        }
        byte[] wire = readCells(luminance, width, height);
        return wire == null ? null : parse(wire);
    }

    private static byte[] readCells(byte[] lum, int width, int height) {
        View view = View.of(width, height);
        int[] darkCal = new int[COLS * ROWS];
        int[] lightCal = new int[COLS * ROWS];
        int nd = 0;
        int nl = 0;
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                if (isPayloadCell(x, y)) {
                    continue;
                }
                int v = sample(lum, width, height, view, x, y);
                if (borderBlack(x, y)) {
                    darkCal[nd++] = v;
                } else {
                    lightCal[nl++] = v;
                }
            }
        }
        if (nd == 0 || nl == 0) {
            return null;
        }
        Arrays.sort(darkCal, 0, nd);
        Arrays.sort(lightCal, 0, nl);
        int black = darkCal[nd / 2];
        int white = lightCal[nl / 2];
        int contrast = white - black;
        if (contrast < MIN_CONTRAST) {
            return null;
        }
        int mid = (black + white) >>> 1;
        int band = contrast / 4;

        byte[] wire = new byte[FRAME_BYTES];
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                int v = sample(lum, width, height, view, x, y);
                int cell;
                if (v <= mid - band) {
                    cell = 1;
                } else if (v >= mid + band) {
                    cell = 0;
                } else {
                    return null; // uncertain cell
                }
                if (isPayloadCell(x, y)) {
                    if (cell == 1) {
                        int bit = (y - PAY_Y0) * PAY_COLS + (x - PAY_X0);
                        wire[bit >> 3] |= (byte) (1 << (7 - (bit & 7)));
                    }
                } else if (cell != (borderBlack(x, y) ? 1 : 0)) {
                    return null; // marker mismatch
                }
            }
        }
        return wire;
    }

    private static int sample(byte[] lum, int width, int height, View view, int x, int y) {
        int px = (int) (view.offX + (x + 0.5) * view.cellW);
        int py = (int) (view.offY + (y + 0.5) * view.cellH);
        if (px < 0) {
            px = 0;
        } else if (px >= width) {
            px = width - 1;
        }
        if (py < 0) {
            py = 0;
        } else if (py >= height) {
            py = height - 1;
        }
        return lum[py * width + px] & 0xFF;
    }

    private static boolean isPayloadCell(int x, int y) {
        return x >= PAY_X0 && x < COLS - PAY_X0 && y >= PAY_Y0 && y < ROWS - PAY_Y0;
    }

    private static boolean borderBlack(int x, int y) {
        if (x == 0 || x == COLS - 1 || y == 0 || y == ROWS - 1) {
            return ((x + y) & 1) == 0;
        }
        return false; // the ring around the payload is solid white
    }

    private static final class View {
        final double offX;
        final double offY;
        final double cellW;
        final double cellH;

        private View(double offX, double offY, double cellW, double cellH) {
            this.offX = offX;
            this.offY = offY;
            this.cellW = cellW;
            this.cellH = cellH;
        }

        /** Largest centered 10:9 rectangle: the source frame scaled to fit. */
        static View of(int width, int height) {
            final double aspect = 10.0 / 9.0;
            double viewW;
            double viewH;
            double offX;
            double offY;
            if (width > height * aspect) {
                viewH = height;
                viewW = height * aspect;
                offX = (width - viewW) / 2;
                offY = 0;
            } else {
                viewW = width;
                viewH = width / aspect;
                offX = 0;
                offY = (height - viewH) / 2;
            }
            return new View(offX, offY, viewW / COLS, viewH / ROWS);
        }
    }

    // ----------------------------------------------------------------- parse

    /** Strict bounded parse of a 144-byte wire frame; null on any violation. */
    static VisualFrame parse(byte[] f) {
        if (f == null || f.length != FRAME_BYTES) {
            return null;
        }
        if (f[0] != 'X' || f[1] != '7' || f[2] != 'V' || f[3] != '1') {
            return null;
        }
        int type = f[4] & 0xFF;
        int length = f[5] & 0xFF;
        if (type > TYPE_DATA || length > PAYLOAD_MAX || f[6] != 0 || f[7] != 0) {
            return null;
        }
        if (readLE32(f, 140) != crc32(f, 0, 140)) {
            return null;
        }
        for (int i = 20 + length; i < 140; i++) {
            if (f[i] != 0) {
                return null;
            }
        }
        VisualFrame frame = new VisualFrame();
        frame.type = type;
        frame.length = length;
        frame.fileCrc = readLE32(f, 8);
        frame.fileSize = readLE32(f, 12);
        frame.block = readLE32(f, 16);
        if (type == TYPE_MANIFEST) {
            if (frame.block != BLOCK_MANIFEST || length < 1 || length > 12
                    || frame.fileSize > MAX_FILE_SIZE
                    || !isValidName(f, 20, length)) {
                return null;
            }
        } else {
            if (frame.block == BLOCK_MANIFEST || frame.fileSize < 1
                    || frame.fileSize > MAX_FILE_SIZE || length < 1) {
                return null;
            }
            long end = frame.block * PAYLOAD_MAX + length;
            if (end > frame.fileSize
                    || (length != PAYLOAD_MAX && end != frame.fileSize)) {
                return null;
            }
        }
        frame.payload = Arrays.copyOfRange(f, 20, 20 + length);
        return frame;
    }

    // ------------------------------------------------------------ validation

    /**
     * 8.3 filename over [A-Za-z0-9]: 1..8 characters, optionally "." plus 1..3
     * characters. Rejects separators, control bytes, NUL padding, extra or
     * leading/trailing dots, and anything else usable for path traversal.
     */
    static boolean isValidName(byte[] b, int off, int len) {
        if (len < 1 || len > 12) {
            return false;
        }
        int dot = -1;
        for (int i = 0; i < len; i++) {
            int c = b[off + i] & 0xFF;
            if (c == '.') {
                if (dot >= 0 || i == 0 || i > 8 || i == len - 1 || len - i - 1 > 3) {
                    return false;
                }
                dot = i;
            } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                    || (c >= 'a' && c <= 'z') || "!#$%&'()-@^_`{}~".indexOf(c) >= 0)) {
                return false;
            }
        }
        return dot >= 0 || len <= 8;
    }

    private static long crc32(byte[] b, int off, int len) {
        CRC32 crc = new CRC32();
        crc.update(b, off, len);
        return crc.getValue();
    }

    private static long readLE32(byte[] b, int off) {
        return (b[off] & 0xFFL)
                | (b[off + 1] & 0xFFL) << 8
                | (b[off + 2] & 0xFFL) << 16
                | (b[off + 3] & 0xFFL) << 24;
    }

}
