package chat.buku.skate3;

import android.content.Context;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** Installs character files that are intentionally shipped inside the APK. */
final class BundledMods {
    private static final String SEIYU_VERSION = "1.0.0-bundled.1";
    private static final String SEIYU_MODEL_ASSET = "mods/seiyu/base.obj";
    private static final String SEIYU_TEXTURE_ASSET = "mods/seiyu/texture_diffuse.png";
    private static final long SEIYU_MODEL_SIZE = 920451L;
    private static final long SEIYU_TEXTURE_SIZE = 2156122L;
    private static final String SEIYU_MODEL_SHA256 =
        "a2c2ea56e2136d68cd1b65b07a77d263976100f5638e104c2082a3bb150435fb";
    private static final String SEIYU_TEXTURE_SHA256 =
        "aaae6cb98b9819ef2a032808945bfabdbb908947c3ee2f01354861138567ef52";
    private static final String VERSION_MARKER = ".bundled-seiyu-version";

    private BundledMods() {}

    static boolean isSeiyuReady(Path gameRoot) {
        Path target = gameRoot.resolve("mods/penguin");
        try {
            Path marker = target.resolve(VERSION_MARKER);
            if (!Files.isRegularFile(marker) ||
                    !new String(Files.readAllBytes(marker), StandardCharsets.UTF_8)
                        .trim().equals(SEIYU_VERSION)) {
                return false;
            }
            return verified(target.resolve("base.obj"), SEIYU_MODEL_SIZE,
                            SEIYU_MODEL_SHA256) &&
                   verified(target.resolve("texture_diffuse.png"), SEIYU_TEXTURE_SIZE,
                            SEIYU_TEXTURE_SHA256);
        } catch (IOException exception) {
            return false;
        }
    }

    static void installSeiyu(Context context, Path gameRoot) throws IOException {
        Path target = gameRoot.resolve("mods/penguin");
        Files.createDirectories(target);
        Path pendingModel = target.resolve(".base.obj.bundled-installing");
        Path pendingTexture = target.resolve(".texture_diffuse.png.bundled-installing");
        Path pendingMarker = target.resolve(".bundled-seiyu-version.installing");
        try {
            copyVerified(context, SEIYU_MODEL_ASSET, pendingModel,
                         SEIYU_MODEL_SIZE, SEIYU_MODEL_SHA256);
            copyVerified(context, SEIYU_TEXTURE_ASSET, pendingTexture,
                         SEIYU_TEXTURE_SIZE, SEIYU_TEXTURE_SHA256);
            Files.write(pendingMarker,
                        (SEIYU_VERSION + "\n").getBytes(StandardCharsets.UTF_8),
                        StandardOpenOption.CREATE,
                        StandardOpenOption.TRUNCATE_EXISTING);
            replace(pendingModel, target.resolve("base.obj"));
            replace(pendingTexture, target.resolve("texture_diffuse.png"));
            replace(pendingMarker, target.resolve(VERSION_MARKER));
        } finally {
            Files.deleteIfExists(pendingModel);
            Files.deleteIfExists(pendingTexture);
            Files.deleteIfExists(pendingMarker);
        }
        if (!isSeiyuReady(gameRoot)) {
            throw new IOException("The bundled Seiyu files did not pass final verification.");
        }
    }

    private static void copyVerified(Context context, String asset, Path destination,
                                     long expectedSize, String expectedHash) throws IOException {
        MessageDigest digest = sha256Digest();
        long copied = 0;
        byte[] buffer = new byte[256 * 1024];
        Files.deleteIfExists(destination);
        try (InputStream input = context.getAssets().open(asset);
             OutputStream output = Files.newOutputStream(destination,
                 StandardOpenOption.CREATE_NEW)) {
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                copied += read;
                if (copied > expectedSize) {
                    throw new IOException("The bundled " + asset + " is unexpectedly large.");
                }
                output.write(buffer, 0, read);
                digest.update(buffer, 0, read);
            }
        } catch (IOException exception) {
            Files.deleteIfExists(destination);
            throw exception;
        }
        if (copied != expectedSize || !hex(digest.digest()).equals(expectedHash)) {
            Files.deleteIfExists(destination);
            throw new IOException("The bundled " + asset + " failed verification.");
        }
    }

    private static boolean verified(Path path, long expectedSize, String expectedHash)
            throws IOException {
        if (!Files.isRegularFile(path) || Files.size(path) != expectedSize) return false;
        MessageDigest digest = sha256Digest();
        byte[] buffer = new byte[256 * 1024];
        try (InputStream input = Files.newInputStream(path)) {
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                digest.update(buffer, 0, read);
            }
        }
        return hex(digest.digest()).equals(expectedHash);
    }

    private static MessageDigest sha256Digest() throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable.", exception);
        }
    }

    private static void replace(Path source, Path destination) throws IOException {
        try {
            Files.move(source, destination, StandardCopyOption.ATOMIC_MOVE,
                       StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(source, destination, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder output = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) output.append(String.format("%02x", value & 0xff));
        return output.toString();
    }
}
