package com.emulate.app

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight

/** Google Sans Flex (variable font) at the weights the UI uses. Hangul falls back to the system font. */
@OptIn(ExperimentalTextApi::class)
val GoogleSans = FontFamily(
    listOf(400, 500, 600, 700).map { weight ->
        Font(
            R.font.google_sans_flex,
            weight = FontWeight(weight),
            variationSettings = FontVariation.Settings(FontVariation.weight(weight)),
        )
    },
)

private fun TextStyle.sans() = copy(fontFamily = GoogleSans)

private val AppTypography = Typography().run {
    Typography(
        displayLarge = displayLarge.sans(), displayMedium = displayMedium.sans(), displaySmall = displaySmall.sans(),
        headlineLarge = headlineLarge.sans(), headlineMedium = headlineMedium.sans(), headlineSmall = headlineSmall.sans(),
        titleLarge = titleLarge.sans(), titleMedium = titleMedium.sans(), titleSmall = titleSmall.sans(),
        bodyLarge = bodyLarge.sans(), bodyMedium = bodyMedium.sans(), bodySmall = bodySmall.sans(),
        labelLarge = labelLarge.sans(), labelMedium = labelMedium.sans(), labelSmall = labelSmall.sans(),
    )
}

// Used before Android 12 (no wallpaper colours): a blue-violet scheme.
private val FallbackLight = lightColorScheme(
    primary = Color(0xFF4A5BB8), onPrimary = Color.White,
    primaryContainer = Color(0xFFDDE1FF), onPrimaryContainer = Color(0xFF001257),
    secondary = Color(0xFF5A5D72), onSecondary = Color.White,
    secondaryContainer = Color(0xFFDFE1F9), onSecondaryContainer = Color(0xFF171B2C),
    tertiary = Color(0xFF76546E), onTertiary = Color.White,
    tertiaryContainer = Color(0xFFFFD7F2), onTertiaryContainer = Color(0xFF2C1229),
    background = Color(0xFFFBF8FF), surface = Color(0xFFFBF8FF),
    surfaceVariant = Color(0xFFE2E1EC), onSurfaceVariant = Color(0xFF45464F),
    outline = Color(0xFF767680), outlineVariant = Color(0xFFC6C5D0),
)

private val FallbackDark = darkColorScheme(
    primary = Color(0xFFB8C4FF), onPrimary = Color(0xFF16297F),
    primaryContainer = Color(0xFF31429E), onPrimaryContainer = Color(0xFFDDE1FF),
    secondary = Color(0xFFC3C5DD), onSecondary = Color(0xFF2C2F42),
    secondaryContainer = Color(0xFF424659), onSecondaryContainer = Color(0xFFDFE1F9),
    tertiary = Color(0xFFE5BAD8), onTertiary = Color(0xFF44263F),
    tertiaryContainer = Color(0xFF5D3C56), onTertiaryContainer = Color(0xFFFFD7F2),
    background = Color(0xFF131318), surface = Color(0xFF131318),
    surfaceVariant = Color(0xFF45464F), onSurfaceVariant = Color(0xFFC6C5D0),
    outline = Color(0xFF90909A), outlineVariant = Color(0xFF45464F),
)

/** Material You: wallpaper colours on Android 12+, following the system light/dark setting. */
@Composable
fun EmulateTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val colors = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> FallbackDark
        else -> FallbackLight
    }
    MaterialTheme(colorScheme = colors, typography = AppTypography, content = content)
}
