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
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class LauncherActivity extends Activity {
    private static final int REQUEST_ISO = 1001;
    private static final int REQUEST_TITLE_UPDATE = 1002;
    private static final int REQUEST_SEIYU_MODEL = 1003;
    private static final int REQUEST_SEIYU_TEXTURE = 1004;
    private static final int REQUEST_GPU_DRIVER = 1005;
    private static final long INSTALL_HEADROOM = 512L * 1024 * 1024;
    private static final long MAX_SEIYU_MODEL_SIZE = 32L * 1024 * 1024;
    private static final long MAX_SEIYU_TEXTURE_SIZE = 64L * 1024 * 1024;
    private static final String BUKU_GITHUB =
        "https://github.com/Buku313/Skate3-Mobile";
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
    private Button languageButton;
    private Button primaryButton;
    private Button characterButton;
    private Button gpuDriverButton;
    private Button secondaryButton;
    private Button tertiaryButton;
    private Button updateButton;
    private Button reportButton;
    private AlertDialog modStoreListDialog;
    private boolean busy;
    private boolean checkingUpdate;
    private boolean updatePromptShown;
    private boolean awaitingInstallPermission;
    private boolean bundledSeiyuAttempted;
    private AppUpdater.UpdateInfo availableUpdate;
    private File pendingUpdateApk;
    private long lastProgressUpdate;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Opening the launcher icon over a live SDL session used to leave its
        // SurfaceView abandoned. Remove this duplicate launcher immediately and
        // reveal the game that is already underneath it.
        if (Skate3Activity.isSessionActive()) {
            finish();
            return;
        }
        File external = getExternalFilesDir(null);
        storageRoot = external != null ? external : getFilesDir();
        gameDirectory = new File(storageRoot, "game");
        partialDirectory = new File(storageRoot, "game-installing");
        setupLog = new File(getFilesDir(), "phone-setup.log");
        buildInterface();
        refreshInterface();
        checkForAppUpdate(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (awaitingInstallPermission && pendingUpdateApk != null &&
            getPackageManager().canRequestPackageInstalls()) {
            awaitingInstallPermission = false;
            if (AppUpdater.install(this, pendingUpdateApk)) pendingUpdateApk = null;
        }
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

        LinearLayout brandRow = new LinearLayout(this);
        brandRow.setOrientation(LinearLayout.HORIZONTAL);
        brandRow.setGravity(Gravity.CENTER_VERTICAL);

        TextView bukuBrand = githubBrand("BUKU", Gravity.START);
        TextView numberBrand = githubBrand("313", Gravity.END);
        brandRow.addView(bukuBrand, new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        brandRow.addView(numberBrand, new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        content.addView(brandRow, matchWrap(dp(8)));

        languageButton = actionButton(false);
        setButton(languageButton, LauncherStrings.languageButton(this),
                  view -> showLanguagePicker(), false);
        content.addView(languageButton, matchFixed(dp(44), dp(12)));

        TextView eyebrow = text("PHONE-ONLY INSTALLER", 13, Color.rgb(255, 112, 28));
        eyebrow.setGravity(Gravity.CENTER);
        content.addView(eyebrow, matchWrap(dp(4)));

        TextView title = text("SKATE 3", 30, Color.WHITE);
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
        gpuDriverButton = actionButton(false);
        secondaryButton = actionButton(false);
        tertiaryButton = actionButton(false);
        updateButton = actionButton(false);
        reportButton = actionButton(false);
        content.addView(primaryButton, matchFixed(dp(58), dp(10)));
        content.addView(characterButton, matchFixed(dp(54), dp(10)));
        content.addView(secondaryButton, matchFixed(dp(54), dp(10)));
        content.addView(tertiaryButton, matchFixed(dp(54), dp(10)));
        content.addView(updateButton, matchFixed(dp(50), dp(18)));
        setButton(updateButton, "CHECK FOR APP UPDATES", view -> checkForAppUpdate(true), false);
        content.addView(reportButton, matchFixed(dp(50), dp(18)));
        setButton(reportButton, "BUG REPORT / DEVICE DIAGNOSTICS",
                  view -> BugReporter.show(this), false);
        content.addView(gpuDriverButton, matchFixed(dp(46), dp(18)));

        TextView requirements = text(
            "Requires Android 13+, ARM64, Vulkan, and about 8 GB free after the ISO is already on your device or USB drive. Touch controls are included.",
            12, Color.rgb(125, 125, 132));
        requirements.setGravity(Gravity.CENTER);
        requirements.setLineSpacing(0, 1.1f);
        content.addView(requirements, matchWrap(dp(18)));

        setContentView(scroll);
    }

    private void showLanguagePicker() {
        String[] languages = { "🇺🇸 English", "🇧🇷 Português (Brasil)" };
        int selected = LauncherStrings.isPortuguese(this) ? 1 : 0;
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle(localize("Launcher language"))
            .setSingleChoiceItems(languages, selected, null)
            .setNegativeButton(localize("Cancel"), null)
            .create();
        dialog.setOnShowListener(ignored -> {
            dialog.getListView().setOnItemClickListener((parent, view, position, id) -> {
                LauncherStrings.setLanguage(this,
                    position == 1 ? LauncherStrings.PORTUGUESE_BRAZIL : LauncherStrings.ENGLISH);
                dialog.dismiss();
                recreate();
            });
        });
        dialog.show();
    }

    private void refreshInterface() {
        if (busy) {
            return;
        }
        setButtonsEnabled(true);
        progressBar.setVisibility(View.GONE);
        String unsupported = compatibilityProblem();
        if (unsupported != null) {
            setLocalizedText(statusText, "DEVICE NOT SUPPORTED");
            setLocalizedText(detailText, unsupported);
            setButton(primaryButton, "CLOSE", view -> finish(), true);
            hide(characterButton);
            hide(gpuDriverButton);
            hide(secondaryButton);
            hide(tertiaryButton);
            return;
        }

        boolean scopedReady = isGameReady(gameDirectory.toPath());
        boolean legacyReady = legacyGameReady();
        if (scopedReady || legacyReady) {
            File activeGame = scopedReady ? gameDirectory : new File("/storage/emulated/0/skate3");
            if (ensureBundledSeiyu(activeGame)) return;
            setLocalizedText(statusText, "READY TO SKATE");
            setLocalizedText(detailText, Build.MODEL + "\nGame files: " + activeGame.getAbsolutePath());
            setButton(primaryButton, "PLAY SKATE 3", view -> launchGame(), true);
            configureSeiyuInstallButton(activeGame);
            configureAdvancedButton();
            if (scopedReady) {
                setButton(secondaryButton, "REPAIR OR REINSTALL", view -> confirmReinstall(), false);
            } else {
                hide(secondaryButton);
            }
            setButton(tertiaryButton, "VIEW SETUP LOG", view -> showLog(), false);
            return;
        }

        if (isExtractionComplete()) {
            setLocalizedText(statusText, "GAME EXTRACTED");
            setLocalizedText(detailText, "Finish by downloading the verified 1.7 MB Title Update 3, or select the package yourself.");
            setButton(primaryButton, "FINISH SETUP AUTOMATICALLY", view -> finishSetupOnline(), true);
            hide(characterButton);
            hide(gpuDriverButton);
            setButton(secondaryButton, "SELECT TITLE UPDATE FILE", view -> pickTitleUpdate(), false);
            setButton(tertiaryButton, "START OVER", view -> confirmStartOver(), false);
            return;
        }

        setLocalizedText(statusText, "ONE FILE NEEDED");
        setLocalizedText(detailText,
            "Select your Skate 3 Xbox 360 ISO. It can be in Downloads, on an SD card, or on a connected USB drive.\n\nFree space: " +
            humanBytes(new StatFs(storageRoot.getAbsolutePath()).getAvailableBytes()));
        setButton(primaryButton, "SELECT MY SKATE 3 ISO", view -> pickIso(), true);
        hide(characterButton);
        hide(gpuDriverButton);
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
        } else if (requestCode == REQUEST_GPU_DRIVER) {
            importGpuDriver(uri);
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
                        setLocalizedText(statusText, "EXTRACTING GAME");
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
            setLocalizedText(statusText, "INSTALLING TITLE UPDATE 3");
            setLocalizedText(detailText, "Downloading and verifying 1.7 MB...");
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
        try {
            BundledMods.installSeiyu(this, gameDirectory.toPath());
            appendLog("Bundled Seiyu Paradise Penguin files installed.");
        } catch (IOException exception) {
            appendLog("Bundled Seiyu installation will retry from the launcher: " +
                      cleanMessage(exception));
        }
        runOnUiThread(() -> {
            busy = false;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            progressBar.setProgress(1000);
            setLocalizedText(statusText, "INSTALLATION COMPLETE");
            setLocalizedText(detailText, "Your files were verified and stayed on this device.");
            setButton(primaryButton, "PLAY SKATE 3", view -> launchGame(), true);
            configureSeiyuInstallButton(gameDirectory);
            configureAdvancedButton();
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
            setLocalizedText(detailText, humanBytes(copied) + " of " + humanBytes(total) +
                                             "\n" + currentFile);
        });
    }

    private void showDownloadProgress(long copied, long total, String message) {
        int progress = total > 0 ? (int) Math.min(1000, copied * 1000 / total) : 0;
        runOnUiThread(() -> {
            progressBar.setProgress(progress);
            setLocalizedText(detailText, message + "\n" + humanBytes(copied) +
                                         (total > 0 ? " of " + humanBytes(total) : ""));
        });
    }

    private void launchGame() {
        if (!isGameReady(gameDirectory.toPath()) && !legacyGameReady()) {
            Toast.makeText(this, localize("Game installation needs repair."), Toast.LENGTH_LONG).show();
            refreshInterface();
            return;
        }
        startActivity(new Intent(this, Skate3Activity.class));
        finish();
    }

    private void checkForAppUpdate(boolean manual) {
        if (checkingUpdate || busy) return;
        checkingUpdate = true;
        updateButton.setEnabled(false);
        updateButton.setText(localize("CHECKING FOR UPDATES..."));
        worker.execute(() -> {
            try {
                AppUpdater.UpdateInfo result = AppUpdater.check(this);
                runOnUiThread(() -> {
                    checkingUpdate = false;
                    availableUpdate = result;
                    refreshUpdateButton();
                    if (result != null && (!updatePromptShown || manual)) {
                        updatePromptShown = true;
                        showUpdatePrompt(result);
                    } else if (result == null && manual) {
                        Toast.makeText(this, localize("You already have the newest build."),
                                       Toast.LENGTH_SHORT).show();
                    }
                });
            } catch (Exception exception) {
                appendLog("Update check failed: " + stackTrace(exception));
                runOnUiThread(() -> {
                    checkingUpdate = false;
                    updateButton.setEnabled(true);
                    updateButton.setText(localize("CHECK FOR APP UPDATES"));
                    if (manual) {
                        Toast.makeText(this, "Could not check for updates: " +
                                       cleanMessage(exception), Toast.LENGTH_LONG).show();
                    }
                });
            }
        });
    }

    private void refreshUpdateButton() {
        if (availableUpdate == null) {
            setButton(updateButton, "APP UP TO DATE", view -> checkForAppUpdate(true), false);
        } else {
            setButton(updateButton, "UPDATE TO " + availableUpdate.versionName,
                      view -> showUpdatePrompt(availableUpdate), false);
        }
    }

    private void showUpdatePrompt(AppUpdater.UpdateInfo info) {
        new AlertDialog.Builder(this)
            .setTitle("Skate 3 " + info.versionName)
            .setMessage(info.notes + "\n\n" + localize("The app will verify the download, then Android will ask you to tap Install. Your game files and settings stay in place."))
            .setNegativeButton(localize("Later"), null)
            .setPositiveButton(localize("Download update"), (dialog, which) -> downloadAppUpdate(info))
            .show();
    }

    private void downloadAppUpdate(AppUpdater.UpdateInfo info) {
        if (busy) return;
        setBusy("DOWNLOADING APP UPDATE", "Starting " + info.versionName + "...", true);
        worker.execute(() -> {
            try {
                File apk = AppUpdater.downloadApk(this, info, (copied, total) ->
                    showDownloadProgress(copied, total, "Downloading Skate 3 " +
                                         info.versionName));
                pendingUpdateApk = apk;
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setProgress(1000);
                    setLocalizedText(statusText, "UPDATE VERIFIED");
                    setLocalizedText(detailText, "Android will now open the installer. Your game files stay in place.");
                    setButtonsEnabled(true);
                    boolean opened = AppUpdater.install(this, apk);
                    awaitingInstallPermission = !opened;
                    if (opened) pendingUpdateApk = null;
                    else Toast.makeText(this,
                        localize("Allow installs from Skate 3, then return here."),
                        Toast.LENGTH_LONG).show();
                });
            } catch (Exception exception) {
                appendLog("App update failed: " + stackTrace(exception));
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setVisibility(View.GONE);
                    new AlertDialog.Builder(this)
                        .setTitle("Update needs attention")
                        .setMessage("The app update was not installed: " + cleanMessage(exception))
                        .setPositiveButton("OK", (dialog, which) -> refreshInterface())
                        .show();
                });
            }
        });
    }

    private void configureSeiyuInstallButton(File activeGame) {
        boolean ready = BundledMods.isSeiyuReady(activeGame.toPath());
        String label = ready
            ? "CHARACTERS  •  SEIYU INCLUDED"
            : "CHARACTERS  •  RESTORE SEIYU";
        setButton(characterButton, label, view -> showSeiyuIncluded(activeGame), false);
    }

    private boolean ensureBundledSeiyu(File activeGame) {
        if (BundledMods.isSeiyuReady(activeGame.toPath()) || bundledSeiyuAttempted) {
            return false;
        }
        bundledSeiyuAttempted = true;
        setBusy("PREPARING SEIYU", "Installing the included playable penguin...", true);
        worker.execute(() -> {
            try {
                BundledMods.installSeiyu(this, activeGame.toPath());
                appendLog("Bundled Seiyu Paradise Penguin files installed.");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setProgress(1000);
                    Toast.makeText(this,
                        "Seiyu is included and ready in RB + Start > Mods.",
                        Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                appendLog("Could not install bundled Seiyu: " + stackTrace(exception));
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setVisibility(View.GONE);
                    new AlertDialog.Builder(this)
                        .setTitle("Seiyu needs attention")
                        .setMessage("The included Seiyu files could not be installed: " +
                                    cleanMessage(exception) +
                                    "\n\nSkate 3 can still use the original skater. You can retry from Characters.")
                        .setPositiveButton("OK", (dialog, which) -> refreshInterface())
                        .show();
                });
            }
        });
        return true;
    }

    private void showSeiyuIncluded(File activeGame) {
        boolean ready = BundledMods.isSeiyuReady(activeGame.toPath());
        AlertDialog.Builder dialog = new AlertDialog.Builder(this)
            .setTitle("Seiyu Paradise Penguin")
            .setMessage(ready
                ? "Seiyu comes with Skate 3 Mobile and is ready to skate. In game, press RB + Start and open Mods > Playable Character to switch between Seiyu and the original skater."
                : "Seiyu comes with Skate 3 Mobile, but his local files need to be restored. The original skater is still available.")
            .setNegativeButton("Close", null)
            .setPositiveButton("Open Mod Store", (ignored, which) -> openModStore())
            .setNeutralButton("Restore Seiyu", (ignored, which) -> restoreBundledSeiyu());
        dialog.show();
    }

    private void restoreBundledSeiyu() {
        if (busy) return;
        setBusy("RESTORING SEIYU", "Verifying the character files included in the APK...", true);
        worker.execute(() -> {
            try {
                BundledMods.installSeiyu(this, activeGameDirectory().toPath());
                appendLog("Bundled Seiyu Paradise Penguin files restored.");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setProgress(1000);
                    Toast.makeText(this, "Seiyu restored and ready.", Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                showModStoreFailure("Could not restore Seiyu: " +
                                    cleanMessage(exception), exception);
            }
        });
    }

    private void configureAdvancedButton() {
        CustomGpuDriver.Driver driver = CustomGpuDriver.installed(this);
        String label = driver != null && driver.enabled
            ? "ADVANCED OPTIONS  •  CUSTOM GPU DRIVER ACTIVE"
            : "ADVANCED OPTIONS";
        setButton(gpuDriverButton, label, view -> showAdvancedOptions(), false);
    }

    private void showAdvancedOptions() {
        CustomGpuDriver.Driver driver = CustomGpuDriver.installed(this);
        boolean customActive = driver != null && driver.enabled;
        String message = customActive
            ? "A custom GPU driver is active. System Driver is the normal and recommended setting. Change this only while troubleshooting a device-specific graphics problem."
            : "Skate 3 is using the device System Driver. This is the recommended setting. If the game already works, leave these options unchanged.";
        AlertDialog.Builder dialog = new AlertDialog.Builder(this)
            .setTitle("Advanced options")
            .setMessage(message)
            .setNegativeButton("Close", null);
        if (CustomGpuDriver.isLikelyAdrenoDevice()) {
            dialog.setPositiveButton("GPU driver experiments",
                                     (ignored, which) -> showGpuDriverMenu());
            if (customActive) {
                // Keep recovery available from the first dialog even if a
                // vendor theme or a broken custom driver makes the detailed
                // choices difficult to use.
                dialog.setNeutralButton("Use System Driver", (ignored, which) -> {
                    CustomGpuDriver.useSystem(this);
                    Toast.makeText(this, "System Driver selected for the next launch.",
                                   Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            }
        } else {
            dialog.setPositiveButton("OK", null);
        }
        dialog.show();
    }

    private void showGpuDriverMenu() {
        if (!CustomGpuDriver.isLikelyAdrenoDevice()) {
            new AlertDialog.Builder(this)
                .setTitle("Experimental GPU driver")
                .setMessage("This device does not appear to use a Snapdragon / Adreno GPU. Turnip is not compatible here, so Skate 3 will stay on the System Driver.")
                .setPositiveButton("OK", null)
                .show();
            return;
        }

        CustomGpuDriver.Driver driver = CustomGpuDriver.installed(this);
        String selected = driver != null && driver.enabled
            ? driver.label() : "System Driver";

        // Do not combine AlertDialog.setMessage and setItems here. Samsung,
        // Retroid and AYN vendor themes have all shipped dialog layouts that
        // measure the message at full height and leave the item list invisible.
        // A real custom panel gives every action its own visible, focusable
        // button and also works with controller/keyboard navigation.
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(22), dp(8), dp(22), dp(6));

        String details = "Selected: " + selected +
            "\n\nSystem Driver is recommended. Custom drivers are optional and only intended for troubleshooting Snapdragon / Adreno devices.";
        if (driver != null) {
            details += "\n\nImported: " + driver.label() +
                "\nVendor: " + driver.vendor + "\nAuthor: " + driver.author;
        }
        TextView description = text(details, 14, Color.rgb(205, 205, 210));
        description.setLineSpacing(0, 1.12f);
        panel.addView(description, matchWrap(dp(16)));

        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("Experimental GPU driver")
            .setView(panel)
            .setNegativeButton("Close", null)
            .create();

        Button system = actionButton(false);
        setButton(system, "USE SYSTEM DRIVER" +
                  (driver == null || !driver.enabled ? "  •  SELECTED" : ""), view -> {
            CustomGpuDriver.useSystem(this);
            Toast.makeText(this, "System Driver selected for the next launch.",
                           Toast.LENGTH_LONG).show();
            dialog.dismiss();
            refreshInterface();
        }, false);
        panel.addView(system, matchFixed(dp(52), dp(8)));

        if (driver != null) {
            Button custom = actionButton(false);
            setButton(custom, "USE IMPORTED DRIVER" +
                      (driver.enabled ? "  •  SELECTED" : ""), view -> {
                dialog.dismiss();
                confirmCustomGpuDriver(driver);
            }, false);
            panel.addView(custom, matchFixed(dp(52), dp(8)));
        }

        Button importDriver = actionButton(false);
        setButton(importDriver,
                  driver == null ? "IMPORT ZIP / ADPKG" : "IMPORT ANOTHER ZIP / ADPKG",
                  view -> {
                      dialog.dismiss();
                      pickGpuDriver();
                  }, false);
        panel.addView(importDriver, matchFixed(dp(52), dp(8)));

        if (driver != null) {
            Button remove = actionButton(false);
            setButton(remove, "REMOVE IMPORTED DRIVER", view -> {
                dialog.dismiss();
                confirmRemoveGpuDriver(driver);
            }, false);
            panel.addView(remove, matchFixed(dp(52), 0));
        }

        dialog.show();
    }

    private void confirmCustomGpuDriver(CustomGpuDriver.Driver driver) {
        new AlertDialog.Builder(this)
            .setTitle("Enable experimental driver?")
            .setMessage("Do not enable this if Skate 3 already works. " + driver.label() +
                        " will replace the working System Driver only inside Skate 3. It may crash, black-screen, or render incorrectly. You can return here and restore System Driver.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Enable anyway", (dialog, which) -> {
                try {
                    CustomGpuDriver.useCustom(this);
                    Toast.makeText(this, "Custom driver selected for the next launch.",
                                   Toast.LENGTH_LONG).show();
                    refreshInterface();
                } catch (IOException exception) {
                    showFailure("Could not select the custom GPU driver: " +
                                cleanMessage(exception), exception);
                }
            })
            .show();
    }

    private void pickGpuDriver() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // Turnip packages use both .zip and .adpkg. Several Android file
        // pickers report ADPKG as application/octet-stream (or no useful MIME
        // type at all), so a zip-only filter hides perfectly valid packages.
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/zip", "application/x-zip-compressed",
            "application/octet-stream"
        });
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_GPU_DRIVER);
        } catch (ActivityNotFoundException exception) {
            try {
                startActivityForResult(intent, REQUEST_GPU_DRIVER);
            } catch (ActivityNotFoundException second) {
                showFailure("Android could not open its file picker.", second);
            }
        }
    }

    private void importGpuDriver(Uri uri) {
        setBusy("IMPORTING GPU DRIVER", "Checking " + displayName(uri) + "...", true);
        worker.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("Android could not open the selected ZIP.");
                CustomGpuDriver.Driver driver = CustomGpuDriver.importPackage(this, input);
                appendLog("Imported custom GPU driver " + driver.label() + ".");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setVisibility(View.GONE);
                    Toast.makeText(this, driver.label() +
                        " imported but not enabled. System Driver remains selected.",
                        Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                showFailure("Could not import the GPU driver: " + cleanMessage(exception),
                            exception);
            }
        });
    }

    private void confirmRemoveGpuDriver(CustomGpuDriver.Driver driver) {
        new AlertDialog.Builder(this)
            .setTitle("Remove custom driver?")
            .setMessage("This removes " + driver.label() +
                        " from Skate 3 and selects the System Driver.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Remove", (dialog, which) -> {
                try {
                    CustomGpuDriver.remove(this);
                    Toast.makeText(this, "Custom driver removed.", Toast.LENGTH_SHORT).show();
                    refreshInterface();
                } catch (IOException exception) {
                    showFailure("Could not remove the custom GPU driver: " +
                                cleanMessage(exception), exception);
                }
            })
            .show();
    }

    private File activeGameDirectory() {
        if (isGameReady(gameDirectory.toPath())) {
            return gameDirectory;
        }
        return new File("/storage/emulated/0/skate3");
    }

    private void openModStore() {
        if (busy) return;
        setBusy("OPENING MOD STORE", "Loading the latest available mods...", true);
        worker.execute(() -> {
            try {
                List<ModStore.Mod> mods = ModStore.loadCatalog();
                Path gameRoot = activeGameDirectory().toPath();
                boolean[] installed = new boolean[mods.size()];
                for (int index = 0; index < mods.size(); ++index) {
                    installed[index] = ModStore.isInstalled(gameRoot, mods.get(index));
                }
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setVisibility(View.GONE);
                    refreshInterface();
                    showModStoreList(mods, installed);
                });
            } catch (Exception exception) {
                appendLog("Mod Store catalog failed: " + stackTrace(exception));
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setVisibility(View.GONE);
                    refreshInterface();
                    new AlertDialog.Builder(this)
                        .setTitle("Mod Store unavailable")
                        .setMessage("The online catalog could not be loaded: " +
                                    cleanMessage(exception) +
                                    "\n\nSeiyu is included with the app and remains available offline.")
                        .setPositiveButton("Close", null)
                        .show();
                });
            }
        });
    }

    private void showModStoreList(List<ModStore.Mod> mods, boolean[] installed) {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(15, 15, 18));
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(18), dp(18), dp(18), dp(6));
        scroll.addView(list, new ScrollView.LayoutParams(
            ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));

        TextView intro = text(
            "Seiyu is included with the app. Community character mods use size-limited, SHA-256 verified downloads before installation.",
            14, Color.rgb(190, 190, 198));
        intro.setLineSpacing(0, 1.12f);
        list.addView(intro, matchWrap(dp(16)));

        for (int index = 0; index < mods.size(); ++index) {
            ModStore.Mod mod = mods.get(index);
            boolean modInstalled = installed[index];
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(dp(18), dp(15), dp(18), dp(14));
            card.setBackgroundColor(Color.rgb(31, 31, 37));

            TextView name = text(mod.name, 20, Color.WHITE);
            name.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            card.addView(name, matchWrap(dp(5)));

            TextView meta = text(
                (modInstalled ? "INSTALLED" : "AVAILABLE") + "  •  v" + mod.version +
                "  •  " + humanBytes(mod.downloadSize()),
                12, modInstalled ? Color.rgb(65, 215, 255) : Color.rgb(255, 112, 28));
            meta.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            card.addView(meta, matchWrap(dp(8)));

            TextView description = text(mod.description, 14, Color.rgb(180, 180, 188));
            description.setLineSpacing(0, 1.12f);
            card.addView(description, matchWrap(dp(12)));

            Button open = actionButton(!modInstalled);
            setButton(open, modInstalled ? "MANAGE MOD" : "VIEW AND INSTALL",
                      view -> {
                          dismissModStoreList();
                          showModStoreDetails(mod, modInstalled);
                      }, !modInstalled);
            card.addView(open, matchFixed(dp(48), 0));
            list.addView(card, matchWrap(dp(12)));
        }
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("MOD STORE")
            .setView(scroll)
            .setNegativeButton("Close", null)
            .create();
        modStoreListDialog = dialog;
        dialog.setOnDismissListener(ignored -> {
            if (modStoreListDialog == dialog) modStoreListDialog = null;
        });
        dialog.show();
    }

    private void dismissModStoreList() {
        if (modStoreListDialog != null) {
            AlertDialog dialog = modStoreListDialog;
            modStoreListDialog = null;
            dialog.dismiss();
        }
    }

    private void showModStoreDetails(ModStore.Mod mod, boolean installed) {
        boolean bundledSeiyu = mod.id.equals("seiyu-paradise-penguin");
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(22), dp(18), dp(22), dp(12));
        panel.setBackgroundColor(Color.rgb(15, 15, 18));

        TextView description = text(mod.description, 15, Color.rgb(210, 210, 216));
        description.setLineSpacing(0, 1.15f);
        panel.addView(description, matchWrap(dp(18)));

        TextView details = text(
            "Version " + mod.version + "\nDownload: " + humanBytes(mod.downloadSize()) +
            "\nStatus: " + (bundledSeiyu ? "Included with Skate 3 Mobile" :
                              (installed ? "Installed" : "Not installed")) +
            "\n\nAfter installation, choose the character in RB + Start > Mods.",
            13, Color.rgb(150, 150, 160));
        details.setLineSpacing(0, 1.16f);
        panel.addView(details, matchWrap(0));

        AlertDialog.Builder dialog = new AlertDialog.Builder(this)
            .setTitle(mod.name)
            .setView(panel)
            .setNegativeButton("Close", null);
        if (bundledSeiyu) {
            dialog.setPositiveButton("Restore bundled copy",
                                     (ignored, which) -> restoreBundledSeiyu());
        } else {
            dialog.setPositiveButton(installed ? "Reinstall" : "Install",
                                     (ignored, which) -> installStoreMod(mod));
        }
        if (installed && !bundledSeiyu) {
            dialog.setNeutralButton("Remove", (ignored, which) -> confirmRemoveStoreMod(mod));
        }
        dialog.show();
    }

    private void installStoreMod(ModStore.Mod mod) {
        if (busy) return;
        setBusy("INSTALLING " + mod.name.toUpperCase(Locale.US),
                "Starting verified download...", true);
        worker.execute(() -> {
            try {
                ModStore.install(activeGameDirectory().toPath(), mod,
                    (copied, total, message) -> showDownloadProgress(copied, total, message));
                appendLog("Mod Store installed " + mod.name + " " + mod.version + ".");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    progressBar.setProgress(1000);
                    Toast.makeText(this, mod.name +
                        " installed. Select it in RB + Start > Mods.",
                        Toast.LENGTH_LONG).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                showModStoreFailure("Could not install " + mod.name + ": " +
                                    cleanMessage(exception), exception);
            }
        });
    }

    private void confirmRemoveStoreMod(ModStore.Mod mod) {
        new AlertDialog.Builder(this)
            .setTitle("Remove " + mod.name + "?")
            .setMessage("This removes only the downloaded mod files. Your game installation and original skater are not changed.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Remove", (dialog, which) -> removeStoreMod(mod))
            .show();
    }

    private void removeStoreMod(ModStore.Mod mod) {
        if (busy) return;
        setBusy("REMOVING MOD", "Removing " + mod.name + "...", false);
        worker.execute(() -> {
            try {
                ModStore.uninstall(activeGameDirectory().toPath(), mod);
                appendLog("Mod Store removed " + mod.name + ".");
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    Toast.makeText(this, mod.name + " removed.", Toast.LENGTH_SHORT).show();
                    refreshInterface();
                });
            } catch (Exception exception) {
                showModStoreFailure("Could not remove " + mod.name + ": " +
                                    cleanMessage(exception), exception);
            }
        });
    }

    private void showModStoreFailure(String message, Exception exception) {
        appendLog(message + "\n" + stackTrace(exception));
        runOnUiThread(() -> {
            busy = false;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            progressBar.setVisibility(View.GONE);
            new AlertDialog.Builder(this)
                .setTitle("Mod Store needs attention")
                .setMessage(message + "\n\nYour existing game and mod files were not changed.")
                .setPositiveButton("OK", (dialog, which) -> refreshInterface())
                .show();
        });
    }

    private void explainSeiyuInstall() {
        new AlertDialog.Builder(this)
            .setTitle("Restore Seiyu Paradise Penguin")
            .setMessage("Seiyu is bundled with the app and the original skater remains the default. Manual import is available only for development or restoring custom files. In game, press RB + Start and open Mods to switch characters.")
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
            .setTitle(localize("Repair or reinstall?"))
            .setMessage(localize("This removes the extracted game files from this app, then lets you select your ISO again. Your original ISO is not changed."))
            .setNegativeButton(localize("Cancel"), null)
            .setPositiveButton(localize("Continue"), (dialog, which) -> startOver())
            .show();
    }

    private void confirmStartOver() {
        new AlertDialog.Builder(this)
            .setTitle(localize("Discard partial setup?"))
            .setMessage(localize("Only the incomplete copy created by this installer will be removed. Your original ISO is not changed."))
            .setNegativeButton(localize("Cancel"), null)
            .setPositiveButton(localize("Start over"), (dialog, which) -> startOver())
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
        String contents = localize("No setup log has been written yet.");
        try {
            if (setupLog.isFile()) {
                contents = new String(Files.readAllBytes(setupLog.toPath()), StandardCharsets.UTF_8);
            }
        } catch (IOException exception) {
            contents = cleanMessage(exception);
        }
        new AlertDialog.Builder(this)
            .setTitle(localize("Setup log"))
            .setMessage(contents)
            .setPositiveButton("OK", null)
            .show();
    }

    private void setBusy(String status, String detail, boolean progressVisible) {
        busy = true;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setLocalizedText(statusText, status);
        setLocalizedText(detailText, detail);
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
                .setTitle(localize("Setup needs attention"))
                .setMessage(localize(message) + "\n\n" + localize("Your original ISO was not changed."))
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
        return localize("selected file");
    }

    private void setButtonsEnabled(boolean enabled) {
        languageButton.setEnabled(enabled);
        primaryButton.setEnabled(enabled);
        characterButton.setEnabled(enabled);
        gpuDriverButton.setEnabled(enabled);
        secondaryButton.setEnabled(enabled);
        tertiaryButton.setEnabled(enabled);
        updateButton.setEnabled(enabled && !checkingUpdate);
        reportButton.setEnabled(enabled);
    }

    private void setButton(Button button, String label, View.OnClickListener listener,
                           boolean primary) {
        button.setText(localize(label));
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
        text.setText(localize(value));
        text.setTextSize(size);
        text.setTextColor(color);
        return text;
    }

    private TextView githubBrand(String label, int gravity) {
        TextView brand = text(label, 16, Color.rgb(255, 104, 24));
        brand.setGravity(gravity);
        brand.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        brand.setPadding(0, dp(8), 0, dp(8));
        brand.setClickable(true);
        brand.setFocusable(true);
        brand.setContentDescription(label + ", " + localize("Open Buku313 on GitHub"));
        brand.setOnClickListener(view -> openBukuGitHub());
        return brand;
    }

    private void openBukuGitHub() {
        try {
            Intent browser = new Intent(Intent.ACTION_VIEW, Uri.parse(BUKU_GITHUB));
            browser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(Intent.createChooser(browser, "Open Buku313 on GitHub"));
        } catch (ActivityNotFoundException exception) {
            Toast.makeText(this, BUKU_GITHUB, Toast.LENGTH_LONG).show();
        }
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

    private String localize(String english) {
        return LauncherStrings.text(this, english);
    }

    private void setLocalizedText(TextView view, String english) {
        view.setText(localize(english));
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
