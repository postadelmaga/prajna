/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QRect>
#include <QSGDynamicTexture>

class QRhi;
class QRhiTexture;

/*
 * A scene-graph texture fed with partial updates. The engine reports which
 * rectangle of the page changed; only that region is uploaded to the GPU
 * (typing or a streaming answer touches a few percent of the frame), instead
 * of re-creating a full texture from a QImage on every frame.
 *
 * Requires the RHI scene graph and a BGRA8 texture format (the engine's
 * native layout); callers fall back to createTextureFromImage otherwise.
 */
class FrameTexture : public QSGDynamicTexture
{
    Q_OBJECT

public:
    explicit FrameTexture(QRhi *rhi);
    ~FrameTexture() override;

    static bool isSupported(QRhi *rhi);

    // Partial update of a frame of `size` (device pixels) whose `region`
    // changed. Returns the rectangle the caller must now write into
    // staging() as tightly packed BGRA rows: `region` united with whatever
    // still waits for upload, or the whole frame when the size changed (or
    // on the first update). Nothing is uploaded until commitStaging(): a
    // caller whose copy turned out torn simply calls beginUpdate() again.
    QRect beginUpdate(const QSize &size, const QRect &region);
    uchar *staging();
    void commitStaging();
    // Frees the staging buffer of an update that won't be uploaded (torn
    // copy, item hidden); what is dirty stays dirty for the next one.
    void dropStaging();

    qint64 comparisonKey() const override;
    QRhiTexture *rhiTexture() const override;
    QSize textureSize() const override;
    bool hasAlphaChannel() const override;
    bool hasMipmaps() const override;
    bool updateTexture() override;
    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates) override;

private:
    QRhi *m_rhi;
    QRhiTexture *m_texture = nullptr;
    QSize m_size;
    QRect m_dirty;        // pending upload, device pixels
    QByteArray m_pending; // m_dirty's pixels, tightly packed BGRA
    bool m_ready = false; // m_pending holds a complete copy of m_dirty
};
