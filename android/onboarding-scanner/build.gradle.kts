// QR scanning for onboarding only (M1.10e.4c, decision E15 as revised): CameraX preview and frames,
// decoded on the device by ZXing core (pure Java, Apache-2.0), QR format only. Fully offline: no
// network, no telemetry, nothing initialized at app start; camera frames never leave the process and
// nothing is stored. Only the onboarding screen depends on it.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "cz.nowass.dispenser.scanner"
    compileSdk {
        version = release(37)
    }
    defaultConfig {
        minSdk = 26
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.camera.core)
    implementation(libs.androidx.camera.camera2)
    implementation(libs.androidx.camera.lifecycle)
    // Compose-native viewfinder (camera-view would add AppCompat, Fragment, camera-video and Media3)
    implementation(libs.androidx.camera.compose)
    implementation(libs.zxing.core)
    testImplementation(libs.junit)
}
