package chat.buku.skate3;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

final class TitleUpdateInstaller {
    private static final String DOWNLOAD_URL =
        "https://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid=21774";
    private static final int MAX_PACKAGE_SIZE = 256 * 1024 * 1024;
    private static final int BLOCK_SIZE = 0x1000;
    private static final int END_OF_CHAIN = 0xFFFFFF;

    private static final Payload[] PAYLOADS = {
        new Payload("default.xexp", 1_701_888,
            "eb9ef9109dfa6d940df2e156e7eaeda4603d2b2319ca6451f324b1c27f2b1f4c"),
        new Payload("data/webkit/EAWebkit.xexp", 4_096,
            "5d4a308d2a6c768fc27c8b62ccb6661171dc504f8c0011a3e116cf0074e09438")
    };

    interface ProgressListener {
        void onProgress(long copiedBytes, long totalBytes, String message);
    }

    private static final class Payload {
        final String path;
        final int size;
        final String sha256;

        Payload(String path, int size, String sha256) {
            this.path = path;
            this.size = size;
            this.sha256 = sha256;
        }
    }

    private static final class Entry {
        final String path;
        final boolean directory;
        final int startBlock;
        final int length;

        Entry(String path, boolean directory, int startBlock, int length) {
            this.path = path;
            this.directory = directory;
            this.startBlock = startBlock;
            this.length = length;
        }
    }

    private TitleUpdateInstaller() {}

    static void downloadAndInstall(Path gameRoot, ProgressListener listener) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(DOWNLOAD_URL).openConnection();
        connection.setConnectTimeout(15_000);
        connection.setReadTimeout(30_000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "Skate3-Mobile-Android");
        try {
            int status = connection.getResponseCode();
            if (status < 200 || status >= 300) {
                throw new IOException("Title Update server returned HTTP " + status + ".");
            }
            long advertised = connection.getContentLengthLong();
            if (advertised > MAX_PACKAGE_SIZE) {
                throw new IOException("The Title Update download is unexpectedly large.");
            }
            try (InputStream input = connection.getInputStream()) {
                byte[] packageBytes = readLimited(input, advertised, listener);
                installPackage(packageBytes, gameRoot);
            }
        } finally {
            connection.disconnect();
        }
    }

    static void installPackage(InputStream input, Path gameRoot) throws IOException {
        installPackage(readLimited(input, -1, null), gameRoot);
    }

    static boolean isInstalled(Path gameRoot) {
        try {
            for (Payload payload : PAYLOADS) {
                Path file = gameRoot.resolve(payload.path);
                if (!Files.isRegularFile(file) || Files.size(file) != payload.size ||
                        !sha256(Files.readAllBytes(file)).equals(payload.sha256)) {
                    return false;
                }
            }
            return true;
        } catch (IOException exception) {
            return false;
        }
    }

    private static byte[] readLimited(InputStream input, long advertised,
                                      ProgressListener listener) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(
            advertised > 0 && advertised < Integer.MAX_VALUE ? (int) advertised : 2 * 1024 * 1024);
        byte[] buffer = new byte[64 * 1024];
        long copied = 0;
        for (;;) {
            int read = input.read(buffer);
            if (read < 0) {
                break;
            }
            copied += read;
            if (copied > MAX_PACKAGE_SIZE) {
                throw new IOException("The Title Update file is unexpectedly large.");
            }
            output.write(buffer, 0, read);
            if (listener != null) {
                listener.onProgress(copied, advertised, "Downloading Title Update 3");
            }
        }
        return output.toByteArray();
    }

    private static void installPackage(byte[] data, Path gameRoot) throws IOException {
        StfsReader reader = new StfsReader(data);
        Map<String, Entry> files = new HashMap<>();
        for (Entry entry : reader.listEntries()) {
            if (!entry.directory) {
                files.put(entry.path, entry);
            }
        }
        if (files.size() != PAYLOADS.length) {
            throw new IOException("The selected package is not the required Skate 3 Title Update 3.");
        }

        for (Payload payload : PAYLOADS) {
            Entry entry = files.get(payload.path);
            if (entry == null || entry.length != payload.size) {
                throw new IOException("Title Update 3 is missing " + payload.path + ".");
            }
            byte[] contents = reader.readFile(entry.startBlock, entry.length);
            if (!sha256(contents).equals(payload.sha256)) {
                throw new IOException("Title Update verification failed for " + payload.path + ".");
            }
            atomicWrite(gameRoot.resolve(payload.path), contents);
        }
    }

    private static void atomicWrite(Path destination, byte[] data) throws IOException {
        Files.createDirectories(destination.getParent());
        Path temporary = destination.resolveSibling(destination.getFileName() + ".tmp");
        Files.write(temporary, data);
        try {
            Files.move(temporary, destination, StandardCopyOption.ATOMIC_MOVE,
                       StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(temporary, destination, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static String sha256(byte[] data) throws IOException {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            char[] digits = "0123456789abcdef".toCharArray();
            char[] result = new char[digest.length * 2];
            for (int i = 0; i < digest.length; ++i) {
                int value = Byte.toUnsignedInt(digest[i]);
                result[i * 2] = digits[value >>> 4];
                result[i * 2 + 1] = digits[value & 15];
            }
            return new String(result);
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable.", exception);
        }
    }

    private static final class StfsReader {
        private final byte[] data;
        private final int headerSize;
        private final int metadataOffset = 0x344;
        private final int volumeDescriptorOffset = metadataOffset + 0x35;
        private final int blocksPerHashTable;

        StfsReader(byte[] data) throws IOException {
            this.data = data;
            requireRange(0, 0x400);
            String magic = new String(data, 0, 4, java.nio.charset.StandardCharsets.US_ASCII);
            if (!magic.equals("CON ") && !magic.equals("LIVE") && !magic.equals("PIRS")) {
                throw new IOException("The selected file is not an Xbox 360 STFS package.");
            }
            headerSize = be32(0x340);
            if (be32(metadataOffset + 0x65) != 0) {
                throw new IOException("The selected package is not an STFS volume.");
            }
            int flags = u8(volumeDescriptorOffset + 2);
            blocksPerHashTable = (flags & 1) != 0 ? 1 : 2;
        }

        List<Entry> listEntries() throws IOException {
            int tableBlockCount = le16(volumeDescriptorOffset + 3);
            int tableBlock = u24le(volumeDescriptorOffset + 5);
            List<Entry> entries = new ArrayList<>();

            for (int tableIndex = 0; tableIndex < tableBlockCount; ++tableIndex) {
                int tableOffset = blockToOffset(tableBlock);
                for (int index = 0; index < 0x40; ++index) {
                    int offset = tableOffset + index * 0x40;
                    requireRange(offset, 0x40);
                    if (u8(offset) == 0) {
                        break;
                    }
                    int flags = u8(offset + 40);
                    int nameLength = flags & 0x3F;
                    if (nameLength == 0 || nameLength > 40) {
                        throw new IOException("The Title Update contains an invalid file entry.");
                    }
                    String name = new String(data, offset, nameLength,
                                             java.nio.charset.StandardCharsets.UTF_8);
                    boolean directory = (flags & 0x80) != 0;
                    int startBlock = u24le(offset + 47);
                    int parentIndex = be16(offset + 50);
                    int length = be32(offset + 52);
                    if (length < 0) {
                        throw new IOException("The Title Update contains an oversized file.");
                    }
                    String parent = "";
                    if (parentIndex != 0xFFFF) {
                        if (parentIndex < 0 || parentIndex >= entries.size()) {
                            throw new IOException("The Title Update contains an invalid directory reference.");
                        }
                        parent = entries.get(parentIndex).path;
                    }
                    String path = parent.isEmpty() ? name : parent.replaceAll("/+$", "") + "/" + name;
                    entries.add(new Entry(path, directory, startBlock, length));
                }
                int next = nextBlock(tableBlock);
                if (next == END_OF_CHAIN) {
                    break;
                }
                tableBlock = next;
            }
            return entries;
        }

        byte[] readFile(int startBlock, int size) throws IOException {
            byte[] output = new byte[size];
            int outputOffset = 0;
            int block = startBlock;
            while (outputOffset < size && block != END_OF_CHAIN) {
                int chunk = Math.min(BLOCK_SIZE, size - outputOffset);
                int offset = blockToOffset(block);
                requireRange(offset, chunk);
                System.arraycopy(data, offset, output, outputOffset, chunk);
                outputOffset += chunk;
                block = nextBlock(block);
            }
            if (outputOffset != size) {
                throw new IOException("The Title Update file chain ended unexpectedly.");
            }
            return output;
        }

        private int blockToOffset(int blockIndex) throws IOException {
            long block = Integer.toUnsignedLong(blockIndex);
            long original = block;
            long base = 170;
            for (int level = 0; level < 3; ++level) {
                block += ((original + base) / base) * blocksPerHashTable;
                if (original < base) {
                    break;
                }
                base *= 170;
            }
            long offset = roundUp(Integer.toUnsignedLong(headerSize), BLOCK_SIZE) + (block << 12);
            if (offset < 0 || offset > Integer.MAX_VALUE) {
                throw new IOException("The Title Update contains an invalid block offset.");
            }
            return (int) offset;
        }

        private int nextBlock(int blockIndex) throws IOException {
            int hashOffset = hashOffset(blockIndex, 0);
            int entryOffset = hashOffset + Math.floorMod(blockIndex, 170) * 0x18;
            return be32(entryOffset + 0x14) & 0xFFFFFF;
        }

        private int hashOffset(int blockIndex, int level) throws IOException {
            long offset = roundUp(Integer.toUnsignedLong(headerSize), BLOCK_SIZE) +
                          (Integer.toUnsignedLong(hashBlockNumber(blockIndex, level)) << 12);
            if (offset < 0 || offset > Integer.MAX_VALUE) {
                throw new IOException("The Title Update contains an invalid hash offset.");
            }
            return (int) offset;
        }

        private int hashBlockNumber(int blockIndex, int level) {
            int blockStep0 = 170 + blocksPerHashTable;
            int blockStep1 = 28_900 + (171 * blocksPerHashTable);
            if (level == 0) {
                if (blockIndex < 170) return 0;
                int block = (blockIndex / 170) * blockStep0;
                block += ((blockIndex / 28_900) + 1) * blocksPerHashTable;
                return blockIndex < 28_900 ? block : block + blocksPerHashTable;
            }
            if (level == 1) {
                return blockIndex < 28_900 ? blockStep0 :
                    (blockIndex / 28_900) * blockStep1 + blocksPerHashTable;
            }
            return blockStep1;
        }

        private int u8(int offset) throws IOException {
            requireRange(offset, 1);
            return Byte.toUnsignedInt(data[offset]);
        }

        private int be16(int offset) throws IOException {
            requireRange(offset, 2);
            return (u8(offset) << 8) | u8(offset + 1);
        }

        private int be32(int offset) throws IOException {
            requireRange(offset, 4);
            return (u8(offset) << 24) | (u8(offset + 1) << 16) |
                   (u8(offset + 2) << 8) | u8(offset + 3);
        }

        private int le16(int offset) throws IOException {
            requireRange(offset, 2);
            return u8(offset) | (u8(offset + 1) << 8);
        }

        private int u24le(int offset) throws IOException {
            requireRange(offset, 3);
            return u8(offset) | (u8(offset + 1) << 8) | (u8(offset + 2) << 16);
        }

        private void requireRange(int offset, int length) throws IOException {
            if (offset < 0 || length < 0 || offset > data.length || length > data.length - offset) {
                throw new IOException("The Title Update package is truncated or corrupt.");
            }
        }

        private static long roundUp(long value, long alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    }
}
