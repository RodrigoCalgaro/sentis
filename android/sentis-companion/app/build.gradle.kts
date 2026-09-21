plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.sentis.companion"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.sentis.companion"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1-link-test"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}
