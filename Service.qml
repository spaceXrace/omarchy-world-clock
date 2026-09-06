import QtQuick
import Quickshell.Io

Item {
  id: root
  property bool stopping: false

  Process {
    id: wallpaper
    command: [Qt.resolvedUrl("bin/world-clock").toString().replace("file://", ""), "run"]
    running: true
    onExited: function(_exitCode, _exitStatus) {
      if (!root.stopping) restartTimer.restart()
    }
  }

  Timer {
    id: restartTimer
    interval: 2000
    repeat: false
    onTriggered: if (!root.stopping) wallpaper.running = true
  }

  Component.onDestruction: {
    root.stopping = true
    restartTimer.stop()
    wallpaper.running = false
  }
}
