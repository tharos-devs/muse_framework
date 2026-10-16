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
#pragma once

class QWindow;

namespace muse::uicomponents {
class MacOSWindowLevelController
{
public:
    //! NOTE: above the app's windows, and still shown while the app is inactive
    static void setAlwaysAboveApp(QWindow* window);
    //! NOTE: above the app's windows, or at the normal level (e.g. behind another app's windows while it's active)
    static void setFloating(QWindow* window, bool floating);
    //! NOTE: removes the window from the screen without hiding it for Qt (which would close a dialog), or puts it back
    static void setOrderedOut(QWindow* window, bool orderedOut);
};
}
