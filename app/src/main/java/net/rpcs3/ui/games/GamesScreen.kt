package net.rpcs3.ui.games

import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import net.rpcs3.FirmwareRepository
import net.rpcs3.Game
import net.rpcs3.GameInfo
import net.rpcs3.GameProgressType
import net.rpcs3.GameRepository
import net.rpcs3.ProgressRepository
import net.rpcs3.RPCS3Activity

private fun withAlpha(color: Color, alpha: Float): Color {
    return Color(
        red = color.red,
        green = color.green,
        blue = color.blue,
        alpha = alpha
    )
}

@Composable
fun GameItem(game: Game) {
    val context = LocalContext.current
    Column {
        Card(onClick = {
            if (FirmwareRepository.version.value == null) {
                // TODO: firmware not installed
            }
            else if (FirmwareRepository.progressChannel.value != null) {
                // TODO: firmware in use
            } else if (game.info.path != "$" && game.findProgress(
                    arrayOf(
                        GameProgressType.Install,
                        GameProgressType.Remove
                    )
                ) == null
            ) {
                if (game.findProgress(GameProgressType.Compile) != null) {
                    // TODO: game is compiling
                } else {
                    GameRepository.onBoot(game)
                    val emulatorWindow = Intent(
                        context,
                        RPCS3Activity::class.java
                    )
                    emulatorWindow.putExtra("path", game.info.path)
                    context.startActivity(emulatorWindow)
                }
            }
        }, shape = RectangleShape, modifier = Modifier.fillMaxSize()) {
            Box(
                modifier = Modifier
                    .height(128.dp)
                    .align(alignment = Alignment.CenterHorizontally)
                    .fillMaxSize()
            ) {
                val iconPath = game.info.iconPath
                if (iconPath.value != null) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                        modifier = Modifier.fillMaxSize()
                    ) {
                        AsyncImage(
                            model = iconPath.value,
                            contentDescription = null
                        )
                    }
                }

                if (game.progressList.isNotEmpty()) {
                    Row(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(withAlpha(Color.DarkGray, 0.6f))
                    ) {}

                    val progressChannel = game.progressList.first().id
                    val progress = ProgressRepository.getItem(progressChannel)
                    val progressValue = progress?.value?.value
                    val maxValue = progress?.value?.max

                    if (progressValue != null && maxValue != null) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.Center,
                            modifier = Modifier.fillMaxSize()
                        ) {
                            if (maxValue.longValue != 0L) {
                                CircularProgressIndicator(
                                    modifier = Modifier
                                        .width(64.dp)
                                        .height(64.dp),
                                    color = MaterialTheme.colorScheme.secondary,
                                    trackColor = MaterialTheme.colorScheme.surfaceVariant,
                                    progress = {
                                        progressValue.longValue.toFloat() / maxValue.longValue.toFloat()
                                    },
                                )
                            } else {
                                CircularProgressIndicator(
                                    modifier = Modifier
                                        .width(64.dp)
                                        .height(64.dp),
                                    color = MaterialTheme.colorScheme.secondary,
                                    trackColor = MaterialTheme.colorScheme.surfaceVariant,
                                )
                            }
                        }
                    }
                }

//                val name = game.info.name.value
//                if (name != null) {
//                    Row(
//                        verticalAlignment = Alignment.Bottom,
//                        horizontalArrangement = Arrangement.Center,
//                        modifier = Modifier.fillMaxSize()
//                    ) {
//                        Text(name, textAlign = TextAlign.Center)
//                    }
//                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GamesScreen() {
    val games = remember { GameRepository.list() }
    val isRefreshing = false

    PullToRefreshBox(
        isRefreshing = isRefreshing,
        onRefresh = {},
    ) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 128.dp),
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxSize()
        ) {
            items(games.size) { index ->
                GameItem(games[index])
            }
        }
    }
}

@Preview
@Composable
fun GamesScreenPreview() {
    listOf(
        "Minecraft",
        "Skate 3",
        "Mirror's Edge",
        "Demon's Souls"
    ).forEach { x -> GameRepository.addPreview(arrayOf(GameInfo(x, x))) }

    GamesScreen()
}