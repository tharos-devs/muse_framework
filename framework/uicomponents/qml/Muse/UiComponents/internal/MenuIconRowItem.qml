/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
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

import Muse.Ui
import Muse.UiComponents

//! NOTE: a menu item whose subitems are shown as a row of buttons (see MenuItem::iconRow): each one with the
//! subitem's icon, or its title without one, highlighted when checked. Left/Right move along the row, Up/Down leave it
Item {
    id: root

    property var modelData: null

    property NavigationPanel navigationPanel: null
    property int navigationRow: 0

    readonly property int buttonSize: 28
    readonly property int textButtonMargins: 4
    readonly property int spacing: 2
    readonly property int sideMargins: 6

    readonly property bool hasActiveButton: {
        for (var i = 0; i < repeater.count; ++i) {
            var button = repeater.itemAt(i)
            if (button && button.navigation.active) {
                return true
            }
        }
        return false
    }

    signal handleMenuItem(string itemId)
    signal buttonHovered()
    signal navigateUpRequested()
    signal navigateDownRequested()

    implicitHeight: 32

    function hasIcon(item) {
        return Boolean(item.icon) && item.icon !== IconCode.NONE
    }

    //! NOTE: also for a row not shown yet (see MenuMetrics.qml)
    function calculatedWidth(item) {
        var subitems = Boolean(item) && Boolean(item.subitems) ? item.subitems : []
        var width = 2 * root.sideMargins
        for (var i = 0; i < subitems.length; ++i) {
            if (i > 0) {
                width += root.spacing
            }
            textMetrics.text = subitems[i].title
            width += hasIcon(subitems[i]) ? root.buttonSize
                                          : Math.max(root.buttonSize, Math.ceil(textMetrics.advanceWidth) + 2 * root.textButtonMargins)
        }
        return width
    }

    function requestActive() {
        for (var i = 0; i < repeater.count; ++i) {
            var button = repeater.itemAt(i)
            if (button && button.enabled) {
                button.navigation.requestActive()
                return true
            }
        }
        return false
    }

    TextMetrics {
        id: textMetrics
        font: ui.theme.bodyBoldFont
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: root.sideMargins
        anchors.verticalCenter: parent.verticalCenter

        spacing: root.spacing

        Repeater {
            id: repeater

            model: Boolean(root.modelData) && Boolean(root.modelData.subitems) ? root.modelData.subitems : []

            FlatButton {
                id: button

                required property var modelData
                required property int index

                readonly property bool hasIcon: root.hasIcon(modelData)

                width: hasIcon ? root.buttonSize : Math.max(root.buttonSize, implicitWidth)
                height: root.buttonSize
                margins: root.textButtonMargins
                minWidth: root.buttonSize

                enabled: modelData.enabled
                accentButton: modelData.checked
                transparent: !modelData.checked

                icon: hasIcon ? modelData.icon : IconCode.NONE
                text: hasIcon ? "" : modelData.title
                textFont: ui.theme.bodyBoldFont

                toolTipTitle: modelData.title
                toolTipDescription: modelData.description
                toolTipShortcut: modelData.shortcuts

                navigation.panel: root.navigationPanel
                navigation.row: root.navigationRow
                navigation.column: index
                navigation.name: modelData.id
                navigation.accessible.name: modelData.title

                navigation.onNavigationEvent: function(event) {
                    switch (event.type) {
                    case NavigationEvent.Up:
                        root.navigateUpRequested()
                        event.accepted = true
                        break
                    case NavigationEvent.Down:
                        root.navigateDownRequested()
                        event.accepted = true
                        break
                    }
                }

                Connections {
                    target: button.mouseArea

                    function onContainsMouseChanged() {
                        if (button.mouseArea.containsMouse) {
                            root.buttonHovered()
                        }
                    }
                }

                onClicked: {
                    root.handleMenuItem(modelData.id)
                }
            }
        }
    }
}
