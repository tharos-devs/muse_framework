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

ColumnLayout {
    id: root

    property string appName: ""
    property string updateVersion: ""

    signal detailsRequested()
    signal installRequested()
    signal dismissRequested()

    spacing: 8

    ColumnLayout {
        spacing: 4

        RowLayout {
            Layout.fillWidth: true

            spacing: 8

            StyledTextLabel {
                Layout.fillWidth: true

                horizontalAlignment: Text.AlignLeft

                text: qsTrc("update", "Update available")
                font: ui.theme.largeBodyBoldFont
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

        StyledTextLabel {
            Layout.fillWidth: true

            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap

            text: root.updateVersion.length > 0 ? root.appName + " " + root.updateVersion : root.appName
        }
    }

    RowLayout {
        Layout.fillWidth: true

        spacing: 4

        FlatButton {
            Layout.fillWidth: true

            text: qsTrc("update", "See details")

            onClicked: {
                root.detailsRequested()
            }
        }

        FlatButton {
            Layout.fillWidth: true

            text: qsTrc("update", "Update")
            accentButton: true

            onClicked: {
                root.installRequested()
            }
        }
    }
}
