package com.videorenamer.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import com.videorenamer.ui.theme.AppColors

/** 统一卡片容器：22dp 圆角 + 细描边 + 深色卡片底 */
@Composable
fun AppCard(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit
) {
    Box(
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(22.dp))
            .background(AppColors.Surface)
            .border(1.dp, AppColors.BorderSubtle, RoundedCornerShape(22.dp))
    ) {
        content()
    }
}
