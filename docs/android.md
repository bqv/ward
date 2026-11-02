# The Android client

There are two clients. **`clients/android`** is the real one: project
ward-android, application Ward, a Kotlin implementation of the frozen protocol
behind the copied dsh-android screens. Its own notes and its list of missing
parity live in `clients/android/README.md`; the JNI client below is kept as the
reference for the C library path.

The JNI client lives in **`clients/android-ward`**: the JNI layer (`ward_jni.c`), the
Java wrapper (`uk/xa0/ward/WardClient.java`), and a minimal app
(`app/AndroidManifest.xml`, `app/src/uk/xa0/ward/MainActivity.java`).

`clients/android` is a different project — a DSH Jetpack Compose client that
landed in this tree separately and shares nothing with this one but the
directory name that used to hold it. The two are kept apart deliberately: this
client speaks ward's protocol through the JNI layer, that one reimplements the
DSH web client against a DSH host.

The transport is loopback TCP. Start the daemon with a listener the phone can
reach through `adb reverse`:

```
WARD_TCP_BIND=127.0.0.1:45872 ./out/ward up --socket /run/user/$(id -u)/ward.sock --state ~/.local/state/ward
adb -s <serial> reverse tcp:45872 tcp:45872
```

`adb reverse` makes the device's `127.0.0.1:45872` arrive at the host's listener
from adbd, which ward accepts because the peer is loopback and the listener is
the TCP one. The Unix socket keeps its stronger `SO_PEERCRED` check.

## Build

```
NDK=/opt/android-sdk/ndk/29.0.14206865
SDK=/opt/android-sdk
BT=$SDK/build-tools/36.0.0
JAR=$SDK/platforms/android-34/android.jar
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang
```

1. **libcbor for the device** — the one dependency the client does not carry:

```
tar xf /var/cache/distfiles/libcbor-0.14.0.tar.gz -C ~/tmp/wardjni/src
cmake -S ~/tmp/wardjni/src/libcbor-0.14.0 -B ~/tmp/wardjni/src/libcbor-0.14.0/build-arm64 \
  -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DBUILD_SHARED_LIBS=OFF \
  -DCBOR_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$HOME/tmp/wardjni/prefix-arm64
cmake --build .../build-arm64 && cmake --install .../build-arm64
```

2. **the JNI library** (API 28, because `getrandom` is what core asks for):

```
$CC -std=c11 -D_GNU_SOURCE -fPIC -shared -Isrc -I$HOME/tmp/wardjni/prefix-arm64/include \
  clients/android/ward_jni.c src/client/client.c src/proto/codec.c src/proto/frame.c \
  src/core/util.c -L$HOME/tmp/wardjni/prefix-arm64/lib -lcbor \
  -o out/android/arm64-v8a/libwardclient.so
```

3. **the APK** — `aapt2 compile`/`link` against the manifest (which needs
`<uses-sdk>` or the installer refuses it), `javac -bootclasspath $JAR`,
`d8` (run it through JDK 21: the build-tools' d8 is class file 55 and the
system `java` is 8), then zip `classes.dex` and the `.so`, `zipalign`, and
`apksigner` with a debug key.

## Verified

On the disposable handset (`91e25acb`):

```
adb install -r -d ward-signed.apk
adb reverse tcp:45872 tcp:45872
adb shell am start -W -S -n uk.xa0.ward/.MainActivity
adb logcat -s ward
I ward    : session dd8ae525d9994557
```

A real session, created by the phone over the socket — the M6 acceptance line.

## Traps met on the way

- the manifest needs an explicit `targetSdkVersion`, or the installer fails with
  `INSTALL_FAILED_DEPRECATED_SDK_VERSION`;
- `d8` from build-tools 36 is class file 55 and will not run under the system
  `java` 8 — invoke it through JDK 21;
- `javac` cannot write into `~/tmp` (SELinux: `java_t` may not write
  `user_tmp_t`); build into `out/`;
- `getrandom` needs API 28, so the JNI library is built for `android28`, while
  libcbor can stay at 26.
