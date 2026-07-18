#include "x11capture.h"

#include <QScopeGuard>
#include <QString>

#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <xcb/composite.h>
#include <xcb/randr.h>
#include <xcb/xfixes.h>
}

namespace {
struct FreeDeleter {
    void operator()(void* ptr) const
    {
        free(ptr);
    }
};

template <typename T>
using XcbReply = std::unique_ptr<T, FreeDeleter>;

constexpr xcb_atom_t noAtom = XCB_ATOM_NONE;

bool hasConnectionError(xcb_connection_t* connection)
{
    return !connection || xcb_connection_has_error(connection) != 0;
}

const xcb_query_extension_reply_t* extensionData(xcb_connection_t* connection, xcb_extension_t* extension)
{
    if (hasConnectionError(connection)) {
        return nullptr;
    }
    const auto* data = xcb_get_extension_data(connection, extension);
    return data && data->present ? data : nullptr;
}

int maskShift(quint32 mask)
{
    if (mask == 0) {
        return 0;
    }

    int shift = 0;
    while ((mask & 1U) == 0) {
        mask >>= 1U;
        ++shift;
    }
    return shift;
}

int maskBits(quint32 mask)
{
    int bits = 0;
    while (mask != 0) {
        bits += int(mask & 1U);
        mask >>= 1U;
    }
    return bits;
}

quint8 scaleMaskedChannel(quint32 pixel, quint32 mask, int shift, int bits)
{
    if (mask == 0 || bits <= 0) {
        return 0;
    }

    const quint32 value = (pixel & mask) >> shift;
    const quint32 maxValue = (1U << bits) - 1U;
    return static_cast<quint8>((value * 255U + maxValue / 2U) / maxValue);
}

quint32 readPixel(const uchar* src, int bitsPerPixel, int byteOrder)
{
    switch (bitsPerPixel) {
    case 32:
        if (byteOrder == XCB_IMAGE_ORDER_MSB_FIRST) {
            return (quint32(src[0]) << 24U) | (quint32(src[1]) << 16U) | (quint32(src[2]) << 8U) | quint32(src[3]);
        }
        return quint32(src[0]) | (quint32(src[1]) << 8U) | (quint32(src[2]) << 16U) | (quint32(src[3]) << 24U);
    case 24:
        if (byteOrder == XCB_IMAGE_ORDER_MSB_FIRST) {
            return (quint32(src[0]) << 16U) | (quint32(src[1]) << 8U) | quint32(src[2]);
        }
        return quint32(src[0]) | (quint32(src[1]) << 8U) | (quint32(src[2]) << 16U);
    case 16:
        if (byteOrder == XCB_IMAGE_ORDER_MSB_FIRST) {
            return (quint32(src[0]) << 8U) | quint32(src[1]);
        }
        return quint32(src[0]) | (quint32(src[1]) << 8U);
    default:
        return 0;
    }
}

QRect geometryRect(const xcb_get_geometry_reply_t* geometry)
{
    if (!geometry || geometry->width == 0 || geometry->height == 0) {
        return {};
    }
    return QRect(geometry->x, geometry->y, geometry->width, geometry->height);
}

xcb_atom_t internAtom(xcb_connection_t* connection, const char* name)
{
    if (hasConnectionError(connection)) {
        return noAtom;
    }

    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, 1, std::strlen(name), name);
    XcbReply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, cookie, nullptr));
    return reply ? reply->atom : noAtom;
}

xcb_window_t readWindowProperty(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property)
{
    if (hasConnectionError(connection) || property == noAtom) {
        return XCB_NONE;
    }

    xcb_get_property_cookie_t cookie = xcb_get_property(connection, 0, window, property, XCB_ATOM_WINDOW, 0, 1);
    XcbReply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection, cookie, nullptr));
    if (!reply || reply->format != 32 || xcb_get_property_value_length(reply.get()) < int(sizeof(xcb_window_t))) {
        return XCB_NONE;
    }

    return *reinterpret_cast<xcb_window_t*>(xcb_get_property_value(reply.get()));
}

bool visualMasks(xcb_connection_t* connection, xcb_visualid_t visualId, quint32* redMask, quint32* greenMask, quint32* blueMask)
{
    if (hasConnectionError(connection) || !redMask || !greenMask || !blueMask) {
        return false;
    }

    const xcb_setup_t* setup = xcb_get_setup(connection);
    for (xcb_screen_iterator_t screenIt = xcb_setup_roots_iterator(setup); screenIt.rem; xcb_screen_next(&screenIt)) {
        for (xcb_depth_iterator_t depthIt = xcb_screen_allowed_depths_iterator(screenIt.data); depthIt.rem; xcb_depth_next(&depthIt)) {
            for (xcb_visualtype_iterator_t visualIt = xcb_depth_visuals_iterator(depthIt.data); visualIt.rem; xcb_visualtype_next(&visualIt)) {
                if (visualIt.data->visual_id == visualId) {
                    *redMask = visualIt.data->red_mask;
                    *greenMask = visualIt.data->green_mask;
                    *blueMask = visualIt.data->blue_mask;
                    return true;
                }
            }
        }
    }

    return false;
}

bool pixmapFormatForDepth(xcb_connection_t* connection, quint8 depth, int* bitsPerPixel, int* scanlinePad)
{
    if (hasConnectionError(connection) || !bitsPerPixel || !scanlinePad) {
        return false;
    }

    const xcb_setup_t* setup = xcb_get_setup(connection);
    for (xcb_format_iterator_t it = xcb_setup_pixmap_formats_iterator(setup); it.rem; xcb_format_next(&it)) {
        if (it.data->depth == depth) {
            *bitsPerPixel = it.data->bits_per_pixel;
            *scanlinePad = it.data->scanline_pad;
            return true;
        }
    }

    return false;
}

QImage convertImageReply(xcb_connection_t* connection, const xcb_get_image_reply_t* reply, xcb_visualid_t visualId, const QSize& size)
{
    if (!reply || size.isEmpty() || reply->depth < 15) {
        return {};
    }

    quint32 redMask = 0;
    quint32 greenMask = 0;
    quint32 blueMask = 0;
    if (!visualMasks(connection, visualId, &redMask, &greenMask, &blueMask)) {
        return {};
    }

    int bitsPerPixel = 0;
    int scanlinePad = 0;
    if (!pixmapFormatForDepth(connection, reply->depth, &bitsPerPixel, &scanlinePad)) {
        bitsPerPixel = reply->depth <= 16 ? 16 : 32;
        scanlinePad = 32;
    }
    if (bitsPerPixel != 16 && bitsPerPixel != 24 && bitsPerPixel != 32) {
        return {};
    }

    const qsizetype expectedStride = ((qsizetype(size.width()) * bitsPerPixel + scanlinePad - 1) / scanlinePad) * (scanlinePad / 8);
    const qsizetype dataLength = xcb_get_image_data_length(reply);
    if (expectedStride <= 0 || dataLength < expectedStride * size.height()) {
        return {};
    }

    const QByteArray data(reinterpret_cast<const char*>(xcb_get_image_data(reply)), dataLength);
    return X11Capture::convertToRgb32(data,
        size,
        int(expectedStride),
        bitsPerPixel,
        xcb_get_setup(connection)->image_byte_order,
        redMask,
        greenMask,
        blueMask);
}

QImage cursorImageFromReply(const xcb_xfixes_get_cursor_image_reply_t* reply)
{
    if (!reply || reply->width == 0 || reply->height == 0) {
        return {};
    }

    const int width = reply->width;
    const int height = reply->height;
    const quint32* src = xcb_xfixes_get_cursor_image_cursor_image(reply);
    if (!src) {
        return {};
    }

    QImage cursor(width, height, QImage::Format_ARGB32_Premultiplied);
    if (cursor.isNull()) {
        return {};
    }

    for (int y = 0; y < height; ++y) {
        auto* dst = reinterpret_cast<QRgb*>(cursor.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const quint32 argb = src[y * width + x];
            const int alpha = int((argb >> 24U) & 0xffU);
            const int red = int((argb >> 16U) & 0xffU);
            const int green = int((argb >> 8U) & 0xffU);
            const int blue = int(argb & 0xffU);
            dst[x] = qRgba((red * alpha + 127) / 255, (green * alpha + 127) / 255, (blue * alpha + 127) / 255, alpha);
        }
    }

    return cursor;
}
}

class X11CapturePrivate {
public:
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    bool ownsConnection = false;
    bool compositeAvailable = false;
    bool randrAvailable = false;
    bool xfixesAvailable = false;
    xcb_atom_t activeWindowAtom = noAtom;

    void resetConnection(xcb_connection_t* newConnection, xcb_screen_t* newScreen, bool takeOwnership)
    {
        cleanup();
        connection = newConnection;
        screen = newScreen;
        ownsConnection = takeOwnership;
        refreshExtensions();
    }

    void cleanup()
    {
        if (connection) {
            xcb_flush(connection);
            if (ownsConnection) {
                xcb_disconnect(connection);
            }
        }
        connection = nullptr;
        screen = nullptr;
        ownsConnection = false;
        compositeAvailable = false;
        randrAvailable = false;
        xfixesAvailable = false;
        activeWindowAtom = noAtom;
    }

    bool valid() const
    {
        return screen && !hasConnectionError(connection);
    }

    QRect rootRect() const
    {
        if (!screen) {
            return {};
        }
        return QRect(0, 0, screen->width_in_pixels, screen->height_in_pixels);
    }

    void refreshExtensions()
    {
        compositeAvailable = extensionData(connection, &xcb_composite_id) != nullptr;
        randrAvailable = extensionData(connection, &xcb_randr_id) != nullptr;
        xfixesAvailable = extensionData(connection, &xcb_xfixes_id) != nullptr;
        activeWindowAtom = internAtom(connection, "_NET_ACTIVE_WINDOW");
    }

    QImage captureDrawable(xcb_drawable_t drawable, xcb_visualid_t visual, const QRect& sourceRect, const QRect& bounds, bool embedCursor)
    {
        if (!valid() || drawable == XCB_NONE) {
            return {};
        }

        const QRect captureRect = X11Capture::boundedCaptureRect(sourceRect, bounds);
        if (captureRect.isEmpty()) {
            return {};
        }

        xcb_generic_error_t* error = nullptr;
        xcb_get_image_cookie_t cookie = xcb_get_image(connection,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            drawable,
            captureRect.x(),
            captureRect.y(),
            captureRect.width(),
            captureRect.height(),
            std::numeric_limits<quint32>::max());
        XcbReply<xcb_get_image_reply_t> reply(xcb_get_image_reply(connection, cookie, &error));
        if (error) {
            free(error);
            return {};
        }

        QImage image = convertImageReply(connection, reply.get(), visual, captureRect.size());
        if (image.isNull()) {
            return {};
        }

        if (embedCursor) {
            embedCursorImage(&image, captureRect);
        }
        return image;
    }

    QImage captureWindow(xcb_window_t window, bool embedCursor)
    {
        if (!valid() || window == XCB_NONE) {
            return {};
        }

        xcb_generic_error_t* geometryError = nullptr;
        XcbReply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), &geometryError));
        if (geometryError) {
            free(geometryError);
            return {};
        }
        const QRect bounds = geometryRect(geometry.get());
        if (bounds.isEmpty()) {
            return {};
        }

        xcb_generic_error_t* attributesError = nullptr;
        XcbReply<xcb_get_window_attributes_reply_t> attributes(
            xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), &attributesError));
        if (attributesError) {
            free(attributesError);
            return {};
        }
        if (!attributes) {
            return {};
        }

        xcb_pixmap_t pixmap = XCB_NONE;
        xcb_drawable_t drawable = window;
        if (compositeAvailable) {
            pixmap = xcb_generate_id(connection);
            xcb_generic_error_t* pixmapError = xcb_request_check(connection, xcb_composite_name_window_pixmap_checked(connection, window, pixmap));
            if (pixmapError) {
                free(pixmapError);
                pixmap = XCB_NONE;
            } else {
                drawable = pixmap;
            }
        }

        const auto pixmapCleanup = qScopeGuard([this, pixmap]() {
            if (pixmap != XCB_NONE && connection) {
                xcb_generic_error_t* error = xcb_request_check(connection, xcb_free_pixmap_checked(connection, pixmap));
                free(error);
            }
        });

        QImage image = captureDrawable(drawable, attributes->visual, QRect(QPoint(0, 0), bounds.size()), QRect(QPoint(0, 0), bounds.size()), false);
        if (!image.isNull() && embedCursor) {
            QPoint windowRootPosition(bounds.topLeft());
            xcb_translate_coordinates_cookie_t translateCookie = xcb_translate_coordinates(connection, window, screen->root, 0, 0);
            XcbReply<xcb_translate_coordinates_reply_t> translate(xcb_translate_coordinates_reply(connection, translateCookie, nullptr));
            if (translate) {
                windowRootPosition = QPoint(translate->dst_x, translate->dst_y);
            }
            embedCursorImage(&image, QRect(windowRootPosition, image.size()));
        }
        return image;
    }

    void embedCursorImage(QImage* image, const QRect& captureRootRect)
    {
        if (!image || image->isNull() || !xfixesAvailable || hasConnectionError(connection)) {
            return;
        }

        xcb_xfixes_get_cursor_image_cookie_t cookie = xcb_xfixes_get_cursor_image(connection);
        XcbReply<xcb_xfixes_get_cursor_image_reply_t> reply(xcb_xfixes_get_cursor_image_reply(connection, cookie, nullptr));
        if (!reply) {
            return;
        }

        const QImage cursor = cursorImageFromReply(reply.get());
        X11Capture::compositeCursor(image,
            cursor,
            QPoint(reply->xhot, reply->yhot),
            QPoint(reply->x, reply->y),
            captureRootRect);
    }

    QRect outputGeometry(const QString& outputUniqueId)
    {
        if (!valid() || !randrAvailable || outputUniqueId.isEmpty()) {
            return {};
        }

        XcbReply<xcb_randr_get_screen_resources_reply_t> resources(
            xcb_randr_get_screen_resources_reply(connection, xcb_randr_get_screen_resources(connection, screen->root), nullptr));
        if (!resources) {
            return {};
        }

        const int outputCount = xcb_randr_get_screen_resources_outputs_length(resources.get());
        xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_outputs(resources.get());
        for (int i = 0; i < outputCount; ++i) {
            XcbReply<xcb_randr_get_output_info_reply_t> info(
                xcb_randr_get_output_info_reply(connection, xcb_randr_get_output_info(connection, outputs[i], XCB_CURRENT_TIME), nullptr));
            if (!info || info->connection != XCB_RANDR_CONNECTION_CONNECTED || info->crtc == XCB_NONE) {
                continue;
            }

            const QString name = QString::fromUtf8(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(info.get())),
                xcb_randr_get_output_info_name_length(info.get()));
            if (name != outputUniqueId && QString::number(outputs[i]) != outputUniqueId) {
                continue;
            }

            XcbReply<xcb_randr_get_crtc_info_reply_t> crtc(
                xcb_randr_get_crtc_info_reply(connection, xcb_randr_get_crtc_info(connection, info->crtc, XCB_CURRENT_TIME), nullptr));
            if (!crtc || crtc->width == 0 || crtc->height == 0) {
                return {};
            }
            return QRect(crtc->x, crtc->y, crtc->width, crtc->height);
        }

        return {};
    }

    xcb_window_t activeWindow() const
    {
        if (!valid() || activeWindowAtom == noAtom) {
            return XCB_NONE;
        }
        return readWindowProperty(connection, screen->root, activeWindowAtom);
    }
};

X11Capture::X11Capture(QObject* parent)
    : QObject(parent)
    , d(new X11CapturePrivate)
{
    int screenNumber = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screenNumber);
    if (hasConnectionError(connection)) {
        if (connection) {
            xcb_disconnect(connection);
        }
        return;
    }

    xcb_screen_t* screen = nullptr;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; it.rem && i < screenNumber; ++i) {
        xcb_screen_next(&it);
    }
    if (it.rem) {
        screen = it.data;
    }
    d->resetConnection(connection, screen, true);
}

X11Capture::X11Capture(xcb_connection_t* connection, xcb_screen_t* screen, bool takeOwnership, QObject* parent)
    : QObject(parent)
    , d(new X11CapturePrivate)
{
    d->resetConnection(connection, screen, takeOwnership);
}

X11Capture::~X11Capture()
{
    d->cleanup();
    delete d;
}

bool X11Capture::isValid() const
{
    return d->valid();
}

void X11Capture::setConnection(xcb_connection_t* connection, xcb_screen_t* screen, bool takeOwnership)
{
    d->resetConnection(connection, screen, takeOwnership);
}

QImage X11Capture::captureWorkspace(bool includeCursor)
{
    if (!d->valid()) {
        return {};
    }
    const QRect rootRect = d->rootRect();
    return d->captureDrawable(d->screen->root, d->screen->root_visual, rootRect, rootRect, includeCursor);
}

QImage X11Capture::captureWindow(quint64 xid, bool includeCursor)
{
    if (xid == 0 || xid > std::numeric_limits<xcb_window_t>::max()) {
        return {};
    }
    return d->captureWindow(static_cast<xcb_window_t>(xid), includeCursor);
}

QImage X11Capture::captureOutput(const QString& outputUniqueId, bool includeCursor)
{
    if (!d->valid()) {
        return {};
    }

    const QRect outputRect = d->outputGeometry(outputUniqueId);
    if (outputRect.isEmpty()) {
        return {};
    }

    return d->captureDrawable(d->screen->root, d->screen->root_visual, outputRect, d->rootRect(), includeCursor);
}

QImage X11Capture::captureActiveWindow(bool includeCursor)
{
    return d->captureWindow(d->activeWindow(), includeCursor);
}

QRect X11Capture::boundedCaptureRect(const QRect& requested, const QRect& bounds)
{
    if (requested.isEmpty() || bounds.isEmpty()) {
        return {};
    }
    return requested.intersected(bounds);
}

QImage X11Capture::convertToRgb32(const QByteArray& data,
    const QSize& size,
    int bytesPerLine,
    int bitsPerPixel,
    int byteOrder,
    quint32 redMask,
    quint32 greenMask,
    quint32 blueMask)
{
    if (size.isEmpty() || bytesPerLine <= 0 || (bitsPerPixel != 16 && bitsPerPixel != 24 && bitsPerPixel != 32)) {
        return {};
    }

    const int bytesPerPixel = bitsPerPixel / 8;
    const qsizetype minimumStride = qsizetype(size.width()) * bytesPerPixel;
    if (bytesPerLine < minimumStride || data.size() < qsizetype(bytesPerLine) * size.height()) {
        return {};
    }

    const int redShift = maskShift(redMask);
    const int greenShift = maskShift(greenMask);
    const int blueShift = maskShift(blueMask);
    const int redBits = maskBits(redMask);
    const int greenBits = maskBits(greenMask);
    const int blueBits = maskBits(blueMask);
    if (redBits == 0 || greenBits == 0 || blueBits == 0) {
        return {};
    }

    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    for (int y = 0; y < size.height(); ++y) {
        const uchar* src = bytes + qsizetype(y) * bytesPerLine;
        auto* dst = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const quint32 pixel = readPixel(src + x * bytesPerPixel, bitsPerPixel, byteOrder);
            dst[x] = qRgb(scaleMaskedChannel(pixel, redMask, redShift, redBits),
                scaleMaskedChannel(pixel, greenMask, greenShift, greenBits),
                scaleMaskedChannel(pixel, blueMask, blueShift, blueBits));
        }
    }

    image.detach();
    return image;
}

void X11Capture::compositeCursor(QImage* target,
    const QImage& cursor,
    const QPoint& hotSpot,
    const QPoint& cursorRootPosition,
    const QRect& captureRootRect)
{
    if (!target || target->isNull() || cursor.isNull() || captureRootRect.isEmpty()) {
        return;
    }

    QImage source = cursor.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage destination = target->format() == QImage::Format_RGB32 ? *target : target->convertToFormat(QImage::Format_RGB32);
    const QPoint topLeft = cursorRootPosition - hotSpot - captureRootRect.topLeft();
    const QRect cursorRect(topLeft, source.size());
    const QRect targetBounds(QPoint(0, 0), destination.size());
    const QRect drawRect = cursorRect.intersected(targetBounds);
    if (drawRect.isEmpty()) {
        if (destination.cacheKey() != target->cacheKey()) {
            *target = destination;
        }
        return;
    }

    for (int y = drawRect.top(); y <= drawRect.bottom(); ++y) {
        auto* dst = reinterpret_cast<QRgb*>(destination.scanLine(y));
        const auto* src = reinterpret_cast<const QRgb*>(source.constScanLine(y - cursorRect.top()));
        for (int x = drawRect.left(); x <= drawRect.right(); ++x) {
            const QRgb s = src[x - cursorRect.left()];
            const int alpha = qAlpha(s);
            if (alpha == 0) {
                continue;
            }
            if (alpha == 255) {
                dst[x] = qRgb(qRed(s), qGreen(s), qBlue(s));
                continue;
            }
            const QRgb dpx = dst[x];
            dst[x] = qRgb(qRed(s) + (qRed(dpx) * (255 - alpha) + 127) / 255,
                qGreen(s) + (qGreen(dpx) * (255 - alpha) + 127) / 255,
                qBlue(s) + (qBlue(dpx) * (255 - alpha) + 127) / 255);
        }
    }

    destination.detach();
    *target = destination;
}
