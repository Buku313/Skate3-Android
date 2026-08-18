package chat.buku.skate3;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class LauncherActivity extends Activity {
    private static final int REQUEST_ISO = 1001;
    private static final int REQUEST_TITLE_UPDATE = 1002;
    private static final int REQUEST_SEIYU_MODEL = 1003;
    private static final int REQUEST_SEIYU_TEXTURE = 1004;
    private static final long INSTALL_HEADROOM = 512L * 1024 * 1024;
    private static final long MAX_SEIYU_MODEL_SIZE = 32L * 1024 * 1024;
    private static final long MAX_SEIYU_TEXTURE_SIZE = 64L * 1024 * 1024;
    private static final String COMPLETE_MARKER = ".iso-extraction-complete";
    private static final String EXPECTED_DEFAULT_XEX =
        "1db39496585c521d17a2137804f42cf73ebed2b32cac166ec42dbf772f4dcf7f";
    private static final String EXPECTED_WEBKIT_XEX =
        "0ee66b9558c888147f4ffdfd748cfc28950e80e46a97321da2de2afdb068523f";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private File storageRoot;
    private File gameDirectory;
    private File partialDirectory;
    private File setupLog;
    private TextView statusText;
    private TextView detailText;
    private ProgressBar progressBar;
    private Button primaryButton;
    private Button characterButton;
    private Button secondaryButton;
    private Button tertiaryButton;
    private boolean busy;
    private long lastProgressUpdate;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        File external = getExternalFilesDir(null);
        storageRoot = external != null ? external : getFilesDir();
        gameDirectory = new File(storageRoot, "game");
        partialDirectory = new File(storageRoot, "game-installing");
        setupLog = new File(getFilesDir(), "phone-setup.log");
        buildInterface();
        refreshInterface();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            worker.shutdownNow();
        }
    }

    private void buildInterface() {
        int padding = dp(24);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(10, 10, 12));

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER_HORIZONTAL);
        content.setPadding(padding, dp(28), padding, dp(28));
        scroll.addView(content, new ScrollView.LayoutParams(
            ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));

        TextView eyebrow = text("PHONE-ONLY INSTALLER", 13, Color.rgb(255, 112, 28));
        eyebrow.setGravity(Gravity.CENTER);
        content.addView(eyebrow, matchWrap(dp(4)));

        TextView title = text("SKATE 3 MOBILE", 30, Color.WHITE);
        title.setGravity(Gravity.CENTER);
        title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        content.addView(title, matchWrap(dp(8)));

        TextView intro = text(
            "No computer required. Select an Xbox 360 ISO that you dumped from your own Skate 3 copy. The game stays on this device.",
            16, Color.rgb(205, 205, 210));
        intro.setGravity(Gravity.CENTER);
        intro.setLineSpacing(0, 1.12f);
        content.addView(intro, matchWrap(dp(24)));

        statusText = text("Checking installation...", 20, Color.WHITE);
        statusText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        statusText.setGravity(Gravity.CENTER);
        content.addView(statusText, matchWrap(dp(8)));

        detailText = text("", 14, Color.rgb(170, 170, 178));
        detailText.setGravity(Gravity.CENTER);
        detailText.setLineSpacing(0, 1.15f);
        content.addView(detailText, matchWrap(dp(18)));

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setProgressTintList(ColorStateList.valueOf(Color.rgb(255, 104, 24)));
        progressBar.setVisibility(View.GONE);
        content.addView(progressBar, matchFixed(dp(10), dp(18)));

        primaryButton = actionButton(true);
        characterButton = actionButton(false);
        secondaryButton = actionButton(false);
        tertiaryButton = actionButton(false);
        content.addView(primaryButton, matchFixed(dp(58), dp(10)));
        content.addView(characterButton, matchFixed(dp(54), dp(10)));
        content.addView(secondaryButton, matchFixed(dp(54), dp(10)));
        content.addView(tertiaryButton, matchFixed(dp(54), dp(10)));

        TextView requirements = text(
            "Requires Android 13+, ARM64, Vulkan, a controller, and about 8 GB free after the ISO is already on your device or USB drive.",
            12, Color.rgb(125, 125, 132));
        requirements.setGravity(Gravity.CENTER);
        requirements.setLineSpacing(0, 1.1f);
        content.addView(requirements, matchWrap(dp(18)));

        setContentView(scroll);
    }

    private void refreshInterface() {
        if (busy) {
            return;
        }
        setButtonsEnabled(true);
        progressBar.setVisibility(View.GONE);
        String unsupported = compatibilityProblem();
        if (unsupported != null) {
            statusText.setText("DEVICE NOT SUPPORTED");
            detailText.setText(unsupported);
            setButton(primaryButton, "CLOSE", view -> finish(), true);
            hide(characterButton);
            hide(secondaryButton);
            hide(tertiaryButton);
            return;
        }

        boolean scopedReady = isGameReady(gameDirectory.toPath());
        boolean legacyReady = legacyGameReady();
        if (scopedReady || legacyReady) {
            statusText.setText("READY TO SKATE");
            File activeGame = scopedReady ? gameDirectory : new File("/storage/emulated/0/skate3");
            detailText.setText(Build.MODEL + "\nGame files: " + activeGame.getAbsolutePath());
            setButton(primaryButton, "PLAY SKATE 3", view -> launchGame(), true);
            configureSeiyuInstallButton(activeGame);
            if (scopedReady) {
                setButton(secondaryButton, "REPAIR OR REINSTALL", view -> confirmReinstall(), false);
            } else {
                hide(secondaryButton);
            }
            setButton(tertiaryButton, "VIEW SETUP LOG", view -> showLog(), false);
            return;
        }

        if (isExtractionComplete()) {
            statusText.setText("GAME EXTRACTED");
            detailText.setText("Finish by downloading the verified 1.7 MB Title Update 3, or select the package yourself.");
            setButton(primaryButton, "FINISH SETUP AUTOMATICALLY", view -> finishSetupOnline(), true);
            hide(characterButton);
            setButton(secondaryButton, "SELECT TITLE UPDATE FILE", view -> pickTitleUpdate(), false);
            setButton(tertiaryButton, "START OVER", view -> confirmStartOver(), false);
            return;
        }

        statusText.setText("ONE FILE NEEDED");
        detailText.setText("Select your Skate 3 Xbox 360 ISO. It can be in Downloads, on an SD card, or on a connected USB drive.\n\nFree space: " +
                           humanBytes(new StatFs(storageRoot.getAbsolutePath()).getAvailableBytes()));
        setButton(primaryButton, "SELECT MY SKATE 3 ISO", view -> pickIso(), true);
        hide(characterButton);
        hide(secondaryButton);
        if (setupLog.isFile()) {
            setButton(tertiaryButton, "VIEW LAST SETUP LOG", view -> showLog(), false);
        } else {
            hide(tertiaryButton);
        }
    }

    private void pickIso() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_ISO);
        } catch (ActivityNotFoundException exception) {
            showFailure("Android could not open its file picker.", exception);
        }
    }

    private void pickTitleUpdate() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_TITLE_UPDATE);
        } catch (ActivityNotFoundException exception) {
            showFailure("Android could not open its file picker.", exception);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            if (requestCode == REQUEST_SEIYU_TEXTURE) {
                busy = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                refreshInterface();
            }
            return;
        }
        Uri uri = data.getData();
        if (requestCode == REQUEST_ISO) {
            try {
                getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
            }
            beginIsoInstallation(uri);
        } else if (requestCode == REQUEST_TITLE_UPDATE) {
            installSelectedTitleUpdate(uri);
        } else if (requestCode == REQUEST_SEIYU_MODEL) {
            importSeiyuModel(uri);
        } else if (requestCode == REQUEST_SEIYU_TEXTURE) {
            importSeiyuTexture(uri);
        }
    }

    private void beginIsoInstallation(Uri uri) {
        setBusy("INSPECTING ISO", "Checking " + displayName(uri), false);
        worker.execute(() -> {
            boolean extractionComplete = false;
            try {
                deleteRecursively(partialDirectory);
                Files.createDirectories(partialDirectory.toPath());
                try (ParcelFileDescriptor descriptor =
                         getContentResolver().openFileDescriptor(uri, "r");
                     FileInputStream input = descriptor == null ? null :
                         new FileInputStream(descriptor.getFileDescriptor())) {
                    if (descriptor == null || input == null) {
                        throw new IOException("Android could not open the selected ISO.");
                    }
                    FileChannel channel = input.getChannel();
                    XboxIsoExtractor.Inspection inspection = XboxIsoExtractor.inspect(channel);
                    long available = new StatFs(storageRoot.getAbsolutePath()).getAvailableBytes();
                    if (available < inspection.totalBytes + INSTALL_HEADROOM) {
                        throw new IOException("Not enough free space. This install needs " +
                            humanBytes(inspection.totalBytes + INSTALL_HEADROOM) +
                            ", but only " + humanBytes(available) + " is available.");
                    }
                    runOnUiThread(() -> {
                        statusText.setText("EXTRACTING GAME");
                        progressBar.setVisibility(View.VISIBLE);
                    });
                    XboxIsoExtractor.extract(channel, inspection, partialDirectory.toPath(),
                                             this::showExtractionProgress);
                }
                verifyRetailGame(partialDirectory.toPath());
                Files.write(partialDirectory.toPath().resolve(COMPLETE_MARKER),
                            "Skate 3 retail files verified\n".getBytes(StandardCharsets.UTF_8));
                extractionComplete = true;
                appendLog("ISO extraction and retail-file verification completed.");
                installTitleUpdateOnlineAndFinalize();
            } catch (Exception exception) {
                if (!extractionComplete) {
                    try {
                        deleteRecursively(partialDirectory);
                    } catch (IOException cleanupError) {
                        exception.addSuppressed(cleanupError);
                    }
                }
                showFailure("Setup stopped: " + cleanMessage(exception), exception);
            }
        });
    }

    private void finishSetupOnline() {
        setBusy("INSTALLING TITLE UPDATE 3", "Downloading and verifying 1.7 MB...", true);
        worker.execute(() -> {
            try {
                verifyRetailGame(partialDirectory.toPath());
                installTitleUpdateOnlineAndFinalize();
            } catch (Exception exception) {
                showFailure("Title Update setup stopped: " + cleanMessage(exception), exception);
            }
        });
    }

    private void installTitleUpdateOnlineAndFinalize() throws IOException {
        runOnUiThread(() -> {
            statusText.setText("INSTALLING TITLE UPDATE 3");
            detailText.setText("Downloading and verifying 1.7 MB...");
            progressBar.setProgress(0);
            progressBar.setVisibility(View.VISIBLE);
        });
        TitleUpdateInstaller.downloadAndInstall(partialDirectory.toPath(),
                                                this::showDownloadProgress);
        finalizeInstallation();
    }

    private void installSelectedTitleUpdate(Uri uri) {
        setBusy("VERIFYING TITLE UPDATE 3", "Reading " + displayName(uri), true);
        worker.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) {
                    throw new IOException("Android could not open the selected Title Update file.");
                }
                verifyRetailGame(partialDirectory.toPath());
                TitleUpdateInstaller.installPackage(input, partialDirectory.toPath());
                finalizeInstallation();
            } catch (Exception exception) {
                showFailure("Title Update setup stopped: " + cleanMessage(exception), exception);
            }
        });
    }

    private void finalizeInstallation() throws IOException {
        if (!TitleUpdateInstaller.isInstalled(partialDirectory.toPath())) {
            throw new IOException("Title Update 3 did not pass final verification.");
        }
        Files.deleteIfExists(partialDirectory.toPath().resolve(COMPLETE_MARKER));
        if (gameDirectory.exists()) {
            deleteRecursively(gameDirectory);
        }
        try {
            Files.move(partialDirectory.toPath(), gameDirectory.toPath(),
                       StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(partialDirectory.toPath(), gameDirectory.toPath());
        }
        appendLog("Phone-only installation completed at " + gameDirectory.getAbsolutePath());
        runOnUiThread(() -> {
            busy = false;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            progressBar.setProgress(1000);
            statusText.setText("INSTALLATION COMPLETE");
            detailText.setText("Your files were verified and stayed on this device.");
            setButton(primaryButton, "PLAY SKATE 3", view -> launchGame(), true);
            configureSeiyuInstallButton(gameDirectory);
            hide(secondaryButton);
            hide(tertiaryButton);
        });
    }

    private void showExtractionProgress(long copied, long total, String currentFile) {
        long now = System.nanoTime();
        if (copied < total && now - lastProgressUpdate < 150_000_000L) {
            return;
        }
        lastProgressUpdate = now;
        int progress = total > 0 ? (int) Math.min(1000, copied * 1000 / total) : 0;
        runOnUiThread(() -> {
            progressBar.setProgress(progress);
            detailText.setText(humanBytes(copied) + " of " + humanBytes(total) +
                               "\n" + currentFile);
        });
    }

    private void showDownloadProgress(long copied, long total, String message) {
        int progress = total > 0 ? (int) Math.min(1000, copied * 1000 / total) : 0;
        runOnUiThread(() -> {
            progressBar.setProgress(progress);
            detailText.setText(message + "\n" + humanBytes(copied) +
                               (total > 0 ? " of " + humanBytes(total) : ""));
        });
    }

    private void launchGame() {
        if (!isGameReady(gameDirectory.toPath()) && !legacyGameReady()) {
            Toast.makeText(this, "Game installation needs repair.", Toast.LENGTH_LONG).show();
            refreshInterface();
            return;
        }
        startActivity(new Intent(this, Skate3Activity.class));
        finish();
    }

    private void configureSeiyuInstallButton(File activeGame) {
        boolean assetsInstalled = seiyuAssetsInstalled(activeGame);
        String label = assetsInstalled
            ? "SEIYU MOD FILES: INSTALLED"
            : "OPTIONAL: ADD SEIYU MOD FILES";
        setButton(characterButton, label, view -> explainSeiyuInstall(), false);
    }

    private static boolean seiyuAssetsInstalled(File activeGame) {
        File mod = new File(activeGame, "mods/penguin");
        return new File(mod, "base.obj").isFile() &&
               new File(mod, "texture_diffuse.png").isFile();
    }

    private File activeGameDirectory() {
        if (isGameReady(gameDirectory.toPath())) {
            return gameDirectory;
        }
        return new File("/storage/emulated/0/skate3");
    }

    private void explainSeiyuInstall() {
        new AlertDialog.Builder(this)
            .setTitle("Add Seiyu Paradise Penguin")
            .setMessage("Seiyu is optional and is not bundled with the app. If you have the Seiyu model files, choose base.obj and then texture_diffuse.png. In game, press RB + Start and open Mods to switch between the original skater and Seiyu.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Choose files", (dialog, which) -> pickSeiyuModel())
            .show();
    }

    private void pickSeiyuModel() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.putExtra(Intent.EXTRA_TITLE, "Choose Seiyu base.obj");
        startActivityForResult(intent, REQUEST_SEIYU_MODEL);
    }

    private void pickSeiyuTexture() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/png");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.putExtra(Intent.EXTRA_TITLE, "Choose Seiyu texture_diffuse.png");
        startActivityForResult(intent, REQUEST_SEIYU_TEXTURE);
    }

    private void importSeiyuModel(Uri uri) {
        setBusy("ADDING SEIYU", "Copying the character model...", false);
        worker.execute(() -> {
            try {
                Path mod = activeGameDirectory().toPath().resolve("mods/penguin");
                Files.createDirectories(mod);
                copyUriLimited(uri, mod.resolve("base.obj.installing"), MAX_SEIYU_MODEL_SIZE, false);
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    pickSeiyuTexture();
                });
            } catch (Exception exception) {
                showFailure("Could not add the Seiyu model: " + cleanMessage(exception), exception);
            }
        });
    }

    private void importSeiyuTexture(Uri uri) {
        setBusy("ADDING SEIYU", "Copying and checking the texture...", false);
        worker.execute(() -> {
            Path mod = activeGameDirectory().toPath().resolve("mods/penguin");
            Path pendingModel = mod.resolve("base.obj.installing");
            Path pendingTexture = mod.resolve("texture_diffuse.png.installing");
            try {
                if (!Files.isRegularFile(pendingModel)) {
                    throw new IOException("Choose the Seiyu model again first.");
                }
                copyUriLimited(uri, pendingTexture, MAX_SEIYU_TEXTURE_SIZE, true);
                Files.move(pendingModel, mod.resolve("base.obj"),
                           StandardCopyOption.REPLACE_EXISTING);
                Files.move(pendingTexture, mod.resolve("texture_diffuse.png"),
                           StandardCopyOption.REPLACE_EXISTING);
                appendLog("Seiyu Paradise Penguin files installed.");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    Toast.makeText(this, "Seiyu installed. Choose it in RB + Start > Mods.",
                                   Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                try {
                    Files.deleteIfExists(pendingTexture);
                } catch (IOException ignored) {
                }
                showFailure("Could not add the Seiyu texture: " + cleanMessage(exception), exception);
            }
        });
    }

    private void copyUriLimited(Uri uri, Path destination, long maximumBytes,
                                boolean requirePng) throws IOException {
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) {
                throw new IOException("Android could not open the selected file.");
            }
            Files.deleteIfExists(destination);
            byte[] buffer = new byte[256 * 1024];
            long copied = 0;
            try (java.io.OutputStream output = Files.newOutputStream(destination)) {
                for (;;) {
                    int read = input.read(buffer);
                    if (read < 0) break;
                    copied += read;
                    if (copied > maximumBytes) {
                        throw new IOException("The selected Seiyu file is unexpectedly large.");
                    }
                    output.write(buffer, 0, read);
                }
            } catch (IOException exception) {
                Files.deleteIfExists(destination);
                throw exception;
            }
            if (copied == 0) {
                Files.deleteIfExists(destination);
                throw new IOException("The selected Seiyu file is empty.");
            }
        }
        if (requirePng) {
            byte[] header = new byte[8];
            try (InputStream input = Files.newInputStream(destination)) {
                if (input.read(header) != header.length ||
                    header[0] != (byte) 0x89 || header[1] != 'P' || header[2] != 'N' ||
                    header[3] != 'G' || header[4] != '\r' || header[5] != '\n' ||
                    header[6] != 0x1A || header[7] != '\n') {
                    Files.deleteIfExists(destination);
                    throw new IOException("The selected texture is not a PNG file.");
                }
            }
        }
    }

    private boolean legacyGameReady() {
        File legacy = new File("/storage/emulated/0/skate3");
        return isGameReady(legacy.toPath());
    }

    private boolean isGameReady(Path root) {
        return Files.isRegularFile(root.resolve("default.xex")) &&
               Files.isRegularFile(root.resolve("data/webkit/EAWebkit.xex")) &&
               TitleUpdateInstaller.isInstalled(root);
    }

    private boolean isExtractionComplete() {
        return new File(partialDirectory, COMPLETE_MARKER).isFile();
    }

    private void verifyRetailGame(Path root) throws IOException {
        verifyFile(root.resolve("default.xex"), EXPECTED_DEFAULT_XEX, "default.xex");
        verifyFile(root.resolve("data/webkit/EAWebkit.xex"), EXPECTED_WEBKIT_XEX,
                   "data/webkit/EAWebkit.xex");
    }

    private void verifyFile(Path path, String expectedHash, String label) throws IOException {
        if (!Files.isRegularFile(path)) {
            throw new IOException("The ISO did not provide " + label + ".");
        }
        try (InputStream input = Files.newInputStream(path)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1024 * 1024];
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                digest.update(buffer, 0, read);
            }
            if (!hex(digest.digest()).equals(expectedHash)) {
                throw new IOException(label + " is not the supported USA/Europe retail version.");
            }
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable.", exception);
        }
    }

    private String compatibilityProblem() {
        boolean arm64 = false;
        for (String abi : Build.SUPPORTED_ABIS) {
            arm64 |= abi.equals("arm64-v8a");
        }
        if (!arm64) {
            return "This build requires a 64-bit ARM Android device.";
        }
        if (Build.VERSION.SDK_INT < 33) {
            return "This build requires Android 13 or newer.";
        }
        if (!getPackageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)) {
            return "This device does not report the required Vulkan support.";
        }
        return null;
    }

    private void confirmReinstall() {
        new AlertDialog.Builder(this)
            .setTitle("Repair or reinstall?")
            .setMessage("This removes the extracted game files from this app, then lets you select your ISO again. Your original ISO is not changed.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Continue", (dialog, which) -> startOver())
            .show();
    }

    private void confirmStartOver() {
        new AlertDialog.Builder(this)
            .setTitle("Discard partial setup?")
            .setMessage("Only the incomplete copy created by this installer will be removed. Your original ISO is not changed.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Start over", (dialog, which) -> startOver())
            .show();
    }

    private void startOver() {
        setBusy("CLEANING UP", "Removing this app's installed copy...", false);
        worker.execute(() -> {
            try {
                deleteRecursively(partialDirectory);
                deleteRecursively(gameDirectory);
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    refreshInterface();
                });
            } catch (Exception exception) {
                showFailure("Cleanup stopped: " + cleanMessage(exception), exception);
            }
        });
    }

    private void showLog() {
        String contents = "No setup log has been written yet.";
        try {
            if (setupLog.isFile()) {
                contents = new String(Files.readAllBytes(setupLog.toPath()), StandardCharsets.UTF_8);
            }
        } catch (IOException exception) {
            contents = cleanMessage(exception);
        }
        new AlertDialog.Builder(this)
            .setTitle("Setup log")
            .setMessage(contents)
            .setPositiveButton("OK", null)
            .show();
    }

    private void setBusy(String status, String detail, boolean progressVisible) {
        busy = true;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        statusText.setText(status);
        detailText.setText(detail);
        progressBar.setProgress(0);
        progressBar.setVisibility(progressVisible ? View.VISIBLE : View.GONE);
        setButtonsEnabled(false);
    }

    private void showFailure(String message, Exception exception) {
        appendLog(message + "\n" + stackTrace(exception));
        runOnUiThread(() -> {
            busy = false;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            progressBar.setVisibility(View.GONE);
            new AlertDialog.Builder(this)
                .setTitle("Setup needs attention")
                .setMessage(message + "\n\nYour original ISO was not changed.")
                .setPositiveButton("OK", (dialog, which) -> refreshInterface())
                .show();
        });
    }

    private synchronized void appendLog(String text) {
        try (FileWriter writer = new FileWriter(setupLog, true)) {
            writer.write(String.format(Locale.US, "\n[%tF %<tT] %s\n", System.currentTimeMillis(), text));
        } catch (IOException ignored) {
        }
    }

    private static void deleteRecursively(File target) throws IOException {
        if (!target.exists()) return;
        File[] children = target.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        if (!target.delete() && target.exists()) {
            throw new IOException("Could not remove " + target.getAbsolutePath());
        }
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
                uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        } catch (Exception ignored) {
        }
        return "selected file";
    }

    private void setButtonsEnabled(boolean enabled) {
        primaryButton.setEnabled(enabled);
        characterButton.setEnabled(enabled);
        secondaryButton.setEnabled(enabled);
        tertiaryButton.setEnabled(enabled);
    }

    private void setButton(Button button, String label, View.OnClickListener listener,
                           boolean primary) {
        button.setText(label);
        button.setOnClickListener(listener);
        button.setVisibility(View.VISIBLE);
        button.setEnabled(true);
        button.setTextColor(primary ? Color.BLACK : Color.WHITE);
        button.setBackgroundTintList(ColorStateList.valueOf(
            primary ? Color.rgb(255, 104, 24) : Color.rgb(48, 48, 54)));
    }

    private static void hide(View view) {
        view.setVisibility(View.GONE);
    }

    private Button actionButton(boolean primary) {
        Button button = new Button(this);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        button.setTextColor(primary ? Color.BLACK : Color.WHITE);
        button.setBackgroundTintList(ColorStateList.valueOf(
            primary ? Color.rgb(255, 104, 24) : Color.rgb(48, 48, 54)));
        return button;
    }

    private TextView text(String value, int size, int color) {
        TextView text = new TextView(this);
        text.setText(value);
        text.setTextSize(size);
        text.setTextColor(color);
        return text;
    }

    private LinearLayout.LayoutParams matchWrap(int bottomMargin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = bottomMargin;
        return params;
    }

    private LinearLayout.LayoutParams matchFixed(int height, int bottomMargin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, height);
        params.bottomMargin = bottomMargin;
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static String humanBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        double value = bytes;
        String[] units = { "B", "KB", "MB", "GB", "TB" };
        int unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            ++unit;
        }
        return String.format(Locale.US, value >= 10 ? "%.1f %s" : "%.2f %s", value, units[unit]);
    }

    private static String cleanMessage(Throwable throwable) {
        String message = throwable.getMessage();
        return message == null || message.isBlank() ? throwable.getClass().getSimpleName() : message;
    }

    private static String stackTrace(Throwable throwable) {
        StringWriter text = new StringWriter();
        throwable.printStackTrace(new PrintWriter(text));
        return text.toString();
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] result = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; ++i) {
            int value = Byte.toUnsignedInt(bytes[i]);
            result[i * 2] = digits[value >>> 4];
            result[i * 2 + 1] = digits[value & 15];
        }
        return new String(result);
    }
}
