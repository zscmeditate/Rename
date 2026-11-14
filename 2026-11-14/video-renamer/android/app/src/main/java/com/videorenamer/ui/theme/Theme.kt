package com.videorenamer.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val AppColorScheme = darkColorScheme(
    background = AppColors.Background,
    surface = AppColors.Surface,
    surfaceVariant = AppColors.SurfaceElevated,
    primary = AppColors.Primary,
    onPrimary = AppColors.Background,
    secondary = AppColors.PrimarySoft,
    onSecondary = AppColors.TextPrimary,
    error = AppColors.Error,
    onBackground = AppColors.TextPrimary,
    onSurface = AppColors.TextPrimary,
    onSurfaceVariant = AppColors.TextSecondary,
    outline = AppColors.BorderSubtle,
)

@Composable
fun AppTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = AppColorScheme,
        typography = Typography,
        content = content
    )
}
