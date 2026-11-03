package org.kobalt.android;

import android.content.ContentResolver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.BatteryManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.os.Build;
import android.view.View;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowInsetsAnimation;
import android.widget.Toast;
import org.qtproject.qt.android.bindings.QtActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;

public class KobaltActivity extends QtActivity {
    private static final int PICK_BOOK = 41;
    private static final int PICK_FOLDER = 42;
    private static final long MAX_BOOK_BYTES = 128L * 1024 * 1024;
    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private final AtomicLong request = new AtomicLong();
    private BooxDevice boox;
    private BooxRefresh booxRefresh;
    private boolean resumed;
    private boolean ready = false;
    private boolean reading = false, libraryDark = false;
    private volatile boolean readingGeometryReady = false;
    private volatile boolean libraryGeometryReady = false;
    private volatile long libraryContentSize;
    private int surfaceWidth, surfaceHeight;
    private int surfaceLeft, surfaceTop, surfaceRight, surfaceBottom, surfaceCorner;
    private String pendingPath;
    private native void openBook(String path);
    private native boolean turnPage(int direction);
    private native void folderChosen(String uri, String label);
    private native void surfaceResized(int width, int height, int left, int top, int right, int bottom, int corner);
    private final HashSet<Integer> pageKeys = new HashSet<>();

    @Override public void setRequestedOrientation(int orientation) {
        // Framework orientation requests must not override the user's system policy.
        super.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        final int key = event.getKeyCode();
        final boolean next = key == KeyEvent.KEYCODE_VOLUME_DOWN || key == KeyEvent.KEYCODE_PAGE_DOWN;
        final boolean previous = key == KeyEvent.KEYCODE_VOLUME_UP || key == KeyEvent.KEYCODE_PAGE_UP;
        if (next || previous) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                if (ready && turnPage(next ? 1 : -1)) {
                    pageKeys.add(key);
                    return true;
                }
            } else if (pageKeys.contains(key)) {
                if (event.getAction() == KeyEvent.ACTION_UP) pageKeys.remove(key);
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public void onCreate(Bundle state) {
        // Qt can start its native thread in super.onCreate. Publish capabilities first.
        boox = new BooxDevice();
        booxRefresh = new BooxRefresh();
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
        View decor = getWindow().getDecorView();
        HashSet<WindowInsetsAnimation> barAnimations = new HashSet<>();
        if (Build.VERSION.SDK_INT >= 30) {
            decor.setWindowInsetsAnimationCallback(new WindowInsetsAnimation.Callback(
                    WindowInsetsAnimation.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                @Override public void onPrepare(WindowInsetsAnimation animation) {
                    if ((animation.getTypeMask() & WindowInsets.Type.systemBars()) != 0) {
                        barAnimations.add(animation);
                        readingGeometryReady = false;
                        libraryGeometryReady = false;
                    }
                }
                @Override public WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> running) {
                    return insets;
                }
                @Override public void onEnd(WindowInsetsAnimation animation) {
                    barAnimations.remove(animation);
                }
            });
        }
        // Publish readiness only after Android has laid out the final insets.
        // Qt reads this snapshot from its own GUI thread.
        decor.getViewTreeObserver().addOnPreDrawListener(() -> {
            // Qt may still have the launch orientation's fullscreen dimensions.
            // Publish actual activity bounds once ready and whenever they change.
            final int width = decor.getWidth(), height = decor.getHeight();
            if (booxRefresh != null) booxRefresh.publish(decor, resumed && hasWindowFocus());
            WindowInsets insets = decor.getRootWindowInsets();
            boolean hidden = false;
            int left = 0, top = 0, right = 0, bottom = 0, corner = 0;
            if (insets != null) {
                // Android 12 reports rounded screen corners; footer text keeps clear of them.
                if (Build.VERSION.SDK_INT >= 31) {
                    for (int position : new int[] {android.view.RoundedCorner.POSITION_TOP_LEFT,
                            android.view.RoundedCorner.POSITION_TOP_RIGHT, android.view.RoundedCorner.POSITION_BOTTOM_LEFT,
                            android.view.RoundedCorner.POSITION_BOTTOM_RIGHT}) {
                        android.view.RoundedCorner rounded = insets.getRoundedCorner(position);
                        if (rounded != null) corner = Math.max(corner, rounded.getRadius());
                    }
                }
                // systemBars also includes a caption bar, absent on handhelds.
                // Its invisibility must not keep the Library handoff waiting.
                if (Build.VERSION.SDK_INT >= 30) hidden = !insets.isVisible(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                else {
                    android.view.DisplayCutout cutout = insets.getDisplayCutout();
                    hidden = insets.getSystemWindowInsetTop() <= (cutout == null ? 0 : cutout.getSafeInsetTop())
                        && insets.getSystemWindowInsetBottom() <= (cutout == null ? 0 : cutout.getSafeInsetBottom());
                }
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    left = safe.left; top = safe.top; right = safe.right; bottom = safe.bottom;
                } else {
                    left = insets.getSystemWindowInsetLeft(); top = insets.getSystemWindowInsetTop();
                    right = insets.getSystemWindowInsetRight(); bottom = insets.getSystemWindowInsetBottom();
                    android.view.DisplayCutout cutout = insets.getDisplayCutout();
                    if (cutout != null) {
                        left = Math.max(left, cutout.getSafeInsetLeft());
                        top = Math.max(top, cutout.getSafeInsetTop());
                        right = Math.max(right, cutout.getSafeInsetRight());
                        bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                    }
                }
            }
            if (ready && width > 0 && height > 0 && (width != surfaceWidth || height != surfaceHeight
                    || left != surfaceLeft || top != surfaceTop || right != surfaceRight || bottom != surfaceBottom
                    || corner != surfaceCorner)) {
                surfaceWidth = width; surfaceHeight = height;
                surfaceLeft = left; surfaceTop = top; surfaceRight = right; surfaceBottom = bottom; surfaceCorner = corner;
                surfaceResized(width, height, left, top, right, bottom, corner);
            }
            readingGeometryReady = reading && hidden && barAnimations.isEmpty();
            libraryGeometryReady = !reading && insets != null && !hidden && barAnimations.isEmpty();
            if (libraryGeometryReady) {
                libraryContentSize = ((long)Math.max(0, width - 2 * Math.max(left, right)) << 32)
                    | (Math.max(0, height - top - bottom) & 0xffffffffL);
            } else libraryContentSize = 0;
            return true;
        });
        immersive();
        receive(getIntent());
    }
    @Override public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        receive(intent);
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) { pageKeys.clear(); if (booxRefresh != null) booxRefresh.cancel(); }
        if (focus) {
            immersive();
            // Keep a focus target in touch mode. Otherwise Android can consume
            // the first Page Up/Down press just to establish keyboard focus.
            View content = findViewById(android.R.id.content);
            if (content != null) {
                content.setFocusableInTouchMode(true);
                if (!content.hasFocus()) content.requestFocus();
            }
        }
    }
    private void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                if (reading) controller.hide(WindowInsets.Type.systemBars());
                else {
                    controller.show(WindowInsets.Type.systemBars());
                    int lightBars = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                        | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                    controller.setSystemBarsAppearance(libraryDark ? 0 : lightBars, lightBars);
                }
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(reading ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE : View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | (libraryDark ? 0 : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR));
        }
    }
    public void readerLibraryDark(boolean dark) {
        runOnUiThread(() -> { libraryDark = dark; if (!reading) immersive(); });
    }
    public void showStartup() { runOnUiThread(() -> { readingGeometryReady = false; libraryGeometryReady = false; reading = false; immersive(); }); }
    public void showReading() { runOnUiThread(() -> { readingGeometryReady = false; libraryGeometryReady = false; reading = true; immersive(); }); }
    public boolean readerGeometryReady() { return readingGeometryReady; }
    public boolean libraryGeometryReady() { return libraryGeometryReady; }
    public long libraryContentSize() { return libraryContentSize; }
    // Called only after Qt has loaded the native library and connected its UI.
    public int readerBattery() {
        Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery == null) return -1;
        int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
        if (level < 0 || scale <= 0) return -1;
        int percent = Math.min(100, level * 100 / scale);
        return percent + (battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0 ? 256 : 0);
    }
    @Override protected void onResume() { super.onResume(); resumed = true; }
    @Override protected void onPause() {
        resumed = false;
        if (booxRefresh != null) booxRefresh.cancel();
        super.onPause();
    }
    public boolean readerRefreshAvailable() { return booxRefresh != null && booxRefresh.available(); }
    public boolean readerPrepareRefresh(int[] regions, boolean clean) {
        return booxRefresh != null && booxRefresh.prepare(regions, clean);
    }
    public boolean readerLightAvailable() { return boox != null && boox.available(); }
    public int readerLight(int operation, int type, int value) { return boox == null ? -1 : boox.light(operation, type, value); }
    public int readerDensity() { return getResources().getDisplayMetrics().densityDpi; }
    public void readerReady() {
        ready = true;
        showStartup();
        if (pendingPath != null) { openBook(pendingPath); pendingPath = null; }
    }
    public void chooseBook() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/epub+zip");
        try { startActivityForResult(intent, PICK_BOOK); }
        catch (android.content.ActivityNotFoundException error) { toast("No document picker is installed."); }
    }
    public void chooseFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try { startActivityForResult(intent, PICK_FOLDER); }
        catch (android.content.ActivityNotFoundException error) { toast("No folder picker is installed."); }
    }
    @Override protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code == PICK_BOOK && result == RESULT_OK && data != null) importBook(data.getData());
        if (code == PICK_FOLDER && result == RESULT_OK && data != null && data.getData() != null) {
            Uri tree = data.getData();
            try {
                // Keep read access after restarts. The library reads books in place.
                getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException error) {
                toast("Kobalt could not keep access to this folder.");
                return;
            }
            String label = tree.getLastPathSegment();
            try (Cursor cursor = getContentResolver().query(DocumentsContract.buildDocumentUriUsingTree(tree,
                    DocumentsContract.getTreeDocumentId(tree)), new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                    null, null, null)) {
                if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null) label = cursor.getString(0);
            } catch (Exception ignored) {
                // The URI's last segment is a usable label.
            }
            if (ready) folderChosen(tree.toString(), label == null ? "" : label);
        }
    }
    public void releaseFolder(String uri) {
        try {
            getContentResolver().releasePersistableUriPermission(Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // The grant is already gone.
        }
    }
    // Called from the library's scan thread. Returns name, document URI, size
    // and modification time for every EPUB below the folder, or null when the
    // folder cannot be read. Hidden folders are skipped.
    public String[] listLibraryFolder(String treeUri) {
        try {
            Uri tree = Uri.parse(treeUri);
            ContentResolver resolver = getContentResolver();
            ArrayList<String> found = new ArrayList<>();
            ArrayDeque<String> folders = new ArrayDeque<>();
            folders.add(DocumentsContract.getTreeDocumentId(tree));
            String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED};
            int visited = 0;
            while (!folders.isEmpty() && visited++ < 20000) {
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, folders.poll());
                try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
                    if (cursor == null) continue;
                    while (cursor.moveToNext()) {
                        String id = cursor.getString(0), name = cursor.getString(1), type = cursor.getString(2);
                        if (id == null || name == null || name.startsWith(".")) continue;
                        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(type)) { folders.add(id); continue; }
                        if (!name.toLowerCase(java.util.Locale.ROOT).endsWith(".epub")) continue;
                        found.add(DocumentsContract.buildDocumentUriUsingTree(tree, id).toString());
                        found.add(name);
                        found.add(Long.toString(cursor.isNull(3) ? 0 : cursor.getLong(3)));
                        found.add(Long.toString(cursor.isNull(4) ? 0 : cursor.getLong(4)));
                    }
                }
            }
            return found.toArray(new String[0]);
        } catch (Exception error) {
            return null;
        }
    }
    // Called from the scan thread. The caller owns and closes the descriptor.
    public int openLibraryDocument(String uri) {
        try {
            ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return descriptor == null ? -1 : descriptor.detachFd();
        } catch (Exception error) {
            return -1;
        }
    }
    public String syncFilename(String path) {
        return getSharedPreferences("book-filenames", MODE_PRIVATE).getString(path, "");
    }
    private void rememberSyncFilename(Uri uri, String path) {
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String name = cursor.getString(0);
                if (name != null && !name.isEmpty())
                    getSharedPreferences("book-filenames", MODE_PRIVATE).edit().putString(path, name).apply();
            }
        } catch (Exception error) {
            // Providers may omit names. Binary matching still works without one.
        }
    }
    // Copies a library book to the reader's cache path, unless the copy there
    // is current, then opens it. Only the open book is kept in the cache.
    public void openLibraryBook(String uri, String destination, long size, long modified) {
        final long generation = request.incrementAndGet();
        importer.execute(() -> {
            File target = new File(destination);
            File temporary = null;
            try {
                File folder = target.getParentFile();
                if (folder == null || (!folder.isDirectory() && !folder.mkdirs())) throw new java.io.IOException("Cannot create the book cache");
                if (!target.isFile() || target.length() != size || target.lastModified() < modified) {
                    temporary = File.createTempFile("library-", ".part", folder);
                    try (InputStream input = getContentResolver().openInputStream(Uri.parse(uri));
                         FileOutputStream output = new FileOutputStream(temporary)) {
                        if (input == null) throw new java.io.IOException("Cannot read this book");
                        byte[] buffer = new byte[65536];
                        int count;
                        while ((count = input.read(buffer)) != -1) {
                            if (request.get() != generation) return;
                            output.write(buffer, 0, count);
                        }
                        output.getFD().sync();
                    }
                    if (!temporary.renameTo(target)) throw new java.io.IOException("Cannot save this book");
                }
                rememberSyncFilename(Uri.parse(uri), destination);
                File[] others = folder.listFiles();
                if (others != null)
                    for (File other : others)
                        if (!other.equals(target) && other.getName().endsWith(".epub")) other.delete();
                runOnUiThread(() -> {
                    if (request.get() != generation || isFinishing() || isDestroyed()) return;
                    if (ready) openBook(destination); else pendingPath = destination;
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (request.get() != generation || isDestroyed()) return;
                    toast("Could not open EPUB: " + error.getMessage());
                    // An empty path returns the library from its loading view.
                    if (ready) openBook("");
                });
            } finally {
                if (temporary != null) temporary.delete();
            }
        });
    }
    private void receive(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) importBook(intent.getData());
    }
    private void toast(String message) { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); }
    private void importBook(Uri uri) {
        if (uri == null || !"content".equals(uri.getScheme())) {
            toast("Open an EPUB through Android's document picker.");
            return;
        }
        final long generation = request.incrementAndGet();
        importer.execute(() -> {
            File temporary = null;
            try {
                File books = new File(getFilesDir(), "books");
                if (!books.isDirectory() && !books.mkdirs()) throw new java.io.IOException("Cannot create book storage");
                temporary = File.createTempFile("import-", ".part", books);
                MessageDigest hash = MessageDigest.getInstance("SHA-256");
                try (InputStream input = getContentResolver().openInputStream(uri);
                     FileOutputStream output = new FileOutputStream(temporary)) {
                    if (input == null) throw new java.io.IOException("Cannot read this document");
                    byte[] buffer = new byte[65536];
                    long total = 0;
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        if (request.get() != generation) return;
                        total += count;
                        if (total > MAX_BOOK_BYTES) throw new java.io.IOException("EPUB exceeds the 128 MiB import limit");
                        output.write(buffer, 0, count);
                        hash.update(buffer, 0, count);
                    }
                    if (total == 0) throw new java.io.IOException("This document is empty");
                    output.getFD().sync();
                }
                StringBuilder key = new StringBuilder();
                for (byte value : hash.digest()) key.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                File destination = new File(books, key + ".epub");
                if (!destination.exists() && !temporary.renameTo(destination)) throw new java.io.IOException("Cannot save this document");
                final String path = destination.getAbsolutePath();
                rememberSyncFilename(uri, path);
                runOnUiThread(() -> {
                    if (request.get() != generation || isFinishing() || isDestroyed()) return;
                    if (ready) openBook(path); else pendingPath = path;
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (request.get() == generation && !isDestroyed()) toast("Could not open EPUB: " + error.getMessage());
                });
            } finally {
                if (temporary != null) temporary.delete();
            }
        });
    }
    public void exitReader() { finishAndRemoveTask(); }
    @Override protected void onDestroy() {
        ready = false;
        if (booxRefresh != null) booxRefresh.cancel();
        request.incrementAndGet();
        importer.shutdownNow();
        super.onDestroy();
    }
}
