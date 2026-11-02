/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QImage>
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

    // Queue `region` of `image` (device pixels) for upload. Only that
    // region's bytes are copied; the image is not retained.
    void setImage(const QImage &image, const QRect &region);

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
};
