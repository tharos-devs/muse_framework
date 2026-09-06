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
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

/** APIDOC
 * A toolbar-style button with two independently clickable regions: a main
 * area (icon + text) for the button's primary action, and a small attached
 * arrow area that opens a dropdown menu - unlike MenuButton/PopupButton,
 * where the whole button opens the menu, here only the arrow does.
 * @class SplitButton
 * @hideconstructor
 */
FocusScope {
    id: root

    property int icon: IconCode.NONE
    property string text: ""

    property bool checked: false

    property var menuItems: []

    property string toolTipTitle: ""
    property string toolTipDescription: ""

    readonly property bool isMenuOpened: menuLoader.isMenuOpened

    property alias navigation: navCtrl

    signal clicked()
    signal handleMenuItem(string itemId)
    signal aboutToOpenMenu()

    readonly property bool isHovered: mainMouseArea.containsMouse || arrowMouseArea.containsMouse
    readonly property bool isPressed: mainMouseArea.pressed || arrowMouseArea.pressed

    readonly property color normalColor: root.checked ? ui.theme.accentColor : ui.theme.buttonColor
    readonly property color hoverHitColor: root.checked ? ui.theme.accentColor : ui.theme.buttonColor

    readonly property int arrowAreaWidth: 20

    implicitWidth: mainContent.implicitWidth + 2 * mainContentMargins + root.arrowAreaWidth
    implicitHeight: ui.theme.defaultButtonSize

    readonly property real mainContentMargins: 12

    objectName: root.text

    NavigationControl {
        id: navCtrl
        name: root.objectName !== "" ? root.objectName : "SplitButton"
        enabled: root.enabled && root.visible

        accessible.role: MUAccessible.Button
        accessible.name: Boolean(root.text) ? root.text : root.toolTipTitle
        accessible.visualItem: root

        onTriggered: {
            if (navCtrl.enabled) {
                root.clicked()
                navCtrl.notifyAboutControlWasTriggered()
            }
        }
    }

    Rectangle {
        id: background
        anchors.fill: parent

        color: root.normalColor
        opacity: ui.theme.buttonOpacityNormal

        radius: 3
        border.width: ui.theme.borderWidth
        border.color: ui.theme.strokeColor

        NavigationFocusBorder {
            navigationCtrl: navCtrl
        }

        states: [
            State {
                name: "PRESSED"
                when: root.isPressed

                PropertyChanges {
                    target: background
                    color: root.hoverHitColor
                    opacity: ui.theme.buttonOpacityHit
                }
            },

            State {
                name: "HOVERED"
                when: root.isHovered && !root.isPressed

                PropertyChanges {
                    target: background
                    color: root.hoverHitColor
                    opacity: ui.theme.buttonOpacityHover
                }
            }
        ]
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: mainArea

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: root.mainContentMargins
            Layout.rightMargin: root.mainContentMargins

            RowLayout {
                id: mainContent

                anchors.centerIn: parent
                spacing: 8

                StyledIconLabel {
                    Layout.alignment: Qt.AlignVCenter
                    iconCode: root.icon
                    font: ui.theme.iconsFont
                    color: ui.theme.fontPrimaryColor
                    visible: !isEmpty
                }

                StyledTextLabel {
                    Layout.alignment: Qt.AlignVCenter
                    text: root.text
                    font: ui.theme.bodyFont
                    visible: !isEmpty
                }
            }

            MouseArea {
                id: mainMouseArea
                anchors.fill: parent

                enabled: root.enabled
                hoverEnabled: true

                onClicked: {
                    ui.tooltip.hide(root, true)
                    navCtrl.requestActiveByInteraction()
                    root.clicked()
                }

                onContainsMouseChanged: {
                    if (!Boolean(root.toolTipTitle)) {
                        return
                    }

                    if (mainMouseArea.containsMouse) {
                        ui.tooltip.show(root, root.toolTipTitle, root.toolTipDescription, "")
                    } else {
                        ui.tooltip.hide(root)
                    }
                }
            }
        }

        Item {
            id: arrowArea

            Layout.fillHeight: true
            Layout.preferredWidth: root.arrowAreaWidth

            StyledIconLabel {
                anchors.centerIn: parent
                iconCode: IconCode.SMALL_ARROW_DOWN
                font: ui.theme.iconsFont
                color: ui.theme.fontPrimaryColor
            }

            MouseArea {
                id: arrowMouseArea
                anchors.fill: parent

                enabled: root.enabled
                hoverEnabled: true

                onClicked: {
                    ui.tooltip.hide(root, true)
                    // Gives the caller a chance to refresh root.menuItems synchronously (e.g. its
                    // checked state) right before the menu is actually shown, in case it only
                    // updates reactively and could otherwise be stale between opens.
                    root.aboutToOpenMenu()
                    menuLoader.parent = root
                    menuLoader.toggleOpened(root.menuItems)
                }
            }
        }
    }

    StyledMenuLoader {
        id: menuLoader

        onHandleMenuItem: function(itemId) {
            root.handleMenuItem(itemId)
        }
    }
}
