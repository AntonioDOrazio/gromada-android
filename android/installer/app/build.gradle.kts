import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// CI passes the commit and run number: the app compares them with the nightly release.
val gitCommit = System.getenv("GITHUB_SHA") ?: "dev"
val buildNumber = System.getenv("GITHUB_RUN_NUMBER")?.toIntOrNull() ?: 1

android {
    namespace = "io.github.antoniodorazio.gromada.installer"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.github.antoniodorazio.gromada.installer"
        minSdk = 29
        targetSdk = 35
        versionCode = buildNumber
        versionName = "0.1.$buildNumber-${gitCommit.take(7)}"
        buildConfigField("String", "GIT_COMMIT", "\"$gitCommit\"")
        buildConfigField("String", "DEFAULT_REPO", "\"AntonioDOrazio/gromada-android\"")
        // Emulator end-to-end test with the real RetroArch (e2e/run.sh, installer-e2e.yml).
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildFeatures { buildConfig = true }

    // A fixed, public debug key so that nightly APKs install over each other (the
    // per-machine default debug key would change at every CI run). It is NOT a secret:
    // replace it with a real release key before distributing outside this project.
    signingConfigs {
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        // Sideloaded tool: all-files access is intended (see docs/installer.md).
        disable += setOf("ScopedStorage", "QueryAllPackagesPermission")
        abortOnError = true
        textReport = true
        textOutput = file("stdout")
    }
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}

dependencies {
    implementation(project(":common"))
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test:core:1.6.1")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
