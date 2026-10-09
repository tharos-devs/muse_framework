/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "dockwindow.h"

#include "kddockwidgets/src/LayoutSaver.h"
#include "kddockwidgets/src/core/DockRegistry.h"
#include "kddockwidgets/src/core/FloatingWindow.h"
#include "kddockwidgets/src/core/Layout.h"
#include "kddockwidgets/src/core/MainWindow.h"
#include "kddockwidgets/src/qtquick/views/MainWindow.h"
#include "kddockwidgets/src/qtquick/views/DockWidget.h"
#include "kddockwidgets/src/qtquick/views/View.h"
#include "kddockwidgets/src/core/Group.h"
#include "kddockwidgets/src/core/Window_p.h"

#include "global/async/async.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>

#include "dockcentralview.h"
#include "dockpageview.h"
#include "dockpanelview.h"
#include "dockstatusbar.h"
#include "docktoolbarview.h"
#include "dockingholderview.h"
#include "dockwindow.h"
#include "topleveltoolbarslayout.h"

#include "muse_framework_config.h"

#include "log.h"

using namespace muse::dock;
using namespace muse::async;

namespace muse::dock {
static const QString SECONDARY_WINDOW_OPEN_KEY("dockSecondaryWindowOpen");
static const QString SECONDARY_WINDOW_LAYOUT_KEY_PREFIX("dockSecondaryWindowLayout/");
static const QString WINDOW_GEOMETRY_STATE_NAME("@windowGeometry");

static const QList<Location> POSSIBLE_LOCATIONS {
    Location::Left,
    Location::Right,
    Location::Top,
    Location::Bottom
};

static KDDockWidgets::Location locationToKLocation(Location location)
{
    switch (location) {
    case Location::Left: return KDDockWidgets::Location_OnLeft;
    case Location::Right: return KDDockWidgets::Location_OnRight;
    case Location::Top: return KDDockWidgets::Location_OnTop;
    case Location::Bottom: return KDDockWidgets::Location_OnBottom;
    case Location::Center: break;
    case Location::Undefined: break;
    }

    return KDDockWidgets::Location_None;
}

static void clearRegistry(int ctx)
{
    TRACEFUNC;

    auto* registry = KDDockWidgets::DockRegistry::self(ctx);

    for (KDDockWidgets::Core::MainWindow* mw : registry->mainwindows()) {
        mw->layout()->clearLayout();
    }

    registry->clear();

    for (KDDockWidgets::Core::DockWidget* dock : registry->dockwidgets()) {
        registry->unregisterDockWidget(dock);
    }

    for (KDDockWidgets::Core::Group* group : registry->groups()) {
        const auto dockWidgets = group->dockWidgets();
        for (KDDockWidgets::Core::DockWidget* dock : dockWidgets) {
            group->removeWidget(dock);
        }
        registry->unregisterGroup(group);
    }
}
}

//! NOTE: KDDockWidgets refuses a whole layout naming a main window it doesn't know, and even crashes restoring
//! a dock position pointing into one - so the layouts saved as the regular window/page states, which builds
//! without the secondary window may read too, never mention it: its main window, the dock positions in it and
//! the floating windows parented to it are stripped (the docks it held count as closed there), while the full
//! layout is kept aside (see splitSecondaryWindowLayout())
static QByteArray withoutSecondaryWindow(const QByteArray& layout)
{
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(layout, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return layout;
    }

    QJsonObject root = document.object();
    QJsonArray mainWindows = root.value(QStringLiteral("mainWindows")).toArray();

    qsizetype secondaryIndex = -1;
    for (qsizetype i = 0; i < mainWindows.size(); ++i) {
        if (mainWindows.at(i).toObject().value(QStringLiteral("uniqueName")).toString() == QLatin1String(SECONDARY_WINDOW_NAME)) {
            secondaryIndex = i;
            break;
        }
    }

    if (secondaryIndex < 0) {
        return layout;
    }

    QJsonArray closedDocks = root.value(QStringLiteral("closedDockWidgets")).toArray();
    const QJsonObject frames = mainWindows.at(secondaryIndex).toObject()
                               .value(QStringLiteral("multiSplitterLayout")).toObject()
                               .value(QStringLiteral("frames")).toObject();
    for (const QJsonValue& frame : frames) {
        for (const QJsonValue& dockName : frame.toObject().value(QStringLiteral("dockWidgets")).toArray()) {
            if (!closedDocks.contains(dockName)) {
                closedDocks.append(dockName);
            }
        }
    }

    mainWindows.removeAt(secondaryIndex);

    QJsonArray floatingWindows = root.value(QStringLiteral("floatingWindows")).toArray();
    for (qsizetype i = 0; i < floatingWindows.size(); ++i) {
        QJsonObject floatingWindow = floatingWindows.at(i).toObject();
        const int parentIndex = floatingWindow.value(QStringLiteral("parentIndex")).toInt(-1);
        if (parentIndex == secondaryIndex) {
            floatingWindow.insert(QStringLiteral("parentIndex"), 0);
        } else if (parentIndex > secondaryIndex) {
            floatingWindow.insert(QStringLiteral("parentIndex"), parentIndex - 1);
        }
        floatingWindows.replace(i, floatingWindow);
    }

    QJsonArray allDocks = root.value(QStringLiteral("allDockWidgets")).toArray();
    for (qsizetype i = 0; i < allDocks.size(); ++i) {
        QJsonObject dock = allDocks.at(i).toObject();
        QJsonObject lastPosition = dock.value(QStringLiteral("lastPosition")).toObject();
        QJsonArray placeholders;

        for (const QJsonValue& placeholder : lastPosition.value(QStringLiteral("placeholders")).toArray()) {
            const QJsonObject placeholderObj = placeholder.toObject();
            const bool isInSecondaryWindow = !placeholderObj.value(QStringLiteral("isFloatingWindow")).toBool()
                                             && placeholderObj.value(QStringLiteral("mainWindowUniqueName")).toString()
                                             == QLatin1String(SECONDARY_WINDOW_NAME);
            if (!isInSecondaryWindow) {
                placeholders.append(placeholder);
            }
        }

        lastPosition.insert(QStringLiteral("placeholders"), placeholders);
        dock.insert(QStringLiteral("lastPosition"), lastPosition);
        allDocks.replace(i, dock);
    }

    root.insert(QStringLiteral("mainWindows"), mainWindows);
    root.insert(QStringLiteral("floatingWindows"), floatingWindows);
    root.insert(QStringLiteral("allDockWidgets"), allDocks);
    root.insert(QStringLiteral("closedDockWidgets"), closedDocks);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QString layoutHash(const QByteArray& layout)
{
    return QString::fromLatin1(QCryptographicHash::hash(layout, QCryptographicHash::Sha1).toHex());
}

DockWindow::DockWindow(QQuickItem* parent)
    : QQuickItem(parent), muse::Contextable(muse::iocCtxForQmlObject(this)),
    m_toolBars(this),
    m_pages(this)
{
}

DockWindow::~DockWindow()
{
    dockWindowProvider()->deinit();
}

void DockWindow::componentComplete()
{
    TRACEFUNC;

    QQuickItem::componentComplete();

    static const QString name = "mainWindow";

    m_mainWindow = new KDDockWidgets::QtQuick::MainWindow(iocContext()->id, name,
                                                          KDDockWidgets::MainWindowOption_None,
                                                          this);

    m_topLevelToolBarsLayout = new TopLevelToolBarsLayout(m_mainWindow, this);

    connect(qApp, &QCoreApplication::aboutToQuit, this, &DockWindow::onQuit);
    connect(this, &QQuickItem::windowChanged, this, &DockWindow::windowPropertyChanged);
}

void DockWindow::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (m_currentPage && !qFuzzyCompare(newGeometry.width(), oldGeometry.width())) {
        polish();
    }
}

void DockWindow::updatePolish()
{
    QQuickItem::updatePolish();

    if (m_topLevelToolBarsLayout) {
        m_topLevelToolBarsLayout->relayout();
    }
}

void DockWindow::onQuit()
{
    TRACEFUNC;

    IF_ASSERT_FAILED(m_currentPage) {
        return;
    }

    savePageState(m_currentPage->objectName());
    m_reloadCurrentPageAllowed = false;

    //! NOTE: the windows closing on quit isn't the user closing the secondary window
    if (m_secondaryWindow) {
        m_secondaryWindow->disconnect(this);
    }

    clearRegistry(iocContext()->id);

    saveWindowGeometry();
}

QString DockWindow::currentPageUri() const
{
    return m_currentPage ? m_currentPage->uri() : QString();
}

QQmlListProperty<muse::dock::DockToolBarView> DockWindow::toolBarsProperty()
{
    return m_toolBars.property();
}

QQmlListProperty<muse::dock::DockPageView> DockWindow::pagesProperty()
{
    return m_pages.property();
}

QQuickWindow* DockWindow::windowProperty() const
{
    return window();
}

QQmlComponent* DockWindow::secondaryWindowComponent() const
{
    return m_secondaryWindowComponent;
}

void DockWindow::setSecondaryWindowComponent(QQmlComponent* component)
{
    if (component == m_secondaryWindowComponent) {
        return;
    }

    m_secondaryWindowComponent = component;
    emit secondaryWindowComponentChanged();
}

void DockWindow::init()
{
    clearRegistry(iocContext()->id);

    //! NOTE: before any layout gets restored: KDDockWidgets refuses a layout naming an unknown main window
    initSecondaryWindow();

    restoreGeometry();

    dockWindowProvider()->init(this);

    uiState()->windowGeometryChanged().onNotify(this, [this]() {
        reloadCurrentPage();
    });

    workspaceManager()->currentWorkspaceAboutToBeChanged().onNotify(this, [this]() {
        if (const DockPageView* page = currentPage()) {
            savePageState(page->objectName());
        }
    });
}

void DockWindow::loadPage(const QString& uri, const QVariantMap& params)
{
    TRACEFUNC;

    if (currentPageUri() == uri) {
        if (m_currentPage) {
            m_currentPage->setParams(params);
        }
        return;
    }

    const bool isFirstOpening = (m_currentPage == nullptr);

    if (!isFirstOpening) {
        const QString pageName = m_currentPage->objectName();
        uiState()->pageState(pageName).notification.disconnect(this);
        savePageState(pageName);
        clearRegistry(iocContext()->id);
        m_currentPage->setVisible(false);
        m_currentPage->deinit();
    }

    bool ok = doLoadPage(uri, params);
    if (!ok) {
        return;
    }

    if (checkLayoutIsCorrupted()) {
        LOGE() << "Layout is corrupted, restoring default";
        restoreDefaultLayout();
    }

    auto notifyAboutPageLoaded = [this, &uri]() {
        emit currentPageUriChanged(uri);
        emit pageLoaded();
        notifyAboutDocksOpenStatus();
    };

    if (isFirstOpening) {
        async::Async::call(this, [this, notifyAboutPageLoaded]() {
            if (!m_hasGeometryBeenRestored
                || (m_mainWindow->window()->isFullScreen())) {
                //! NOTE: show window as maximized if no geometry has been restored
                //! or if the user had closed app in FullScreen mode
                // m_mainWindow->window()->->showMaximized(); // todo kddock
            }

            notifyAboutPageLoaded();
        });
    } else {
        notifyAboutPageLoaded();
    }
}

void DockWindow::openPage(const QString& uri)
{
    interactive()->open(uri.toStdString());
}

bool DockWindow::isDockOpen(const QString& dockName) const
{
    if (!m_currentPage) {
        return false;
    }

    //! NOTE: a dock left in the hidden secondary window isn't visible to the user
    const DockBase* dock = m_currentPage->dockByName(dockName);
    if (dock && dock->isInSecondaryWindow() && !isSecondaryWindowShown()) {
        return false;
    }

    return m_currentPage->isDockOpen(dockName);
}

void DockWindow::toggleDock(const QString& dockName)
{
    setDockOpen(dockName, !isDockOpen(dockName));
}

void DockWindow::setDockOpen(const QString& dockName, bool open)
{
    if (!m_currentPage) {
        return;
    }

    DockBase* dock = m_currentPage->dockByName(dockName);
    if (open && dock && dock->belongsToSecondaryWindow()) {
        //! NOTE: reopen it where it was in the secondary window (rather than tabbing it
        //! next to a sibling panel of the main window), and make sure that one is shown
        dock->open();
        setSecondaryWindowOpen(true);
    } else {
        //! NOTE: a panel without a docked location to go back to (e.g. its siblings, which it would
        //! otherwise be tabbed with, are all in the secondary window) would open floating, with no
        //! way to dock it back - so it gets its default location first (unless it was floating anyway)
        DockPanelView* panel = dynamic_cast<DockPanelView*>(dock);
        if (open && panel && !panel->isOpen() && !panel->reopensFloating() && !panel->dockedMainWindow()
            && !m_currentPage->findPanelForTab(panel)) {
            addDockToDefaultLocation(panel);
        }

        m_currentPage->setDockOpen(dockName, open);
    }

    m_docksOpenStatusChanged.send({ dockName });
}

Channel<QStringList> DockWindow::docksOpenStatusChanged() const
{
    return m_docksOpenStatusChanged;
}

bool DockWindow::isDockFloating(const QString& dockName) const
{
    return m_currentPage && m_currentPage->isDockFloating(dockName);
}

void DockWindow::toggleDockFloating(const QString& dockName)
{
    if (!m_currentPage) {
        return;
    }

    DockBase* dock = m_currentPage->dockByName(dockName);
    if (dock && dock->floating()) {
        const KDDockWidgets::Core::MainWindow* dockedMainWindow = dock->dockedMainWindow();

        //! NOTE: KDDockWidgets can't dock back a floating dock that has no previous docked location
        if (!dockedMainWindow) {
            addDockToDefaultLocation(dock);
            return;
        }

        //! NOTE: docking it back into the secondary window must show that one, if this page has it
        if (isSecondaryMainWindow(dockedMainWindow)) {
            if (m_currentPage->secondaryWindowAvailable()) {
                m_currentPage->toggleDockFloating(dockName);
                setSecondaryWindowOpen(true);
            } else {
                addDockToDefaultLocation(dock);
            }
            return;
        }
    }

    m_currentPage->toggleDockFloating(dockName);
}

DockPageView* DockWindow::currentPage() const
{
    return m_currentPage;
}

QQuickItem& DockWindow::asItem() const
{
    return *m_mainWindow;
}

void DockWindow::restoreDefaultLayout()
{
    TRACEFUNC;

    //! HACK: notify about upcoming change of current URI
    //! so that all subscribers of this channel finish their work.
    //! For example, our popups and tooltips will close.
    interactive()->currentUriAboutToBeChanged().notify();

    if (m_currentPage) {
        for (DockBase* dock : m_currentPage->allDocks()) {
            dock->resetToDefault();
        }
    }

    m_reloadCurrentPageAllowed = false;
    for (const DockPageView* page : m_pages.list()) {
        uiState()->setPageState(page->objectName(), QByteArray());
    }

    uiState()->setWindowGeometry(QByteArray());
    m_reloadCurrentPageAllowed = true;

    //! NOTE: the default layout has nothing in the secondary window
    setSecondaryWindowOpen(false);

    reloadCurrentPage();
}

void DockWindow::initSecondaryWindow()
{
    if (!m_secondaryWindowComponent || m_secondaryWindow) {
        return;
    }

    TRACEFUNC;

    //! NOTE: in its own creation context, which a bound component (pragma ComponentBehavior: Bound) requires
    //! (create() without a context would use the engine's root context)
    QObject* object = m_secondaryWindowComponent->create(m_secondaryWindowComponent->creationContext());
    m_secondaryWindow = qobject_cast<QQuickWindow*>(object);
    if (!m_secondaryWindow) {
        LOGE() << "The secondary window component must be a Window: " << m_secondaryWindowComponent->errorString();
        delete object;
        return;
    }

    //! NOTE: owned by us, but still a top-level window (QWindow::setParent() would embed it)
    static_cast<QObject*>(m_secondaryWindow)->setParent(this);

    QQuickItem* dockArea = m_secondaryWindow->property("dockArea").value<QQuickItem*>();
    if (!dockArea) {
        dockArea = m_secondaryWindow->contentItem();
    }

    m_secondaryMainWindow = new KDDockWidgets::QtQuick::MainWindow(iocContext()->id, SECONDARY_WINDOW_NAME,
                                                                   KDDockWidgets::MainWindowOption_None,
                                                                   dockArea);
    m_secondaryMainWindow->setProperty(SECONDARY_WINDOW_ITEM_PROPERTY, true);

    m_secondaryWindowOpen = uiState()->isVisible(SECONDARY_WINDOW_OPEN_KEY, false);

    //! NOTE: e.g. another workspace was selected, with its own open state for it
    uiState()->isVisibleChanged(SECONDARY_WINDOW_OPEN_KEY).onNotify(this, [this]() {
        applySecondaryWindowOpen(uiState()->isVisible(SECONDARY_WINDOW_OPEN_KEY, false));
    });

    connect(m_secondaryWindow, &QWindow::visibleChanged, this, [this](bool visible) {
        //! NOTE: hidden by something else than us, i.e. closed by the user
        if (!visible && !m_updatingSecondaryWindowVisibility) {
            setSecondaryWindowOpen(false);
        }
    });

    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow* focusWindow) {
        if (focusWindow == m_secondaryWindow) {
            m_mainWindowActivatedLast = false;
        } else if (focusWindow && focusWindow == window()) {
            m_mainWindowActivatedLast = true;
        }
    });
}

bool DockWindow::isSecondaryMainWindow(const KDDockWidgets::Core::MainWindow* mainWindow) const
{
    return mainWindow && m_secondaryMainWindow && mainWindow == m_secondaryMainWindow->mainWindow();
}

bool DockWindow::isMainWindowInFrontAt(const QPoint& globalPos) const
{
    const QWindow* mainWindow = window();
    return m_mainWindowActivatedLast && mainWindow && mainWindow->isVisible() && mainWindow->geometry().contains(globalPos);
}

bool DockWindow::isSecondaryWindowOpen() const
{
    return m_secondaryWindowOpen;
}

void DockWindow::setSecondaryWindowOpen(bool open)
{
    if (!m_secondaryWindow) {
        return;
    }

    if (open == m_secondaryWindowOpen) {
        if (open && m_secondaryWindow->isVisible()) {
            m_secondaryWindow->raise();
            m_secondaryWindow->requestActivate();
        }
        return;
    }

    uiState()->setIsVisible(SECONDARY_WINDOW_OPEN_KEY, open);
    applySecondaryWindowOpen(open);
}

void DockWindow::applySecondaryWindowOpen(bool open)
{
    if (open == m_secondaryWindowOpen) {
        return;
    }

    m_secondaryWindowOpen = open;

    updateSecondaryWindowVisibility(m_currentPage);

    m_secondaryWindowOpenChanged.notify();

    //! NOTE: whether the docks in it count as open depends on it being shown
    if (m_currentPage) {
        notifyAboutDocksOpenStatus();
    }
}

async::Notification DockWindow::secondaryWindowOpenChanged() const
{
    return m_secondaryWindowOpenChanged;
}

bool DockWindow::isSecondaryWindowShown() const
{
    return m_secondaryWindow && m_secondaryWindow->isVisible();
}

void DockWindow::updateSecondaryWindowVisibility(const DockPageView* page)
{
    if (!m_secondaryWindow) {
        return;
    }

    const bool shown = m_secondaryWindowOpen && page && page->secondaryWindowAvailable();

    //! NOTE: KDDockWidgets hides a window's root view (here, the main window holding the docks) along with
    //! the window itself, when restoring it hidden (see withValidNormalGeometries()), but never shows it again
    if (shown) {
        m_secondaryMainWindow->setVisible(true);
    }

    if (shown == m_secondaryWindow->isVisible()) {
        return;
    }

    m_updatingSecondaryWindowVisibility = true;

    //! NOTE: setVisible() (unlike show()) keeps the window state, e.g. maximized
    m_secondaryWindow->setVisible(shown);

    if (shown) {
        m_secondaryWindow->raise();
    }

    m_updatingSecondaryWindowVisibility = false;
}

void DockWindow::loadPageContent(const DockPageView* page)
{
    TRACEFUNC;

    addDock(page->centralDock());

    loadPanels(page);
    loadToolBars(page);

    if (page->statusBar()) {
        addDock(page->statusBar(), Location::Bottom);
    }

    loadTopLevelToolBars(page);
}

void DockWindow::loadTopLevelToolBars(const DockPageView* page)
{
    TRACEFUNC;

    QList<DockToolBarView*> allToolBars = m_toolBars.list();
    allToolBars << page->mainToolBars();

    DockToolBarView* prevToolBar = nullptr;

    for (DockToolBarView* toolBar : allToolBars) {
        auto location = prevToolBar ? Location::Right : Location::Top;
        addDock(toolBar, location, prevToolBar);
        prevToolBar = toolBar;
    }
}

void DockWindow::loadToolBars(const DockPageView* page)
{
    TRACEFUNC;

    for (DockToolBarView* toolBar : page->toolBars()) {
        addDock(toolBar, toolBar->location());
    }

    for (Location location : POSSIBLE_LOCATIONS) {
        if (auto holder = page->holder(DockType::ToolBar, location)) {
            addDock(holder, location);
        }
    }
}

void DockWindow::loadPanels(const DockPageView* page)
{
    TRACEFUNC;

    for (DockPanelView* panel : page->panels()) {
        if (DockPanelView* destinationPanel = page->findPanelForTab(panel)) {
            addPanelAsTab(panel, destinationPanel, page->actualLocation(destinationPanel));
            continue;
        }

        const Location location = panel->location();
        const bool isSideLocation = location == Location::Left || location == Location::Right;
        addDock(panel, location, isSideLocation ? page->centralDock() : nullptr);
    }

    for (Location location : POSSIBLE_LOCATIONS) {
        if (auto holder = page->holder(DockType::Panel, location)) {
            addDock(holder, location);
        }
    }
}

void DockWindow::addDock(DockBase* dock, Location location, const DockBase* relativeTo)
{
    TRACEFUNC;

    registerDock(dock);

    auto* dockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
        KDDockWidgets::QtQuick::asQQuickItem(dock->dockWidget()));

    KDDockWidgets::QtQuick::DockWidget* relativeDockWidgetView = nullptr;
    if (relativeTo) {
        relativeDockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
            KDDockWidgets::QtQuick::asQQuickItem(relativeTo->dockWidget()));
    }

    auto visibilityOption = dock->defaultVisibility() ? KDDockWidgets::InitialVisibilityOption::StartVisible
                            : KDDockWidgets::InitialVisibilityOption::StartHidden;

    KDDockWidgets::InitialOption options(visibilityOption, dock->preferredSize());

    m_mainWindow->addDockWidget(dockWidgetView, locationToKLocation(location), relativeDockWidgetView, options);
}

//! NOTE: docks it (hidden, if it's closed) in the main window, where loadPageContent() puts it by default
void DockWindow::addDockToDefaultLocation(DockBase* dock)
{
    const Location location = dock->location();
    const bool isSideLocation = location == Location::Left || location == Location::Right;
    const bool isPanel = dock->type() == DockType::Panel;

    registerDock(dock);

    auto* dockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
        KDDockWidgets::QtQuick::asQQuickItem(dock->dockWidget()));

    KDDockWidgets::QtQuick::DockWidget* relativeDockWidgetView = nullptr;
    if (isPanel && isSideLocation && m_currentPage && m_currentPage->centralDock()) {
        relativeDockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
            KDDockWidgets::QtQuick::asQQuickItem(m_currentPage->centralDock()->dockWidget()));
    }

    auto visibilityOption = dock->isOpen() ? KDDockWidgets::InitialVisibilityOption::StartVisible
                            : KDDockWidgets::InitialVisibilityOption::StartHidden;

    m_mainWindow->addDockWidget(dockWidgetView, locationToKLocation(location), relativeDockWidgetView,
                                KDDockWidgets::InitialOption(visibilityOption, dock->preferredSize()));
}

//! NOTE: DropController opens the holders to dock into them: one with no docked location would open floating
//! (KDDockWidgets then refusing, or even crashing on, a drop next to it in the main window). A saved layout
//! where they're all closed doesn't keep one, and one may keep a wrong one (e.g. the bottom holder at the right
//! edge: every drop at the bottom then went to the right), so they always get their default one back: a dock
//! keeps a single place in the main window, the previous one goes away
void DockWindow::ensureHoldersHaveDockedLocation(const DockPageView* page)
{
    for (DockType type : { DockType::Panel, DockType::ToolBar }) {
        for (Location location : POSSIBLE_LOCATIONS) {
            DockingHolderView* holder = page->holder(type, location);
            if (holder && !holder->isOpen()) {
                addDock(holder, location);
            }
        }
    }
}

void DockWindow::addPanelAsTab(DockPanelView* panel, DockPanelView* destinationPanel, Location destinationLocation)
{
    registerDock(panel);

    if (panel->defaultVisibility()) {
        destinationPanel->addPanelAsTab(panel, destinationLocation);
        destinationPanel->setCurrentTabIndex(0);
    }
}

void DockWindow::registerDock(DockBase* dock)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(dock) {
        return;
    }

    auto* registry = KDDockWidgets::DockRegistry::self(iocContext()->id);
    auto* dockWidget = dock->dockWidget();

    if (!registry->containsDockWidget(dockWidget->uniqueName())) {
        registry->registerDockWidget(dockWidget);
    }
}

void DockWindow::handleUnknownDock(const DockPageView* page, DockBase* unknownDock)
{
    DockPanelView* unknownPanel = dynamic_cast<DockPanelView*>(unknownDock);
    if (!unknownPanel) {
        addDock(unknownDock, unknownDock->location(), page->centralDock());
        return;
    }

    if (DockPanelView* destinationPanel = page->findPanelForTab(unknownPanel)) {
        addPanelAsTab(unknownPanel, destinationPanel, page->actualLocation(destinationPanel));
        return;
    }

    DockingHolderView* holder = page->holder(DockType::Panel, unknownPanel->location());
    IF_ASSERT_FAILED(holder) {
        addDock(unknownDock, unknownDock->location(), page->centralDock());
        return;
    }

    registerDock(unknownPanel);

    holder->open(); // init the group...

    auto* holderDockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
        KDDockWidgets::QtQuick::asQQuickItem(holder->dockWidget()));
    if (holderDockWidgetView) {
        KDDockWidgets::Core::Group* group = holderDockWidgetView->group();
        if (group) {
            group->addTab(unknownPanel->dockWidget());
        }
    }

    holder->close();

    if (!unknownPanel->isVisible()) {
        unknownPanel->close();
    }
}

DockPageView* DockWindow::pageByUri(const QString& uri) const
{
    for (DockPageView* page : m_pages.list()) {
        if (page->uri() == uri) {
            return page;
        }
    }

    return nullptr;
}

bool DockWindow::doLoadPage(const QString& uri, const QVariantMap& params)
{
    DockPageView* newPage = pageByUri(uri);
    IF_ASSERT_FAILED(newPage) {
        return false;
    }

    newPage->setVisible(true);

    //! NOTE: shown before its docks get restored into it, so that its layout gets its actual size
    updateSecondaryWindowVisibility(newPage);

    loadPageContent(newPage);
    restorePageState(newPage);
    ensureHoldersHaveDockedLocation(newPage);
    initDocks(newPage);

    newPage->setParams(params);

    m_currentPage = newPage;

    connect(m_currentPage, &DockPageView::layoutRequested,
            this, &DockWindow::forceLayout, Qt::UniqueConnection);

    return true;
}

//! NOTE: returns the layout to save as the regular state, keeping the full one (see withoutSecondaryWindow())
//! aside with the hash of that regular one: it's only used again while the regular state is still that one,
//! i.e. as long as no build without the secondary window has saved its own layout in the meantime
QByteArray DockWindow::splitSecondaryWindowLayout(const QString& stateName, const QByteArray& layout)
{
    if (!m_secondaryMainWindow) {
        return layout;
    }

    const QByteArray regularLayout = withoutSecondaryWindow(layout);

    QString fullLayout;
    if (regularLayout != layout) {
        fullLayout = layoutHash(regularLayout) + u'\n' + QString::fromLatin1(layout.toBase64());
    }

    const QString key = SECONDARY_WINDOW_LAYOUT_KEY_PREFIX + stateName;
    if (uiState()->uiItemState(key) != fullLayout) {
        uiState()->setUiItemState(key, fullLayout);
    }

    return regularLayout;
}

QByteArray DockWindow::mergeSecondaryWindowLayout(const QString& stateName, const QByteArray& regularLayout) const
{
    //! NOTE: KDDockWidgets would refuse the whole layout naming a main window it doesn't know
    if (!m_secondaryMainWindow) {
        return regularLayout;
    }

    const QString fullLayout = uiState()->uiItemState(SECONDARY_WINDOW_LAYOUT_KEY_PREFIX + stateName);

    const qsizetype separatorIndex = fullLayout.indexOf(u'\n');
    if (separatorIndex < 0 || QStringView(fullLayout).left(separatorIndex) != layoutHash(regularLayout)) {
        return regularLayout;
    }

    return QByteArray::fromBase64(QStringView(fullLayout).mid(separatorIndex + 1).toLatin1());
}

void DockWindow::saveWindowGeometry()
{
    /// NOTE: The state of all dock widgets is also saved here,
    /// since the library does not provide the ability to save
    /// and restore only the application geometry.
    uiState()->setWindowGeometry(splitSecondaryWindowLayout(WINDOW_GEOMETRY_STATE_NAME, windowState()));
}

void DockWindow::restoreGeometry()
{
    TRACEFUNC;

    const QByteArray layout = mergeSecondaryWindowLayout(WINDOW_GEOMETRY_STATE_NAME, uiState()->windowGeometry());
    if (layout.isEmpty()) {
        return;
    }

    if (restoreLayout(layout)) {
        m_hasGeometryBeenRestored = true;
    } else {
        LOGE() << "Could not restore the window geometry!";
    }
}

void DockWindow::savePageState(const QString& pageName)
{
    TRACEFUNC;

    m_reloadCurrentPageAllowed = false;
    uiState()->setPageState(pageName, splitSecondaryWindowLayout(pageName, windowState()));
    m_reloadCurrentPageAllowed = true;
}

void DockWindow::restorePageState(const DockPageView* page)
{
    TRACEFUNC;

    const QString& pageName = page->objectName();

    ValNt<QByteArray> pageStateValNt = uiState()->pageState(pageName);
    const QByteArray layout = mergeSecondaryWindowLayout(pageName, pageStateValNt.val);
    const bool layoutIsEmpty = layout.isEmpty();

    QSet<DockBase*> unknownDocks;
    if (!layoutIsEmpty) {
        for (DockBase* dock : page->allDocks()) {
            const KDDockWidgets::Core::DockWidget* dockWidget = dock->dockWidget();
            if (!layout.contains(dockWidget->uniqueName().toLocal8Bit())) {
                unknownDocks.insert(dock);
            }
        }
    }

    /// NOTE: Do not restore geometry
    bool ok = restoreLayout(layout, true /*restoreRelativeToMainWindow*/);
    if (!ok) {
        LOGE() << "Could not restore the state of " << pageName << "!";
    }

    if (!layoutIsEmpty) {
        for (DockBase* dock : unknownDocks) {
            handleUnknownDock(page, dock);
        }
    }

    if (!pageStateValNt.notification.isConnected()) {
        pageStateValNt.notification.onNotify(this, [this, pageName]() {
            bool isCurrentPage = m_currentPage && (m_currentPage->objectName() == pageName);
            if (isCurrentPage) {
                reloadCurrentPage();
            }
        });
    }
}

//! NOTE: KDDockWidgets (QtQuick) only records a window's normal geometry while it's in the normal state, so a
//! window that stayed maximized for a whole session is saved with an empty one - restoring it then sets that
//! empty geometry before maximizing it again, which leaves the window shrunk to its minimum size on macOS.
//! Its maximized geometry is a sane normal geometry to fall back on.
//! A main window is also never restored minimized (e.g. quit while minimized): it would start hidden in the Dock,
//! and be saved minimized again
//! The secondary window is always restored hidden (DockWindow shows it, depending on the current page), and not
//! in full screen, which would make it take over a whole screen as soon as the app starts
static QByteArray withValidNormalGeometries(const QByteArray& layout)
{
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(layout, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return layout;
    }

    QJsonObject root = document.object();
    bool changed = false;

    for (const QString& windowsKey : { QStringLiteral("mainWindows"), QStringLiteral("floatingWindows") }) {
        QJsonArray windows = root.value(windowsKey).toArray();

        for (qsizetype i = 0; i < windows.size(); ++i) {
            QJsonObject window = windows.at(i).toObject();

            constexpr int MINIMIZED_STATE_FLAG = 1; // KDDockWidgets::WindowState::Minimized
            constexpr int FULLSCREEN_STATE_FLAG = 4; // KDDockWidgets::WindowState::FullScreen
            const int windowState = window.value(QStringLiteral("windowState")).toInt();
            if (windowsKey == QStringLiteral("mainWindows") && (windowState & MINIMIZED_STATE_FLAG)) {
                window.insert(QStringLiteral("windowState"), windowState & ~MINIMIZED_STATE_FLAG);
                windows.replace(i, window);
                changed = true;
            }

            const bool isSecondaryWindow = windowsKey == QStringLiteral("mainWindows")
                                           && window.value(QStringLiteral("uniqueName")).toString() == QLatin1String(SECONDARY_WINDOW_NAME);
            if (isSecondaryWindow && (window.value(QStringLiteral("isVisible")).toBool() || (windowState & FULLSCREEN_STATE_FLAG))) {
                window.insert(QStringLiteral("isVisible"), false);
                window.insert(QStringLiteral("windowState"), window.value(QStringLiteral("windowState")).toInt() & ~FULLSCREEN_STATE_FLAG);
                windows.replace(i, window);
                changed = true;
            }

            const QJsonObject normalGeometry = window.value(QStringLiteral("normalGeometry")).toObject();
            const QJsonObject geometry = window.value(QStringLiteral("geometry")).toObject();

            const bool isNormalEmpty = normalGeometry.value(QStringLiteral("width")).toInt() <= 0
                                       || normalGeometry.value(QStringLiteral("height")).toInt() <= 0;
            const bool isGeometryValid = geometry.value(QStringLiteral("width")).toInt() > 0
                                         && geometry.value(QStringLiteral("height")).toInt() > 0;
            if (!isNormalEmpty || !isGeometryValid) {
                continue;
            }

            window.insert(QStringLiteral("normalGeometry"), geometry);
            windows.replace(i, window);
            changed = true;
        }

        if (changed) {
            root.insert(windowsKey, windows);
        }
    }

    return changed ? QJsonDocument(root).toJson(QJsonDocument::Compact) : layout;
}

bool DockWindow::restoreLayout(const QByteArray& layout, bool restoreRelativeToMainWindow)
{
    if (layout.isEmpty()) {
        return true;
    }

    TRACEFUNC;

    auto option = restoreRelativeToMainWindow ? KDDockWidgets::RestoreOption_RelativeToMainWindow
                  : KDDockWidgets::RestoreOption_None;

    KDDockWidgets::LayoutSaver layoutSaver(iocContext()->id, option);
    bool ok = layoutSaver.restoreLayout(withValidNormalGeometries(layout));

    //! NOTE: KDDockWidgets' Layout::onResize() ignores every resize while a restore is in progress,
    //! but a restored floating window's layout view only reaches its real size (the restored window
    //! geometry, propagated through DockFloatingWindow.qml's anchors) during that same restore - its
    //! layout would otherwise stay at whatever size its view had when the layout was deserialized,
    //! leaving the panel stuck smaller than its own window until the user manually resizes it
    syncFloatingLayoutsToViewSize();
    muse::async::Async::call(this, [this]() {
        syncFloatingLayoutsToViewSize();
    });

    return ok;
}

void DockWindow::syncFloatingLayoutsToViewSize()
{
    QList<KDDockWidgets::Core::Layout*> layouts;

    for (KDDockWidgets::Core::FloatingWindow* floatingWindow : KDDockWidgets::DockRegistry::self(iocContext()->id)->floatingWindows()) {
        layouts << floatingWindow->layout();
    }

    //! NOTE: same for the secondary window, which may have just been shown for the restore
    if (m_secondaryMainWindow) {
        layouts << m_secondaryMainWindow->mainWindow()->layout();
    }

    for (KDDockWidgets::Core::Layout* floatingLayout : layouts) {
        if (!floatingLayout || !floatingLayout->view()) {
            continue;
        }

        //! NOTE: a view that hasn't got its real size yet (the deferred call covers it) can't hold
        //! the layout - setting it anyway would only make KDDockWidgets log a layout error
        const QSize viewSize = floatingLayout->view()->size();
        const QSize minSize = floatingLayout->layoutMinimumSize();
        if (viewSize.isEmpty() || viewSize.width() < minSize.width() || viewSize.height() < minSize.height()) {
            continue;
        }

        floatingLayout->setLayoutSize(viewSize);
    }
}

bool DockWindow::checkLayoutIsCorrupted() const
{
    TRACEFUNC;

    for (const DockBase* dock : m_currentPage->allDocks()) {
        if (!dock) {
            continue;
        }

        if (!dock->floatable() && dock->floating()) {
            return true;
        }
    }

    return false;
}

void DockWindow::forceLayout()
{
    m_mainWindow->layoutEqually();
}

QByteArray DockWindow::windowState() const
{
    TRACEFUNC;

    KDDockWidgets::LayoutSaver layoutSaver(iocContext()->id, KDDockWidgets::RestoreOption_None);
    return layoutSaver.serializeLayout();
}

void DockWindow::reloadCurrentPage()
{
    if (!m_reloadCurrentPageAllowed) {
        return;
    }

    TRACEFUNC;

    //! NOTE: e.g. another workspace was selected, with its own open state for the secondary window
    if (m_secondaryWindow) {
        applySecondaryWindowOpen(uiState()->isVisible(SECONDARY_WINDOW_OPEN_KEY, false));
    }

    clearRegistry(iocContext()->id);

    for (DockBase* dock : m_currentPage->allDocks()) {
        dock->deinit();
    }

    QString currentPageUriBackup = currentPageUri();

    /// NOTE: for reset geometry
    m_currentPage = nullptr;

    if (doLoadPage(currentPageUriBackup)) {
        notifyAboutDocksOpenStatus();
    }
}

void DockWindow::initDocks(DockPageView* page)
{
    TRACEFUNC;

    //! before init we should correct toolbars sizes
    m_topLevelToolBarsLayout->adjustContentForAvailableSpace(page);

    for (DockToolBarView* toolbar : m_toolBars.list()) {
        toolbar->setParentItem(this);
        toolbar->init();
    }

    if (page) {
        page->setParentItem(this);
        page->init();
    }

    m_topLevelToolBarsLayout->setUpPageConnections(page);
    m_topLevelToolBarsLayout->scheduleRelayout();
}

void DockWindow::notifyAboutDocksOpenStatus()
{
    const DockPageView* page = currentPage();

    IF_ASSERT_FAILED(page) {
        return;
    }

    QStringList dockNames;

    for (DockToolBarView* toolBar : page->mainToolBars()) {
        dockNames << toolBar->objectName();
    }

    for (DockToolBarView* toolBar : page->toolBars()) {
        dockNames << toolBar->objectName();
    }

    for (DockPanelView* panel : page->panels()) {
        dockNames << panel->objectName();
    }

    if (page->statusBar()) {
        dockNames << page->statusBar()->objectName();
    }

    m_docksOpenStatusChanged.send(dockNames);
}

QList<DockToolBarView*> DockWindow::topLevelToolBars(const DockPageView* page) const
{
    QList<DockToolBarView*> toolBars = m_toolBars.list();

    if (page) {
        toolBars << page->mainToolBars();
    }

    return toolBars;
}
