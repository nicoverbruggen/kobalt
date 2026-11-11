package org.kobalt.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class UserFoldersTest {
    static final class Source implements UserFolders.Source {
        final Map<String, List<UserFolders.Entry>> folders = new HashMap<>();
        final Map<String, byte[]> bytes = new HashMap<>();
        int opens;
        boolean fail;
        public List<UserFolders.Entry> list(String key) throws IOException {
            if (fail) throw new IOException("Grant revoked");
            return new ArrayList<>(folders.getOrDefault(key, Collections.emptyList()));
        }
        public InputStream open(String key) throws IOException {
            ++opens;
            if (fail || !bytes.containsKey(key)) throw new IOException("Read failed");
            return new ByteArrayInputStream(bytes.get(key));
        }
        void file(String folder, String name, String key, String text, long modified) {
            byte[] data = text.getBytes(StandardCharsets.UTF_8);
            bytes.put(key, data);
            folders.computeIfAbsent(folder, k -> new ArrayList<>()).add(
                    new UserFolders.Entry(name, key, false, data.length, modified));
        }
    }
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static String read(Path root, String file) throws IOException {
        return new String(Files.readAllBytes(root.resolve(file)), StandardCharsets.UTF_8);
    }
    interface Attempt { void run() throws IOException; }
    static void fails(Attempt attempt) throws IOException {
        try { attempt.run(); } catch (IOException expected) { return; }
        throw new AssertionError("Expected an IO failure");
    }
    public static void main(String[] args) throws Exception {
        Path root = Paths.get(args[0]);
        Files.createDirectories(root);
        UserFolders cache = new UserFolders(root.toFile());
        Source source = new Source();
        source.file("icons", "cat.svg", "cat", "cat one", 1);
        source.file("icons", "ignore.txt", "txt", "ignored", 1);
        check(cache.sync(2, "icons", source), "Initial mirror missing");
        check(read(root, "custom-pins/cat.svg").equals("cat one"), "Wrong file contents");
        int opened = source.opens;
        check(!cache.sync(2, "icons", source) && source.opens == opened, "Unchanged files copied again");
        source.folders.remove("icons");
        source.file("icons", "dog.svg", "dog", "dog two", 2);
        cache.sync(2, "icons", source);
        check(!Files.exists(root.resolve("custom-pins/cat.svg")), "Removed icon retained");
        source.fail = true;
        fails(() -> cache.sync(2, "icons", source));
        check(read(root, "custom-pins/dog.svg").equals("dog two"), "Grant failure destroyed cached files");
        source.fail = false;
        source.file("icons", "bad.svg", "bad", "broken", 3);
        source.bytes.remove("bad");
        fails(() -> cache.sync(2, "icons", source));
        check(!Files.exists(root.resolve("custom-pins/bad.svg")), "Incomplete mirror activated");
        check(read(root, "custom-pins/dog.svg").equals("dog two"), "Copy failure changed previous mirror");
        source.folders.remove("icons");
        source.file("icons", "huge.svg", "huge", String.join("", Collections.nCopies(17000, "x")), 4);
        cache.sync(2, "icons", source);
        check(Files.size(root.resolve("custom-pins/huge.svg")) == 0, "Oversize SVG not retained as a rejection");
        source.folders.remove("icons");
        source.file("icons", "../outside.svg", "escape", "bad", 4);
        fails(() -> cache.sync(2, "icons", source));
        check(!Files.exists(root.resolve("outside.svg")), "Path traversal accepted");
        source.folders.remove("icons");
        source.file("icons", "same.svg", "one", "a", 5);
        source.file("icons", "same.svg", "two", "b", 5);
        fails(() -> cache.sync(2, "icons", source));
        source.folders.remove("icons");
        source.file("icons", "unknown.svg", "unknown", "one", 0);
        cache.sync(2, "icons", source);
        source.bytes.put("unknown", "two".getBytes(StandardCharsets.UTF_8));
        cache.sync(2, "icons", source);
        check(read(root, "custom-pins/unknown.svg").equals("two"), "Unknown modification dates treated as unchanged");
        Thread.currentThread().interrupt();
        fails(() -> cache.sync(2, "icons", source));
        Thread.interrupted();
        check(read(root, "custom-pins/unknown.svg").equals("two"), "Cancellation changed active mirror");
        source.folders.put("fonts", new ArrayList<>(Arrays.asList(new UserFolders.Entry("family", "family", true, 0, 1))));
        source.file("family", "Regular.ttf", "font", "font bytes", 1);
        cache.sync(1, "fonts", source);
        check(read(root, "fonts/family/Regular.ttf").equals("font bytes"), "Nested fonts not mirrored");
        cache.sync(2, "", source);
        check(!Files.exists(root.resolve("custom-pins")) && Files.exists(root.resolve("fonts")), "Clearing one folder changed another");
        source.folders.put("icons", new ArrayList<>());
        for (int i = 0; i < 257; ++i) source.file("icons", "icon" + i + ".svg", "id" + i, "x", 1);
        fails(() -> cache.sync(2, "icons", source));
        check(!Files.exists(root.resolve("custom-pins")), "Over-limit folder activated partially");
        System.out.println("Android selected-folder mirror checks passed");
    }
}
