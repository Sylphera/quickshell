#include "cursor_image.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

namespace qs::windows {

namespace {

struct Dib {
	Dib(int width, int height) {
		BITMAPINFO info {};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		this->dc = CreateCompatibleDC(nullptr);
		void* pixels = nullptr;
		this->bitmap = CreateDIBSection(this->dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		if (this->bitmap) {
			this->old = SelectObject(this->dc, this->bitmap);
			this->bits = static_cast<quint32*>(pixels);
		}
	}

	~Dib() {
		if (this->old) SelectObject(this->dc, this->old);
		if (this->bitmap) DeleteObject(this->bitmap);
		if (this->dc) DeleteDC(this->dc);
	}

	Dib(const Dib&) = delete;
	Dib& operator=(const Dib&) = delete;

	HDC dc = nullptr;
	HBITMAP bitmap = nullptr;
	HGDIOBJ old = nullptr;
	quint32* bits = nullptr;
};

int channel(quint32 pixel, int shift) { return static_cast<int>((pixel >> shift) & 0xff); }

}

HCURSOR currentCursorHandle() {
	static std::atomic<HCURSOR> last = LoadCursorW(nullptr, IDC_ARROW);
	CURSORINFO info {};
	info.cbSize = sizeof(info);
	if (GetCursorInfo(&info) && info.hCursor != nullptr) last = info.hCursor;
	return last;
}

CursorImage cursorImage(HCURSOR cursor) {
	CursorImage result;
	result.handle = cursor;
	if (!cursor) return result;

	ICONINFO info {};
	if (!GetIconInfo(cursor, &info)) return result;
	BITMAP bitmap {};
	const auto colored = info.hbmColor != nullptr;
	GetObjectW(colored ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap);
	if (info.hbmColor) DeleteObject(info.hbmColor);
	if (info.hbmMask) DeleteObject(info.hbmMask);

	const int width = bitmap.bmWidth;
	const int height = colored ? bitmap.bmHeight : bitmap.bmHeight / 2;
	if (width <= 0 || height <= 0) return result;

	Dib black(width, height);
	Dib white(width, height);
	if (!black.bits || !white.bits) return result;
	const auto count = static_cast<std::size_t>(width) * height;
	std::fill_n(black.bits, count, 0xff000000u);
	std::fill_n(white.bits, count, 0xffffffffu);
	DrawIconEx(black.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
	DrawIconEx(white.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
	GdiFlush();

	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) return result;
	auto* pixels = reinterpret_cast<quint32*>(image.bits());
	std::vector<bool> inverted(count, false);

	for (std::size_t i = 0; i < count; ++i) {
		const auto onBlack = black.bits[i];
		const auto onWhite = white.bits[i];
		auto blackSum = 0;
		auto whiteSum = 0;
		auto spread = 0;
		for (const auto shift: {0, 8, 16}) {
			blackSum += channel(onBlack, shift);
			whiteSum += channel(onWhite, shift);
			spread = std::max(spread, channel(onWhite, shift) - channel(onBlack, shift));
		}

		if (whiteSum + 96 < blackSum) {
			inverted[i] = true;
			pixels[i] = 0xffffffffu;
			continue;
		}

		const auto alpha = std::clamp(255 - spread, 0, 255);
		const auto red = std::min(channel(onBlack, 16), alpha);
		const auto green = std::min(channel(onBlack, 8), alpha);
		const auto blue = std::min(channel(onBlack, 0), alpha);
		pixels[i] = (static_cast<quint32>(alpha) << 24) | (static_cast<quint32>(red) << 16)
		          | (static_cast<quint32>(green) << 8) | static_cast<quint32>(blue);
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const auto i = static_cast<std::size_t>(y) * width + x;
			if (inverted[i] || (pixels[i] >> 24) != 0) continue;
			auto edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy) {
				for (int dx = -1; dx <= 1 && !edge; ++dx) {
					const auto nx = x + dx;
					const auto ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
					edge = inverted[static_cast<std::size_t>(ny) * width + nx];
				}
			}
			if (edge) pixels[i] = 0xff000000u;
		}
	}

	result.image = image;
	result.hotspot = QPoint(static_cast<int>(info.xHotspot), static_cast<int>(info.yHotspot));
	return result;
}

}
