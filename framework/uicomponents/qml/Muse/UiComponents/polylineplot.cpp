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

#include "polylineplot.h"

#include <QCursor>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QBrush>
#include <QHoverEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

#include "realfn.h"

using namespace muse::uicomponents;

namespace {
constexpr double MOVE_THRESHOLD = 3.0;
constexpr double EPSILON = 1e-12;
constexpr double BOUNDARY_MARGIN = 0.1;

static constexpr int INVALID_POINT_IDX = -1;
static constexpr int PENDING_POINT_IDX = -2;

// Matches notevelocityoverlay.cpp's drag-tooltip chip exactly, for visual consistency between
// the two features' drag tooltips.
constexpr qreal VALUE_LABEL_FONT_PX = 11.0;
constexpr qreal VALUE_LABEL_GAP_PX = 4.0;
constexpr qreal VALUE_LABEL_PADDING_X_PX = 4.0;
constexpr qreal VALUE_LABEL_PADDING_Y_PX = 2.0;
constexpr qreal VALUE_LABEL_CORNER_RADIUS_PX = 3.0;
// A point marker (esp. selected, with its middle ring) extends past its bare center by a few px -
// unlike a velocity bar (whose own width already keeps the gap clear of it), the point's own radius
// needs to be added to the gap too, or the chip can overlap the marker itself.
constexpr qreal VALUE_LABEL_POINT_CLEARANCE_PX = 6.0;

static inline qreal toPxX(const QQuickItem* item, qreal xN)
{
    return xN * item->width();
}

static inline qreal toPxY(const QQuickItem* item, qreal yN)
{
    const qreal h = item->height();
    if (h <= 0.0) {
        return 0.0;
    }

    // NOTE: preserve full height of the line at top/bottom edges
    const auto* polyline = static_cast<const PolylinePlot*>(item);
    const qreal inset = std::clamp(polyline->lineWidth() * 0.5, 0.0, h * 0.5);
    const qreal drawableHeight = h - (2.0 * inset);

    return inset + (1.0 - yN) * drawableHeight;
}

static qreal pointToSegmentDistance(const QPointF& point, const QPointF& segmentStart, const QPointF& segmentEnd)
{
    const QPointF segment = segmentEnd - segmentStart;
    const qreal segmentLengthSquared = QPointF::dotProduct(segment, segment);
    if (segmentLengthSquared <= EPSILON) {
        return std::hypot(point.x() - segmentStart.x(), point.y() - segmentStart.y());
    }

    const QPointF segmentStartToPoint = point - segmentStart;
    qreal t = QPointF::dotProduct(segmentStartToPoint, segment) / segmentLengthSquared;
    t = std::max<qreal>(0.0, std::min<qreal>(1.0, t));

    const QPointF closestPoint = segmentStart + segment * t;
    return std::hypot(point.x() - closestPoint.x(), point.y() - closestPoint.y());
}

static GhostPoint ghostPointToSegmentDistance(const QPointF& point, const QPointF& segmentStart, const QPointF& segmentEnd)
{
    const QPointF segment = segmentEnd - segmentStart;
    const qreal segmentLengthSquared = QPointF::dotProduct(segment, segment);
    if (segmentLengthSquared <= EPSILON) {
        const qreal distance = std::hypot(point.x() - segmentStart.x(), point.y() - segmentStart.y());
        return { segmentStart, distance };
    }

    const QPointF segmentStartToPoint = point - segmentStart;
    qreal t = QPointF::dotProduct(segmentStartToPoint, segment) / segmentLengthSquared;
    t = std::max<qreal>(0.0, std::min<qreal>(1.0, t));

    const QPointF closestPoint = segmentStart + segment * t;
    const qreal distance = std::hypot(point.x() - closestPoint.x(), point.y() - closestPoint.y());
    return { closestPoint, distance };
}

// NOTE: can be replaced with std::lerp in C++20
static double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

// compute Y at X using linear interpolation
static double valueAtX(const QVector<QPointF>& sortedPoints, double x)
{
    if (sortedPoints.isEmpty()) {
        return 0.0;
    }

    if (x <= sortedPoints.front().x()) {
        return sortedPoints.front().y();
    }

    if (x >= sortedPoints.back().x()) {
        return sortedPoints.back().y();
    }

    for (int i = 0; i < sortedPoints.size() - 1; ++i) {
        const auto& a = sortedPoints[i];
        const auto& b = sortedPoints[i + 1];
        if (x >= a.x() && x <= b.x()) {
            const double dx = b.x() - a.x();
            if (std::abs(dx) <= EPSILON) {
                return a.y();
            }
            const double t = (x - a.x()) / dx;
            return lerp(a.y(), b.y(), t);
        }
    }

    return sortedPoints.back().y();
}

//! NOTE: same shape as muse::mpe::evaluateAt() - duplicated on purpose (uicomponents doesn't depend on mpe), keep them
//! in sync: two quadratic Bezier arcs meeting at the bend point, sharing a tangent there, with control points clamped
//! to the segment's value range
static double bentValueAt(double from, double to, double bendT, double bendValue, double s)
{
    if (bendT <= 0.0 || bendT >= 1.0) {
        return lerp(from, to, s);
    }

    const auto quadraticBezier = [](double u, double p0, double p1, double p2) {
        const double v = 1.0 - u;
        return v * v * p0 + 2.0 * v * u * p1 + u * u * p2;
    };

    const double range = to - from;
    const double bend = from + std::clamp(bendValue, 0.0, 1.0) * range;
    const double lo = std::min(from, to);
    const double hi = std::max(from, to);
    const double halfSlope = 0.5 * range;

    if (s <= bendT) {
        const double q1 = std::clamp(bend - bendT * halfSlope, lo, hi);
        return quadraticBezier(s / bendT, from, q1, bend);
    }

    const double q2 = std::clamp(bend + (1.0 - bendT) * halfSlope, lo, hi);
    return quadraticBezier((s - bendT) / (1.0 - bendT), bend, q2, to);
}

constexpr int BEND_SAMPLES_PER_SEGMENT = 32;
constexpr qreal BEND_HANDLE_HALF_SIZE_PX = 4.0;
constexpr qreal BEND_HANDLE_MIN_SEGMENT_WIDTH_PX = 24.0; // no handle squeezed between two close points
}

PolylinePlot::PolylinePlot(QQuickItem* parent)
    : QQuickPaintedItem(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);

    setAntialiasing(true);
    setOpaquePainting(false);

    m_standardPointStyle = new PolylinePointStyle(this);
    QObject::connect(m_standardPointStyle, &PolylinePointStyle::styleChanged, this, [this]() {
        update();
    });

    m_ghostPointStyle = new PolylinePointStyle(this);
    QObject::connect(m_ghostPointStyle, &PolylinePointStyle::styleChanged, this, [this]() {
        update();
    });

    m_selectedPointStyle = new PolylinePointStyle(this);
    QObject::connect(m_selectedPointStyle, &PolylinePointStyle::styleChanged, this, [this]() {
        update();
    });
}

void PolylinePlot::init()
{
    dispatcher()->reg(this, "action://cancel", [this](){
        // A bend being dragged isn't committed yet: back to what it was (the model never saw the preview)
        if (m_pressedBendIndex >= 0 && m_pressedBendIndex < m_segmentBends.size()) {
            m_segmentBends[m_pressedBendIndex].value = m_pressedBendOriginalValue;
            rebuildVisiblePoints();
        }

        // emit signal and let decide model what to do
        emit dragCancelled();
        // Qt suppresses hover events while a mouse button is held, so the
        // ghost-point preview would otherwise stay painted at the press
        // position until the user releases and moves the mouse.
        m_hoveredOnLine = false;
        resetGestureState();
    });
}

PolylinePointStyle* PolylinePlot::standardPointStyle()
{
    return m_standardPointStyle;
}

PolylinePointStyle* PolylinePlot::ghostPointStyle()
{
    return m_ghostPointStyle;
}

PolylinePointStyle* PolylinePlot::selectedPointStyle()
{
    return m_selectedPointStyle;
}

bool PolylinePlot::ghostPointsEnabled() const
{
    return m_ghostPointsEnabled;
}

void PolylinePlot::setGhostPointsEnabled(bool v)
{
    if (m_ghostPointsEnabled == v) {
        return;
    }

    m_ghostPointsEnabled = v;
    emit ghostPointsEnabledChanged();

    update();
}

bool PolylinePlot::selectedPointsEnabled() const
{
    return m_selectedPointsEnabled;
}

void PolylinePlot::setSelectedPointsEnabled(bool v)
{
    if (m_selectedPointsEnabled == v) {
        return;
    }

    m_selectedPointsEnabled = v;
    emit selectedPointsEnabledChanged();

    update();
}

QColor PolylinePlot::lineColor() const
{
    return m_lineColor;
}

void PolylinePlot::setLineColor(const QColor& c)
{
    if (m_lineColor == c) {
        return;
    }

    m_lineColor = c;
    emit lineColorChanged();
    update();
}

qreal PolylinePlot::lineWidth() const
{
    return m_lineWidth;
}

void PolylinePlot::setLineWidth(qreal w)
{
    if (m_lineWidth == w) {
        return;
    }

    m_lineWidth = w;
    emit lineWidthChanged();

    update();
}

bool PolylinePlot::drawBackground() const
{
    return m_drawBackground;
}

void PolylinePlot::setDrawBackground(bool v)
{
    if (m_drawBackground == v) {
        return;
    }

    m_drawBackground = v;
    emit drawBackgroundChanged();

    update();
}

qreal PolylinePlot::baselineN() const
{
    return m_baselineN;
}

void PolylinePlot::setBaselineN(qreal v)
{
    v = clamp01(v);

    if (m_baselineN == v) {
        return;
    }

    m_baselineN = v;
    emit baselineNChanged();

    update();
}

qreal PolylinePlot::hitRadius() const
{
    return m_hitRadius;
}

void PolylinePlot::setHitRadius(qreal r)
{
    if (m_hitRadius == r) {
        return;
    }

    m_hitRadius = r;
    emit hitRadiusChanged();
}

bool PolylinePlot::isSnapEnabled() const
{
    return m_isSnapEnabled;
}

void PolylinePlot::setIsSnapEnabled(bool v)
{
    if (m_isSnapEnabled == v) {
        return;
    }

    m_isSnapEnabled = v;
    emit isSnapEnabledChanged();
}

qreal PolylinePlot::snapThresholdPx() const
{
    return m_snapThresholdPx;
}

void PolylinePlot::setSnapThresholdPx(qreal v)
{
    if (m_snapThresholdPx == v) {
        return;
    }

    m_snapThresholdPx = v;
    emit snapThresholdPxChanged();
}

QVector<QPointF> PolylinePlot::points() const
{
    return m_points;
}

void PolylinePlot::setPoints(const QVector<QPointF>& pts)
{
    if (m_points == pts) {
        if (m_points.isEmpty()) {
            updateBaselineFromDefaultValue();

            // invalidate colors under line
            if (!m_colorsUnderLine.empty()) {
                m_colorsUnderLine.clear();
                emit colorsUnderLineChanged();
            }

            update();
        }
        return;
    }
    m_points = pts;
    emit pointsChanged();

    // invalidate colors under line
    if (!m_colorsUnderLine.empty()) {
        m_colorsUnderLine.clear();
        emit colorsUnderLineChanged();
    }

    if (m_points.isEmpty()) {
        updateBaselineFromDefaultValue();
    }

    // if there's pending point, find its index and mark as ready for drag
    if (m_pressed && m_pressedPointIndex == PENDING_POINT_IDX && m_pressedOnLine) {
        if (width() > 0 && height() > 0 && !m_points.isEmpty()) {
            const auto ghostPoint = ghostPointToPolylinePx(m_pressPx);

            QPointF pN(clamp01(ghostPoint.point.x() / width()),
                       1.0 - clamp01(ghostPoint.point.y() / height()));

            const QPointF targetDomain = domainFromNormalized(pN);

            int bestIdx = INVALID_POINT_IDX;
            double bestScore = std::numeric_limits<double>::max();

            for (int i = 0; i < m_points.size(); ++i) {
                const double dx = std::abs(m_points[i].x() - targetDomain.x());
                const double dy = std::abs(m_points[i].y() - targetDomain.y());
                const double score = dx * 1000.0 + dy;
                if (score < bestScore) {
                    bestScore = score;
                    bestIdx = i;
                }
            }

            if (bestIdx >= 0) {
                m_pressedOnPoint = true;
                m_pressedPointIndex = bestIdx;
                m_draggedPointDomain = m_points[bestIdx];
                m_hasDraggedPointDomain = true;
            }
        }
    }

    rebuildVisiblePoints();
}

QVector<QColor> PolylinePlot::colorsUnderLine() const
{
    return m_colorsUnderLine;
}

void PolylinePlot::setColorsUnderLine(const QVector<QColor>& c)
{
    if (m_colorsUnderLine == c) {
        return;
    }

    m_colorsUnderLine = c;
    emit colorsUnderLineChanged();

    update();
}

qreal PolylinePlot::defaultValue() const
{
    return m_defaultValue;
}

void PolylinePlot::setXRangeFrom(qreal v)
{
    if (m_xFrom == v) {
        return;
    }

    m_xFrom = v;
    emit xRangeFromChanged();

    rebuildVisiblePoints();
}

qreal PolylinePlot::xRangeTo() const
{
    return m_xTo;
}

void PolylinePlot::setXRangeTo(qreal v)
{
    if (m_xTo == v) {
        return;
    }

    m_xTo = v;
    emit xRangeToChanged();

    rebuildVisiblePoints();
}

qreal PolylinePlot::yRangeFrom() const
{
    return m_yFrom;
}

void PolylinePlot::setYRangeFrom(qreal v)
{
    if (m_yFrom == v) {
        return;
    }

    m_yFrom = v;
    emit yRangeFromChanged();

    rebuildVisiblePoints();
}

qreal PolylinePlot::yRangeTo() const
{
    return m_yTo;
}

void PolylinePlot::setYRangeTo(qreal v)
{
    if (m_yTo == v) {
        return;
    }

    m_yTo = v;
    emit yRangeToChanged();

    rebuildVisiblePoints();
}

qreal PolylinePlot::ySplitNormalized() const
{
    return m_ySplitNormalized;
}

void PolylinePlot::setYSplitNormalized(qreal v)
{
    v = clamp01(v);
    if (m_ySplitNormalized == v) {
        return;
    }

    m_ySplitNormalized = v;
    emit ySplitNormalizedChanged();

    rebuildVisiblePoints();
}

qreal PolylinePlot::ySplitValue() const
{
    return m_ySplitValue;
}

void PolylinePlot::setYSplitValue(qreal v)
{
    const qreal low = std::min(m_yFrom, m_yTo);
    const qreal high = std::max(m_yFrom, m_yTo);
    qreal clampedValue = std::clamp(v, low, high);

    if (m_ySplitValue == clampedValue) {
        return;
    }

    m_ySplitValue = clampedValue;
    emit ySplitValueChanged();

    rebuildVisiblePoints();
}

bool PolylinePlot::yAxisInverse() const
{
    return m_yAxisInverse;
}

void PolylinePlot::setYAxisInverse(bool v)
{
    if (m_yAxisInverse == v) {
        return;
    }

    m_yAxisInverse = v;
    emit yAxisInverseChanged();
    rebuildVisiblePoints();
}

bool PolylinePlot::hasActivePoint() const
{
    return m_hasActivePoint;
}

qreal PolylinePlot::activePointX() const
{
    return m_activePointPx.x();
}

qreal PolylinePlot::activePointY() const
{
    return m_activePointPx.y();
}

qreal PolylinePlot::activePointValue() const
{
    return m_activePointValue;
}

QString PolylinePlot::activePointLabel() const
{
    return m_activePointLabel;
}

void PolylinePlot::setActivePointLabel(const QString& label)
{
    if (m_activePointLabel == label) {
        return;
    }

    m_activePointLabel = label;
    emit activePointLabelChanged();

    update();
}

void PolylinePlot::setValueLabelColors(const QColor& background, const QColor& text)
{
    m_valueLabelBgColor = background;
    m_valueLabelTextColor = text;
    update();
}

void PolylinePlot::setDefaultValue(qreal v)
{
    if (m_defaultValue == v) {
        return;
    }

    m_defaultValue = v;
    emit defaultValueChanged();

    // if there are no points, baseline should reflect defaultY immediately
    if (m_points.isEmpty()) {
        updateBaselineFromDefaultValue();
        rebuildVisiblePoints();
    }
}

qreal PolylinePlot::xRangeFrom() const
{
    return m_xFrom;
}

qreal PolylinePlot::clamp01(qreal v) const
{
    return std::max<qreal>(0.0, std::min<qreal>(1.0, v));
}

QPointF PolylinePlot::clamp01(const QPointF& p) const
{
    return QPointF(clamp01(p.x()), clamp01(p.y()));
}

bool PolylinePlot::hasValidXRange() const
{
    return std::isfinite(m_xFrom) && std::isfinite(m_xTo) && std::abs(m_xTo - m_xFrom) > EPSILON;
}

bool PolylinePlot::hasValidYRange() const
{
    return std::isfinite(m_yFrom) && std::isfinite(m_yTo) && std::abs(m_yTo - m_yFrom) > EPSILON;
}

bool PolylinePlot::hasValidYSplit() const
{
    return hasValidYRange() && !muse::RealIsEqualOrLess(m_ySplitNormalized, 0.0) && !muse::RealIsEqualOrMore(m_ySplitNormalized, 1.0);
}

qreal PolylinePlot::yDomainFromNormalized(qreal yNormalized) const
{
    const qreal n = clamp01(yNormalized);

    // simple linear mapping (no split)
    if (!hasValidYSplit()) {
        return m_yFrom + n * (m_yTo - m_yFrom);
    }

    const qreal splitNormalized = m_ySplitNormalized;
    const qreal splitValue      = m_ySplitValue;

    // first segment (bottom → split)
    if (n <= splitNormalized) {
        if (splitNormalized <= EPSILON) {
            return splitValue;
        }

        const qreal segmentN = n / splitNormalized;
        return m_yFrom + segmentN * (splitValue - m_yFrom);
    }

    // second segment (split → top)
    const qreal secondSegmentSize = 1.0 - splitNormalized;
    if (secondSegmentSize <= EPSILON) {
        return splitValue;
    }

    const qreal segmentN = (n - splitNormalized) / secondSegmentSize;
    return splitValue + segmentN * (m_yTo - splitValue);
}

qreal PolylinePlot::yNormalizedFromDomain(qreal yDomain) const
{
    // simple linear mapping (no split)
    if (!hasValidYSplit()) {
        return (yDomain - m_yFrom) / (m_yTo - m_yFrom);
    }

    const qreal splitNormalized = m_ySplitNormalized;
    const qreal splitValue      = m_ySplitValue;

    const bool increasing = (m_yTo >= m_yFrom);

    const bool isInFirstSegment
        =increasing ? (yDomain <= splitValue)
          : (yDomain >= splitValue);

    // ---- first segment ----
    if (isInFirstSegment) {
        const qreal segmentSize = splitValue - m_yFrom;
        if (std::abs(segmentSize) <= EPSILON) {
            return splitNormalized;
        }

        const qreal segmentN = (yDomain - m_yFrom) / segmentSize;
        return segmentN * splitNormalized;
    }

    // ---- second segment ----
    const qreal segmentSize = m_yTo - splitValue;
    if (std::abs(segmentSize) <= EPSILON) {
        return splitNormalized;
    }

    const qreal segmentN = (yDomain - splitValue) / segmentSize;
    return splitNormalized + segmentN * (1.0 - splitNormalized);
}

void PolylinePlot::updateBaselineFromDefaultValue()
{
    const qreal baseline = normalizedFromDomain(QPointF(m_xFrom, m_defaultValue)).y();
    if (m_baselineN == baseline) {
        return;
    }

    m_baselineN = baseline;
    emit baselineNChanged();
}

void PolylinePlot::updateActivePoint()
{
    // A bend being dragged: its handle is the active point (its value readout included)
    if (m_pressedBendIndex >= 0) {
        if (const std::optional<QPointF> handleN = bendHandleN(m_pressedBendIndex)) {
            const QPointF newPx(toPxX(this, handleN->x()), toPxY(this, handleN->y()));
            const qreal newValue = domainFromNormalized(*handleN).y();
            const bool changed = !m_hasActivePoint || newPx != m_activePointPx || std::abs(newValue - m_activePointValue) > EPSILON;
            m_hasActivePoint = true;
            m_activePointPx = newPx;
            m_activePointValue = newValue;
            if (changed) {
                emit activePointChanged();
            }
            return;
        }
    }

    const int hoveredDomainIdx = pointIndexAtPx(m_hoverPx);
    int draggedDisplayDomainIdx = INVALID_POINT_IDX;
    if (m_pressed && m_hasDraggedPointDomain && !m_points.isEmpty()) {
        double bestScore = std::numeric_limits<double>::max();
        for (int i = 0; i < m_points.size(); ++i) {
            const double dx = std::abs(m_points[i].x() - m_draggedPointDomain.x());
            const double dy = std::abs(m_points[i].y() - m_draggedPointDomain.y());
            const double score = dx * 1000.0 + dy;
            if (score < bestScore) {
                bestScore = score;
                draggedDisplayDomainIdx = i;
            }
        }
    }

    // m_pressedPointIndex must stay at the original model index (so consumed neighbors can be restored), but selection indices
    // need to shift when other indices are deleted...
    if (m_selectedPointsEnabled && draggedDisplayDomainIdx >= 0) {
        if (m_selectedPointsIndices.size() != 1 || !muse::contains(m_selectedPointsIndices, draggedDisplayDomainIdx)) {
            m_selectedPointsIndices.clear();
            m_selectedPointsIndices.insert(draggedDisplayDomainIdx);
        }
    }

    const int activeDomainIdx = (draggedDisplayDomainIdx >= 0)
                                ? draggedDisplayDomainIdx
                                : ((m_pressedPointIndex >= 0) ? m_pressedPointIndex : hoveredDomainIdx);

    if (activeDomainIdx < 0) {
        if (m_hasActivePoint) {
            m_hasActivePoint = false;
            m_activePointPx = {};
            m_activePointValue = 0.0;
            emit activePointChanged();
        }
        return;
    }

    QPointF activeN;
    bool found = false;
    for (int i = 0; i < m_pointsNVisible.size() && i < m_visibleToDomainIndex.size(); ++i) {
        if (m_visibleToDomainIndex[i] == activeDomainIdx) {
            activeN = m_pointsNVisible[i];
            found = true;
            break;
        }
    }

    if (!found) {
        if (m_hasActivePoint) {
            m_hasActivePoint = false;
            m_activePointPx = {};
            m_activePointValue = 0.0;
            emit activePointChanged();
        }
        return;
    }

    const QPointF newPx(toPxX(this, activeN.x()), toPxY(this, activeN.y()));
    const qreal newValue = (draggedDisplayDomainIdx >= 0 && m_hasDraggedPointDomain)
                           ? m_draggedPointDomain.y()
                           : m_points[activeDomainIdx].y();
    const bool changed = (!m_hasActivePoint
                          || newPx != m_activePointPx
                          || std::abs(newValue - m_activePointValue) > EPSILON);

    m_hasActivePoint = true;
    m_activePointPx = newPx;
    m_activePointValue = newValue;

    if (changed) {
        emit activePointChanged();
    }
}

QPointF PolylinePlot::normalizedFromDomain(const QPointF& p) const
{
    if (!hasValidXRange() || !hasValidYRange()) {
        return clamp01(QPointF(0.0, 0.0));
    }

    const qreal xN = (p.x() - m_xFrom) / (m_xTo - m_xFrom);
    qreal yN = clamp01(yNormalizedFromDomain(p.y()));

    if (m_yAxisInverse) {
        yN = 1.0 - yN;
    }

    return clamp01(QPointF(xN, yN));
}

QPointF PolylinePlot::domainFromNormalized(const QPointF& pN) const
{
    if (!hasValidXRange() || !hasValidYRange()) {
        return QPointF(m_xFrom, m_yFrom);
    }

    const qreal x = m_xFrom + clamp01(pN.x()) * (m_xTo - m_xFrom);

    qreal yT = clamp01(pN.y());
    if (m_yAxisInverse) {
        yT = 1.0 - yT;
    }
    const qreal y = yDomainFromNormalized(yT);

    return QPointF(x, y);
}

QVector<QPointF> PolylinePlot::normalizedFromDomain(const QVector<QPointF>& pts) const
{
    QVector<QPointF> out;
    out.reserve(pts.size());
    for (const auto& p : pts) {
        out.push_back(normalizedFromDomain(p));
    }

    return out;
}

QVector<QPointF> PolylinePlot::domainFromNormalized(const QVector<QPointF>& ptsN) const
{
    QVector<QPointF> out;
    out.reserve(ptsN.size());
    for (const auto& pN : ptsN) {
        out.push_back(domainFromNormalized(pN));
    }

    return out;
}

void PolylinePlot::rebuildVisiblePoints()
{
    m_pointsNVisible.clear();
    m_visibleToDomainIndex.clear();

    const double xRange = (m_xTo - m_xFrom);
    const double yRange = (m_yTo - m_yFrom);

    if (width() <= 0 || height() <= 0 || std::abs(xRange) <= 0 || std::abs(yRange) <= 0) {
        if (m_points.isEmpty()) {
            updateBaselineFromDefaultValue();
        }
        update();
        return;
    }

    if (m_points.isEmpty()) {
        m_linePointsN.clear();
        m_lineColorIndices.clear();
        updateBaselineFromDefaultValue();
        update();
        return;
    }

    struct P {
        QPointF p;
        int idx;
    };

    QVector<P> sortedPointsWithIndexes;
    sortedPointsWithIndexes.reserve(m_points.size());
    for (int i = 0; i < m_points.size(); ++i) {
        sortedPointsWithIndexes.push_back({ m_points[i], i });
    }
    // stable: the in/out copies of a jump share their x, and bends/colors rely on their order
    std::stable_sort(sortedPointsWithIndexes.begin(), sortedPointsWithIndexes.end(),
                     [](const P& a, const P& b) { return a.p.x() < b.p.x(); });

    auto normY = [&](double yAbs) {
        double yn = yNormalizedFromDomain(yAbs);
        if (m_yAxisInverse) {
            yn = 1.0 - yn;
        }
        return yn;
    };

    // The line actually drawn: every point (hidden ones included, they still shape it), bent segments sampled -
    // normalized, each with the color index of the stretch it starts (colors are per segment arriving at a point)
    m_linePointsN.clear();
    m_lineColorIndices.clear();
    const bool bendsValid = hasSegmentBends();
    for (int r = 0; r < sortedPointsWithIndexes.size(); ++r) {
        const P& curr = sortedPointsWithIndexes[r];
        m_linePointsN.push_back(QPointF((curr.p.x() - m_xFrom) / xRange, normY(curr.p.y())));
        m_lineColorIndices.push_back(r + 1);

        if (!bendsValid || r + 1 >= sortedPointsWithIndexes.size()) {
            continue;
        }

        const P& next = sortedPointsWithIndexes[r + 1];
        if (next.idx != curr.idx + 1 || m_segmentBends[curr.idx].isStraight()) {
            continue;
        }

        const SegmentBend& bend = m_segmentBends[curr.idx];
        for (int j = 1; j < BEND_SAMPLES_PER_SEGMENT; ++j) {
            const double s = static_cast<double>(j) / BEND_SAMPLES_PER_SEGMENT;
            const double x = lerp(curr.p.x(), next.p.x(), s);
            const double y = bentY(curr.p, next.p, bend, s);
            m_linePointsN.push_back(QPointF((x - m_xFrom) / xRange, normY(y)));
            m_lineColorIndices.push_back(r + 1);
        }
    }

    // Synthetic boundary points must be interpolated in rendered Y-space.
    // Interpolating in domain Y breaks continuity for split mappings
    // (e.g. segments crossing 0 dB split value).
    const double yAt0N = valueAtX(m_linePointsN, 0.0);
    const double yAt1N = valueAtX(m_linePointsN, 1.0);

    // NOTE: synthetic boundary points are added just outside the visible range
    // so the polyline draws correctly: they allow horizontal segments
    // before the first real point and after the last real point. Especially in
    // cases where polyline is currently visible only partially the screen.
    // These are rendering-only points (no corresponding domain index).

    // left boundary (synthetic)
    m_pointsNVisible.push_back(QPointF(-0.1, yAt0N));
    m_visibleToDomainIndex.push_back(INVALID_POINT_IDX);

    // interior real points
    for (const auto& it : sortedPointsWithIndexes) {
        const auto& p = it.p;
        if (isHiddenPoint(it.idx)) {
            continue;
        }
        // NOTE: build visible points with a margin so points directly at the edges
        // of container do not flash on re-paint
        if (p.x() < (m_xFrom - BOUNDARY_MARGIN) || p.x() > (m_xTo + BOUNDARY_MARGIN)) {
            continue;
        }
        const double xN = (p.x() - m_xFrom) / xRange;
        m_pointsNVisible.push_back(QPointF(std::clamp(xN, 0.0, 1.0), normY(p.y())));
        m_visibleToDomainIndex.push_back(it.idx);
    }

    // right boundary (synthetic)
    m_pointsNVisible.push_back(QPointF(1.1, yAt1N));
    m_visibleToDomainIndex.push_back(INVALID_POINT_IDX);

    updateActivePoint();
    update();
}

QVector<QPointF> PolylinePlot::linePx(QVector<int>* colorIndices) const
{
    QVector<QPointF> pts;

    if (width() <= 0 || height() <= 0) {
        return pts;
    }

    // 0 or 1 point -> horizontal baseline
    if (m_pointsNVisible.size() < 2) {
        qreal yN = m_baselineN;
        if (m_pointsNVisible.size() == 1) {
            yN = m_pointsNVisible[0].y();
        }
        yN = clamp01(yN);

        const qreal y = toPxY(this, yN);
        pts.push_back(QPointF(0.0, y));
        pts.push_back(QPointF(width(), y));

        if (colorIndices) {
            colorIndices->push_back(0);
        }

        return pts;
    }

    // The line through every point, bends included (see rebuildVisiblePoints()), cropped to the item, running
    // flat past the first/last point
    if (!m_linePointsN.isEmpty()) {
        const qreal yAt0 = valueAtX(m_linePointsN, 0.0);
        const qreal yAt1 = valueAtX(m_linePointsN, 1.0);

        int leftColorIndex = 0;
        for (int i = 0; i < m_linePointsN.size() && m_linePointsN[i].x() <= 0.0; ++i) {
            leftColorIndex = m_lineColorIndices[i];
        }

        pts.push_back(QPointF(0.0, toPxY(this, yAt0)));
        if (colorIndices) {
            colorIndices->push_back(leftColorIndex);
        }

        for (int i = 0; i < m_linePointsN.size(); ++i) {
            const QPointF& pN = m_linePointsN[i];
            if (pN.x() <= 0.0 || pN.x() >= 1.0) {
                continue;
            }
            pts.push_back(QPointF(toPxX(this, pN.x()), toPxY(this, pN.y())));
            if (colorIndices) {
                colorIndices->push_back(m_lineColorIndices[i]);
            }
        }

        pts.push_back(QPointF(width(), toPxY(this, yAt1)));
        return pts;
    }

    return pts;
}

QVector<QPointF> PolylinePlot::polylinePx() const
{
    return linePx(nullptr);
}

bool PolylinePlot::isNearLinePx(const QPointF& px) const
{
    const auto pts = polylinePx();
    if (pts.size() < 2) {
        return false;
    }

    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i < pts.size() - 1; ++i) {
        best = std::min(best, pointToSegmentDistance(px, pts[i], pts[i + 1]));
    }
    return best <= m_hitRadius;
}

void PolylinePlot::setLockedPoints(const QVector<bool>& locked)
{
    m_lockedPoints = locked;
}

void PolylinePlot::setHiddenPoints(const QVector<bool>& hidden)
{
    if (m_hiddenPoints == hidden) {
        return;
    }

    m_hiddenPoints = hidden;
    rebuildVisiblePoints();
}

void PolylinePlot::setGroupSelectedPoints(const QVector<bool>& selected)
{
    if (m_groupSelectedPoints == selected) {
        return;
    }

    m_groupSelectedPoints = selected;
    update();
}

bool PolylinePlot::isHiddenPoint(int index) const
{
    return m_hiddenPoints.size() == m_points.size() && index >= 0 && index < m_hiddenPoints.size() && m_hiddenPoints[index];
}

void PolylinePlot::setSegmentBends(const QVector<SegmentBend>& bends)
{
    if (m_segmentBends == bends) {
        return;
    }

    m_segmentBends = bends;
    rebuildVisiblePoints();
}

void PolylinePlot::setValueMapping(ValueMapping yToValue, ValueMapping valueToY)
{
    m_yToValue = std::move(yToValue);
    m_valueToY = std::move(valueToY);
    rebuildVisiblePoints();
}

qreal PolylinePlot::toValue(qreal y) const
{
    return m_yToValue ? m_yToValue(y) : y;
}

qreal PolylinePlot::fromValue(qreal value) const
{
    return m_valueToY ? m_valueToY(value) : value;
}

//! NOTE: the segment's y at s ([0; 1] across it), its bend applied on the values
qreal PolylinePlot::bentY(const QPointF& from, const QPointF& to, const SegmentBend& bend, qreal s) const
{
    return fromValue(bentValueAt(toValue(from.y()), toValue(to.y()), bend.t, bend.value, s));
}

bool PolylinePlot::hasSegmentBends() const
{
    return !m_points.isEmpty() && m_segmentBends.size() == m_points.size() - 1;
}

//! NOTE: the handle sits on the bend point itself, which the line passes through
std::optional<QPointF> PolylinePlot::bendHandleN(int segmentIndex) const
{
    if (!hasSegmentBends() || segmentIndex < 0 || segmentIndex >= m_segmentBends.size() || width() <= 0) {
        return std::nullopt;
    }

    const SegmentBend& bend = m_segmentBends[segmentIndex];
    const QPointF& from = m_points[segmentIndex];
    const QPointF& to = m_points[segmentIndex + 1];
    const qreal fromV = toValue(from.y());
    const qreal toV = toValue(to.y());
    if (!bend.editable || std::abs(toV - fromV) <= EPSILON) {
        return std::nullopt; // a flat segment can't bend: its bend is a fraction of its value range
    }

    // x unclamped (normalizedFromDomain() clamps it): the segment may cross the item's edges
    const qreal xRange = m_xTo - m_xFrom;
    if (!hasValidXRange() || (to.x() - from.x()) / xRange * width() < BEND_HANDLE_MIN_SEGMENT_WIDTH_PX) {
        return std::nullopt;
    }

    const qreal handleXN = (lerp(from.x(), to.x(), bend.t) - m_xFrom) / xRange;
    if (handleXN < 0.0 || handleXN > 1.0) {
        return std::nullopt;
    }

    const qreal handleY = fromValue(lerp(fromV, toV, bend.value));
    return QPointF(handleXN, normalizedFromDomain(QPointF(from.x(), handleY)).y());
}

//! NOTE: the segment (between points i and i + 1) spanning px's x
int PolylinePlot::segmentIndexAtPx(const QPointF& px) const
{
    if (m_points.size() < 2 || width() <= 0 || !hasValidXRange()) {
        return INVALID_POINT_IDX;
    }

    const qreal x = m_xFrom + (px.x() / width()) * (m_xTo - m_xFrom);
    for (int i = 0; i + 1 < m_points.size(); ++i) {
        if (x >= m_points[i].x() && x <= m_points[i + 1].x()) {
            return i;
        }
    }

    return INVALID_POINT_IDX;
}

int PolylinePlot::bendHandleIndexAtPx(const QPointF& px) const
{
    int bestIdx = INVALID_POINT_IDX;
    qreal bestDistance = m_hitRadius;
    for (int i = 0; i < m_segmentBends.size(); ++i) {
        const std::optional<QPointF> handleN = bendHandleN(i);
        if (!handleN) {
            continue;
        }

        const qreal distance = std::hypot(px.x() - toPxX(this, handleN->x()), px.y() - toPxY(this, handleN->y()));
        if (distance <= bestDistance) {
            bestDistance = distance;
            bestIdx = i;
        }
    }

    return bestIdx;
}

int PolylinePlot::pointIndexAtPx(const QPointF& px) const
{
    const bool hasLockedFlags = m_lockedPoints.size() == m_points.size();
    int firstLockedIdx = INVALID_POINT_IDX;

    // search in visible points, skip synthetic boundary points
    for (int i = 0; i < m_pointsNVisible.size(); ++i) {
        const int domainIdx = (i < m_visibleToDomainIndex.size()) ? m_visibleToDomainIndex[i] : INVALID_POINT_IDX;
        if (domainIdx < 0) {
            continue;
        }

        QPointF pN = m_pointsNVisible[i];
        const qreal x = toPxX(this, pN.x());
        const qreal y = toPxY(this, pN.y());
        const qreal dx = px.x() - x;
        const qreal dy = px.y() - y;
        if ((dx * dx + dy * dy) > (m_hitRadius * m_hitRadius)) {
            continue;
        }

        if (hasLockedFlags && m_lockedPoints[domainIdx]) {
            // Keep looking for an overlapping unlocked point
            if (firstLockedIdx == INVALID_POINT_IDX) {
                firstLockedIdx = domainIdx;
            }
            continue;
        }

        return domainIdx;
    }

    return firstLockedIdx;
}

GhostPoint PolylinePlot::ghostPointToPolylinePx(const QPointF& px) const
{
    GhostPoint best;

    const auto pts = polylinePx();
    if (pts.size() < 2) {
        best.point = px;
        best.distToSegment = std::numeric_limits<qreal>::max();
        return best;
    }

    for (int i = 0; i < pts.size() - 1; ++i) {
        const auto res = ghostPointToSegmentDistance(px, pts[i], pts[i + 1]);
        if (res.distToSegment < best.distToSegment) {
            best = res;
        }
    }

    return best;
}

QPointF PolylinePlot::snapToNeighbor(qreal dragPxX, QPointF pDomain) const
{
    if (!m_isSnapEnabled || !m_hasDraggedPointDomain || m_points.size() < 2) {
        return pDomain;
    }

    // Find the dragged point's actual current index in m_points (which may
    // have shifted after neighbor consumption) without modifying
    // m_pressedPointIndex — the model needs that to stay at the original
    // value so it can restore consumed points when dragging back.
    int currentIdx = -1;
    double bestDist = std::numeric_limits<double>::max();
    for (int i = 0; i < m_points.size(); ++i) {
        const double dx = std::abs(m_points[i].x() - m_draggedPointDomain.x());
        const double dy = std::abs(m_points[i].y() - m_draggedPointDomain.y());
        const double dist = dx + dy;
        if (dist < bestDist) {
            bestDist = dist;
            currentIdx = i;
        }
    }

    if (currentIdx < 0) {
        return pDomain;
    }

    // Snap close to — but not exactly onto — a neighbor.
    // Can't have multiple automation points at the exact same timepoint.
    static constexpr double SNAP_EPSILON = 1e-9;

    // Hidden neighbors (e.g. outside of the item) aren't snap targets: nothing to see there
    if (currentIdx + 1 < m_points.size() && !isHiddenPoint(currentIdx + 1)) {
        const qreal neighborPxX = toPxX(this, normalizedFromDomain(m_points[currentIdx + 1]).x());
        if (std::abs(dragPxX - neighborPxX) <= m_snapThresholdPx) {
            pDomain.setX(m_points[currentIdx + 1].x() - SNAP_EPSILON);
        }
    }

    if (currentIdx > 0 && !isHiddenPoint(currentIdx - 1)) {
        const qreal neighborPxX = toPxX(this, normalizedFromDomain(m_points[currentIdx - 1]).x());
        if (std::abs(dragPxX - neighborPxX) <= m_snapThresholdPx) {
            pDomain.setX(m_points[currentIdx - 1].x() + SNAP_EPSILON);
        }
    }

    return pDomain;
}

void PolylinePlot::updateCursor()
{
    const bool interactive = m_hoveredOnLine || m_pressed || (m_pressedPointIndex >= 0);

    if (interactive) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
}

void PolylinePlot::resetGestureState()
{
    m_pressed = false;
    m_pressedOnLine = false;
    m_pressedOnPoint = false;
    m_pressedPointIndex = INVALID_POINT_IDX;
    m_hasDraggedPointDomain = false;
    m_hoveredOnLine = false;
    m_draggedPointDomain = {};
    m_movedSincePress = false;
    m_pressedBendIndex = INVALID_POINT_IDX;
    m_pressPx = QPointF(0.0, 0.0);

    updateCursor();
    updateActivePoint();
    update();
}

void PolylinePlot::geometryChange(const QRectF& newG, const QRectF& oldG)
{
    QQuickPaintedItem::geometryChange(newG, oldG);
    if (newG.size() != oldG.size()) {
        rebuildVisiblePoints();
    }
}

void PolylinePlot::drawLinesAndFillUnder(QPainter* painter) const
{
    QVector<int> colorIndices;
    const auto pts = linePx(&colorIndices);
    if (pts.size() < 2) {
        return;
    }

    QPen pen(m_lineColor);
    pen.setWidthF(m_lineWidth);
    pen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal baselinePxY = toPxY(this, m_baselineN);

    for (int i = 0; i < pts.size() - 1; ++i) {
        const int colorIndex = i < colorIndices.size() ? colorIndices[i] : i;
        if (colorIndex < m_colorsUnderLine.size()) {
            // fill area under line
            const QVector<QPointF> areaUnderLine {
                QPointF(pts[i].x(), baselinePxY), // bottom left
                pts[i], // top left
                pts[i + 1], // top right
                QPointF(pts[i + 1].x(), baselinePxY), // bottom right
            };
            QPainterPath path;
            path.addPolygon(QPolygonF(areaUnderLine));
            painter->fillPath(path, m_colorsUnderLine.at(colorIndex));
        }
        // draw line
        painter->drawLine(pts[i], pts[i + 1]);
    }
}

void PolylinePlot::paintPoint(QPainter* painter, const PolylinePointStyle* style, const QPointF& centre, bool useHoveredStyle) const
{
    IF_ASSERT_FAILED(painter && style) {
        return;
    }

    const qreal centerRadius = useHoveredStyle ? style->centerRadiusHovered() : style->centerRadius();
    const QColor centerColor = useHoveredStyle ? style->centerColorHovered() : style->centerColor();

    const qreal middleRingWidth = useHoveredStyle ? style->middleRingWidthHovered() : style->middleRingWidth();
    const QColor middleRingColor = useHoveredStyle ? style->middleRingColorHovered() : style->middleRingColor();

    const qreal outlineWidth = useHoveredStyle ? style->outlineWidthHovered() : style->outlineWidth();
    const QColor outlineColor = useHoveredStyle ? style->outlineColorHovered() : style->outlineColor();

    if (middleRingWidth > 0.0) {
        const qreal middleRadius = centerRadius + middleRingWidth;
        const qreal outerRadius = middleRadius + outlineWidth;

        painter->setPen(Qt::NoPen);

        painter->setBrush(outlineColor);
        painter->drawEllipse(centre, outerRadius, outerRadius);

        painter->setBrush(middleRingColor);
        painter->drawEllipse(centre, middleRadius, middleRadius);

        painter->setBrush(centerColor);
        painter->drawEllipse(centre, centerRadius, centerRadius);
        return;
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(centerColor);
    painter->drawEllipse(centre, centerRadius, centerRadius);

    if (outlineWidth > 0.0) {
        QPen pen(outlineColor);
        pen.setWidthF(outlineWidth);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(centre, centerRadius, centerRadius);
    }
}

void PolylinePlot::paint(QPainter* painter)
{
    if (!painter) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, antialiasing());

    // draw background overlay
    if (m_drawBackground) {
        QColor overlay = uiConfiguration()->currentTheme().extra["black_color"].value<QColor>();
        overlay.setAlphaF(0.25);

        painter->setPen(Qt::NoPen);
        painter->setBrush(overlay);
        painter->drawRect(boundingRect());
    }

    // draw lines and fill area underneath
    drawLinesAndFillUnder(painter);
    paintBendHandles(painter);

    // draw points
    IF_ASSERT_FAILED(m_standardPointStyle && m_ghostPointStyle && m_selectedPointStyle) {
        return;
    }

    const int n = m_pointsNVisible.size();
    const int hoveredIndex = pointIndexAtPx(m_hoverPx);
    for (int i = 0; i < n; ++i) {
        QPointF pN = m_pointsNVisible[i];
        const QPointF centre(toPxX(this, pN.x()), toPxY(this, pN.y()));

        const int domainIdx = (i < m_visibleToDomainIndex.size()) ? m_visibleToDomainIndex[i] : INVALID_POINT_IDX;
        const bool isGroupSelected = m_groupSelectedPoints.size() == m_points.size() && domainIdx >= 0 && m_groupSelectedPoints[domainIdx];
        const bool isSelected = m_selectedPointsEnabled && domainIdx >= 0
                                && (isGroupSelected || muse::contains(m_selectedPointsIndices, domainIdx));
        const bool isHovered = !(m_pressed && isSelected) && domainIdx >= 0 && domainIdx == hoveredIndex;

        const PolylinePointStyle* style = isSelected ? m_selectedPointStyle : m_standardPointStyle;
        paintPoint(painter, style, centre, /*useHoveredStyle*/ isHovered);
    }

    // draw hover ghost point
    if (m_ghostPointsEnabled && m_hoveredOnLine && m_pressedPointIndex < 0) {
        QPointF hp = m_hoverGhostPx;

        if (m_pointsNVisible.size() < 2) {
            const qreal yN = (m_pointsNVisible.size() == 1) ? m_pointsNVisible[0].y() : m_baselineN;
            hp.setY(toPxY(this, yN));
        }

        if (pointIndexAtPx(hp) < 0 && isNearLinePx(hp)) {
            paintPoint(painter, m_ghostPointStyle, hp, /*useHoveredStyle*/ false);
        }
    }

    // Only the point actually being dragged gets a live value readout.
    if (m_pressed && m_hasActivePoint && !m_activePointLabel.isEmpty()) {
        paintValueLabel(painter);
    }
}

//! NOTE: a small diamond, so it can't be mistaken for a point. Only shown while its segment (or itself) is hovered,
//! while dragged, or once the segment is actually bent - so straight lines aren't cluttered with handles
void PolylinePlot::paintBendHandles(QPainter* painter) const
{
    for (int i = 0; i < m_segmentBends.size(); ++i) {
        const std::optional<QPointF> handleN = bendHandleN(i);
        if (!handleN) {
            continue;
        }

        const bool isActive = i == m_pressedBendIndex || (m_pressedBendIndex < 0 && i == m_hoveredBendIndex);
        const bool isSegmentHovered = m_pressedBendIndex < 0 && m_pressedPointIndex < 0 && i == m_hoveredSegmentIndex;
        if (!isActive && !isSegmentHovered && m_segmentBends[i].isStraight()) {
            continue;
        }

        const QPointF centre(toPxX(this, handleN->x()), toPxY(this, handleN->y()));
        const qreal half = isActive ? BEND_HANDLE_HALF_SIZE_PX + 1.0 : BEND_HANDLE_HALF_SIZE_PX;

        const QPolygonF diamond {
            QPointF(centre.x(), centre.y() - half),
            QPointF(centre.x() + half, centre.y()),
            QPointF(centre.x(), centre.y() + half),
            QPointF(centre.x() - half, centre.y()),
        };

        QPen pen(m_lineColor);
        pen.setWidthF(m_lineWidth);
        painter->setPen(pen);
        painter->setBrush(isActive ? m_lineColor : m_standardPointStyle->centerColor());
        painter->drawPolygon(diamond);
    }
}

void PolylinePlot::paintValueLabel(QPainter* painter) const
{
    QFont font = painter->font();
    font.setPixelSize(static_cast<int>(VALUE_LABEL_FONT_PX));
    painter->setFont(font);

    const QFontMetrics metrics(font);
    const QSize textSize = metrics.size(Qt::TextSingleLine, m_activePointLabel);

    const qreal chipWidth = textSize.width() + 2 * VALUE_LABEL_PADDING_X_PX;
    const qreal chipHeight = textSize.height() + 2 * VALUE_LABEL_PADDING_Y_PX;

    const qreal pointPx = m_activePointPx.x();
    const qreal topPx = m_activePointPx.y();

    // Prefer sitting to the right of the point; flip to the left if there isn't room, rather than
    // letting the chip run off the edge of the staff.
    const qreal offsetPx = VALUE_LABEL_GAP_PX + VALUE_LABEL_POINT_CLEARANCE_PX;
    qreal chipLeft = pointPx + offsetPx;
    if (chipLeft + chipWidth > width()) {
        chipLeft = pointPx - offsetPx - chipWidth;
    }
    chipLeft = std::clamp(chipLeft, 0.0, std::max(0.0, width() - chipWidth));

    const qreal chipTop = std::clamp(topPx - chipHeight / 2.0, 0.0, std::max(0.0, height() - chipHeight));

    const QRectF chipRect(chipLeft, chipTop, chipWidth, chipHeight);

    painter->setPen(Qt::NoPen);
    painter->setBrush(m_valueLabelBgColor);
    painter->drawRoundedRect(chipRect, VALUE_LABEL_CORNER_RADIUS_PX, VALUE_LABEL_CORNER_RADIUS_PX);

    painter->setPen(m_valueLabelTextColor);
    painter->drawText(chipRect, Qt::AlignCenter, m_activePointLabel);
}

void PolylinePlot::hoverMoveEvent(QHoverEvent* e)
{
    // NOTE: even if mouse is still, Qt produces hoverMoveEvents constantly
    if (m_hoverPx == e->position()) {
        // IMPORTANT: let through hover events (otherwise double-click
        // clip selection may be broken)
        e->ignore();
        return;
    }

    m_hoverPx = e->position();

    const bool nearPoint = (pointIndexAtPx(m_hoverPx) >= 0);
    m_hoveredBendIndex = nearPoint ? INVALID_POINT_IDX : bendHandleIndexAtPx(m_hoverPx);
    const bool nearBendHandle = m_hoveredBendIndex >= 0;

    bool nearLine = false;
    if (m_ghostPointsEnabled) {
        const auto proj = ghostPointToPolylinePx(m_hoverPx);
        nearLine = (proj.distToSegment <= m_hitRadius);

        if (m_pointsNVisible.size() >= 2) {
            m_hoverGhostPx = proj.point;
        } else {
            m_hoverGhostPx = m_hoverPx;
        }
    } else {
        // nearLine is still required for updateCursor (m_hoveredOnLine) and to accept events, but
        // if ghost points are disabled we can skip the m_hoverGhostPx logic...
        nearLine = isNearLinePx(m_hoverPx);
    }

    m_hoveredOnLine = (nearPoint || nearLine || nearBendHandle);
    m_hoveredSegmentIndex = (nearLine && !nearPoint) ? segmentIndexAtPx(m_hoverPx) : INVALID_POINT_IDX;
    updateCursor();

    updateActivePoint();
    update();

    // IMPORTANT: only consume hover when automation is actually interactive.
    // Otherwise let underlying ClipItem hover area keep containsMouse=true,
    // so itemHovered stays correct for selection/double-click logic.
    if (nearPoint || nearLine || nearBendHandle || m_pressed || m_pressedPointIndex >= 0) {
        e->accept();
    } else {
        e->ignore();
    }
}

void PolylinePlot::hoverLeaveEvent(QHoverEvent* e)
{
    Q_UNUSED(e);
    m_hoveredOnLine = false;
    m_hoveredBendIndex = INVALID_POINT_IDX;
    m_hoveredSegmentIndex = INVALID_POINT_IDX;
    updateCursor();
    updateActivePoint();
    update();
}

void PolylinePlot::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }

    if (!m_selectedPointsIndices.empty()) {
        m_selectedPointsIndices.clear();
    }

    const int pointIndex = pointIndexAtPx(e->position());
    const bool onPoint = pointIndex >= 0;
    const int bendIndex = onPoint ? INVALID_POINT_IDX : bendHandleIndexAtPx(e->position());
    const bool onBendHandle = bendIndex >= 0;
    const bool onLine  = isNearLinePx(e->position());

    // NOTE: allow clicks on the points, bend handles and lines only
    if (!onPoint && !onBendHandle && !onLine) {
        e->ignore();
        return;
    }

    resetGestureState();

    e->accept();
    updateCursor();

    m_pressed = true;
    m_pressPx = e->position();

    if (onBendHandle) {
        m_pressedBendIndex = bendIndex;
        m_pressedBendOriginalValue = m_segmentBends[bendIndex].value;
        updateActivePoint();
        update();
        return;
    }

    if (onPoint) {
        m_pressedOnPoint = true;
        m_pressedPointIndex = pointIndex;
        if (m_pressedPointIndex >= 0 && m_pressedPointIndex < m_points.size()) {
            if (m_selectedPointsEnabled) {
                m_selectedPointsIndices.insert(m_pressedPointIndex);
            }
            m_draggedPointDomain = m_points[m_pressedPointIndex];
            m_hasDraggedPointDomain = true;
        }
        updateActivePoint();
        return;
    }

    if (onLine) {
        m_pressedOnLine = true;
        m_pressedPointIndex = PENDING_POINT_IDX;
        m_pressedOnPoint = false;

        if (width() > 0 && height() > 0) {
            const auto ghostPoint = ghostPointToPolylinePx(e->position());

            QPointF pN(clamp01(ghostPoint.point.x() / width()),
                       1.0 - clamp01(ghostPoint.point.y() / height()));

            const QPointF pDomain = domainFromNormalized(pN);
            emit pointAdded(pDomain.x(), pDomain.y(), /*completed*/ false);
        }

        updateActivePoint();
        return;
    }
}

void PolylinePlot::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_pressed) {
        e->ignore();
        return;
    }

    e->accept();

    const QPointF pos = e->position();
    if (!m_movedSincePress && (pos - m_pressPx).manhattanLength() > MOVE_THRESHOLD) {
        m_movedSincePress = true;
    }

    // bend a segment: its handle only moves vertically, between the two points' values
    if (m_pressedBendIndex >= 0) {
        // Not a drag yet (a click may still be meant, see mouseReleaseEvent) - and the points may have changed under
        // the drag (e.g. undone by shortcut): only bend what's still there
        if (!m_movedSincePress || width() <= 0 || height() <= 0 || !hasSegmentBends()
            || m_pressedBendIndex >= m_segmentBends.size()) {
            return;
        }

        const QPointF pN = clamp01(QPointF(pos.x() / width(), 1.0 - (pos.y() / height())));
        const qreal yDomain = domainFromNormalized(pN).y();
        const QPointF& from = m_points[m_pressedBendIndex];
        const QPointF& to = m_points[m_pressedBendIndex + 1];
        const qreal fromV = toValue(from.y());
        const qreal rangeV = toValue(to.y()) - fromV;
        if (std::abs(rangeV) <= EPSILON) {
            return; // flat now: nothing to bend
        }
        const qreal value = std::clamp((toValue(yDomain) - fromV) / rangeV, 0.0, 1.0);

        m_segmentBends[m_pressedBendIndex].value = value;
        rebuildVisiblePoints();
        emit segmentBendMoved(m_pressedBendIndex, value, /*completed*/ false);
        updateActivePoint();
        return;
    }

    // drag point (2+ points only)
    if (m_pressedPointIndex >= 0) {
        if (width() <= 0 || height() <= 0) {
            return;
        }

        QPointF pN(pos.x() / width(), 1.0 - (pos.y() / height()));
        pN = clamp01(pN);

        const QPointF pDomain = snapToNeighbor(pos.x(), domainFromNormalized(pN));

        m_draggedPointDomain = pDomain;
        m_hasDraggedPointDomain = true;
        const int pointsCount = m_points.size();
        emit pointMoved(m_pressedPointIndex, pDomain.x(), pDomain.y(), /*completed*/ false);

        // The model may have moved the point elsewhere than the mouse (e.g. only vertically): follow where it actually
        // is, or the active point (value readout, selection) would be looked up at the mouse and land on a neighbor
        if (m_points.size() == pointsCount && m_pressedPointIndex < m_points.size()) {
            m_draggedPointDomain = m_points[m_pressedPointIndex];
        }
        updateActivePoint();

        return;
    }
}

void PolylinePlot::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_pressed) {
        e->ignore();
        return;
    }
    e->accept();

    const bool isClick = !m_movedSincePress;
    const QPointF rel = e->position();

    // commit a bend
    if (m_pressedBendIndex >= 0) {
        // as it was on press (a drag has already changed it)
        const bool wasHandleShown = m_pressedBendIndex < m_segmentBends.size()
                                    && !SegmentBend { m_segmentBends[m_pressedBendIndex].t, m_pressedBendOriginalValue }.isStraight();
        if (!isClick && m_pressedBendIndex < m_segmentBends.size()) {
            emit segmentBendMoved(m_pressedBendIndex, m_segmentBends[m_pressedBendIndex].value, /*completed*/ true);
        } else if (isClick && !wasHandleShown && width() > 0 && height() > 0) {
            // A plain click on a straight segment's (not drawn) handle is a click on the line: add a point there
            const GhostPoint ghostPoint = ghostPointToPolylinePx(m_pressPx);
            const QPointF pN(clamp01(ghostPoint.point.x() / width()), 1.0 - clamp01(ghostPoint.point.y() / height()));
            const QPointF pDomain = domainFromNormalized(pN);
            emit pointAdded(pDomain.x(), pDomain.y(), /*completed*/ false);
            emit pointAdded(pDomain.x(), pDomain.y(), /*completed*/ true);
        }
        emit interactionFinished();
        resetGestureState();
        return;
    }

    // commit point drag
    if (!isClick && m_pressedPointIndex >= 0) {
        QPointF pN(rel.x() / width(), 1.0 - (rel.y() / height()));
        pN = clamp01(pN);

        const QPointF pDomain = snapToNeighbor(rel.x(), domainFromNormalized(pN));

        m_draggedPointDomain = pDomain;
        m_hasDraggedPointDomain = true;
        emit pointMoved(m_pressedPointIndex, pDomain.x(), pDomain.y(), /*completed*/ true);
        emit interactionFinished();
        resetGestureState();
        return;
    }

    // click-without-drag on line: finalize the point that was added during press
    if (isClick && m_pressedOnLine && m_pressedPointIndex >= 0 && m_pressedPointIndex < m_points.size()) {
        const QPointF pDomain = m_points[m_pressedPointIndex];
        emit pointAdded(pDomain.x(), pDomain.y(), /*completed*/ true);
    }

    emit interactionFinished();
    resetGestureState();
}

void PolylinePlot::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }

    const int idx = pointIndexAtPx(e->position());
    if (idx < 0) {
        // a bend handle: straight again
        const int bendIndex = bendHandleIndexAtPx(e->position());
        if (bendIndex < 0) {
            e->ignore();
            return;
        }

        e->accept();
        m_segmentBends[bendIndex] = SegmentBend { 0.5, 0.5, m_segmentBends[bendIndex].editable };
        rebuildVisiblePoints();
        emit segmentBendMoved(bendIndex, 0.5, /*completed*/ true);
        emit interactionFinished();
        resetGestureState();
        return;
    }

    e->accept();

    emit pointRemoved(idx, /*completed*/ true);
    emit interactionFinished();

    resetGestureState();
}
