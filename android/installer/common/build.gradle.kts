import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("org.jetbrains.kotlin.jvm")
    application
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}

dependencies {
    testImplementation(kotlin("test"))
    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
    maxHeapSize = "1g"
    // RealImageTest runs when GROMADA_CUE / GROMADA_BIN / GROMADA_DATA are set.
    testLogging { events("passed", "skipped", "failed"); showStandardStreams = false }
}

application {
    mainClass.set("io.github.antoniodorazio.gromada.extract.Cli")
    applicationName = "gromada-extract"
}

// Single runnable jar for the desktop CLI: java -jar gromada-extract.jar image.cue out/
tasks.register<Jar>("cliJar") {
    archiveFileName.set("gromada-extract.jar")
    manifest { attributes("Main-Class" to "io.github.antoniodorazio.gromada.extract.Cli") }
    from(sourceSets.main.get().output)
    dependsOn(configurations.runtimeClasspath)
    from({ configurations.runtimeClasspath.get().filter { it.name.endsWith(".jar") }.map { zipTree(it) } }) {
        exclude("META-INF/*.SF", "META-INF/*.DSA", "META-INF/*.RSA", "META-INF/versions/**", "META-INF/*.kotlin_module")
    }
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
}
