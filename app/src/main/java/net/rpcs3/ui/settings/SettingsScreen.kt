package net.rpcs3.ui.settings

import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

@Composable
fun SettingsItem(item: String, onClick: () -> Unit = { }) {
    ElevatedCard(
        elevation = CardDefaults.cardElevation(
            defaultElevation = 6.dp
        ),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ),
        onClick = { onClick() },
        modifier = Modifier
            .size(width = 320.dp, height = 100.dp)
            .padding(20.dp)
    ) {
        Text(
            text = item,
            modifier = Modifier
                .padding(16.dp),
            textAlign = TextAlign.Center,
        )
    }
}

@Composable
fun SettingsScreen() {
    val items = mutableListOf("Install Firmware", "Install custom driver")

    for (i in 0..10) {
        items += i.toString()
    }
    val scrollState = rememberScrollState()
    var selectedFileUri by remember { mutableStateOf<Uri?>(null) }
    val mContext = LocalContext.current

    val pickFileLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent(),
        onResult = { uri: Uri? ->
            selectedFileUri = uri
            val toast = Toast.makeText(mContext, uri.toString(), Toast.LENGTH_SHORT)
            toast.show()
        }
    )

    Scaffold(
        modifier = Modifier.padding(vertical = 256.dp)
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .verticalScroll(scrollState)
                .fillMaxSize()
                .padding(paddingValues),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            items.forEach { item ->
                SettingsItem(item) {
                    if (item == "Install Firmware") {
                        pickFileLauncher.launch("*/*")
                    }
                }
            }
        }
    }
}