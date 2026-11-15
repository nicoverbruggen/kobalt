package org.kobalt.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import androidx.work.BackoffPolicy;
import androidx.work.Constraints;
import androidx.work.Data;
import androidx.work.ExistingWorkPolicy;
import androidx.work.NetworkType;
import androidx.work.OneTimeWorkRequest;
import androidx.work.OutOfQuotaPolicy;
import androidx.work.WorkManager;
import androidx.work.Worker;
import androidx.work.WorkerParameters;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/** A finite, durable sync attempt. It never starts Qt or opens the reader profile. */
public final class KosyncWork extends Worker {
    private static final String TAG = "kobalt-kosync-pause";
    private static boolean active;
    private static boolean workerBusy;
    private static int nativeRequests;
    private long deadline;
    private static final Object gate = new Object();
    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(TAG, Context.MODE_PRIVATE);
    }

    public KosyncWork(Context context, WorkerParameters parameters) {
        super(context, parameters);
    }

    public static void setActive(Context context, boolean value) {
        synchronized (gate) {
            active = value;
            if (value) {
                // Invalidate before the reader can change position or account.
                preferences(context).edit().putString("generation", UUID.randomUUID().toString()).apply();
                WorkManager.getInstance(context).cancelAllWorkByTag(TAG);
            }
        }
    }

    public static boolean beginNative() {
        synchronized (gate) {
            if (workerBusy) return false;
            ++nativeRequests;
            return true;
        }
    }

    public static void endNative() {
        synchronized (gate) { --nativeRequests; }
    }

    public static void enqueue(Context context, String snapshot) {
        // WorkManager's input limit includes its encoding overhead.
        if (snapshot.getBytes(StandardCharsets.UTF_8).length > 8000) return;
        synchronized (gate) {
            if (active) return;
            String generation = preferences(context).getString("generation", "initial");
            try {
                JSONObject job = new JSONObject(snapshot);
                String document = job.getString("document");
                if (!document.matches("[0-9a-f]{32}") || job.getString("progress").isEmpty()
                        || !job.has("account")) return;
                // Capture acknowledged worker progress now, not when a retry
                // eventually runs. An old job must not adopt a newer baseline.
                job.put("delivered", preferences(context).getString(deliveryKey(job), ""));
                snapshot = job.toString();
                if (snapshot.getBytes(StandardCharsets.UTF_8).length > 8000) return;
                Data data = new Data.Builder().putString("snapshot", snapshot)
                        .putString("generation", generation).build();
                OneTimeWorkRequest work = buildWork(data, Build.VERSION.SDK_INT);
                WorkManager.getInstance(context).enqueueUniqueWork(TAG + "-" + document,
                        // Qt can report inactive and suspended for one pause.
                        // Resume cancels the old generation before new work.
                        ExistingWorkPolicy.KEEP, work);
            } catch (Exception invalidSnapshot) {
                // Invalid or oversized snapshots must never reach a server.
            }
        }
    }

    static OneTimeWorkRequest buildWork(Data data, int sdk) {
        OneTimeWorkRequest.Builder builder = new OneTimeWorkRequest.Builder(KosyncWork.class)
                .setInputData(data).addTag(TAG)
                .setConstraints(new Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS);
        // Older Android versions require foreground-service information for
        // expedited work. Ordinary scheduling needs no service or notification.
        if (sdk >= 31)
            builder.setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST);
        return builder.build();
    }

    private boolean current() {
        synchronized (gate) {
            return !active && !isStopped() && getInputData().getString("generation").equals(
                    preferences(getApplicationContext()).getString("generation", "initial"));
        }
    }

    static boolean beginWorker() {
        synchronized (gate) {
            if (workerBusy || nativeRequests != 0) return false;
            workerBusy = true;
            return true;
        }
    }

    static void endWorker() {
        synchronized (gate) { workerBusy = false; }
    }

    @Override public Result doWork() {
        synchronized (gate) {
            if (!current()) return Result.success();
            if (!beginWorker()) return Result.retry();
        }
        deadline = (System.nanoTime() / 1000000) + 15000;
        try {
            JSONObject job = new JSONObject(getInputData().getString("snapshot"));
            File accountFile = new File(job.getString("account_path")).getCanonicalFile();
            String privateRoot = getApplicationContext().getFilesDir().getCanonicalPath() + File.separator;
            if (!accountFile.getPath().startsWith(privateRoot) || !accountFile.getName().equals("kosync.json"))
                return Result.failure();
            byte[] bytes = read(new FileInputStream(accountFile));
            JSONObject account = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            if (!accountMatches(job, account)) return Result.success();
            if (account.getInt("schema") != 1 || !account.getBoolean("automatic")
                    || account.getString("key").isEmpty()) return Result.success();
            String document = job.getString("document");
            Reply reply = request(account, "GET", "/syncs/progress/" + document, null);
            if (reply.retry) return Result.retry();
            JSONObject remote = progress(reply);
            if (remote == null) return Result.failure();
            if (!mayUpload(job, remote, account.getString("device_id"))) return Result.success();
            if (remote.optString("progress").equals(job.getString("progress"))) {
                recordSuccess(account, job);
                return Result.success();
            }
            // Account edits, foreground resume and cancellation invalidate the
            // snapshot. Foreground HTTP cannot run until this exchange ends.
            if (!current() || !accountMatches(job, new JSONObject(new String(
                    read(new FileInputStream(accountFile)), StandardCharsets.UTF_8))))
                return Result.success();
            JSONObject upload = new JSONObject();
            upload.put("document", document).put("progress", job.getString("progress"))
                    .put("percentage", job.getDouble("percentage")).put("device", job.getString("device"))
                    .put("device_id", account.getString("device_id"));
            if (account.optBoolean("send_metadata", false) && job.has("metadata")) {
                JSONObject metadata = new JSONObject();
                JSONObject saved = job.getJSONObject("metadata");
                for (String field : new String[] { "filename", "title", "authors" })
                    if (!saved.optString(field).isEmpty()) metadata.put(field, saved.getString(field));
                if (metadata.length() != 0) upload.put("metadata", metadata);
            }
            Reply pushed = request(account, "PUT", "/syncs/progress", upload.toString());
            if (pushed.retry) return Result.retry();
            if (!acknowledged(pushed, document))
                return Result.failure();
            recordSuccess(account, job);
            return Result.success();
        } catch (java.io.IOException networkOrStorageFailure) {
            return Result.retry();
        } catch (Exception invalidData) {
            return Result.failure();
        } finally {
            endWorker();
        }
    }

    private void recordSuccess(JSONObject account, JSONObject job) throws Exception {
        String identity = account.getString("server") + "\u0000" + account.getString("username") + "\u0000"
                + account.getString("device_id") + "\u0000" + hex(MessageDigest.getInstance("SHA-256")
                    .digest(account.getString("key").getBytes(StandardCharsets.US_ASCII)));
        // Only informational state, separate from the reader's exclusive profile.
        preferences(getApplicationContext()).edit().putLong(successKey(identity), System.currentTimeMillis())
                .putString(deliveryKey(job), job.getString("progress")).commit();
    }

    static String deliveryKey(JSONObject job) throws Exception {
        JSONObject account = job.getJSONObject("account");
        return "delivered:" + successKey(account.getString("server") + "\u0000"
                + account.getString("username") + "\u0000" + account.getString("device_id") + "\u0000"
                + account.getString("key_sha256") + "\u0000" + job.getString("document"));
    }

    public static long lastSync(Context context, String identity) {
        return preferences(context).getLong(successKey(identity), 0);
    }

    static String successKey(String identity) {
        try {
            return "success:" + hex(MessageDigest.getInstance("SHA-256")
                    .digest(identity.getBytes(StandardCharsets.UTF_8)));
        } catch (java.security.NoSuchAlgorithmException unavailable) {
            throw new IllegalStateException(unavailable);
        }
    }

    static boolean accountMatches(JSONObject job, JSONObject account) throws Exception {
        JSONObject expected = job.getJSONObject("account");
        for (String field : new String[] { "server", "username", "device_id" })
            if (!expected.getString(field).equals(account.getString(field))) return false;
        return expected.getString("key_sha256").equals(hex(MessageDigest.getInstance("SHA-256")
                    .digest(account.getString("key").getBytes(StandardCharsets.US_ASCII))))
                && expected.getString("device_name").equals(account.optString("device_name", ""))
                && expected.getBoolean("automatic") == account.getBoolean("automatic")
                && expected.getString("match_method").equals(account.optString("match_method", "binary"))
                && expected.getBoolean("send_metadata") == account.optBoolean("send_metadata", false);
    }

    // Be stricter than an interactive sync: a background worker cannot ask
    // which position to keep. Changed remote evidence leaves that choice to
    // the foreground reader, including changes made by this same device.
    static boolean mayUpload(JSONObject job, JSONObject remote, String device) throws Exception {
        JSONObject baseline = job.getJSONObject("baseline");
        if (baseline.optBoolean("pending", true)) return false;
        boolean found = remote.has("progress");
        if (!found) return !baseline.getBoolean("found") && baseline.getString("uploaded").isEmpty()
                && job.optString("delivered").isEmpty();
        String position = remote.getString("progress");
        if (position.equals(job.getString("progress"))) return true;
        if (remote.optString("device_id").equals(device)
                && (position.equals(baseline.getString("uploaded"))
                    || position.equals(job.optString("delivered")))) return true;
        return baseline.getBoolean("found") && position.equals(baseline.getString("position"))
                && remote.optString("device_id").equals(baseline.getString("device_id"))
                && remote.optLong("timestamp", 0) == baseline.getLong("timestamp");
    }

    static boolean acknowledged(Reply reply, String document) throws Exception {
        if (reply.status != 200) return false;
        JSONObject response = new JSONObject(reply.body);
        if (response.has("document"))
            return document.equals(response.opt("document"));
        return response.length() == 1 && "progress updated".equals(response.opt("status"));
    }

    static JSONObject progress(Reply reply) throws Exception {
        if (reply.status == 404) {
            JSONObject object = new JSONObject(reply.body);
            boolean missing = object.length() == 1 && "not found".equals(object.opt("status"));
            boolean grimmoryMissing = object.length() == 3 && object.opt("status") instanceof Number
                    && ((Number) object.opt("status")).doubleValue() == 404
                    && "No progress found for user and book".equals(object.opt("message"))
                    && object.opt("timestamp") instanceof String && !object.getString("timestamp").isEmpty();
            return missing || grimmoryMissing ? new JSONObject() : null;
        }
        if (reply.status != 200) return null;
        if (reply.body.trim().equals("[]")) return new JSONObject();
        JSONObject object = new JSONObject(reply.body);
        if (object.length() == 0) return object;
        Object position = object.opt("progress"), percent = object.opt("percentage");
        // A Grimmory book row can exist before any remote reading state.
        if (position == JSONObject.NULL && percent == JSONObject.NULL
                && object.opt("timestamp") == JSONObject.NULL
                && object.opt("document") instanceof String
                && object.getString("document").matches("[0-9a-f]{32}")
                && object.opt("device") instanceof String && object.opt("device_id") instanceof String)
            return new JSONObject();
        if (!(position instanceof String) || ((String) position).isEmpty() || ((String) position).length() > 4096
                || !(percent instanceof Number) || !Double.isFinite(((Number) percent).doubleValue())
                || ((Number) percent).doubleValue() < 0 || ((Number) percent).doubleValue() > 1
                || !(object.opt("device") instanceof String)
                || (object.has("device_id") && !(object.opt("device_id") instanceof String))) return null;
        if (object.has("timestamp") && object.opt("timestamp") != JSONObject.NULL) {
            Object stamp = object.opt("timestamp");
            if (!(stamp instanceof Number) || !Double.isFinite(((Number) stamp).doubleValue())
                    || ((Number) stamp).doubleValue() < 0 || ((Number) stamp).doubleValue() > 1e12) return null;
        }
        return object;
    }

    static final class Reply {
        int status;
        String body;
        boolean retry;
    }

    private Reply request(JSONObject account, String method, String path, String body) throws Exception {
        return request(account, method, path, body, this::current, deadline);
    }

    static Reply request(JSONObject account, String method, String path, String body,
                         BooleanSupplier current, long deadline) throws Exception {
        if (!current.getAsBoolean()) throw new IllegalStateException("Cancelled sync");
        if (System.nanoTime() / 1000000 >= deadline)
            throw new java.net.SocketTimeoutException("Sync deadline");
        URI base = new URI(account.getString("server"));
        if (!("https".equals(base.getScheme()) || "http".equals(base.getScheme()))
                || base.getHost() == null || base.getUserInfo() != null || base.getQuery() != null
                || base.getFragment() != null) throw new IllegalArgumentException("Invalid server");
        String prefix = base.getPath() == null ? "" : base.getPath().replaceAll("/+$", "");
        URI uri = new URI(base.getScheme(), null, base.getHost(), base.getPort(), prefix + path, null, null);
        HttpURLConnection connection = (HttpURLConnection) uri.toURL().openConnection();
        try {
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(5000);
            connection.setReadTimeout(5000);
            connection.setRequestMethod(method);
            connection.setRequestProperty("Accept", "application/vnd.koreader.v1+json");
            connection.setRequestProperty("x-auth-user", account.getString("username"));
            connection.setRequestProperty("x-auth-key", account.getString("key"));
            if (body != null) {
                byte[] data = body.getBytes(StandardCharsets.UTF_8);
                connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", "application/json");
                connection.setFixedLengthStreamingMode(data.length);
                try (java.io.OutputStream output = connection.getOutputStream()) { output.write(data); }
            }
            Reply result = new Reply();
            result.status = connection.getResponseCode();
            result.retry = result.status == 429 || result.status >= 500;
            InputStream stream = result.status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (stream == null) result.body = "";
            else try (InputStream input = stream; ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (!current.getAsBoolean()) throw new IllegalStateException("Cancelled sync");
                    if ((System.nanoTime() / 1000000) >= deadline)
                        throw new java.net.SocketTimeoutException("Sync deadline");
                    if (bytes.size() + count > 16384) throw new IllegalArgumentException("Oversized sync data");
                    bytes.write(buffer, 0, count);
                }
                result.body = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            }
            return result;
        } finally { connection.disconnect(); }
    }

    private static byte[] read(InputStream source) throws java.io.IOException {
        try (InputStream input = source; ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (bytes.size() + count > 16384) throw new IllegalArgumentException("Oversized sync data");
                bytes.write(buffer, 0, count);
            }
            return bytes.toByteArray();
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }
}
