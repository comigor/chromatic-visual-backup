#!/usr/bin/env python3
"""Standalone build for the Chromatic visual backup app.

Produces a signed APK with Android SDK/NDK tools and a JDK. No Gradle.

Usage:
    python3 build.py [--sdk /path/to/android/sdk] [--out out.apk]

Defaults: SDK from $ANDROID_HOME / $ANDROID_SDK_ROOT or ~/Library/Android/sdk,
platform android-35, build-tools 35.0.0, NDK 28.2.13676358.
Output: build/ChromaticVisual.apk
(signed with a generated debug key stored in build/debug.keystore).
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import platform
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK_DEFAULT = os.path.expanduser("~/Library/Android/sdk")
PLATFORM = "android-35"
BUILD_TOOLS = "35.0.0"
MIN_SDK = 24
TARGET_SDK = 35
NDK_VERSION = "28.2.13676358"

KEYSTORE = "debug.keystore"
KS_ALIAS = "androiddebugkey"
KS_PASS = "android"


def fail(msg):
    print("build.py: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def find_sdk(arg_sdk):
    sdk = arg_sdk or os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") \
        or SDK_DEFAULT
    if not os.path.isdir(sdk):
        fail("Android SDK not found at %r (pass --sdk or set ANDROID_HOME)" % sdk)
    return sdk


def find_java():
    java_home = os.environ.get("JAVA_HOME")
    if java_home and os.path.isfile(os.path.join(java_home, "bin", "javac")):
        return os.path.join(java_home, "bin")
    for javac in ("javac", "keytool"):
        if shutil.which(javac) is None:
            fail("%s not found on PATH and JAVA_HOME unset (a JDK is required)" % javac)
    return None  # use PATH


def tool(path_list, name):
    p = os.path.join(*path_list, name)
    if not os.path.exists(p):
        fail("missing build tool: %s" % p)
    return p


def build_native(sdk, build):
    host = {"Darwin": "darwin-x86_64", "Linux": "linux-x86_64"}.get(platform.system())
    if host is None:
        fail("native capture build supports macOS and Linux hosts")
    toolchain = os.path.join(sdk, "ndk", NDK_VERSION, "toolchains", "llvm", "prebuilt", host, "bin")
    libraries = []
    for abi, target in (("arm64-v8a", "aarch64-linux-android"),
                        ("x86_64", "x86_64-linux-android")):
        compiler = tool([toolchain], target + str(MIN_SDK) + "-clang")
        directory = os.path.join(build, "native", abi)
        os.makedirs(directory, exist_ok=True)
        library = os.path.join(directory, "libchromatic_uvc.so")
        run([compiler, "-std=c11", "-O2", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
             "-Wl,-z,max-page-size=16384", "-o", library,
             os.path.join(HERE, "native", "uvc_capture.c")])
        libraries.append((library, "lib/" + abi + "/libchromatic_uvc.so"))
    return libraries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", help="Android SDK root")
    ap.add_argument("--out", default=os.path.join("build", "ChromaticVisual.apk"))
    args = ap.parse_args()

    sdk = find_sdk(args.sdk)
    bt_dir = os.path.join(sdk, "build-tools")
    if not os.path.isdir(os.path.join(bt_dir, BUILD_TOOLS)):
        versions = sorted(os.listdir(bt_dir)) if os.path.isdir(bt_dir) else []
        fail("build-tools %s not found (have: %s)" % (BUILD_TOOLS, ", ".join(versions) or "none"))
    bt = os.path.join(bt_dir, BUILD_TOOLS)

    android_jar = os.path.join(sdk, "platforms", PLATFORM, "android.jar")
    if not os.path.isfile(android_jar):
        fail("platform %s not found (need %s)" % (PLATFORM, android_jar))

    java_bin = find_java()
    javac = os.path.join(java_bin, "javac") if java_bin else "javac"
    keytool = os.path.join(java_bin, "keytool") if java_bin else "keytool"
    aapt2 = tool([bt], "aapt2")
    d8 = tool([bt], "d8")
    zipalign = tool([bt], "zipalign")
    apksigner = tool([bt], "apksigner")

    build = os.path.join(HERE, "build")
    for name in ("compiled", "classes", "dex", "gen", "native"):
        directory = os.path.join(build, name)
        if os.path.exists(directory):
            shutil.rmtree(directory)
    for d in ("compiled", "classes", "dex", "gen", "native"):
        os.makedirs(os.path.join(build, d), exist_ok=True)
    gen = os.path.join(build, "gen")
    out_apk = os.path.abspath(os.path.join(HERE, args.out))

    # 1. Compile resources.
    res_zip = os.path.join(build, "compiled", "res.zip")
    run([aapt2, "compile", "--dir", os.path.join(HERE, "res"), "-o", res_zip])

    # 2. Link resources + manifest into a resource-only APK.
    unsigned = os.path.join(build, "app.unsigned.apk")
    run([aapt2, "link", "-o", unsigned,
         "-I", android_jar,
         "--manifest", os.path.join(HERE, "AndroidManifest.xml"),
         "--java", gen,
         "--min-sdk-version", str(MIN_SDK),
         "--target-sdk-version", str(TARGET_SDK),
         "--version-code", "3", "--version-name", "2.1",
         res_zip])

    # 3. Compile Java sources (app + generated R) against android.jar.
    srcs = sorted(glob.glob(os.path.join(HERE, "src", "dev", "borges", "chromaticproof",
                                         "*.java")))
    if not srcs:
        fail("no Java sources found under src/")
    r_java = glob.glob(os.path.join(gen, "**", "R.java"), recursive=True)
    classes = os.path.join(build, "classes")
    run([javac, "--release", "11", "-Xlint:all",
         "-classpath", android_jar,
         "-d", classes] + srcs + r_java)

    # 4. Dex the classes.
    class_files = []
    for root, _dirs, files in os.walk(classes):
        for f in files:
            if f.endswith(".class"):
                class_files.append(os.path.join(root, f))
    if not class_files:
        fail("javac produced no .class files")
    run([d8, "--release", "--min-api", str(MIN_SDK),
         "--lib", android_jar,
         "--output", os.path.join(build, "dex")] + class_files)

    # 5. Add classes.dex into the APK.
    native_libraries = build_native(sdk, build)
    with zipfile.ZipFile(unsigned, "a") as zf:
        zf.write(os.path.join(build, "dex", "classes.dex"), "classes.dex")
        for library, archive_path in native_libraries:
            zf.write(library, archive_path)

    # 6. zipalign (must run before apksigner so v2/v3 signatures stay valid).
    aligned = os.path.join(build, "app.aligned.apk")
    run([zipalign, "-f", "4", unsigned, aligned])

    # 7. Generate (once) and sign with a debug key.
    ks = os.path.join(build, KEYSTORE)
    if not os.path.exists(ks):
        run([keytool, "-genkeypair", "-keystore", ks,
             "-storepass", KS_PASS, "-keypass", KS_PASS,
             "-alias", KS_ALIAS,
             "-dname", "CN=Android Debug,O=Android,C=US",
             "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000"])
    run([apksigner, "sign",
         "--ks", ks, "--ks-pass", "pass:" + KS_PASS,
         "--key-pass", "pass:" + KS_PASS, "--ks-key-alias", KS_ALIAS,
         "--out", out_apk, aligned])
    run([apksigner, "verify", "--verbose", out_apk])

    print("OK: " + out_apk)
    print("install with: adb install -r " + out_apk)


if __name__ == "__main__":
    main()
