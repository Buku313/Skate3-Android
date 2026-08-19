package chat.buku.skate3;

import android.content.Context;
import android.os.Build;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Installs an AdrenoTools ADPKG into app-private storage. */
final class CustomGpuDriver {
    private static final String ENABLED_PREFERENCE = "custom_gpu_driver_enabled";
    private static final String PREFERENCES = "gpu_driver";
    private static final String ACTIVE_DIRECTORY = "active";
    private static final String INSTALL_METADATA = ".skate3-driver.json";
    private static final int MAX_FILES = 96;
    private static final long MAX_FILE_SIZE = 192L * 1024 * 1024;
    private static final long MAX_TOTAL_SIZE = 384L * 1024 * 1024;
    private static final long MAX_METADATA_SIZE = 128L * 1024;

    static final class Driver {
        final String name;
        final String vendor;
        final String version;
        final String author;
        final String libraryName;
        final Path directory;
        final boolean enabled;

        Driver(String name, String vendor, String version, String author,
               String libraryName, Path directory, boolean enabled) {
            this.name = name;
            this.vendor = vendor;
            this.version = version;
            this.author = author;
            this.libraryName = libraryName;
            this.directory = directory;
            this.enabled = enabled;
        }

        String label() {
            return name + " " + version;
        }
    }

    private CustomGpuDriver() {}

    static boolean isLikelyAdrenoDevice() {
        String identity = (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + " " +
                           Build.HARDWARE + " " + Build.BOARD)
            .toLowerCase(Locale.US);
        return identity.contains("qualcomm") || identity.contains("qti") ||
               identity.contains("qcom") || identity.contains("snapdragon") ||
               identity.contains("adreno") || identity.contains("sm8");
    }

    static Driver installed(Context context) {
        Path active = root(context).resolve(ACTIVE_DIRECTORY);
        Path marker = active.resolve(INSTALL_METADATA);
        try {
            if (!Files.isRegularFile(marker) || Files.size(marker) > MAX_METADATA_SIZE) return null;
            JSONObject metadata = new JSONObject(
                new String(Files.readAllBytes(marker), StandardCharsets.UTF_8));
            String relativeDirectory = metadata.getString("directory");
            String libraryName = metadata.getString("libraryName");
            if (!safeFileName(libraryName)) return null;
            Path directory = active.resolve(relativeDirectory).normalize();
            if (!directory.startsWith(active) ||
                    !Files.isRegularFile(directory.resolve(libraryName))) return null;
            boolean enabled = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                .getBoolean(ENABLED_PREFERENCE, false);
            return new Driver(metadata.getString("name"), metadata.getString("vendor"),
                              metadata.getString("driverVersion"),
                              metadata.getString("author"), libraryName, directory,
                              enabled && isLikelyAdrenoDevice());
        } catch (Exception ignored) {
            return null;
        }
    }

    static Driver importPackage(Context context, InputStream rawInput) throws IOException {
        if (!isLikelyAdrenoDevice()) {
            throw new IOException("Turnip drivers can only be enabled on Snapdragon / Adreno devices.");
        }
        Path root = root(context);
        Files.createDirectories(root);
        Path staging = root.resolve("importing-" + System.nanoTime());
        Files.createDirectory(staging);
        try {
            extractPackage(rawInput, staging);
            Path packageMetadata = findPackageMetadata(staging);
            JSONObject metadata = readPackageMetadata(packageMetadata);
            validateMetadata(metadata);

            Path packageDirectory = packageMetadata.getParent();
            String libraryName = metadata.getString("libraryName");
            Path mainLibrary = packageDirectory.resolve(libraryName).normalize();
            if (!mainLibrary.getParent().equals(packageDirectory) ||
                    !Files.isRegularFile(mainLibrary)) {
                throw new IOException("The driver ZIP does not contain " + libraryName +
                                      " beside meta.json.");
            }
            validateLibraries(staging);

            JSONObject installMetadata = new JSONObject();
            installMetadata.put("schema", 1);
            installMetadata.put("name", bounded(metadata.getString("name"), 80, "name"));
            installMetadata.put("vendor", bounded(metadata.getString("vendor"), 32, "vendor"));
            installMetadata.put("driverVersion",
                                bounded(metadata.getString("driverVersion"), 64, "driverVersion"));
            installMetadata.put("author", bounded(metadata.getString("author"), 80, "author"));
            installMetadata.put("libraryName", libraryName);
            String relativeDirectory = staging.relativize(packageDirectory).toString();
            installMetadata.put("directory", relativeDirectory.isEmpty() ? "." : relativeDirectory);
            Files.write(staging.resolve(INSTALL_METADATA),
                        installMetadata.toString(2).getBytes(StandardCharsets.UTF_8),
                        StandardOpenOption.CREATE_NEW);

            Path active = root.resolve(ACTIVE_DIRECTORY);
            Path backup = root.resolve("previous");
            deleteRecursively(backup);
            if (Files.exists(active)) move(active, backup);
            try {
                move(staging, active);
                deleteRecursively(backup);
            } catch (IOException exception) {
                if (Files.exists(backup) && !Files.exists(active)) move(backup, active);
                throw exception;
            }
            context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
                .putBoolean(ENABLED_PREFERENCE, true).apply();
            Driver driver = installed(context);
            if (driver == null) throw new IOException("The imported driver could not be activated.");
            return driver;
        } catch (Exception exception) {
            deleteRecursively(staging);
            if (exception instanceof IOException) throw (IOException) exception;
            throw new IOException("The driver package is invalid: " + clean(exception), exception);
        }
    }

    static void useSystem(Context context) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putBoolean(ENABLED_PREFERENCE, false).apply();
    }

    static void useCustom(Context context) throws IOException {
        if (!isLikelyAdrenoDevice()) {
            throw new IOException("Turnip is not available on this non-Adreno device.");
        }
        if (installed(context) == null) throw new IOException("Import a driver ZIP first.");
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putBoolean(ENABLED_PREFERENCE, true).apply();
    }

    static void remove(Context context) throws IOException {
        useSystem(context);
        deleteRecursively(root(context).resolve(ACTIVE_DIRECTORY));
    }

    static String diagnostic(Context context) {
        Driver driver = installed(context);
        if (driver == null) return "System Driver";
        return driver.enabled ? "Custom: " + driver.label() :
                                "System Driver (custom installed: " + driver.label() + ")";
    }

    private static Path root(Context context) {
        return context.getFilesDir().toPath().resolve("gpu-drivers");
    }

    private static void extractPackage(InputStream rawInput, Path staging) throws IOException {
        long total = 0;
        int files = 0;
        byte[] buffer = new byte[256 * 1024];
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(rawInput))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; zip.closeEntry()) {
                String name = entry.getName();
                if (name == null || name.isBlank() || name.indexOf('\0') >= 0 ||
                        name.contains("\\") || name.startsWith("/") || name.length() > 240) {
                    throw new IOException("The driver ZIP contains an unsafe path.");
                }
                Path target = staging.resolve(name).normalize();
                if (!target.startsWith(staging)) {
                    throw new IOException("The driver ZIP contains a path outside the package.");
                }
                if (entry.isDirectory()) {
                    Files.createDirectories(target);
                    continue;
                }
                if (++files > MAX_FILES) throw new IOException("The driver ZIP contains too many files.");
                if (Files.exists(target)) throw new IOException("The driver ZIP contains duplicate files.");
                Files.createDirectories(target.getParent());
                long fileSize = 0;
                try (OutputStream output = Files.newOutputStream(target, StandardOpenOption.CREATE_NEW)) {
                    for (;;) {
                        int read = zip.read(buffer);
                        if (read < 0) break;
                        fileSize += read;
                        total += read;
                        if (fileSize > MAX_FILE_SIZE || total > MAX_TOTAL_SIZE) {
                            throw new IOException("The driver ZIP is unexpectedly large.");
                        }
                        output.write(buffer, 0, read);
                    }
                }
                if (fileSize == 0) throw new IOException("The driver ZIP contains an empty file.");
            }
        }
        if (files == 0) throw new IOException("The selected file is not a driver ZIP.");
    }

    private static Path findPackageMetadata(Path staging) throws IOException {
        List<Path> matches = new ArrayList<>();
        try (Stream<Path> paths = Files.walk(staging)) {
            paths.filter(path -> path.getFileName() != null &&
                                 path.getFileName().toString().equals("meta.json"))
                 .forEach(matches::add);
        }
        if (matches.size() != 1) {
            throw new IOException("The driver ZIP must contain exactly one meta.json.");
        }
        return matches.get(0);
    }

    private static JSONObject readPackageMetadata(Path path) throws IOException {
        if (Files.size(path) > MAX_METADATA_SIZE) {
            throw new IOException("The driver metadata is unexpectedly large.");
        }
        try {
            return new JSONObject(new String(Files.readAllBytes(path), StandardCharsets.UTF_8));
        } catch (Exception exception) {
            throw new IOException("meta.json is not valid JSON.", exception);
        }
    }

    private static void validateMetadata(JSONObject metadata) throws IOException {
        try {
            if (metadata.getInt("schemaVersion") != 1) {
                throw new IOException("This driver package schema is not supported.");
            }
            bounded(metadata.getString("name"), 80, "name");
            bounded(metadata.getString("vendor"), 32, "vendor");
            bounded(metadata.getString("driverVersion"), 64, "driverVersion");
            bounded(metadata.getString("packageVersion"), 64, "packageVersion");
            bounded(metadata.getString("author"), 80, "author");
            String library = metadata.getString("libraryName");
            if (!safeFileName(library) || !library.endsWith(".so")) {
                throw new IOException("meta.json has an invalid libraryName.");
            }
            int minimumApi = metadata.getInt("minApi");
            if (minimumApi < 21 || minimumApi > Build.VERSION.SDK_INT) {
                throw new IOException("This driver requires Android API " + minimumApi +
                                      ", but this device is API " + Build.VERSION.SDK_INT + ".");
            }
        } catch (IOException exception) {
            throw exception;
        } catch (Exception exception) {
            throw new IOException("meta.json is missing required ADPKG fields.", exception);
        }
    }

    private static void validateLibraries(Path staging) throws IOException {
        int libraries = 0;
        try (Stream<Path> paths = Files.walk(staging)) {
            for (Path path : (Iterable<Path>) paths::iterator) {
                if (!Files.isRegularFile(path) || !path.getFileName().toString().endsWith(".so")) {
                    continue;
                }
                ++libraries;
                validateArm64Elf(path);
                path.toFile().setReadable(true, true);
                path.toFile().setExecutable(true, true);
            }
        }
        if (libraries == 0) throw new IOException("The driver ZIP contains no shared libraries.");
    }

    private static void validateArm64Elf(Path path) throws IOException {
        byte[] header = new byte[20];
        try (InputStream input = Files.newInputStream(path)) {
            int offset = 0;
            while (offset < header.length) {
                int read = input.read(header, offset, header.length - offset);
                if (read < 0) break;
                offset += read;
            }
            int type = (header[16] & 0xff) | ((header[17] & 0xff) << 8);
            int machine = (header[18] & 0xff) | ((header[19] & 0xff) << 8);
            if (offset != header.length || header[0] != 0x7f || header[1] != 'E' ||
                    header[2] != 'L' || header[3] != 'F' || header[4] != 2 ||
                    header[5] != 1 || type != 3 || machine != 183) {
                throw new IOException(path.getFileName() + " is not an ARM64 Android library.");
            }
        }
    }

    private static String bounded(String value, int maximum, String field) throws IOException {
        if (value == null || value.isBlank() || value.length() > maximum ||
                value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0) {
            throw new IOException("meta.json has an invalid " + field + ".");
        }
        return value.trim();
    }

    private static boolean safeFileName(String value) {
        return value != null && value.length() >= 4 && value.length() <= 120 &&
               !value.equals(".") && !value.equals("..") &&
               value.indexOf('/') < 0 && value.indexOf('\\') < 0 &&
               value.indexOf('\0') < 0;
    }

    private static void move(Path source, Path destination) throws IOException {
        try {
            Files.move(source, destination, StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(source, destination);
        }
    }

    private static void deleteRecursively(Path target) throws IOException {
        if (!Files.exists(target)) return;
        try (Stream<Path> paths = Files.walk(target)) {
            for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) {
                Files.deleteIfExists(path);
            }
        }
    }

    private static String clean(Throwable throwable) {
        String message = throwable.getMessage();
        return message == null || message.isBlank() ?
            throwable.getClass().getSimpleName() : message;
    }
}
