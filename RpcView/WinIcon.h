#pragma once

#include <windows.h>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

inline QPixmap RpcViewPixmapFromHICON(HICON icon)
{
	if (icon == NULL)
		return QPixmap();

	ICONINFO info;
	ZeroMemory(&info, sizeof(info));
	if (!GetIconInfo(icon, &info))
		return QPixmap();

	BITMAP bmp;
	ZeroMemory(&bmp, sizeof(bmp));
	HBITMAP source = info.hbmColor != NULL ? info.hbmColor : info.hbmMask;
	if (!GetObject(source, sizeof(bmp), &bmp))
	{
		if (info.hbmMask != NULL) DeleteObject(info.hbmMask);
		if (info.hbmColor != NULL) DeleteObject(info.hbmColor);
		return QPixmap();
	}

	const int width = bmp.bmWidth;
	const int height = info.hbmColor != NULL ? bmp.bmHeight : bmp.bmHeight / 2;
	HDC screen = GetDC(NULL);
	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = width;
	bi.bmiHeader.biHeight = -height;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void* bits = NULL;
	HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	HDC mem = CreateCompatibleDC(screen);
	HGDIOBJ old = SelectObject(mem, dib);
	DrawIconEx(mem, 0, 0, icon, width, height, 0, NULL, DI_NORMAL);

	QImage image(width, height, QImage::Format_ARGB32);
	if (bits != NULL && !image.isNull())
		memcpy(image.bits(), bits, (size_t)width * (size_t)height * 4);

	SelectObject(mem, old);
	DeleteDC(mem);
	DeleteObject(dib);
	ReleaseDC(NULL, screen);
	if (info.hbmMask != NULL) DeleteObject(info.hbmMask);
	if (info.hbmColor != NULL) DeleteObject(info.hbmColor);
	return QPixmap::fromImage(image);
}
