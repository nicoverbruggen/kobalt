package org.kobalt.android;

import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class KosyncWorkTest {
    // Older supported devices can run the worker without foreground-service information.
    @Test public void schedulingAcrossAndroidVersions() {
        assertFalse(KosyncWork.buildWork(androidx.work.Data.EMPTY, 28).getWorkSpec().expedited);
        assertFalse(KosyncWork.buildWork(androidx.work.Data.EMPTY, 30).getWorkSpec().expedited);
        assertTrue(KosyncWork.buildWork(androidx.work.Data.EMPTY, 31).getWorkSpec().expedited);
        assertTrue(KosyncWork.buildWork(androidx.work.Data.EMPTY, 36).getWorkSpec().expedited);
    }

    // Preference keys must not retain XML control characters from account tuples.
    @Test public void timestampIdentity() {
        String key = KosyncWork.successKey("server\u0000user\u0000device\u0000hash");
        assertTrue(key.matches("success:[0-9a-f]{64}"));
        assertNotEquals(key, KosyncWork.successKey("server\u0000other\u0000device\u0000hash"));
    }

    // Foreground chains and worker exchanges must never overlap.
    @Test public void requestOwnership() {
        assertTrue(KosyncWork.beginNative());
        assertTrue(KosyncWork.beginNative());
        assertFalse(KosyncWork.beginWorker());
        KosyncWork.endNative();
        assertFalse(KosyncWork.beginWorker());
        KosyncWork.endNative();
        assertTrue(KosyncWork.beginWorker());
        assertFalse(KosyncWork.beginWorker());
        assertFalse(KosyncWork.beginNative());
        KosyncWork.endWorker();
        assertTrue(KosyncWork.beginNative());
        KosyncWork.endNative();
    }

    // Informational time writes keep retries valid; account and policy edits revoke them.
    @Test public void accountChanges() throws Exception {
        JSONObject account = new JSONObject().put("server", "https://example.invalid")
                .put("username", "fixture").put("device_id", "self").put("key", "abc")
                .put("automatic", true);
        JSONObject expected = new JSONObject(account.toString());
        expected.remove("key");
        expected.put("key_sha256", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
                .put("device_name", "").put("match_method", "binary").put("send_metadata", false);
        JSONObject job = new JSONObject().put("account", expected);
        assertTrue(KosyncWork.accountMatches(job, account));
        account.put("last_sync", "2026-11-13T12:00:00Z");
        assertTrue(KosyncWork.accountMatches(job, account));
        for (String field : new String[] { "server", "username", "device_id", "key", "device_name", "match_method" }) {
            JSONObject changed = new JSONObject(account.toString()).put(field, "changed");
            assertFalse(KosyncWork.accountMatches(job, changed));
        }
        assertFalse(KosyncWork.accountMatches(job, new JSONObject(account.toString()).put("automatic", false)));
        assertFalse(KosyncWork.accountMatches(job, new JSONObject(account.toString()).put("send_metadata", true)));
    }

    // Exercise the real HTTP transport against a disposable loopback server.
    @Test public void transportContract() throws Exception {
        java.net.ServerSocket server = new java.net.ServerSocket(0, 4, java.net.InetAddress.getByName("127.0.0.1"));
        server.setSoTimeout(5000);
        java.util.List<String> received = java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
        Thread responder = new Thread(() -> {
            try {
                for (int request = 0; request < 3; ++request) {
                    try (java.net.Socket socket = server.accept()) {
                        socket.setSoTimeout(5000);
                        java.io.BufferedReader input = new java.io.BufferedReader(new java.io.InputStreamReader(
                                socket.getInputStream(), java.nio.charset.StandardCharsets.UTF_8));
                        StringBuilder observed = new StringBuilder(input.readLine());
                        String line;
                        int length = 0;
                        while ((line = input.readLine()) != null && !line.isEmpty()) {
                            observed.append("\n").append(line);
                            if (line.toLowerCase(java.util.Locale.ROOT).startsWith("content-length:"))
                                length = Integer.parseInt(line.substring(line.indexOf(':') + 1).trim());
                        }
                        char[] body = new char[length];
                        int offset = 0;
                        while (offset < length) {
                            int count = input.read(body, offset, length - offset);
                            if (count < 0) throw new java.io.EOFException();
                            offset += count;
                        }
                        observed.append("\n").append(body);
                        received.add(observed.toString());
                        String response = request == 2
                                ? "HTTP/1.1 302 Found\r\nLocation: /destination\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                        socket.getOutputStream().write(response.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    }
                }
            } catch (Throwable error) { failure.set(error); }
        }, "kosync-loopback");
        responder.start();
        try {
            JSONObject account = new JSONObject().put("server", "http://127.0.0.1:" + server.getLocalPort() + "/prefix")
                    .put("username", "fixture").put("key", "dummy");
            long deadline = System.nanoTime() / 1000000 + 10000;
            assertEquals(200, KosyncWork.request(account, "GET", "/syncs/progress/document", null, () -> true, deadline).status);
            String observed = received.get(0).toLowerCase(java.util.Locale.ROOT);
            assertTrue(observed.startsWith("get /prefix/syncs/progress/document "));
            assertTrue(observed.contains("accept: application/vnd.koreader.v1+json"));
            assertTrue(observed.contains("x-auth-user: fixture"));
            assertTrue(observed.contains("x-auth-key: dummy"));
            assertEquals(200, KosyncWork.request(account, "PUT", "/syncs/progress", "{\"progress\":\"position\"}", () -> true, deadline).status);
            assertTrue(received.get(1).endsWith("{\"progress\":\"position\"}"));
            assertEquals(302, KosyncWork.request(account, "GET", "/redirect", null, () -> true, deadline).status);
            assertThrows(IllegalStateException.class, () -> KosyncWork.request(account, "PUT", "/syncs/progress", "{}", () -> false, deadline));
            assertThrows(java.net.SocketTimeoutException.class, () -> KosyncWork.request(account, "GET", "/syncs/progress/document", null, () -> true, 0));
            responder.join(5000);
            assertFalse(responder.isAlive());
            assertNull(failure.get());
            assertEquals(3, received.size());
        } finally {
            server.close();
            responder.join(5000);
        }
    }

    // A later pause may advance our acknowledged worker upload, but not a newer server position.
    @Test public void repeatedDeferredUpload() throws Exception {
        JSONObject job = job();
        job.getJSONObject("baseline").put("found", false).put("uploaded", "");
        job.put("delivered", "last-worker-upload");
        assertTrue(KosyncWork.mayUpload(job, remote("last-worker-upload", "self", 20), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("newer-worker-upload", "self", 21), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("last-worker-upload", "other", 20), "self"));
        assertFalse(KosyncWork.mayUpload(job, new JSONObject(), "self"));
        job.getJSONObject("baseline").put("pending", true);
        assertFalse(KosyncWork.mayUpload(job, remote("last-worker-upload", "self", 20), "self"));
    }

    private JSONObject job() throws Exception {
        return new JSONObject("{progress:'local-new',baseline:{found:true,position:'remote-old',device_id:'other',timestamp:12,uploaded:'local-old',pending:false}}".replace('\'', '"'));
    }
    private JSONObject remote(String position, String device, long time) throws Exception {
        return new JSONObject().put("progress", position).put("percentage", .5)
                .put("device", "Reader").put("device_id", device).put("timestamp", time);
    }

    // A saved comparison permits only the remote state the reader already saw.
    @Test public void retainedComparison() throws Exception {
        JSONObject job = job();
        assertTrue(KosyncWork.mayUpload(job, remote("remote-old", "other", 12), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("remote-new", "other", 13), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("remote-old", "other", 13), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("remote-old", "third", 12), "self"));
        job.getJSONObject("baseline").put("pending", true);
        assertFalse(KosyncWork.mayUpload(job, remote("remote-old", "other", 12), "self"));
    }

    // A newer upload from this device must not be replaced by an older queued position.
    @Test public void sameDeviceAndMissingRemote() throws Exception {
        JSONObject job = job();
        assertTrue(KosyncWork.mayUpload(job, remote("local-old", "self", 13), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("local-newer", "self", 14), "self"));
        assertTrue(KosyncWork.mayUpload(job, remote("local-new", "self", 14), "self"));
        assertFalse(KosyncWork.mayUpload(job, new JSONObject(), "self"));
        job.getJSONObject("baseline").put("found", false).put("uploaded", "");
        assertTrue(KosyncWork.mayUpload(job, new JSONObject(), "self"));
        assertFalse(KosyncWork.mayUpload(job, remote("foreign", "other", 14), "self"));
    }

    private JSONObject parse(int status, String body) throws Exception {
        KosyncWork.Reply reply = new KosyncWork.Reply();
        reply.status = status;
        reply.body = body;
        return KosyncWork.progress(reply);
    }

    // Only the supported missing-progress response permits a first upload.
    @Test public void missingProgressContract() throws Exception {
        assertEquals(0, parse(200, "{}").length());
        assertEquals(0, parse(200, "[]").length());
        assertEquals(0, parse(404, "{\"status\":\"not found\"}").length());
        assertNull(parse(404, "{}"));
        assertNull(parse(404, "{\"status\":\"not found\",\"extra\":true}"));
        assertNull(parse(401, "{}"));
        assertNull(parse(500, "{}"));
    }

    // The deferred worker accepts the same missing Grimmory state as the reader.
    @Test public void grimmoryProgressContract() throws Exception {
        JSONObject missing = new JSONObject().put("status", 404)
                .put("message", "No progress found for user and book").put("timestamp", "2026-11-15T12:00:00");
        assertEquals(0, parse(404, missing.toString()).length());
        for (Object status : new Object[] { 401, "404" })
            assertNull(parse(404, new JSONObject(missing.toString()).put("status", status).toString()));
        assertNull(parse(404, new JSONObject(missing.toString()).put("message", "Book not found").toString()));
        assertNull(parse(404, new JSONObject(missing.toString()).put("timestamp", "").toString()));
        assertNull(parse(404, new JSONObject(missing.toString()).put("progress", "private").toString()));
        JSONObject row = new JSONObject().put("document", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
                .put("progress", JSONObject.NULL).put("percentage", JSONObject.NULL)
                .put("timestamp", JSONObject.NULL).put("device", "Grimmory").put("device_id", "Grimmory");
        assertEquals(0, parse(200, row.toString()).length());
        assertNull(parse(200, new JSONObject(row.toString()).put("percentage", .5).toString()));
        assertNull(parse(200, new JSONObject(row.toString()).put("timestamp", 12).toString()));
        assertNull(parse(200, new JSONObject(row.toString()).put("document", "invalid").toString()));
        assertNull(parse(200, new JSONObject(row.toString()).put("device_id", 7).toString()));
        row.put("progress", "valid").put("percentage", .5);
        assertNotNull(parse(200, row.toString()));
    }

    // Confirm supported writes without accepting contradictory or unrelated responses.
    @Test public void uploadAcknowledgements() throws Exception {
        String document = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        KosyncWork.Reply reply = new KosyncWork.Reply();
        reply.status = 200;
        for (String body : new String[] { "{\"document\":\"" + document + "\"}",
                                         "{\"status\":\"progress updated\"}" }) {
            reply.body = body;
            assertTrue(KosyncWork.acknowledged(reply, document));
            reply.status = 201;
            assertFalse(KosyncWork.acknowledged(reply, document));
            reply.status = 200;
        }
        for (String body : new String[] { "{}", "{\"status\":\"not found\"}",
                "{\"status\":\"progress updated\",\"document\":\"wrong\"}",
                "{\"status\":\"progress updated\",\"error\":\"failed\"}" }) {
            reply.body = body;
            assertFalse(KosyncWork.acknowledged(reply, document));
        }
    }

    // Malformed server progress cannot authorize an upload.
    @Test public void invalidRemoteProgress() throws Exception {
        assertNotNull(parse(200, remote("valid", "other", 12).toString()));
        for (Object invalid : new Object[] { -1, 2, "0.5", JSONObject.NULL }) {
            JSONObject remote = remote("valid", "other", 12).put("percentage", invalid);
            assertNull(parse(200, remote.toString()));
        }
        assertNull(parse(200, remote("", "other", 12).toString()));
        assertNull(parse(200, remote("valid", "other", -1).toString()));
        assertNull(parse(200, remote("valid", "other", 12).put("device_id", 7).toString()));
    }
}
