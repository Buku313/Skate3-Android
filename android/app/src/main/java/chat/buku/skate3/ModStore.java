package chat.buku.skate3;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

final class ModStore {
    static final String CATALOG_URL =
        "https://buku313.github.io/Skate3-Mobile/mods/catalog.json";
    private static final int MAX_CATALOG_SIZE = 256 * 1024;
    private static final long MAX_ASSET_SIZE = 64L * 1024 * 1024;

    interface Progress {
        void update(long copied, long total, String message);
    }

    static final class Asset {
        final String path;
        final String url;
        final String sha256;
        final long size;

        Asset(String path, String url, String sha256, long size) {
            this.path = path;
            this.url = url;
            this.sha256 = sha256;
            this.size = size;
        }
    }

    static final class Mod {
        final String id;
        final String name;
        final String version;
        final String description;
        final List<Asset> assets;

        Mod(String id, String name, String version, String description,
            List<Asset> assets) {
            this.id = id;
            this.name = name;
            this.version = version;
            this.description = description;
            this.assets = Collections.unmodifiableList(assets);
        }

        long downloadSize() {
            long total = 0;
            for (Asset asset : assets) total += asset.size;
            return total;
        }
    }

    private ModStore() {}

    static List<Mod> loadCatalog() throws Exception {
        byte[] contents = downloadBytes(CATALOG_URL, MAX_CATALOG_SIZE);
        JSONObject root = new JSONObject(new String(contents, StandardCharsets.UTF_8));
        if (root.getInt("schema") != 1) {
            throw new IOException("The Mod Store catalog version is not supported.");
        }
        JSONArray entries = root.getJSONArray("mods");
        if (entries.length() == 0 || entries.length() > 32) {
            throw new IOException("The Mod Store catalog has an invalid number of entries.");
        }
        List<Mod> mods = new ArrayList<>();
        for (int index = 0; index < entries.length(); ++index) {
            JSONObject entry = entries.getJSONObject(index);
            String id = entry.getString("id");
            String name = entry.getString("name");
            String version = entry.getString("version");
            String description = entry.getString("description");
            if (!id.matches("[a-z0-9][a-z0-9-]{1,63}") || name.isBlank() ||
                    name.length() > 80 || version.isBlank() || version.length() > 32 ||
                    description.isBlank() || description.length() > 500) {
                throw new IOException("The Mod Store contains an invalid mod entry.");
            }

            JSONArray files = entry.getJSONArray("assets");
            if (files.length() == 0 || files.length() > 16) {
                throw new IOException("The Mod Store entry for " + name + " has invalid files.");
            }
            List<Asset> assets = new ArrayList<>();
            for (int fileIndex = 0; fileIndex < files.length(); ++fileIndex) {
                JSONObject file = files.getJSONObject(fileIndex);
                Asset asset = new Asset(
                    file.getString("path"),
                    file.getString("url"),
                    file.getString("sha256").toLowerCase(Locale.US),
                    file.getLong("size"));
                validateAsset(asset);
                assets.add(asset);
            }
            mods.add(new Mod(id, name, version, description, assets));
        }
        return Collections.unmodifiableList(mods);
    }

    static boolean isInstalled(Path gameRoot, Mod mod) {
        try {
            for (Asset asset : mod.assets) {
                Path target = safeTarget(gameRoot, asset.path);
                if (!Files.isRegularFile(target) || Files.size(target) != asset.size ||
                        !sha256(target).equals(asset.sha256)) {
                    return false;
                }
            }
            return true;
        } catch (IOException exception) {
            return false;
        }
    }

    static void install(Path gameRoot, Mod mod, Progress progress) throws IOException {
        List<Path> pending = new ArrayList<>();
        long total = mod.downloadSize();
        long completed = 0;
        try {
            for (Asset asset : mod.assets) {
                Path target = safeTarget(gameRoot, asset.path);
                Files.createDirectories(target.getParent());
                Path temporary = target.resolveSibling(target.getFileName() + ".mod-store-download");
                Files.deleteIfExists(temporary);
                pending.add(temporary);
                long before = completed;
                downloadAsset(asset, temporary, (copied, ignored, message) -> {
                    if (progress != null) progress.update(before + copied, total, message);
                });
                completed += asset.size;
            }

            for (int index = 0; index < mod.assets.size(); ++index) {
                Path target = safeTarget(gameRoot, mod.assets.get(index).path);
                moveReplacing(pending.get(index), target);
            }
        } catch (IOException exception) {
            for (Path temporary : pending) {
                try {
                    Files.deleteIfExists(temporary);
                } catch (IOException ignored) {
                }
            }
            throw exception;
        }
    }

    static void uninstall(Path gameRoot, Mod mod) throws IOException {
        for (Asset asset : mod.assets) {
            Files.deleteIfExists(safeTarget(gameRoot, asset.path));
        }
    }

    private static void validateAsset(Asset asset) throws IOException {
        if (!asset.path.startsWith("mods/") || asset.path.contains("\\") ||
                asset.path.contains("..") || asset.path.length() > 180 ||
                !asset.url.startsWith("https://") ||
                !asset.sha256.matches("[0-9a-f]{64}") ||
                asset.size <= 0 || asset.size > MAX_ASSET_SIZE) {
            throw new IOException("The Mod Store contains an invalid asset.");
        }
        URL url = new URL(asset.url);
        if (!"https".equals(url.getProtocol()) ||
                !"buku313.github.io".equalsIgnoreCase(url.getHost())) {
            throw new IOException("The Mod Store asset host is not trusted.");
        }
    }

    private static Path safeTarget(Path gameRoot, String relative) throws IOException {
        Path root = gameRoot.toAbsolutePath().normalize();
        Path mods = root.resolve("mods").normalize();
        Path target = root.resolve(relative).normalize();
        if (!target.startsWith(mods)) {
            throw new IOException("The Mod Store requested an unsafe install path.");
        }
        return target;
    }

    private static void downloadAsset(Asset asset, Path destination, Progress progress)
            throws IOException {
        HttpURLConnection connection = open(asset.url, "application/octet-stream");
        long advertised = connection.getContentLengthLong();
        if (advertised > MAX_ASSET_SIZE || (advertised >= 0 && advertised != asset.size)) {
            connection.disconnect();
            throw new IOException("The download size for " + asset.path + " is invalid.");
        }

        MessageDigest digest = sha256Digest();
        long copied = 0;
        byte[] buffer = new byte[256 * 1024];
        try (BufferedInputStream input = new BufferedInputStream(connection.getInputStream());
             FileOutputStream output = new FileOutputStream(destination.toFile())) {
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                copied += read;
                if (copied > asset.size || copied > MAX_ASSET_SIZE) {
                    throw new IOException("The download for " + asset.path + " is too large.");
                }
                output.write(buffer, 0, read);
                digest.update(buffer, 0, read);
                if (progress != null) {
                    progress.update(copied, asset.size, "Downloading " + fileName(asset.path));
                }
            }
            output.getFD().sync();
        } catch (IOException exception) {
            Files.deleteIfExists(destination);
            throw exception;
        } finally {
            connection.disconnect();
        }

        if (copied != asset.size || !hex(digest.digest()).equals(asset.sha256)) {
            Files.deleteIfExists(destination);
            throw new IOException("Verification failed for " + fileName(asset.path) + ".");
        }
    }

    private static byte[] downloadBytes(String address, int limit) throws IOException {
        HttpURLConnection connection = open(address, "application/json");
        try (BufferedInputStream input = new BufferedInputStream(connection.getInputStream());
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                if (output.size() + read > limit) {
                    throw new IOException("The Mod Store catalog is unexpectedly large.");
                }
                output.write(buffer, 0, read);
            }
            return output.toByteArray();
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection open(String address, String accept) throws IOException {
        URL url = new URL(address);
        if (!"https".equals(url.getProtocol()) ||
                !"buku313.github.io".equalsIgnoreCase(url.getHost())) {
            throw new IOException("The Mod Store refused an untrusted download address.");
        }
        HttpURLConnection connection = (HttpURLConnection) url.openConnection();
        connection.setConnectTimeout(12_000);
        connection.setReadTimeout(45_000);
        connection.setInstanceFollowRedirects(true);
        connection.setUseCaches(false);
        connection.setRequestProperty("Cache-Control", "no-cache");
        connection.setRequestProperty("Accept", accept);
        connection.setRequestProperty("User-Agent", "Skate3-Mobile-Mod-Store");
        int status = connection.getResponseCode();
        if (status < 200 || status >= 300) {
            connection.disconnect();
            throw new IOException("Mod Store server returned HTTP " + status + ".");
        }
        return connection;
    }

    private static void moveReplacing(Path source, Path destination) throws IOException {
        try {
            Files.move(source, destination, StandardCopyOption.ATOMIC_MOVE,
                       StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(source, destination, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static MessageDigest sha256Digest() throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable.", exception);
        }
    }

    private static String sha256(Path file) throws IOException {
        MessageDigest digest = sha256Digest();
        byte[] buffer = new byte[256 * 1024];
        try (BufferedInputStream input = new BufferedInputStream(Files.newInputStream(file))) {
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                digest.update(buffer, 0, read);
            }
        }
        return hex(digest.digest());
    }

    private static String fileName(String path) {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.substring(slash + 1) : path;
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] result = new char[bytes.length * 2];
        for (int index = 0; index < bytes.length; ++index) {
            int value = Byte.toUnsignedInt(bytes[index]);
            result[index * 2] = digits[value >>> 4];
            result[index * 2 + 1] = digits[value & 15];
        }
        return new String(result);
    }
}
