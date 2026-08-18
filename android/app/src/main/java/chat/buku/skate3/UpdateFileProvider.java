package chat.buku.skate3;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/** Read-only provider exposing only the updater's verified cached APK. */
public final class UpdateFileProvider extends ContentProvider {
    private File updateFile() {
        return new File(new File(getContext().getCacheDir(), "updates"), "update.apk");
    }

    private void verify(Uri uri) throws FileNotFoundException {
        if (uri == null || !"update.apk".equals(uri.getLastPathSegment()) ||
            !updateFile().isFile()) {
            throw new FileNotFoundException("Verified update package not found.");
        }
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.android.package-archive";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        verify(uri);
        if (!"r".equals(mode)) throw new FileNotFoundException("Read-only provider.");
        return ParcelFileDescriptor.open(updateFile(), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        try {
            verify(uri);
        } catch (FileNotFoundException exception) {
            return null;
        }
        String[] columns = projection != null ? projection :
            new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        MatrixCursor.RowBuilder row = cursor.newRow();
        File file = updateFile();
        for (String column : columns) {
            if (OpenableColumns.DISPLAY_NAME.equals(column)) row.add("Skate3-Mobile-Android.apk");
            else if (OpenableColumns.SIZE.equals(column)) row.add(file.length());
            else row.add(null);
        }
        return cursor;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw unsupported(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw unsupported(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw unsupported();
    }

    private UnsupportedOperationException unsupported() {
        return new UnsupportedOperationException("Read-only provider.");
    }
}
