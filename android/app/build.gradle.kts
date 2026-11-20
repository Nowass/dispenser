plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.ksp)
    alias(libs.plugins.androidx.room)
}

/** Bundles the repository's CHANGELOG.md (single source of truth) into the app assets. */
abstract class BundleChangelogTask : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val changelog: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun bundle() {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        changelog.get().asFile.copyTo(File(out, "CHANGELOG.md"), overwrite = true)
    }
}

val bundleChangelog = tasks.register<BundleChangelogTask>("bundleChangelog") {
    changelog.set(layout.projectDirectory.file("../../CHANGELOG.md"))
}

androidComponents {
    // Release host (JVM) tests exist only for the build-type checks in src/testRelease (the device-feature
    // flag); the full suite runs once, on debug (testReleaseUnitTest is filtered below).
    beforeVariants(selector().withBuildType("release")) { variant ->
        (variant as com.android.build.api.variant.HasHostTestsBuilder)
            .hostTests[com.android.build.api.variant.HostTestBuilder.UNIT_TEST_TYPE]?.enable = true
    }
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(bundleChangelog, BundleChangelogTask::outputDir)
    }
}

apply(from = "third-party-licenses.gradle.kts")

/** Public GitHub Releases source (configured in gradle.properties; no credentials anywhere). */
val githubOwner = providers.gradleProperty("dispenser.github.owner").get()
val githubRepo = providers.gradleProperty("dispenser.github.repo").get()
val githubSourceLinkEnabled = providers.gradleProperty("dispenser.github.sourceLinkEnabled").get()

/**
 * Optional local release signing (A1.5). Values come from Gradle properties outside the
 * repository, normally `~/.gradle/gradle.properties`:
 *
 *   dispenser.signing.storeFile=/absolute/path/to/dispenser-release.p12
 *   dispenser.signing.storePassword=…
 *   dispenser.signing.keyAlias=dispenser-release
 *   dispenser.signing.keyPassword=…
 *
 * Without them the release build stays unsigned (CI, pull requests). A partial configuration
 * fails the build instead of silently producing an unsigned APK. See docs/RELEASING.md.
 */
val releaseSigningKeys = listOf("storeFile", "storePassword", "keyAlias", "keyPassword")
val releaseSigning: Map<String, String>? = releaseSigningKeys
    .associateWith { providers.gradleProperty("dispenser.signing.$it").orNull?.takeIf(String::isNotBlank) }
    .let { values ->
        val missing = values.filterValues { it == null }.keys
        when {
            missing.isEmpty() -> values.mapValues { it.value!! }
            missing.size == releaseSigningKeys.size -> null
            // Names only; never print configured values.
            else -> throw GradleException("Incomplete release signing configuration, missing: " + missing.joinToString { "dispenser.signing.$it" })
        }
    }

room {
    // Exported schema JSON is committed so future migrations can be written and tested.
    schemaDirectory("$projectDir/schemas")
}

android {
    namespace = "cz.nowass.dispenser"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "cz.nowass.dispenser"
        minSdk = 26
        targetSdk = 37
        // SemVer versionName; versionCode strictly increasing for every published APK (docs/RELEASING.md).
        versionCode = 6
        versionName = "0.5.0-beta.2"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // M1.10e.1: native Noise provider (libdspnoise). arm64-v8a and armeabi-v7a for phones,
        // x86_64 for emulators; x86 is not shipped (decision E16).
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
            }
        }

        resValue("string", "github_owner", githubOwner)
        resValue("string", "github_repo", githubRepo)
        resValue("bool", "github_source_link_enabled", githubSourceLinkEnabled)
    }

    signingConfigs {
        if (releaseSigning != null) {
            create("release") {
                storeFile = file(releaseSigning.getValue("storeFile"))
                storeType = "pkcs12"
                storePassword = releaseSigning.getValue("storePassword")
                keyAlias = releaseSigning.getValue("keyAlias")
                keyPassword = releaseSigning.getValue("keyPassword")
                // minSdk 26: APK Signature Scheme v2 + v3 only.
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        debug {
            // Separate app next to the release install ("Dispenser DEBUG", src/debug/res).
            applicationIdSuffix = ".debug"
            // SemVer build metadata: ignored by the update checker's version comparison.
            versionNameSuffix = "+debug"
            // BLE device features (pairing, invitations, phones): debug only until hardware acceptance
            // (v0.5.0-beta.2 decision A1). Off: Settings keeps the disabled "coming later" placeholder.
            resValue("bool", "device_features_enabled", "true")
            // BENCH ONLY tools (pump configuration and readiness for the GPIO bring-up): debug builds only.
            resValue("bool", "bench_tools_enabled", "true")
        }
        release {
            resValue("bool", "device_features_enabled", "false")
            resValue("bool", "bench_tools_enabled", "false")
            if (releaseSigning != null) signingConfig = signingConfigs.getByName("release")
            optimization {
                enable = true
                packageScope = setOf("androidx.**", "kotlin.**", "kotlinx.**")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    // M1.10e.1: pinned NDK and SDK CMake (docs/ANDROID_BLE_DESIGN.md section 9).
    ndkVersion = "30.0.16248370"
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }
    sourceSets {
        // Shared vectors for the instrumented JNI test (read from assets on the device/emulator).
        getByName("androidTest").assets.srcDir(rootProject.file("../test-vectors"))
    }
    buildFeatures {
        compose = true
        resValues = true
    }
    testOptions {
        // Robolectric (Room tests on the JVM) needs merged Android resources.
        unitTests.isIncludeAndroidResources = true
    }
}

// The shared test vectors (repository root test-vectors/: recurrence, BLE protocol) live outside
// this Gradle project; declare them as test inputs so that a changed vector reruns the tests.
tasks.withType<Test>().configureEach {
    inputs.dir(rootProject.file("../test-vectors")).withPathSensitivity(PathSensitivity.RELATIVE)
}

// Release host tests: only the build-type checks (src/testRelease); everything else runs on debug.
tasks.withType<Test>().matching { it.name == "testReleaseUnitTest" }.configureEach {
    filter.includeTestsMatching("cz.nowass.dispenser.buildtype.*")
}

// Robolectric's Android framework jar is resolved by Gradle (pinned in the version catalog;
// Gradle's retries, checksums and the CI dependency cache apply) instead of being downloaded by
// Robolectric at test time. The tests run Robolectric offline against build/robolectric-deps; a
// Robolectric upgrade that needs another jar fails with "Unable to locate dependency".
val robolectricRuntime: Configuration by configurations.creating {
    isTransitive = false
    isCanBeConsumed = false
    isCanBeResolved = true
}
val robolectricDepsDir = layout.buildDirectory.dir("robolectric-deps")
val prepareRobolectricRuntime = tasks.register<Sync>("prepareRobolectricRuntime") {
    from(robolectricRuntime)
    into(robolectricDepsDir)
}
tasks.withType<Test>().configureEach {
    inputs.files(prepareRobolectricRuntime).withPropertyName("robolectricRuntime")
        .withPathSensitivity(PathSensitivity.NAME_ONLY)
    systemProperty("robolectric.offline", "true")
    systemProperty("robolectric.dependency.dir", robolectricDepsDir.get().asFile.absolutePath)
}

// ---- M1.10e.1: native test builds (never part of the APK) -----------------------------------
// The JVM unit tests load libdspnoise_hosttest: the app's JNI sources plus test hooks (responder
// role, fixed ephemerals), built for this host with the pinned SDK CMake and the system C compiler.
// `nativeAndroidTest` builds the vector runner with the NDK (x86_64, static, bionic + FORTIFY) and
// runs it on the Linux host (CI). Downloads (pinned noise-c/libsodium) are cached in build/dsp_deps.
val sdkDir = androidComponents.sdkComponents.sdkDirectory
val cmakeExe = sdkDir.map { it.file("cmake/3.31.6/bin/cmake").asFile.absolutePath }
val ninjaExe = sdkDir.map { it.file("cmake/3.31.6/bin/ninja").asFile.absolutePath }
val ndkDir = sdkDir.map { it.dir("ndk/30.0.16248370").asFile.absolutePath }
val nativeTestSrc = layout.projectDirectory.dir("src/test/native")
val hostNativeDir = layout.buildDirectory.dir("native-test/host")
val ndkNativeDir = layout.buildDirectory.dir("native-test/android-x86_64")
val depsCache = layout.buildDirectory.dir("dsp_deps")
val testJdk = javaToolchains.launcherFor { languageVersion.set(JavaLanguageVersion.of(25)) }
    .map { it.metadata.installationPath.asFile.absolutePath }

val configureHostNativeTest = tasks.register<Exec>("configureHostNativeTest") {
    description = "Configures the host build of the native Noise test library (M1.10e.1)."
    inputs.dir(nativeTestSrc)
    inputs.dir(layout.projectDirectory.dir("src/main/cpp"))
    outputs.file(hostNativeDir.map { it.file("build.ninja") })
    environment("DSP_DEPS_CACHE", depsCache.get().asFile.absolutePath)
    commandLine(cmakeExe.get(), "-S", nativeTestSrc.asFile.absolutePath, "-B", hostNativeDir.get().asFile.absolutePath,
        "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=${ninjaExe.get()}", "-DCMAKE_BUILD_TYPE=Release",
        "-DDSP_JDK_HOME=${testJdk.get()}")
}
val buildHostNativeTest = tasks.register<Exec>("buildHostNativeTest") {
    description = "Builds libdspnoise_hosttest and the native vector runner for this host (M1.10e.1)."
    dependsOn(configureHostNativeTest)
    inputs.dir(nativeTestSrc)
    inputs.dir(layout.projectDirectory.dir("src/main/cpp"))
    outputs.file(hostNativeDir.map { it.file("libdspnoise_hosttest.so") })
    commandLine(cmakeExe.get(), "--build", hostNativeDir.get().asFile.absolutePath)
}
val hostNativeVectors = tasks.register<Exec>("hostNativeVectors") {
    description = "Runs the native vector runner (host build): official Noise, ble_session, ble_handshake."
    dependsOn(buildHostNativeTest)
    inputs.dir(rootProject.file("../test-vectors"))
    commandLine(hostNativeDir.get().file("dspnoise_vectors").asFile.absolutePath,
        depsCache.get().dir("noise-c-62c2fdca7a874ea898761147bb062ca1cde9f36d/tests/vector").asFile.absolutePath,
        rootProject.file("../test-vectors").absolutePath)
}
val configureNdkNativeTest = tasks.register<Exec>("configureNdkNativeTest") {
    description = "Configures the NDK x86_64 static build of the native vector runner (M1.10e.1)."
    // the configure steps share DSP_DEPS_CACHE, whose unlocked git checkout must not run concurrently
    mustRunAfter(configureHostNativeTest)
    inputs.dir(nativeTestSrc)
    outputs.file(ndkNativeDir.map { it.file("build.ninja") })
    environment("DSP_DEPS_CACHE", depsCache.get().asFile.absolutePath)
    commandLine(cmakeExe.get(), "-S", nativeTestSrc.asFile.absolutePath, "-B", ndkNativeDir.get().asFile.absolutePath,
        "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=${ninjaExe.get()}", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_TOOLCHAIN_FILE=${ndkDir.get()}/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=x86_64", "-DANDROID_PLATFORM=android-26")
}
val buildNdkNativeTest = tasks.register<Exec>("buildNdkNativeTest") {
    description = "Builds the NDK x86_64 static native vector runner and checks that FORTIFY is on."
    dependsOn(configureNdkNativeTest)
    inputs.dir(nativeTestSrc)
    inputs.dir(layout.projectDirectory.dir("src/main/cpp"))
    outputs.file(ndkNativeDir.map { it.file("dspnoise_vectors") })
    commandLine(cmakeExe.get(), "--build", ndkNativeDir.get().asFile.absolutePath)
    val commands = ndkNativeDir.map { it.file("compile_commands.json").asFile }
    doLast {
        check("-D_FORTIFY_SOURCE" in commands.get().readText()) { "the Android build of the vector runner is not compiled with FORTIFY" }
    }
}
tasks.register<Exec>("nativeAndroidTest") {
    description = "Runs the NDK x86_64 static vector runner (bionic, FORTIFY) on this Linux x86_64 host."
    dependsOn(buildNdkNativeTest)
    inputs.dir(rootProject.file("../test-vectors"))
    commandLine(ndkNativeDir.get().file("dspnoise_vectors").asFile.absolutePath,
        depsCache.get().dir("noise-c-62c2fdca7a874ea898761147bb062ca1cde9f36d/tests/vector").asFile.absolutePath,
        rootProject.file("../test-vectors").absolutePath)
}
// Firmware-in-the-loop (M1.10e.2b, E10): the real firmware logic as a host JNI library for the tests.
val fwsimSrc = rootProject.layout.projectDirectory.dir("../firmware/test/fwsim")
val fwsimDir = layout.buildDirectory.dir("native-test/fwsim")
val configureFirmwareSim = tasks.register<Exec>("configureFirmwareSim") {
    description = "Configures libdsp_fwsim (firmware domain/security/transport on the host, test-only)."
    mustRunAfter(configureHostNativeTest, configureNdkNativeTest) // shared DSP_DEPS_CACHE (see above)
    inputs.dir(fwsimSrc)
    outputs.file(fwsimDir.map { it.file("build.ninja") })
    environment("DSP_DEPS_CACHE", depsCache.get().asFile.absolutePath)
    commandLine(cmakeExe.get(), "-S", fwsimSrc.asFile.absolutePath, "-B", fwsimDir.get().asFile.absolutePath,
        "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=${ninjaExe.get()}", "-DCMAKE_BUILD_TYPE=Release",
        "-DDSP_JDK_HOME=${testJdk.get()}")
}
val buildFirmwareSim = tasks.register<Exec>("buildFirmwareSim") {
    description = "Builds libdsp_fwsim for the firmware-in-the-loop tests."
    dependsOn(configureFirmwareSim)
    inputs.dir(rootProject.layout.projectDirectory.dir("../firmware/components"))
    inputs.dir(rootProject.layout.projectDirectory.dir("../firmware/test/host"))
    inputs.dir(fwsimSrc)
    outputs.file(fwsimDir.map { it.file("libdsp_fwsim.so") })
    commandLine(cmakeExe.get(), "--build", fwsimDir.get().asFile.absolutePath)
}
tasks.withType<Test>().configureEach {
    dependsOn(buildHostNativeTest, hostNativeVectors, buildFirmwareSim)
    systemProperty("dsp.fwsim.lib", fwsimDir.get().file("libdsp_fwsim.so").asFile.absolutePath)
    systemProperty("dsp.native.hostlib", hostNativeDir.get().file("libdspnoise_hosttest.so").asFile.absolutePath)
    systemProperty("dsp.vectors.dir", rootProject.file("../test-vectors").absolutePath)
    systemProperty("dsp.noise.vectors.dir", depsCache.get().dir("noise-c-62c2fdca7a874ea898761147bb062ca1cde9f36d/tests/vector").asFile.absolutePath)
    systemProperty("dsp.source.dir", layout.projectDirectory.dir("src/main/java").asFile.absolutePath)
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.core)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.serialization.json)
    // M1.10e.4c: QR scanning only in its own module (E15); ZXing core only renders invitation QR codes.
    implementation(project(":onboarding-scanner"))
    implementation(libs.zxing.core)
    ksp(libs.androidx.room.compiler)
    testImplementation(libs.junit)
    testImplementation(libs.robolectric)
    robolectricRuntime(libs.robolectric.android.all)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.okhttp.mockwebserver)
    testImplementation(libs.kotlinx.coroutines.test)
    // Robolectric Compose tests of the shell in DEMO and DEVICE mode (JVM only; no emulator in CI, E17).
    testImplementation(platform(libs.androidx.compose.bom))
    testImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}