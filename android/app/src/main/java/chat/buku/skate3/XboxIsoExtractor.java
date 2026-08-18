package chat.buku.skate3;

import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class XboxIsoExtractor {
    private static final long SECTOR_SIZE = 2048;
    private static final long[] POSSIBLE_GAME_OFFSETS = {
        0x00000000L, 0x0000FB20L, 0x00020600L, 0x02080000L, 0x0FD90000L
    };
    private static final byte[] XDVDFS_MAGIC =
        "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private static final int COPY_BUFFER_SIZE = 4 * 1024 * 1024;
    private static final int MAX_DIRECTORY_NODES = 500_000;

    interface ProgressListener {
        void onProgress(long copiedBytes, long totalBytes, String currentFile);
    }

    static final class Inspection {
        final List<Entry> entries;
        final long totalBytes;

        Inspection(List<Entry> entries, long totalBytes) {
            this.entries = entries;
            this.totalBytes = totalBytes;
        }
    }

    private static final class Entry {
        final String path;
        final long offset;
        final long size;

        Entry(String path, long offset, long size) {
            this.path = path;
            this.offset = offset;
            this.size = size;
        }
    }

    private static final class PendingNode {
        final long directoryOffset;
        final int nodeOffset;
        final String prefix;

        PendingNode(long directoryOffset, int nodeOffset, String prefix) {
            this.directoryOffset = directoryOffset;
            this.nodeOffset = nodeOffset;
            this.prefix = prefix;
        }
    }

    private XboxIsoExtractor() {}

    static Inspection inspect(FileChannel input) throws IOException {
        long fileSize = input.size();
        if (fileSize <= 0) {
            throw new IOException("The selected file has no readable size. Copy the ISO to local or USB storage and try again.");
        }

        long gameOffset = -1;
        ByteBuffer magic = ByteBuffer.allocate(XDVDFS_MAGIC.length);
        for (long candidate : POSSIBLE_GAME_OFFSETS) {
            long magicOffset = candidate + 32 * SECTOR_SIZE;
            if (magicOffset + XDVDFS_MAGIC.length > fileSize) {
                continue;
            }
            magic.clear();
            readFully(input, magicOffset, magic);
            if (matches(magic.array(), XDVDFS_MAGIC)) {
                gameOffset = candidate;
                break;
            }
        }
        if (gameOffset < 0) {
            throw new IOException("This is not a recognized Xbox 360 game ISO.");
        }

        ByteBuffer root = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        readFully(input, gameOffset + 32 * SECTOR_SIZE + 20, root);
        root.flip();
        long rootSector = Integer.toUnsignedLong(root.getInt());
        long rootSize = Integer.toUnsignedLong(root.getInt());
        if (rootSize < 13 || rootSize > 32L * 1024 * 1024) {
            throw new IOException("The Xbox ISO root directory is invalid.");
        }

        List<Entry> entries = parseDirectory(
            input, fileSize, gameOffset, gameOffset + rootSector * SECTOR_SIZE);
        boolean hasDefault = false;
        boolean hasWebkit = false;
        long total = 0;
        for (Entry entry : entries) {
            String lower = entry.path.toLowerCase(Locale.ROOT);
            hasDefault |= lower.equals("default.xex");
            hasWebkit |= lower.equals("data/webkit/eawebkit.xex");
            total = Math.addExact(total, entry.size);
        }
        if (!hasDefault || !hasWebkit) {
            throw new IOException("The ISO is missing Skate 3's required default.xex or EAWebkit.xex file.");
        }
        return new Inspection(entries, total);
    }

    static void extract(FileChannel input, Inspection inspection, Path targetRoot,
                        ProgressListener listener) throws IOException {
        Files.createDirectories(targetRoot);
        Path normalizedRoot = targetRoot.toAbsolutePath().normalize();
        ByteBuffer buffer = ByteBuffer.allocateDirect(COPY_BUFFER_SIZE);
        long copied = 0;

        for (Entry entry : inspection.entries) {
            if (isUnsafePath(entry.path)) {
                throw new IOException("The ISO contains an unsafe path: " + entry.path);
            }
            Path target = normalizedRoot.resolve(entry.path).normalize();
            if (!target.startsWith(normalizedRoot)) {
                throw new IOException("The ISO contains a path outside the game directory.");
            }
            Path parent = target.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }

            try (FileChannel output = FileChannel.open(
                    target, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING,
                    StandardOpenOption.WRITE)) {
                long remaining = entry.size;
                long readOffset = entry.offset;
                while (remaining > 0) {
                    int chunk = (int) Math.min(remaining, COPY_BUFFER_SIZE);
                    buffer.clear();
                    buffer.limit(chunk);
                    readFully(input, readOffset, buffer);
                    buffer.flip();
                    while (buffer.hasRemaining()) {
                        output.write(buffer);
                    }
                    remaining -= chunk;
                    readOffset += chunk;
                    copied += chunk;
                    listener.onProgress(copied, inspection.totalBytes, entry.path);
                }
            }
        }
    }

    private static List<Entry> parseDirectory(FileChannel input, long fileSize, long gameOffset,
                                               long directoryOffset) throws IOException {
        ArrayDeque<PendingNode> pending = new ArrayDeque<>();
        pending.push(new PendingNode(directoryOffset, 0, ""));
        List<Entry> entries = new ArrayList<>();
        int visited = 0;

        while (!pending.isEmpty()) {
            if (++visited > MAX_DIRECTORY_NODES) {
                throw new IOException("The Xbox ISO directory tree is unexpectedly large.");
            }
            PendingNode node = pending.pop();
            long entryOffset = node.directoryOffset + Integer.toUnsignedLong(node.nodeOffset);
            ByteBuffer header = ByteBuffer.allocate(14).order(ByteOrder.LITTLE_ENDIAN);
            readFully(input, entryOffset, header);
            header.flip();

            int left = Short.toUnsignedInt(header.getShort());
            int right = Short.toUnsignedInt(header.getShort());
            long sector = Integer.toUnsignedLong(header.getInt());
            long length = Integer.toUnsignedLong(header.getInt());
            int attributes = Byte.toUnsignedInt(header.get());
            int nameLength = Byte.toUnsignedInt(header.get());
            if (nameLength == 0 || nameLength > 240) {
                throw new IOException("The Xbox ISO contains an invalid directory entry.");
            }

            ByteBuffer nameBytes = ByteBuffer.allocate(nameLength);
            readFully(input, entryOffset + 14, nameBytes);
            String name = new String(nameBytes.array(), StandardCharsets.UTF_8);
            if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.equals(".") || name.equals("..")) {
                throw new IOException("The Xbox ISO contains an unsafe file name.");
            }

            if (left != 0) {
                pending.push(new PendingNode(node.directoryOffset, left * 4, node.prefix));
            }
            if (right != 0) {
                pending.push(new PendingNode(node.directoryOffset, right * 4, node.prefix));
            }

            String fullPath = node.prefix + name;
            boolean directory = (attributes & 0x10) != 0;
            if (directory) {
                if (length != 0) {
                    pending.push(new PendingNode(gameOffset + sector * SECTOR_SIZE, 0,
                                                 fullPath + "/"));
                }
            } else {
                long offset = gameOffset + sector * SECTOR_SIZE;
                if (offset < 0 || length < 0 || offset > fileSize || length > fileSize - offset) {
                    throw new IOException("The Xbox ISO contains a file outside its data area.");
                }
                entries.add(new Entry(fullPath, offset, length));
            }
        }
        return entries;
    }

    private static void readFully(FileChannel channel, long offset, ByteBuffer buffer)
            throws IOException {
        long position = offset;
        while (buffer.hasRemaining()) {
            int read = channel.read(buffer, position);
            if (read < 0) {
                throw new EOFException("The ISO ended unexpectedly.");
            }
            if (read == 0) {
                throw new IOException("The selected storage provider stopped reading the ISO.");
            }
            position += read;
        }
    }

    private static boolean matches(byte[] left, byte[] right) {
        if (left.length != right.length) {
            return false;
        }
        for (int i = 0; i < left.length; ++i) {
            if (left[i] != right[i]) {
                return false;
            }
        }
        return true;
    }

    private static boolean isUnsafePath(String path) {
        if (path.isEmpty() || path.startsWith("/") || path.startsWith("\\") || path.indexOf('\\') >= 0) {
            return true;
        }
        for (String component : path.split("/", -1)) {
            if (component.isEmpty() || component.equals(".") || component.equals("..")) {
                return true;
            }
        }
        return false;
    }
}
