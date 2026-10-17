# Android reader

This target builds the standalone Android app from the same reader, UI, EPUB inspection and storage code as the Kobo targets. Android supplies document import, keyboard, orientation and battery services. It does not link Nickel.

The opening screen offers Choose book and Exit. Choose book launches the Android document picker. It can also receive an EPUB from another app through Android's Open with menu. It copies the selected document into app-private storage, with a 128 MiB limit. While reading, Volume Down and Page Down turn forward; Volume Up and Page Up turn back. On the startup and loading screens, volume buttons keep their Android behaviour. A turn starts on the initial press, with no extra turn on hold or release. Device button settings can change which events Android sends to the app. The opening screen and reader keep their controls inside Android's safe area for camera cutouts and system bars. Insets can change with rotation or when system bars appear; the reader resizes to the available canvas. Settings, bookmarks and reading position are local. Stats uses the shared word counter and parsed-document cache, with the same counting rules as Kobo. Restarting shows the startup screen with a Reopen last book card. Tap it to restore the last successfully opened book and its reading position. Very long title and author labels are shortened for the card without changing the book. There is no recent-books list.

The Android version name follows the source version; the numeric version code defaults to 1 for development and can be set with `--version-code`. Dictionary lookup, frontlight control and device-specific e-ink waveforms are not implemented. This is a development build, not a finished Android release. The reader reads EPUB resources locally. Android's INTERNET permission is used for dictionary downloads. It does not connect to Kobo accounts or sync services.

## Toolchain

The build uses these versions:

| Input | Version |
|---|---|
| Qt host tools and Android kit | 6.11.3 |
| Qt modules | Core, Gui, Widgets, Network and Sensors |
| Android SDK / build tools | API 36 / 36.0.0 |
| Android NDK | 27.2.12479018 |
| Minimum device API | 28, Android 9 |
| Default device ABI | arm64-v8a |

Install Python 3.10 or later, CMake, Ninja, pkg-config and patch. On macOS, install OpenJDK 21 with [Homebrew](https://formulae.brew.sh/formula/openjdk@21):

```sh
brew install openjdk@21
```

Install the matching Qt desktop host tools and Android kit; do not use the Kobo-patched Qt SDK. The dependency URLs and SHA-256 pins are in `dependencies/manifest.json` and `android/dependencies.json`.

With Android command-line tools installed, obtain the SDK packages using `sdkmanager`. Review its license prompts:

```sh
sdkmanager 'platforms;android-36' 'build-tools;36.0.0' 'ndk;27.2.12479018' 'platform-tools'
```

Run the following exports in the same terminal as the build command, each time you open a new terminal. You can save these non-secret paths in a local shell setup file and source it before building. For an Apple Silicon macOS installation with Qt under `~/Qt`:

```sh
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/27.2.12479018"
export QT_HOST_PATH="$HOME/Qt/6.11.3/macos"
export QT_ANDROID_ARM64_PATH="$HOME/Qt/6.11.3/android_arm64_v8a"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
```

Debug builds use the shared `android/debug.keystore` in this repository, independently of `ANDROID_USER_HOME`. Keep this file unchanged so builds from different machines can update the same debug installation.

On Linux, use the installed Linux Qt host kit and local SDK/JDK paths. For an x86_64 emulator, install the Qt Android x86_64 kit and set `QT_ANDROID_PATH` to it. The CI container provides the Linux AMD64 toolchain. Other local host setups need validation.

Check that the selected tools exist before building:

```sh
test -f "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
test -x "$QT_HOST_PATH/bin/qt-cmake"
test -x "$QT_ANDROID_ARM64_PATH/bin/qt-cmake"
test -x "$ANDROID_HOME/build-tools/36.0.0/apksigner"
"$JAVA_HOME/bin/java" -version
cmake --version
ninja --version
pkg-config --version
```

Each `test` command succeeds silently. A nonzero exit status means the selected file is missing. A `missing ANDROID_NDK_ROOT` error from the build script means the variable is unset in that terminal; run the export block above. Installing the SDK alone does not set these variables.

## Build

Debug builds use `com.nicoverbruggen.kobalt.debug` and appear as Kobalt on the home screen. Their displayed version ends in `-debug`. Release builds use `com.nicoverbruggen.kobalt`. Both apps can be installed together and keep separate books, settings and reading progress.

Change to the root of your public Kobalt checkout before running any command below. For a debug build:

```sh
python3 android/build.py --jobs 2
```

The debug APK is `build/android/arm64-v8a/app/android-build/kobalt_android.apk`. It is signed with the shared `android/debug.keystore` and is suitable for sideloaded testing. The public debug key uses alias `androiddebugkey` and `android` for both passwords. Its certificate SHA-256 is `874630ab00cefbeb01d39d625b596db4e55e333563d7521478f682cad18466f5`. Anyone with this repository can sign a debug update. GitHub beta APKs use this testing identity. Never use it for the release channel. Debug signing does not need a key from 1Password or `~/.android/`. Android and Kobo apply the same CREngine patches from `dependencies/crengine-patches.txt`. Changes to those patches or the publication policy invalidate cached books and inspection results. The helper uses independent Android build directories, verifies dependency archives before extraction, and rejects unsupported resource-service symbols before packaging. `--offline` requires cached native dependency archives and font assets; it does not put Gradle into offline mode. `--native-only` compiles without packaging an APK. `--abi x86_64` selects the emulator ABI.

Use `--version-code NUMBER` with a number at least as large as the installed debug build. Install onto an explicitly selected test device with `install -r` to preserve its data:

```sh
adb -s DEVICE_SERIAL install -r build/android/arm64-v8a/app/android-build/kobalt_android.apk
```

## GitHub builds and signing policy

GitHub CI builds ARM64 debug APKs on pushes to `main` and `develop`, version tags and manual runs. Download `Kobalt-android-arm64-debug.apk` from the `kobalt-android` artifact, or from a tagged release starting with Beta 4. CI uses the shared public debug key and requires no signing secret. Get these APKs directly from this repository. The public key lets anyone sign a modified APK that Android can accept as an update, so its signature alone does not establish who built it.

Debug builds allow developers to attach a debugger through an authorized Android debugging connection. They use the `.debug` app ID and keep their data separate from release builds. Switching to the release app does not transfer books, settings or reading progress. Keep the testing and release signing identities separate.

CI uses its workflow run number as the numeric version code. A rerun of the same run keeps that code. Each later run gets a larger code. Use a code at least as large as the installed GitHub APK when building a local replacement. The displayed version follows the source version and ends in `-debug`.

The published SDK provides Qt, Android tools and the JDK. From a public checkout with Python 3 and Docker or Podman, build the same APK with:

```sh
python3 tools/ci_android.py --version-code 1 --jobs 2
```

This requires a clean checkout, pulls `ghcr.io/nicoverbruggen/kobuild:android-v0.2.0` and builds in a disposable container. It selects Linux AMD64 even on ARM64 hosts and prints a warning about emulation. Use the native macOS Qt and Android SDKs with `android/build.py` for local Apple Silicon builds when available. `--allow-dirty` permits local validation; release drafts reject those artifacts. `--sdk-image IMAGE --no-pull` selects an already installed image for local SDK validation. Outputs are `build/release/Kobalt-android-arm64-debug.apk` and `build/release/android.json`.

### Build caches

Android builds share dependency caches between worktrees. The default root is `.cache/kobalt/` in the primary checkout's parent directory. For a checkout at `Kobalt/kobalt-oss/`, this is `Kobalt/.cache/kobalt/`. Set `KOBALT_DEPENDENCY_CACHE` to select another root. Application object files stay in each checkout's `build/android/` directory.

| Shared folder | Contents |
|---|---|
| `sources/<SHA-256>/` | Verified dependency source archives. Each checkout extracts its own sources. |
| `artifacts/android-<ABI>-<host>-<architecture>/<dependency>/<key>/` | Compiled libraries, installed headers and build configuration, with file hashes. |
| `gradle/` | Gradle distributions and dependency downloads, managed by Gradle. |

Each compiled dependency key includes its source checksum, patches, compiler, SDK, build options and upstream dependency keys. Checkout paths are excluded. A CREngine patch change rebuilds CREngine while preserving unrelated FreeType, HarfBuzz and OpenSSL artifacts. Missing or damaged artifacts are rebuilt. Installation checks file contents and preserves timestamps for unchanged files. This avoids unnecessary downstream compilation. OpenSSL builds only the shared TLS libraries packaged in the APK.

Native builds use the shared Gradle folder unless `GRADLE_USER_HOME` is set. The build imports the previous checkout-local `.cache/android-gradle/` folder when the shared folder does not exist. Reading fonts remain in the verified checkout-local `vendor/ebook-fonts/` folder.

The CI helper copies Linux dependency artifacts into the disposable SDK container and saves them afterwards, including after a build failure. It keeps application objects disposable. GitHub Actions restores dependency artifacts from earlier commits, while the dependency keys and file hashes determine what can be reused. macOS artifacts and Linux artifacts remain separate. Source bundles still contain the required source archives rather than compiled cache entries.

Each dependency prints whether it was built or reused and its elapsed time. `build/android/<ABI>/dependency-timings.json` records those results. The CI artifact record also includes build duration and dependency results. Compare an unchanged rebuild, a source edit and a dependency edit separately when measuring speed. A successful compiler cache hit does not replace APK verification or runtime tests.

The helper verifies the package ID, debug flag, version, SDK levels, ABI, shared signing certificate, signature and 16 KiB library alignment before exporting the APK. Distribute the matching `Kobalt-source.tar.gz` artifact with it. The source bundle includes Android's dependency versions and the Qt Base and Sensors sources that match the SDK.

F-Droid distribution is not integrated. A future release channel must use a private signing identity. It must never use the shared debug key.

## Release signing

The APK is signed, not the source checkout or Git commit. Release signing uses your Android private key. Git commit signing is separate.

### Restore an existing key

Restore your `.p12` key from secure storage to a folder outside the repository, for example `$HOME/kobalt-signing/kobalt-release.p12`. Keep its passphrase in a password manager, such as 1Password. The commands below use that example path; change it if you store the key elsewhere.

```sh
chmod 600 "$HOME/kobalt-signing/kobalt-release.p12"
"$JAVA_HOME/bin/keytool" -list -v \
  -keystore "$HOME/kobalt-signing/kobalt-release.p12" \
  -storetype PKCS12 -alias kobalt-release
```

Enter the passphrase at the prompt. Check the alias and certificate SHA-256 fingerprint against your saved record. If the key has a different alias, use that alias in both this command and `--key-alias` below. Do not paste the passphrase into a shell command, source file or chat. Keep a separate secure backup of the key and test that the backup opens. Future updates need the same signing identity.

### Generate a key only if none exists

Skip this step if you already have a signing key. Do not generate a replacement key for each build or machine. The following creates a PKCS#12 key outside the repository and prompts for its passphrase:

```sh
(
  umask 077
  mkdir -p "$HOME/kobalt-signing" &&
  "$JAVA_HOME/bin/keytool" -genkeypair \
    -keystore "$HOME/kobalt-signing/kobalt-release.p12" \
    -storetype PKCS12 -alias kobalt-release \
    -keyalg RSA -keysize 3072 -sigalg SHA256withRSA \
    -validity 10000 -dname "CN=Kobalt"
)
```

Save the file, passphrase, alias and certificate SHA-256 fingerprint in your password manager. Verify a copy downloaded from the password manager with the `keytool -list` command before relying on it.

### Build and sign

To prepare an unsigned release build:

```sh
python3 android/build.py --release --jobs 2
```

Release outputs use `build/android/arm64-v8a-release/`.

To build and sign with a PKCS#12 key exported from your password manager, run this in your terminal:

```sh
python3 android/build.py --release --jobs 2 \
  --keystore "$HOME/kobalt-signing/kobalt-release.p12" --key-alias kobalt-release
```

The script asks for the passphrase without echoing it. It passes the passphrase to Android's signing tool through standard input, not a command argument or file. This expects the same password for the PKCS#12 store and private key. Keep the file outside the checkout. The alias defaults to `kobalt-release`.

To sign an existing release APK without rebuilding:

```sh
python3 android/sign.py \
  build/android/arm64-v8a-release/app/android-build/kobalt_android.apk \
  --keystore "$HOME/kobalt-signing/kobalt-release.p12"
```

Both commands align native libraries to 16 KiB boundaries before signing, then verify the signature and alignment. The verified output is `build/android/arm64-v8a-release/app/android-build/kobalt_android-signed.apk`. A signing or verification failure preserves any previous signed output. The certificate fingerprint is printed for comparison with your saved fingerprint. No key or passphrase is copied into the build directory. The scripts do not retrieve secrets from a password manager.

### Verify and install

The signing script already verifies the APK. To repeat the checks manually:

```sh
"$ANDROID_HOME/build-tools/36.0.0/apksigner" verify --verbose --print-certs \
  build/android/arm64-v8a-release/app/android-build/kobalt_android-signed.apk
"$ANDROID_HOME/build-tools/36.0.0/zipalign" -c -P 16 4 \
  build/android/arm64-v8a-release/app/android-build/kobalt_android-signed.apk
"$ANDROID_HOME/build-tools/36.0.0/aapt2" dump badging \
  build/android/arm64-v8a-release/app/android-build/kobalt_android-signed.apk
```

Check that the package is `com.nicoverbruggen.kobalt`, that the version is the one you intend to distribute, and that the signer certificate SHA-256 fingerprint matches your key. Do not modify the APK after signing. A failed signing command leaves any previous signed APK intact; do not mistake that older file for a new build.

Enable USB debugging on the test device and authorize this computer on the device. Select the device explicitly:

```sh
adb devices -l
adb -s DEVICE_SERIAL install -r \
  build/android/arm64-v8a-release/app/android-build/kobalt_android-signed.apk
```

Replace `DEVICE_SERIAL` with the serial reported by `adb devices`. A signature mismatch means the installed app uses another key, such as a debug key. A release-signed APK cannot update that installation. Uninstalling the old app removes its app-private books, settings and reading progress, so preserve needed data before choosing that route. Do not change the release key just to match a temporary debug build.

### Prepare subsequent releases

The application ID stays `com.nicoverbruggen.kobalt`. The displayed version comes from `KOBALT_RELEASE_VERSION`, an extracted source bundle, an exact Git version tag, or the root `VERSION` file, in that order. Android's numeric version code defaults to 1; changing the displayed version does not increment it. Pass `--version-code NUMBER` to `android/build.py` for each release. Use a number greater than every previously distributed release, between 1 and 2100000000. The helper sets the `QT_ANDROID_VERSION_CODE` target property through `KOBALT_ANDROID_VERSION_CODE` and does not increment it automatically. Confirm the packaged value with `aapt2 dump badging`.

Never commit a release keystore or its passphrase. The shared debug keystore and its standard passwords are intentionally public. Use the same signing key for future updates and increase the version code for each release. Use the public debug key only for the clearly labelled GitHub testing channel described above. Before distribution, validate the signed APK on target hardware and provide the corresponding source and dependency sources described in the root README.

See Android's [app signing guide](https://developer.android.com/studio/publish/app-signing), [apksigner reference](https://developer.android.com/tools/apksigner) and [versioning guide](https://developer.android.com/studio/publish/versioning) for the platform requirements.

## Validation

Hardware refresh quality and physical page-button mapping need device tests; an Android emulator does not reproduce an e-reader's display driver.

Qt accessibility currently has limited support for the embedded reader. After a reader session closes, the opening screen can remain visible without its buttons appearing in the Android accessibility tree. Touch input still works.

## Icon

The adaptive launcher icon uses an outlined open book with a small bookmark and an Instrument Serif K. Android chooses the outer mask. Android 13 and later can use its monochrome layer for themed icons.

[Promotional SVG](../docs/assets/kobalt-android.svg) contains the rounded-square version. All lettering is converted to paths, so it renders without an installed font. The matching Android vector is `app/res/drawable/kobalt_mark.xml`. Both use the same book, bookmark and K paths; keep those paths together when editing the artwork.

Android builds compile and package pinned OpenSSL 3.5.9 libraries for HTTPS dictionary downloads. Qt’s TLS plugin uses `libssl_3.so` and `libcrypto_3.so` from the APK. Certificate verification remains enabled. The libraries support 16 KiB Android memory pages.
