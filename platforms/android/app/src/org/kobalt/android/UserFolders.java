package org.kobalt.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;

// Worker-only mirrors of explicitly granted document trees. Readers need seekable local files.
final class UserFolders {
    static final String[] KINDS = { "dictionaries", "fonts", "custom-pins" };
    static final class Entry {
        final String name, key;
        final boolean directory;
        final long size, modified;
        Entry(String name, String key, boolean directory, long size, long modified) {
            this.name = name; this.key = key; this.directory = directory;
            this.size = size; this.modified = modified;
        }
    }
    interface Source {
        List<Entry> list(String key) throws IOException;
        InputStream open(String key) throws IOException;
    }
    private static final class FileEntry {
        final Entry entry;
        final String path;
        FileEntry(Entry entry, String path) { this.entry = entry; this.path = path; }
    }
    private final File root;
    UserFolders(File root) { this.root = root; }

    private static boolean accepts(int kind, String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        if (kind == 2) return lower.endsWith(".svg");
        if (kind == 1) return lower.endsWith(".ttf") || lower.endsWith(".otf");
        return lower.endsWith(".ifo") || lower.endsWith(".idx") || lower.endsWith(".syn")
                || lower.endsWith(".dict.dz")
                || name.equals("LICENSE") || name.equals("ATTRIBUTION");
    }
    private static void cancelled() throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("Folder scan cancelled");
    }
    private static void collect(Source source, String key, int kind, String prefix, int depth,
                                List<FileEntry> files, int[] visited) throws IOException {
        cancelled();
        List<Entry> entries = source.list(key);
        if (entries == null) throw new IOException("Cannot read the selected folder");
        entries.sort(Comparator.comparing(e -> e.name, Comparator.nullsFirst(Comparator.naturalOrder())));
        for (Entry entry : entries) {
            cancelled();
            if (++visited[0] > (kind == 2 ? 256 : 2048))
                throw new IOException("Selected folder contains too many entries");
            String name = entry.name;
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                    || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
                throw new IOException("Invalid filename in selected folder");
            if (entry.directory) {
                if (kind != 2 && depth < 2 && !name.startsWith("."))
                    collect(source, entry.key, kind, prefix + name + "/", depth + 1, files, visited);
            } else if (accepts(kind, name)) {
                files.add(new FileEntry(entry, prefix + name));
            }
        }
    }
    // No links are followed, and only this app-owned mirror is removed.
    private static void remove(File file) throws IOException {
        if (!Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(file.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attrs) throws IOException {
                Files.delete(path); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path path, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(path); return FileVisitResult.CONTINUE;
            }
        });
    }
    // A complete snapshot replaces the previous one. Provider failures retain the last good copy.
    boolean sync(int kind, String tree, Source source) throws IOException {
        if (kind < 0 || kind >= KINDS.length) throw new IOException("Unknown folder type");
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create folder cache");
        File current = new File(root, KINDS[kind]);
        File previous = new File(root, KINDS[kind] + "-previous");
        File stage = new File(root, KINDS[kind] + "-pending");
        if (!current.exists() && previous.exists() && !previous.renameTo(current))
            throw new IOException("Cannot recover folder cache");
        remove(stage);
        if (tree.isEmpty()) { remove(current); remove(previous); return true; }
        List<FileEntry> files = new ArrayList<>();
        collect(source, tree, kind, "", 0, files, new int[1]);
        files.sort(Comparator.comparing(f -> f.path));
        StringBuilder signature = new StringBuilder(tree).append('\n');
        boolean known = true;
        Set<String> paths = new HashSet<>();
        for (FileEntry file : files) {
            if (!paths.add(file.path)) throw new IOException("Duplicate filename in selected folder");
            Entry entry = file.entry;
            signature.append(file.path).append('\0').append(entry.key).append('\0')
                    .append(entry.size).append('\0').append(entry.modified).append('\n');
            known &= entry.size >= 0 && entry.modified > 0;
        }
        File inventory = new File(current, ".inventory");
        if (known && inventory.isFile() && signature.toString().equals(
                new String(Files.readAllBytes(inventory.toPath()), StandardCharsets.UTF_8))) return false;
        if (!stage.mkdir()) throw new IOException("Cannot prepare folder cache");
        try {
            long total = 0;
            long limit = kind == 2 ? 16 * 1024 : kind == 1 ? 100L * 1024 * 1024 : 512L * 1024 * 1024;
            byte[] buffer = new byte[65536];
            for (FileEntry file : files) {
                cancelled();
                File destination = new File(stage, file.path);
                File parent = destination.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot prepare folder cache");
                try (OutputStream output = new FileOutputStream(destination)) {
                    // Keep rejected SVG names so the picker can report them.
                    if (kind == 2 && file.entry.size > limit) continue;
                    try (InputStream input = source.open(file.entry.key)) {
                        if (input == null) throw new IOException("Cannot read " + file.path);
                        long used = 0;
                        int count;
                        while ((count = input.read(buffer)) != -1) {
                            cancelled();
                            used += count; total += count;
                            if (used > limit || total > 1024L * 1024 * 1024) {
                                if (kind == 2) { output.close(); new FileOutputStream(destination).close(); break; }
                                throw new IOException("Selected files exceed the folder cache limit");
                            }
                            output.write(buffer, 0, count);
                        }
                    }
                }
            }
            Files.write(new File(stage, ".inventory").toPath(), signature.toString().getBytes(StandardCharsets.UTF_8));
            remove(previous);
            if (current.exists() && !current.renameTo(previous)) throw new IOException("Cannot replace folder cache");
            if (!stage.renameTo(current)) {
                if (previous.exists()) previous.renameTo(current);
                throw new IOException("Cannot activate folder cache");
            }
            remove(previous);
            return true;
        } finally { remove(stage); }
    }
}
