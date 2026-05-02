import java.time.Instant

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.chaquo.python")
}

android {
    namespace = "io.github.ryo100794.pdocker"
    compileSdk = 34
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "io.github.ryo100794.pdocker"
        minSdk = 26
        targetSdk = 34
        versionCode = 21
        versionName = "0.5.2"
        buildConfigField("String", "BUILD_TIME_UTC", "\"${Instant.now()}\"")

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
        getByName("debug") {
            isDebuggable = true
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            // AGP's strip task invokes NDK's llvm-strip which is an
            // x86_64 ELF. On aarch64 hosts it fails to exec. Our .so
            // files are tiny (<20 KB) so skip stripping entirely.
            keepDebugSymbols += listOf("**/*.so")
        }
    }

    buildFeatures {
        buildConfig = true
    }
}

chaquopy {
    defaultConfig {
        version = "3.11"
        pip {
            // pdockerd uses stdlib only
        }
        pyc {
            src = false
        }
    }
    sourceSets {
        getByName("main") {
            srcDir("src/main/python")
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.webkit:webkit:1.11.0")
    implementation("com.google.android.material:material:1.12.0")
}
