// OrcaSlicer for Android, native UI. The engine (liborcacore.so, from OrcaSlicer's libslic3r) and the generated
// assets are staged by mobile/build-mobile.sh into build/staging/<abi> before Gradle runs.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val appVersion = "0.2.2"
val baseVersionCode = 24   // 0.1.x (the wxWidgets port) used 1..3, 0.2.0 21/22, 0.2.1 23/24; one code per ABI below
val staging = file("build/staging")
// The Orca release key (env.sh ORCA_KEYSTORE), so the app updates the earlier releases in place.
val keystore = file(System.getenv("ORCA_KEYSTORE") ?: "../../../hosttools/debug.keystore")

android {
    namespace = "org.orcaport.mobile"
    compileSdk = 34
    defaultConfig {
        applicationId = "org.orcaport.slicer"
        minSdk = 28
        targetSdk = 34
        versionName = appVersion
    }
    signingConfigs {
        if (keystore.exists()) create("orca") {
            storeFile = keystore; storePassword = "android"; keyAlias = "orcadebug"; keyPassword = "android"
        }
    }
    buildTypes {
        getByName("debug") {
            applicationIdSuffix = ".dev"   // side by side with an installed release
            signingConfigs.findByName("orca")?.let { signingConfig = it }
        }
        getByName("release") {
            isMinifyEnabled = false
            signingConfigs.findByName("orca")?.let { signingConfig = it }
        }
    }
    flavorDimensions += "abi"
    productFlavors {
        create("arm64") { dimension = "abi"; ndk { abiFilters += "arm64-v8a" }; versionCode = baseVersionCode + 1 }
        create("x86") { dimension = "abi"; ndk { abiFilters += "x86_64" }; versionCode = baseVersionCode + 2 }
    }
    sourceSets {
        getByName("main") { jniLibs.srcDir(File(staging, "jni")); assets.srcDir(File(staging, "assets")) }
        // the ABI's Python extension modules (build-mobile.sh)
        getByName("x86") { assets.srcDir(File(staging, "assets-x86_64")) }
        getByName("arm64") { assets.srcDir(File(staging, "assets-arm64-v8a")) }
    }
    buildFeatures { compose = true; buildConfig = true }
    packaging { jniLibs { useLegacyPackaging = true } }
    androidResources { noCompress += listOf("zip") }
    composeOptions { kotlinCompilerExtensionVersion = "1.5.14" }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("com.caverock:androidsvg-aar:1.4")
    implementation("androidx.webkit:webkit:1.11.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
