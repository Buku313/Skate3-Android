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

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.zip.GZIPInputStream;

final class BugReporter {
    private static final String ISSUE_URL =
        "https://github.com/Buku313/Skate3-Mobile/issues/new";

    private BugReporter() {}

    static void show(Activity activity) {
        Diagnostic diagnostic = collect(activity);
        new AlertDialog.Builder(activity)
            .setTitle(LauncherStrings.text(activity, "Report a developer-build bug"))
            .setMessage(LauncherStrings.text(activity, "GitHub will open with this device's safe technical details already filled in. The same details will be copied so you can paste them if the browser removes a field.\n\nNo ISO, game file, save, account name, or private path is included."))
            .setNegativeButton(LauncherStrings.text(activity, "Cancel"), null)
            .setNeutralButton(LauncherStrings.text(activity, "Copy only"), (dialog, which) -> copy(activity, diagnostic.report))
            .setPositiveButton(LauncherStrings.text(activity, "Open GitHub"), (dialog, which) -> {
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
        String gpuDriver = CustomGpuDriver.diagnostic(context);
        String input = inputMethod();
        String exits = recentExits(context);
        String runtime = runtimeEvidence(context);
        long pageSize = Os.sysconf(OsConstants._SC_PAGESIZE);
        String vulkan = vulkanVersion(context);
        long availableMb = Runtime.getRuntime().maxMemory() / (1024 * 1024);

        String report =
            "Skate 3 diagnostics\n" +
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
            "GPU driver: " + gpuDriver + "\n" +
            "Input: " + input + "\n" +
            "Recent process exits:\n" + exits + "\n\n" +
            "Native renderer evidence:\n" + runtime;
        String webReport = report.length() <= 5000 ? report :
            report.substring(0, 5000) +
            "\n[Report shortened for the browser. Paste the copied full report in a comment.]";
        return new Diagnostic(version, device, android, soc, profile, input,
                              report, webReport);
    }

    private static String graphicsProfile(Context context) {
        File[] candidates = {
            new File(new File(context.getFilesDir(), "skate3"), "settings.toml"),
            new File(context.getFilesDir(), "settings.toml")
        };
        for (File settings : candidates) {
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
        }
        return "Not explicitly saved (app default: Performance)";
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
            boolean traceIncluded = false;
            for (ApplicationExitInfo exit : exits) {
                text.append("- ").append(format.format(new Date(exit.getTimestamp())))
                    .append(": ").append(reasonName(exit.getReason()))
                    .append(", status=").append(exit.getStatus())
                    .append(", pss=").append(exit.getPss()).append(" KiB")
                    .append(", rss=").append(exit.getRss()).append(" KiB");
                text.append('\n');
                if (!traceIncluded &&
                    exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    String trace = nativeTrace(exit);
                    if (!trace.isEmpty()) {
                        text.append("  Native tombstone:\n");
                        for (String line : trace.split("\\R")) {
                            text.append("  ").append(line).append('\n');
                        }
                        traceIncluded = true;
                    }
                }
            }
            return text.toString().trim();
        } catch (Exception exception) {
            return "- unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    private static String nativeTrace(ApplicationExitInfo exit) {
        try (InputStream raw = exit.getTraceInputStream()) {
            if (raw == null) return "";
            byte[] encoded = readLimited(raw, 4 * 1024 * 1024);
            if (encoded.length >= 2 && (encoded[0] & 0xff) == 0x1f &&
                (encoded[1] & 0xff) == 0x8b) {
                try (GZIPInputStream gzip = new GZIPInputStream(
                         new ByteArrayInputStream(encoded))) {
                    encoded = readLimited(gzip, 4 * 1024 * 1024);
                }
            }
            return parseTombstone(encoded);
        } catch (Exception exception) {
            return "trace unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    // Android stores native exit traces as debuggerd Tombstone protobufs.
    // This small wire reader extracts only the crash fields we need, avoiding
    // a large protobuf runtime dependency in the public APK.
    private static String parseTombstone(byte[] data) {
        ProtoReader root = new ProtoReader(data);
        long crashingTid = -1;
        String signal = "";
        String abort = "";
        List<String> causes = new ArrayList<>();
        List<ThreadTrace> threads = new ArrayList<>();
        while (root.hasRemaining()) {
            int tag = root.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 6 && wire == 0) {
                crashingTid = root.readVarint();
            } else if (field == 10 && wire == 2) {
                signal = parseSignal(root.readBytes());
            } else if (field == 14 && wire == 2) {
                abort = clean(root.readString());
            } else if (field == 15 && wire == 2) {
                String cause = parseCause(root.readBytes());
                if (!cause.isEmpty()) causes.add(cause);
            } else if (field == 16 && wire == 2) {
                ThreadTrace thread = parseThreadEntry(root.readBytes());
                if (thread != null) threads.add(thread);
            } else {
                root.skip(wire);
            }
        }
        StringBuilder out = new StringBuilder();
        if (!signal.isEmpty()) out.append("Signal: ").append(signal).append('\n');
        if (!abort.isEmpty()) out.append("Abort: ").append(abort).append('\n');
        for (String cause : causes) out.append("Cause: ").append(cause).append('\n');
        ThreadTrace crashing = null;
        for (ThreadTrace thread : threads) {
            if (thread.tid == crashingTid) {
                crashing = thread;
                break;
            }
        }
        if (crashing == null && !threads.isEmpty()) crashing = threads.get(0);
        if (crashing != null) {
            out.append("Thread: ").append(crashing.tid);
            if (!crashing.name.isEmpty()) out.append(" (").append(crashing.name).append(')');
            out.append('\n');
            for (String frame : crashing.frames) out.append(frame).append('\n');
            for (String note : crashing.notes) out.append("Note: ").append(note).append('\n');
        }
        return out.toString().trim();
    }

    private static String parseSignal(byte[] data) {
        ProtoReader reader = new ProtoReader(data);
        long number = -1;
        String name = "";
        String code = "";
        long address = -1;
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) number = reader.readVarint();
            else if (field == 2 && wire == 2) name = clean(reader.readString());
            else if (field == 4 && wire == 2) code = clean(reader.readString());
            else if (field == 9 && wire == 0) address = reader.readVarint();
            else reader.skip(wire);
        }
        StringBuilder out = new StringBuilder();
        if (!name.isEmpty()) out.append(name);
        else if (number >= 0) out.append(number);
        if (!code.isEmpty()) out.append(" / ").append(code);
        if (address >= 0) out.append(" at 0x").append(Long.toHexString(address));
        return out.toString();
    }

    private static String parseCause(byte[] data) {
        ProtoReader reader = new ProtoReader(data);
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 2) return clean(reader.readString());
            reader.skip(wire);
        }
        return "";
    }

    private static ThreadTrace parseThreadEntry(byte[] data) {
        ProtoReader entry = new ProtoReader(data);
        long key = -1;
        byte[] value = null;
        while (entry.hasRemaining()) {
            int tag = entry.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) key = entry.readVarint();
            else if (field == 2 && wire == 2) value = entry.readBytes();
            else entry.skip(wire);
        }
        if (value == null) return null;
        ThreadTrace thread = parseThread(value);
        if (thread.tid < 0) thread.tid = key;
        return thread;
    }

    private static ThreadTrace parseThread(byte[] data) {
        ThreadTrace thread = new ThreadTrace();
        ProtoReader reader = new ProtoReader(data);
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) thread.tid = reader.readVarint();
            else if (field == 2 && wire == 2) thread.name = clean(reader.readString());
            else if (field == 4 && wire == 2 && thread.frames.size() < 16) {
                String frame = parseFrame(reader.readBytes(), thread.frames.size());
                if (!frame.isEmpty()) thread.frames.add(frame);
            } else if (field == 7 && wire == 2 && thread.notes.size() < 4) {
                thread.notes.add(clean(reader.readString()));
            } else reader.skip(wire);
        }
        return thread;
    }

    private static String parseFrame(byte[] data, int index) {
        ProtoReader reader = new ProtoReader(data);
        long relativePc = -1;
        long functionOffset = -1;
        String function = "";
        String file = "";
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) relativePc = reader.readVarint();
            else if (field == 4 && wire == 2) function = clean(reader.readString());
            else if (field == 5 && wire == 0) functionOffset = reader.readVarint();
            else if (field == 6 && wire == 2) file = new File(reader.readString()).getName();
            else reader.skip(wire);
        }
        if (file.isEmpty() && function.isEmpty() && relativePc < 0) return "";
        StringBuilder out = new StringBuilder(String.format(Locale.US, "#%02d ", index));
        out.append(file.isEmpty() ? "<unknown>" : file);
        if (relativePc >= 0) out.append("+0x").append(Long.toHexString(relativePc));
        if (!function.isEmpty()) out.append(" ").append(function);
        if (functionOffset > 0) out.append("+0x").append(Long.toHexString(functionOffset));
        return out.toString();
    }

    private static byte[] readLimited(InputStream input, int limit) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        while (output.size() < limit) {
            int read = input.read(buffer, 0, Math.min(buffer.length, limit - output.size()));
            if (read < 0) break;
            output.write(buffer, 0, read);
        }
        return output.toByteArray();
    }

    private static String runtimeEvidence(Context context) {
        File directory = new File(context.getFilesDir(), "logs");
        File[] logs = directory.listFiles((dir, name) ->
            name.startsWith("skate3_") && name.endsWith(".log"));
        if (logs == null || logs.length == 0) return "- no runtime log found";
        File latest = logs[0];
        for (File log : logs) if (log.lastModified() > latest.lastModified()) latest = log;
        try {
            byte[] data = readLogWindow(latest);
            String[] lines = new String(data, StandardCharsets.UTF_8).split("\\R");
            List<String> head = new ArrayList<>();
            List<String> tail = new ArrayList<>();
            for (String line : lines) {
                String lower = line.toLowerCase(Locale.US);
                if (!(lower.contains("vulkan") || lower.contains("native-scene:") ||
                      lower.contains("nrhi-vulkan:") || lower.contains("fatal") ||
                      lower.contains("abort") || lower.contains("failed") ||
                      lower.contains("error") || lower.contains("adreno") ||
                      lower.contains("gpu") || lower.contains("guest address space") ||
                      lower.contains("sdl audio") || lower.contains("audio stats") ||
                      lower.contains("audio device") ||
                      lower.contains("shared memory") || lower.contains("sparse residency"))) {
                    continue;
                }
                String safe = sanitizePath(context, line);
                if (head.size() < 24) head.add(safe);
                else {
                    tail.add(safe);
                    if (tail.size() > 72) tail.remove(0);
                }
            }
            if (head.isEmpty() && tail.isEmpty()) return "- no relevant runtime lines found";
            StringBuilder out = new StringBuilder();
            for (String line : head) out.append(line).append('\n');
            if (!tail.isEmpty()) {
                out.append("[latest relevant lines]\n");
                for (String line : tail) out.append(line).append('\n');
            }
            return out.toString().trim();
        } catch (Exception exception) {
            return "- runtime log unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    private static byte[] readLogWindow(File file) throws Exception {
        final int headLimit = 128 * 1024;
        final int tailLimit = 512 * 1024;
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] head = new byte[(int)Math.min(file.length(), headLimit)];
            int read = input.read(head);
            if (read > 0) output.write(head, 0, read);
        }
        if (file.length() > headLimit) {
            output.write("\n[tail of runtime log]\n".getBytes(StandardCharsets.UTF_8));
            try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
                int length = (int)Math.min(file.length(), tailLimit);
                input.seek(file.length() - length);
                byte[] tail = new byte[length];
                input.readFully(tail);
                output.write(tail);
            }
        }
        return output.toByteArray();
    }

    private static String sanitizePath(Context context, String value) {
        String safe = value.replace(context.getFilesDir().getAbsolutePath(), "<app-files>");
        File external = context.getExternalFilesDir(null);
        if (external != null) {
            safe = safe.replace(external.getAbsolutePath(), "<app-external-files>");
        }
        return clean(safe);
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
        clipboard.setPrimaryClip(ClipData.newPlainText("Skate 3 diagnostics", report));
        Toast.makeText(context, LauncherStrings.text(context, "Device diagnostics copied."), Toast.LENGTH_SHORT).show();
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
            .appendQueryParameter("evidence", diagnostic.webReport)
            .build();
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, uri));
        } catch (ActivityNotFoundException exception) {
            Toast.makeText(activity, LauncherStrings.text(activity, "No browser is available. The diagnostics are copied."),
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
        final String webReport;

        Diagnostic(String version, String device, String android, String soc,
                   String profile, String input, String report, String webReport) {
            this.version = version;
            this.device = device;
            this.android = android;
            this.soc = soc;
            this.profile = profile;
            this.input = input;
            this.report = report;
            this.webReport = webReport;
        }
    }

    private static final class ThreadTrace {
        long tid = -1;
        String name = "";
        final List<String> frames = new ArrayList<>();
        final List<String> notes = new ArrayList<>();
    }

    private static final class ProtoReader {
        final byte[] data;
        int position;

        ProtoReader(byte[] data) {
            this.data = data != null ? data : new byte[0];
        }

        boolean hasRemaining() { return position < data.length; }

        int readTag() {
            long value = readVarint();
            return value > 0 && value <= Integer.MAX_VALUE ? (int)value : 0;
        }

        long readVarint() {
            long value = 0;
            for (int shift = 0; shift < 64 && position < data.length; shift += 7) {
                int next = data[position++] & 0xff;
                value |= (long)(next & 0x7f) << shift;
                if ((next & 0x80) == 0) return value;
            }
            throw new IllegalArgumentException("invalid protobuf varint");
        }

        byte[] readBytes() {
            long encodedLength = readVarint();
            if (encodedLength < 0 || encodedLength > Integer.MAX_VALUE ||
                position + encodedLength > data.length) {
                throw new IllegalArgumentException("invalid protobuf length");
            }
            int length = (int)encodedLength;
            byte[] result = new byte[length];
            System.arraycopy(data, position, result, 0, length);
            position += length;
            return result;
        }

        String readString() {
            return new String(readBytes(), StandardCharsets.UTF_8);
        }

        void skip(int wire) {
            if (wire == 0) {
                readVarint();
            } else if (wire == 1) {
                advance(8);
            } else if (wire == 2) {
                long length = readVarint();
                if (length > Integer.MAX_VALUE) throw new IllegalArgumentException("field too big");
                advance((int)length);
            } else if (wire == 5) {
                advance(4);
            } else {
                throw new IllegalArgumentException("unsupported protobuf wire type");
            }
        }

        void advance(int count) {
            if (count < 0 || position + count > data.length) {
                throw new IllegalArgumentException("truncated protobuf");
            }
            position += count;
        }
    }
}
