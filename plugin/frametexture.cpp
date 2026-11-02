/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "frametexture.h"

#include <rhi/qrhi.h>

FrameTexture::FrameTexture(QRhi *rhi)
    : m_rhi(rhi)
{
}

FrameTexture::~FrameTexture()
{
    delete m_texture;
}

bool FrameTexture::isSupported(QRhi *rhi)
{
    return rhi && rhi->isTextureFormatSupported(QRhiTexture::BGRA8);
}

void FrameTexture::setImage(const QImage &image, const QRect &region)
{
    if (image.isNull())
        return;
    if (image.size() != m_size) {
        m_size = image.size();
        m_dirty = image.rect(); // new size: the texture is recreated whole
    } else {
        m_dirty |= region.intersected(image.rect());
    }
    if (m_dirty.isEmpty())
        return;
    // Several syncs may precede one render: re-copy the whole union from the
    // newest image, which is correct for every part of it.
    const QRect r = m_dirty;
    const qsizetype rowBytes = qsizetype(r.width()) * 4;
    m_pending.resize(rowBytes * r.height());
    for (int y = 0; y < r.height(); ++y)
        memcpy(m_pending.data() + rowBytes * y, image.constScanLine(r.y() + y) + qsizetype(r.x()) * 4, rowBytes);
}

qint64 FrameTexture::comparisonKey() const
{
    return m_texture ? qint64(quintptr(m_texture)) : qint64(quintptr(this));
}

QRhiTexture *FrameTexture::rhiTexture() const
{
    return m_texture;
}

QSize FrameTexture::textureSize() const
{
    return m_size;
}

bool FrameTexture::hasAlphaChannel() const
{
    return true;
}

bool FrameTexture::hasMipmaps() const
{
    return false;
}

bool FrameTexture::updateTexture()
{
    return false; // uploads happen in commitTextureOperations
}

void FrameTexture::commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates)
{
    if (m_size.isEmpty() || rhi != m_rhi)
        return;
    if (!m_texture || m_texture->pixelSize() != m_size) {
        if (m_dirty != QRect(QPoint(0, 0), m_size))
            return; // wait for a full frame before (re)creating
        delete m_texture;
        m_texture = rhi->newTexture(QRhiTexture::BGRA8, m_size);
        if (!m_texture->create()) {
            delete m_texture;
            m_texture = nullptr;
            return;
        }
    }
    if (m_dirty.isEmpty())
        return;
    QRhiTextureSubresourceUploadDescription sub(std::exchange(m_pending, {}));
    sub.setSourceSize(m_dirty.size());
    sub.setDestinationTopLeft(m_dirty.topLeft());
    resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription({0, 0, sub}));
    m_dirty = QRect();
}
