/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

RowLayout {
    id: root

    signal dismissRequested()

    spacing: 12

    Timer {
        interval: 10000
        running: true

        onTriggered: {
            root.dismissRequested()
        }
    }

    Rectangle {
        Layout.preferredWidth: 24
        Layout.preferredHeight: 24

        radius: width / 2
        color: "#46A955"

        StyledIconLabel {
            anchors.centerIn: parent

            iconCode: IconCode.TICK_RIGHT_ANGLE_THICK
            color: "white"
        }
    }

    StyledTextLabel {
        Layout.fillWidth: true

        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap

        text: qsTrc("update", "Updated successfully")
        font: ui.theme.bodyBoldFont
    }

    FlatButton {
        icon: IconCode.CLOSE_X_ROUNDED
        transparent: true

        navigation.accessible.name: qsTrc("global", "Dismiss")

        onClicked: {
            root.dismissRequested()
        }
    }
}
