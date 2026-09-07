package dev.borges.chromaticproof;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.BitSet;
import java.util.Objects;
import java.util.zip.CRC32;

/**
 * Receiver for the visual snapshot transfer: latches on the first valid
 * manifest, stages data blocks into a bounded {@code .part} file, and only
 * promotes it to a verified file when every block has arrived and the
 * whole-file CRC32 and length match the manifest.
 *
 * <p>Rules: data before a manifest is ignored; frames for a different
 * identity are ignored; a second manifest with a different identity is a hard
 * error (latch survives only an explicit reset via a new instance); duplicate
 * blocks are compared byte-for-byte against staged data (a mismatch is a hard
 * error, not last-wins). An empty file completes on its manifest.
 *
 * <p>Staging uses a {@link RandomAccessFile} so no whole-file buffer is ever
 * allocated; received blocks are tracked in a {@link BitSet}. {@link #close()}
 * removes the incomplete staging file but keeps any verified file.
 */
public final class VisualTransfer implements AutoCloseable {

  private final File directory;
  private final File partFile;
  private RandomAccessFile part;
  private BitSet received;
  private String name;
  private int totalBlocks;
  private int receivedCount;
  private long fileSize;
  private long fileCrc;
  private int version;
  private int blockBytes;
  private File verifiedFile;
  private IOException deferred;
  private boolean closed;

  public VisualTransfer(File directory) throws IOException {
    this.directory = Objects.requireNonNull(directory, "directory");
    if (!directory.isDirectory()) {
      throw new FileNotFoundException("not a directory: " + directory);
    }
    this.partFile =
        File.createTempFile("chromatic-visual-", ".part", directory);
    this.part = new RandomAccessFile(partFile, "rw");
    this.part.setLength(0);
    this.received = new BitSet();
  }

  /**
   * Consumes one decoded frame. Invalid or mismatched frames are ignored;
   * protocol violations (conflicting manifest, conflicting duplicate block)
   * throw {@link IOException} and the transfer is dead until closed and
   * restarted. Returns true if this frame completed the transfer.
   */
  public boolean accept(VisualFrame frame) throws IOException {
    Objects.requireNonNull(frame, "frame");
    if (closed) {
      throw new IOException("transfer closed");
    }
    if (deferred != null) {
      throw new IOException("transfer failed earlier: " + deferred.getMessage(),
                            deferred);
    }
    try {
      if (frame.isManifest()) {
        return acceptManifest(frame);
      }
      return acceptData(frame);
    } catch (IOException e) {
      deferred = e;
      throw e;
    }
  }

  private boolean acceptManifest(VisualFrame frame) throws IOException {
    String n = frame.name();
    if (name == null) {
      latch(frame);
      return maybeComplete();
    }
    if (n.equals(name) && frame.fileSize == fileSize &&
        frame.fileCrc == fileCrc && frame.version == version) {
      return false; // identical manifest: ignore
    }
    throw new IOException("conflicting manifest: latched \"" + name +
                          "\" crc=" + Long.toHexString(fileCrc) +
                          " size=" + fileSize + ", got \"" + n +
                          "\" crc=" + Long.toHexString(frame.fileCrc) +
                          " size=" + frame.fileSize);
  }

  private void latch(VisualFrame frame) throws IOException {
    if (frame.fileSize > VisualFrame.MAX_FILE_SIZE) {
      throw new IOException("file size over limit: " + frame.fileSize);
    }
    // The on-disk staging path is fixed; the source filename is kept
    // only as data, never used to build a path.
    this.name = frame.name();
    this.fileSize = frame.fileSize;
    this.fileCrc = frame.fileCrc;
    this.version = frame.version;
    this.blockBytes = frame.blockBytes;
    this.totalBlocks = blocksFor(frame.fileSize);
    this.receivedCount = 0;
    this.received = new BitSet(Math.max(totalBlocks, 1));
    this.part.setLength(fileSize);
  }

  private boolean acceptData(VisualFrame frame) throws IOException {
    if (name == null || verifiedFile != null || frame.block >= totalBlocks) {
      return false; // before manifest, after completion, or out of range
    }
    if (frame.fileSize != fileSize || frame.fileCrc != fileCrc ||
        frame.version != version) {
      return false; // different identity: ignore
    }
    int block = (int)frame.block;
    long off = (long)block * blockBytes;
    if (received.get(block)) {
      compareDuplicate(frame, block, off);
      return false;
    }
    part.seek(off);
    part.write(frame.payload, 0, frame.length);
    received.set(block);
    receivedCount++;
    return maybeComplete();
  }

  private void compareDuplicate(VisualFrame frame, int block, long off)
      throws IOException {
    byte[] staged = new byte[frame.length];
    part.seek(off);
    part.readFully(staged);
    for (int i = 0; i < frame.length; i++) {
      if (staged[i] != frame.payload[i]) {
        throw new IOException("conflicting duplicate block " + block +
                              ": differs at offset " + i);
      }
    }
  }

  private boolean maybeComplete() throws IOException {
    if (name == null || receivedCount != totalBlocks) {
      return false;
    }
    // Sync then read the whole file back and verify CRC + length.
    part.getFD().sync();
    long len = part.length();
    if (len != fileSize) {
      throw new IOException("staged length " + len + " != manifest " +
                            fileSize);
    }
    CRC32 crc = new CRC32();
    byte[] buf = new byte[64 * 1024];
    long off = 0;
    while (off < len) {
      part.seek(off);
      int n = part.read(buf, 0, (int)Math.min(buf.length, len - off));
      if (n <= 0) {
        throw new IOException("unexpected EOF reading back staged file");
      }
      crc.update(buf, 0, n);
      off += n;
    }
    if (crc.getValue() != fileCrc) {
      throw new IOException("whole-file CRC mismatch: expected " +
                            Long.toHexString(fileCrc) + ", staged " +
                            Long.toHexString(crc.getValue()));
    }
    promote();
    return true;
  }

  private void promote() throws IOException {
    part.close();
    File dest = verifiedDest();
    if (!partFile.renameTo(dest)) {
      throw new IOException("rename failed: " + partFile + " -> " + dest);
    }
    verifiedFile = dest;
    part = null;
    received = null;
    // name/fileSize/fileCrc/counts stay readable for the UI after completion
  }

  private File verifiedDest() {
    return new File(directory, partFile.getName().replace(".part", ".bin"));
  }

  /** Number of blocks for a file of the given size; zero-size needs 0. */
  private int blocksFor(long size) {
    return (int)((size + blockBytes - 1) / blockBytes);
  }

  // -------------------------------------------------------------- getters

  public String name() { return name; }

  public int receivedBlocks() { return receivedCount; }

  public int totalBlocks() { return totalBlocks; }

  public long fileSize() { return fileSize; }

  public long fileCrc() { return fileCrc; }

  public boolean isComplete() { return verifiedFile != null; }

  /** The verified file, or null until the transfer has fully completed. */
  public File verifiedFile() { return verifiedFile; }

  // ---------------------------------------------------------------- close

  /**
   * Closes staging. Removes the incomplete {@code .part} file; keeps any
   * verified file. Idempotent.
   */
  @Override
  public void close() throws IOException {
    closed = true;
    if (part != null) {
      part.close();
      part = null;
    }
    if (verifiedFile == null && !partFile.delete() && partFile.exists()) {
      throw new IOException("could not delete " + partFile);
    }
  }
}
