import QtQuick
import Quickshell.Io

Item {
  id: root

  Process {
    id: wallpaper
    command: [Qt.resolvedUrl("bin/world-clock").toString().replace("file://", ""), "run"]
    running: true
  }

  Component.onDestruction: wallpaper.running = false
}
