package com.emulate.app

import android.app.Application
import com.google.android.material.color.DynamicColors

class EmulateApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // Material You: View-based screens (the game screen's menus and dialogs) take their colours
        // from the wallpaper on Android 12+. Compose screens do the same in EmulateTheme.
        DynamicColors.applyToActivitiesIfAvailable(this)
    }
}
