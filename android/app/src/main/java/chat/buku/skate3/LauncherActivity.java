package chat.buku.skate3;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

public class LauncherActivity extends Activity {
    private boolean requestedAccess;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        continueLaunch();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (requestedAccess) {
            continueLaunch();
        }
    }

    private void continueLaunch() {
        if (Environment.isExternalStorageManager()) {
            startActivity(new Intent(this, Skate3Activity.class));
            finish();
            return;
        }
        if (!requestedAccess) {
            requestedAccess = true;
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (Exception ignored) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
    }
}
