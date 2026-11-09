package husk;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

/**
 * The phone's gallery (Android's MediaStore, authority "media") as far as an app writes to it: insert() makes an entry and a file
 * for it, the app writes the file through openOutputStream / openFileDescriptor, and once it is whole -- IS_PENDING cleared, or
 * (an app that never set it) the file stopped growing -- it goes to the iPhone's Photos (Native.saveMedia). An Instagram mod's
 * "download" saved a video this way and failed with "the gallery refused the file" while insert() gave null.
 *
 * Queries see the entries made here (an app looking up what it just saved); the phone's own library is not shown.
 */
public final class MediaProvider extends ContentProvider {
    private static MediaProvider sSelf;
    public static synchronized MediaProvider get() { if (sSelf == null) sSelf = new MediaProvider(); return sSelf; }

    private static final class Entry {
        long id; String base, name, mime, relativePath; java.io.File file; boolean pending, published; long added;
        long lastSize = -1; int stable;
    }
    private final java.util.LinkedHashMap<Long, Entry> mEntries = new java.util.LinkedHashMap<>();
    private long mNextId = 1000;
    private Thread mWatcher;

    @Override public boolean onCreate() { return true; }

    private static java.io.File dir() {
        java.io.File d = new java.io.File(ContextImpl.app().getCacheDir(), "husk-media");
        d.mkdirs();
        return d;
    }
    private static String kindOf(Uri u) {
        for (String s : u.getPathSegments()) if (s.equals("video") || s.equals("images") || s.equals("audio") || s.equals("downloads") || s.equals("file")) return s;
        return "file";
    }
    private static String defaultMime(String kind) {
        switch (kind) { case "video": return "video/mp4"; case "images": return "image/jpeg"; case "audio": return "audio/mpeg"; default: return "application/octet-stream"; }
    }
    private Entry entry(Uri u) {
        try { long id = Long.parseLong(u.getLastPathSegment()); synchronized (mEntries) { return mEntries.get(id); } } catch (RuntimeException e) { return null; }
    }

    @Override public Uri insert(Uri u, ContentValues v) {
        if (u == null) return null;
        String kind = kindOf(u);
        Entry e = new Entry();
        e.mime = v != null && v.getAsString("mime_type") != null ? v.getAsString("mime_type") : defaultMime(kind);
        String ext = android.webkit.MimeTypeMap.getSingleton().getExtensionFromMimeType(e.mime);
        e.name = v != null && v.getAsString("_display_name") != null ? v.getAsString("_display_name") : "media-" + System.currentTimeMillis() + (ext != null ? "." + ext : "");
        if (ext != null && e.name.indexOf('.') < 0) e.name += "." + ext;
        e.relativePath = v != null ? v.getAsString("relative_path") : null;
        Integer pending = v != null ? v.getAsInteger("is_pending") : null;
        e.pending = pending != null && pending != 0;
        e.added = System.currentTimeMillis() / 1000;
        synchronized (mEntries) { e.id = mNextId++; mEntries.put(e.id, e); }
        e.file = new java.io.File(dir(), e.id + "-" + e.name.replace('/', '_'));
        try { e.file.createNewFile(); } catch (java.io.IOException x) { android.util.Log.w("MediaStore", "no file for " + e.name + ": " + x); return null; }
        String path = u.getPath();
        e.base = "content://media" + (path == null ? "/external/file" : path.replaceAll("/\\d+$", ""));
        android.util.Log.i("MediaStore", "new " + e.mime + " " + e.name + (e.pending ? " (pending)" : ""));
        if (!e.pending) watch();
        return Uri.parse(e.base + "/" + e.id);
    }

    @Override public int update(Uri u, ContentValues v, String sel, String[] args) {
        Entry e = entry(u);
        if (e == null || v == null) return 0;
        if (v.getAsString("_display_name") != null) e.name = v.getAsString("_display_name");
        if (v.getAsString("mime_type") != null) e.mime = v.getAsString("mime_type");
        Integer pending = v.getAsInteger("is_pending");
        if (pending != null) { e.pending = pending != 0; if (!e.pending) publish(e); }
        return 1;
    }

    @Override public int delete(Uri u, String sel, String[] args) {
        Entry e = entry(u);
        if (e == null) return 0;
        synchronized (mEntries) { mEntries.remove(e.id); }
        if (!e.published) e.file.delete();
        return 1;
    }

    @Override public String getType(Uri u) { Entry e = entry(u); return e != null ? e.mime : null; }

    @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort) {
        String[] cols = proj != null ? proj : new String[] { "_id", "_display_name", "mime_type", "_data", "_size", "is_pending", "relative_path", "date_added" };
        MatrixCursor c = new MatrixCursor(cols);
        Entry only = entry(u);
        java.util.List<Entry> list = new java.util.ArrayList<>();
        synchronized (mEntries) { if (only != null) list.add(only); else if (u.getLastPathSegment() != null && !u.getLastPathSegment().matches("\\d+")) list.addAll(mEntries.values()); }
        for (Entry e : list) {
            Object[] row = new Object[cols.length];
            for (int i = 0; i < cols.length; i++) {
                switch (cols[i]) {
                case "_id": row[i] = e.id; break;
                case "_display_name": case "title": row[i] = e.name; break;
                case "mime_type": row[i] = e.mime; break;
                case "_data": row[i] = e.file.getAbsolutePath(); break;
                case "_size": row[i] = e.file.length(); break;
                case "is_pending": row[i] = e.pending ? 1 : 0; break;
                case "relative_path": row[i] = e.relativePath; break;
                case "date_added": case "date_modified": row[i] = e.added; break;
                default: row[i] = null;
                }
            }
            c.addRow(row);
        }
        return c;
    }

    @Override public ParcelFileDescriptor openFile(Uri u, String mode) throws java.io.FileNotFoundException {
        Entry e = entry(u);
        if (e == null) throw new java.io.FileNotFoundException("No such media: " + u);
        return ParcelFileDescriptor.open(e.file, ParcelFileDescriptor.parseMode(mode));
    }

    private void publish(Entry e) {
        synchronized (e) { if (e.published || e.file.length() == 0) return; e.published = true; }
        android.util.Log.i("MediaStore", "to Photos: " + e.name + ", " + e.file.length() + " bytes");
        Native.saveMedia(e.file.getAbsolutePath(), e.mime);
    }

    /* Entries that were never pending: published once the file has stopped growing for two seconds. */
    private synchronized void watch() {
        if (mWatcher != null) return;
        mWatcher = new Thread(() -> {
            while (true) {
                try { Thread.sleep(1000); } catch (InterruptedException x) { return; }
                java.util.List<Entry> todo = new java.util.ArrayList<>();
                synchronized (mEntries) { for (Entry e : mEntries.values()) if (!e.pending && !e.published) todo.add(e); }
                for (Entry e : todo) {
                    long n = e.file.length();
                    if (n > 0 && n == e.lastSize) { if (++e.stable >= 2) publish(e); } else { e.stable = 0; e.lastSize = n; }
                }
            }
        }, "husk-media");
        mWatcher.setDaemon(true);
        mWatcher.start();
    }
}
