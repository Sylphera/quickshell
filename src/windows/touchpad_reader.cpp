#include "touchpad_reader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtypes.h>

#include <hidsdi.h>

#include "touchpad_swipe.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logTouchpad, "quickshell.windows.touchpad", QtWarningMsg);

constexpr auto* MESSAGE_WINDOW_CLASS = L"QuickshellTouchpad";
constexpr double SCAN_TIME_UNITS_PER_MS = 10;

constexpr USAGE PAGE_GENERIC = 0x01;
constexpr USAGE PAGE_DIGITIZER = 0x0D;
constexpr USAGE USAGE_TOUCHPAD = 0x05;
constexpr USAGE USAGE_X = 0x30;
constexpr USAGE USAGE_Y = 0x31;
constexpr USAGE USAGE_TIP = 0x42;
constexpr USAGE USAGE_CONFIDENCE = 0x47;
constexpr USAGE USAGE_CONTACT_ID = 0x51;
constexpr USAGE USAGE_CONTACT_COUNT = 0x54;
constexpr USAGE USAGE_SCAN_TIME = 0x56;

struct Axis {
	double origin = 0;
	double mm = 0;
};

Axis axisFrom(const HIDP_VALUE_CAPS& caps) {
	auto logical = static_cast<double>(caps.LogicalMax) - caps.LogicalMin;
	auto physical = static_cast<double>(caps.PhysicalMax) - caps.PhysicalMin;
	auto system = caps.Units & 0xF;
	auto unit = system == 1 ? 10.0 : (system == 3 ? 25.4 : 0.0);
	auto exponent = static_cast<int>(caps.UnitsExp & 0xF);
	if (exponent > 7) exponent -= 16;
	if (logical <= 0 || physical <= 0 || unit == 0) return {};
	return {
	    .origin = static_cast<double>(caps.LogicalMin),
	    .mm = physical * std::pow(10.0, exponent) * unit / logical,
	};
}

struct Slot {
	USHORT collection = 0;
	Axis x;
	Axis y;
	bool hasId = false;
};

}

struct TouchpadReader::Device {
	std::vector<std::byte> preparsed;
	ULONG reportLength = 0;
	std::vector<Slot> contactSlots;
	USHORT countCollection = 0;
	bool hasCount = false;
	USHORT scanCollection = 0;
	bool hasScan = false;
	bool hasConfidence = false;

	ULONG expected = 0;
	ULONG received = 0;
	QList<TouchpadContact> contacts;
	quint16 lastScan = 0;
	double timeMs = 0;

	[[nodiscard]] PHIDP_PREPARSED_DATA data() {
		return reinterpret_cast<PHIDP_PREPARSED_DATA>(this->preparsed.data());
	}

	Slot& slot(USHORT collection) {
		auto found = std::ranges::find(this->contactSlots, collection, &Slot::collection);
		if (found != this->contactSlots.end()) return *found;
		this->contactSlots.push_back({.collection = collection});
		return this->contactSlots.back();
	}

	void addValue(const HIDP_VALUE_CAPS& caps, USAGE usage) {
		auto collection = caps.LinkCollection;
		if (caps.UsagePage == PAGE_GENERIC) {
			if (usage == USAGE_X) this->slot(collection).x = axisFrom(caps);
			if (usage == USAGE_Y) this->slot(collection).y = axisFrom(caps);
		} else if (caps.UsagePage == PAGE_DIGITIZER) {
			if (usage == USAGE_CONTACT_ID) this->slot(collection).hasId = true;
			if (usage == USAGE_CONTACT_COUNT) {
				this->countCollection = collection;
				this->hasCount = true;
			}
			if (usage == USAGE_SCAN_TIME) {
				this->scanCollection = collection;
				this->hasScan = true;
			}
		}
	}

	bool value(USAGE page, USHORT collection, USAGE usage, ULONG& out, char* report) {
		auto status = HidP_GetUsageValue(
		    HidP_Input,
		    page,
		    collection,
		    usage,
		    &out,
		    this->data(),
		    report,
		    this->reportLength
		);
		return status == HIDP_STATUS_SUCCESS;
	}

	bool touching(USHORT collection, char* report) {
		auto usages = std::array<USAGE, 16> {};
		auto count = static_cast<ULONG>(usages.size());
		auto status = HidP_GetUsages(
		    HidP_Input,
		    PAGE_DIGITIZER,
		    collection,
		    usages.data(),
		    &count,
		    this->data(),
		    report,
		    this->reportLength
		);
		if (status != HIDP_STATUS_SUCCESS) return false;
		auto end = usages.begin() + count;
		auto has = [&](USAGE usage) { return std::find(usages.begin(), end, usage) != end; };
		return has(USAGE_TIP) && (!this->hasConfidence || has(USAGE_CONFIDENCE));
	}
};

TouchpadReader::TouchpadReader(QObject* parent): QObject(parent) { this->clock.start(); }

TouchpadReader::~TouchpadReader() { this->stop(); }

bool TouchpadReader::start() {
	if (this->messageWindow != nullptr) return true;

	WNDCLASSW wndClass {};
	wndClass.lpfnWndProc = &TouchpadReader::messageWindowProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = MESSAGE_WINDOW_CLASS;
	RegisterClassW(&wndClass);

	this->messageWindow = CreateWindowExW(
	    0,
	    MESSAGE_WINDOW_CLASS,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    wndClass.hInstance,
	    nullptr
	);
	if (this->messageWindow == nullptr) {
		qCWarning(logTouchpad) << "Failed to create the touchpad message window.";
		return false;
	}
	SetWindowLongPtrW(this->messageWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

	auto raw = RAWINPUTDEVICE {
	    .usUsagePage = PAGE_DIGITIZER,
	    .usUsage = USAGE_TOUCHPAD,
	    .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
	    .hwndTarget = this->messageWindow,
	};
	if (RegisterRawInputDevices(&raw, 1, sizeof(raw)) == FALSE) {
		qCWarning(logTouchpad) << "Failed to register for touchpad input:" << GetLastError();
		DestroyWindow(std::exchange(this->messageWindow, nullptr));
		return false;
	}
	return true;
}

void TouchpadReader::stop() {
	if (this->messageWindow == nullptr) return;
	auto raw = RAWINPUTDEVICE {
	    .usUsagePage = PAGE_DIGITIZER,
	    .usUsage = USAGE_TOUCHPAD,
	    .dwFlags = RIDEV_REMOVE,
	    .hwndTarget = nullptr,
	};
	RegisterRawInputDevices(&raw, 1, sizeof(raw));
	SetWindowLongPtrW(this->messageWindow, GWLP_USERDATA, 0);
	DestroyWindow(std::exchange(this->messageWindow, nullptr));
	this->devices.clear();
}

LRESULT CALLBACK
TouchpadReader::messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* reader = reinterpret_cast<TouchpadReader*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (reader != nullptr && msg == WM_INPUT) {
		reader->onInput(reinterpret_cast<HRAWINPUT>(lParam));
	} else if (reader != nullptr && msg == WM_INPUT_DEVICE_CHANGE && wParam == GIDC_REMOVAL) {
		reader->devices.erase(reinterpret_cast<HANDLE>(lParam));
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::unique_ptr<TouchpadReader::Device> TouchpadReader::openDevice(HANDLE handle) {
	UINT size = 0;
	GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &size);
	if (size == 0) return nullptr;

	auto device = std::make_unique<Device>();
	device->preparsed.resize(size);
	auto read = GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, device->preparsed.data(), &size);
	if (read == static_cast<UINT>(-1)) return nullptr;

	HIDP_CAPS caps {};
	if (HidP_GetCaps(device->data(), &caps) != HIDP_STATUS_SUCCESS) return nullptr;
	device->reportLength = caps.InputReportByteLength;

	auto count = caps.NumberInputValueCaps;
	auto values = std::vector<HIDP_VALUE_CAPS>(count);
	if (count == 0) return nullptr;
	auto status = HidP_GetValueCaps(HidP_Input, values.data(), &count, device->data());
	if (status != HIDP_STATUS_SUCCESS) return nullptr;
	values.resize(count);
	for (const auto& value: values) {
		ULONG first = value.IsRange != FALSE ? value.Range.UsageMin : value.NotRange.Usage;
		ULONG last = value.IsRange != FALSE ? value.Range.UsageMax : value.NotRange.Usage;
		for (auto usage = first; usage <= last; usage++) {
			device->addValue(value, static_cast<USAGE>(usage));
		}
	}
	std::erase_if(device->contactSlots, [](const Slot& slot) {
		return !slot.hasId || slot.x.mm == 0 || slot.y.mm == 0;
	});

	count = caps.NumberInputButtonCaps;
	auto buttons = std::vector<HIDP_BUTTON_CAPS>(count);
	if (count > 0) {
		status = HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, device->data());
		buttons.resize(status == HIDP_STATUS_SUCCESS ? count : 0);
	}
	device->hasConfidence = std::ranges::any_of(buttons, [](const HIDP_BUTTON_CAPS& button) {
		auto first = button.IsRange != FALSE ? button.Range.UsageMin : button.NotRange.Usage;
		auto last = button.IsRange != FALSE ? button.Range.UsageMax : button.NotRange.Usage;
		return button.UsagePage == PAGE_DIGITIZER && first <= USAGE_CONFIDENCE
		    && last >= USAGE_CONFIDENCE;
	});

	if (device->contactSlots.empty() || !device->hasCount) {
		qCWarning(logTouchpad) << "Ignoring a touchpad whose reports have no usable contacts.";
		return nullptr;
	}
	qCDebug(logTouchpad) << "Reading a touchpad with" << device->contactSlots.size()
	                     << "contacts per report";
	return device;
}

TouchpadReader::Device* TouchpadReader::device(HANDLE handle) {
	auto found = this->devices.find(handle);
	if (found == this->devices.end()) {
		found = this->devices.emplace(handle, TouchpadReader::openDevice(handle)).first;
	}
	return found->second.get();
}

void TouchpadReader::onInput(HRAWINPUT input) {
	UINT size = 0;
	GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
	if (size == 0) return;
	auto buffer = std::vector<std::byte>(size);
	auto read = GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
	if (read != buffer.size()) return;

	auto* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
	if (raw->header.dwType != RIM_TYPEHID) return;
	auto* device = this->device(raw->header.hDevice);
	if (device == nullptr || raw->data.hid.dwSizeHid != device->reportLength) return;

	auto* reports = reinterpret_cast<char*>(raw->data.hid.bRawData);
	for (DWORD i = 0; i < raw->data.hid.dwCount; i++) {
		this->onReport(*device, reports + static_cast<size_t>(i) * device->reportLength);
	}
}

void TouchpadReader::onReport(Device& device, char* report) {
	ULONG count = 0;
	if (!device.value(PAGE_DIGITIZER, device.countCollection, USAGE_CONTACT_COUNT, count, report)) {
		return;
	}
	if (count > 0) {
		device.expected = count;
		device.received = 0;
		device.contacts.clear();
	} else if (device.received >= device.expected) {
		return;
	}

	for (const auto& slot: device.contactSlots) {
		if (device.received >= device.expected) break;
		device.received++;

		ULONG id = 0;
		ULONG x = 0;
		ULONG y = 0;
		if (!device.touching(slot.collection, report)
		    || !device.value(PAGE_DIGITIZER, slot.collection, USAGE_CONTACT_ID, id, report)
		    || !device.value(PAGE_GENERIC, slot.collection, USAGE_X, x, report)
		    || !device.value(PAGE_GENERIC, slot.collection, USAGE_Y, y, report))
		{
			continue;
		}
		device.contacts.append({
		    .id = id,
		    .x = (x - slot.x.origin) * slot.x.mm,
		    .y = (y - slot.y.origin) * slot.y.mm,
		});
	}
	if (device.received < device.expected) return;

	ULONG scan = 0;
	if (device.hasScan
	    && device.value(PAGE_DIGITIZER, device.scanCollection, USAGE_SCAN_TIME, scan, report))
	{
		auto now = static_cast<quint16>(scan);
		device.timeMs += static_cast<quint16>(now - device.lastScan) / SCAN_TIME_UNITS_PER_MS;
		device.lastScan = now;
	} else {
		device.timeMs = static_cast<double>(this->clock.elapsed());
	}
	emit this->frame(device.contacts, device.timeMs);
}

}
