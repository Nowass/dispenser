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
        versionCode = 4
        versionName = "0.4.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        resValue("string", "github_owner", githubOwner)
        resValue("string", "github_repo", githubRepo)
        resValue("bool", "github_source_link_enabled", githubSourceLinkEnabled)
    }

    buildTypes {
        release {
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