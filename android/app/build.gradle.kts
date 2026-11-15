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
        versionCode = 5
        versionName = "0.5.0-beta.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

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
        }
        release {
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
    buildFeatures {
        compose = true
        resValues = true
    }
    testOptions {
        // Robolectric (Room tests on the JVM) needs merged Android resources.
        unitTests.isIncludeAndroidResources = true
    }
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
    ksp(libs.androidx.room.compiler)
    testImplementation(libs.junit)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.okhttp.mockwebserver)
    testImplementation(libs.kotlinx.coroutines.test)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}