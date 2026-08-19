package chat.buku.skate3;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.system.Os;
import android.system.OsConstants;
import android.view.InputDevice;
import android.widget.Toast;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

final class BugReporter {
    private static final String ISSUE_URL =
        "https://github.com/Buku313/Skate3-Mobile/issues/new";

    private BugReporter() {}

    static void show(Activity activity) {
        Diagnostic diagnostic = collect(activity);
        new AlertDialog.Builder(activity)
            .setTitle("Report a developer-build bug")
            .setMessage("GitHub will open with this device's safe technical details already filled in. The same details will be copied so you can paste them if the browser removes a field.\n\nNo ISO, game file, save, account name, or private path is included.")
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Copy only", (dialog, which) -> copy(activity, diagnostic.report))
            .setPositiveButton("Open GitHub", (dialog, which) -> {
                copy(activity, diagnostic.report);
                open(activity, diagnostic);
            })
            .show();
    }

    private static Diagnostic collect(Context context) {
        String version = "unknown";
        try {
            PackageInfo info = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0);
            version = "v" + info.versionName + " DEV (" + info.getLongVersionCode() + ")";
        } catch (Exception ignored) {
        }

        String device = clean(Build.MANUFACTURER + " " + Build.MODEL);
        String android = "Android " + Build.VERSION.RELEASE + " (API " +
                         Build.VERSION.SDK_INT + ")";
        String soc = clean(Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL);
        if (soc.isEmpty()) soc = clean(Build.HARDWARE);
        String profile = graphicsProfile(context);
        String input = inputMethod();
        String exits = recentExits(context);
        long pageSize = Os.sysconf(OsConstants._SC_PAGESIZE);
        String vulkan = vulkanVersion(context);
        long availableMb = Runtime.getRuntime().maxMemory() / (1024 * 1024);

        String report =
            "Skate 3 Mobile diagnostics\n" +
            "App: " + version + "\n" +
            "Package: " + context.getPackageName() + "\n" +
            "Device: " + device + "\n" +
            "Android: " + android + "\n" +
            "SoC: " + soc + "\n" +
            "Hardware: " + clean(Build.HARDWARE) + "\n" +
            "ABI: " + String.join(", ", Build.SUPPORTED_ABIS) + "\n" +
            "Memory page: " + pageSize + " bytes\n" +
            "Vulkan feature: " + vulkan + "\n" +
            "Java heap limit: " + availableMb + " MiB\n" +
            "Graphics profile: " + profile + "\n" +
            "Input: " + input + "\n" +
            "Recent process exits:\n" + exits;
        return new Diagnostic(version, device, android, soc, profile, input, report);
    }

    private static String graphicsProfile(Context context) {
        File settings = new File(context.getFilesDir(), "settings.toml");
        try {
            if (settings.isFile() && settings.length() <= 1024 * 1024) {
                String text = new String(Files.readAllBytes(settings.toPath()),
                                         StandardCharsets.UTF_8);
                for (String line : text.split("\\R")) {
                    if (!line.contains("skate3_android_quality_profile")) continue;
                    int equals = line.indexOf('=');
                    if (equals >= 0) {
                        String value = line.substring(equals + 1).trim();
                        if (value.startsWith("1")) return "High-End / Quality";
                        if (value.startsWith("0")) return "RG406V / Performance";
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return "I do not know";
    }

    private static String inputMethod() {
        boolean controller = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || id < 0) continue;
            int sources = device.getSources();
            controller |= (sources & (InputDevice.SOURCE_GAMEPAD |
                                      InputDevice.SOURCE_JOYSTICK |
                                      InputDevice.SOURCE_DPAD)) != 0;
        }
        if (!controller) return "Touch controls";
        if (Build.MODEL.toUpperCase(Locale.US).contains("RG406")) {
            return "Built-in handheld controls";
        }
        return "Multiple input methods";
    }

    private static String vulkanVersion(Context context) {
        for (FeatureInfo feature : context.getPackageManager().getSystemAvailableFeatures()) {
            if (!"android.hardware.vulkan.version".equals(feature.name)) continue;
            int version = feature.version;
            return ((version >> 22) & 0x3ff) + "." +
                   ((version >> 12) & 0x3ff) + "." + (version & 0xfff);
        }
        return "not reported";
    }

    private static String recentExits(Context context) {
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(
                context.getPackageName(), 0, 3);
            if (exits.isEmpty()) return "- none recorded";
            SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss 'UTC'", Locale.US);
            format.setTimeZone(TimeZone.getTimeZone("UTC"));
            StringBuilder text = new StringBuilder();
            for (ApplicationExitInfo exit : exits) {
                text.append("- ").append(format.format(new Date(exit.getTimestamp())))
                    .append(": ").append(reasonName(exit.getReason()))
                    .append(", status=").append(exit.getStatus())
                    .append(", pss=").append(exit.getPss()).append(" KiB")
                    .append(", rss=").append(exit.getRss()).append(" KiB");
                text.append('\n');
            }
            return text.toString().trim();
        } catch (Exception exception) {
            return "- unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "normal self-exit";
            case ApplicationExitInfo.REASON_SIGNALED: return "signal";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low memory";
            case ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "initialization failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resources";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "user requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency died";
            case ApplicationExitInfo.REASON_OTHER: return "other";
            case ApplicationExitInfo.REASON_FREEZER: return "app freezer";
            case ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "package state change";
            case ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "package updated";
            default: return "unknown (" + reason + ")";
        }
    }

    private static void copy(Context context, String report) {
        ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
        clipboard.setPrimaryClip(ClipData.newPlainText("Skate 3 Mobile diagnostics", report));
        Toast.makeText(context, "Device diagnostics copied.", Toast.LENGTH_SHORT).show();
    }

    private static void open(Activity activity, Diagnostic diagnostic) {
        Uri uri = Uri.parse(ISSUE_URL).buildUpon()
            .appendQueryParameter("template", "bug_report.yml")
            .appendQueryParameter("title", "[BUG] " + diagnostic.device + " crash")
            .appendQueryParameter("version", diagnostic.version)
            .appendQueryParameter("device", diagnostic.device)
            .appendQueryParameter("android", diagnostic.android)
            .appendQueryParameter("chipset", diagnostic.soc)
            .appendQueryParameter("profile", diagnostic.profile)
            .appendQueryParameter("input", diagnostic.input)
            .appendQueryParameter("evidence", diagnostic.report)
            .build();
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, uri));
        } catch (ActivityNotFoundException exception) {
            Toast.makeText(activity, "No browser is available. The diagnostics are copied.",
                           Toast.LENGTH_LONG).show();
        }
    }

    private static String clean(String value) {
        if (value == null) return "";
        return value.replace('\n', ' ').replace('\r', ' ').trim();
    }

    private static final class Diagnostic {
        final String version;
        final String device;
        final String android;
        final String soc;
        final String profile;
        final String input;
        final String report;

        Diagnostic(String version, String device, String android, String soc,
                   String profile, String input, String report) {
            this.version = version;
            this.device = device;
            this.android = android;
            this.soc = soc;
            this.profile = profile;
            this.input = input;
            this.report = report;
        }
    }
}
