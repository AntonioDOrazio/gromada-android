// Gromada installer: `common` (pure JVM: CD image extraction, FLAC encoder, RetroArch
// helpers, desktop CLI) and `app` (Android UI). The app module needs the Android SDK;
// without it (ANDROID_HOME / local.properties) only `common` is configured.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
    plugins {
        id("com.android.application") version "8.7.3"
        id("org.jetbrains.kotlin.android") version "2.1.0"
        id("org.jetbrains.kotlin.jvm") version "2.1.0"
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "gromada-installer"
include(":common")

val localSdk = file("local.properties").takeIf { it.isFile }?.readLines()
    ?.firstOrNull { it.startsWith("sdk.dir=") }
if (System.getenv("ANDROID_HOME") != null || System.getenv("ANDROID_SDK_ROOT") != null || localSdk != null) {
    include(":app")
} else {
    logger.lifecycle("Android SDK not found: building only :common")
}
