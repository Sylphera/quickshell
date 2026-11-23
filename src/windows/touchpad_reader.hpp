#pragma once

#include <memory>
#include <unordered_map>

#include <qelapsedtimer.h>
#include <qlist.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include "touchpad_swipe.hpp"

namespace qs::windows {

class TouchpadReader: public QObject {
	Q_OBJECT;

public:
	explicit TouchpadReader(QObject* parent = nullptr);
	~TouchpadReader() override;
	Q_DISABLE_COPY_MOVE(TouchpadReader);

	bool start();
	void stop();

signals:
	void frame(const QList<qs::windows::TouchpadContact>& contacts, double timeMs);

private:
	struct Device;

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static std::unique_ptr<Device> openDevice(HANDLE handle);
	Device* device(HANDLE handle);
	void onInput(HRAWINPUT input);
	void onReport(Device& device, char* report);

	HWND messageWindow = nullptr;
	std::unordered_map<HANDLE, std::unique_ptr<Device>> devices;
	QElapsedTimer clock;
};

}
