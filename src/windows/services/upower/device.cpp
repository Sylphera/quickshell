#include "device.hpp"

#include <cmath>
#include <vector>

#include <qt_windows.h>

#include <qobject.h>
#include <qstring.h>
#include <qtypes.h>

#include <winioctl.h>

#include <batclass.h>
#include <setupapi.h>

namespace qs::service::upower {

QString UPowerDeviceState::toString(UPowerDeviceState::Enum status) {
	switch (status) {
	case UPowerDeviceState::Unknown: return "Unknown";
	case UPowerDeviceState::Charging: return "Charging";
	case UPowerDeviceState::Discharging: return "Discharging";
	case UPowerDeviceState::Empty: return "Empty";
	case UPowerDeviceState::FullyCharged: return "Fully Charged";
	case UPowerDeviceState::PendingCharge: return "Pending Charge";
	case UPowerDeviceState::PendingDischarge: return "Pending Discharge";
	default: return "Invalid Status";
	}
}

QString UPowerDeviceType::toString(UPowerDeviceType::Enum type) {
	switch (type) {
	case UPowerDeviceType::Unknown: return "Unknown";
	case UPowerDeviceType::LinePower: return "Line Power";
	case UPowerDeviceType::Battery: return "Battery";
	case UPowerDeviceType::Ups: return "Ups";
	case UPowerDeviceType::Monitor: return "Monitor";
	case UPowerDeviceType::Mouse: return "Mouse";
	case UPowerDeviceType::Keyboard: return "Keyboard";
	case UPowerDeviceType::Pda: return "Pda";
	case UPowerDeviceType::Phone: return "Phone";
	case UPowerDeviceType::MediaPlayer: return "Media Player";
	case UPowerDeviceType::Tablet: return "Tablet";
	case UPowerDeviceType::Computer: return "Computer";
	case UPowerDeviceType::GamingInput: return "Gaming Input";
	case UPowerDeviceType::Pen: return "Pen";
	case UPowerDeviceType::Touchpad: return "Touchpad";
	case UPowerDeviceType::Modem: return "Modem";
	case UPowerDeviceType::Network: return "Network";
	case UPowerDeviceType::Headset: return "Headset";
	case UPowerDeviceType::Speakers: return "Speakers";
	case UPowerDeviceType::Headphones: return "Headphones";
	case UPowerDeviceType::Video: return "Video";
	case UPowerDeviceType::OtherAudio: return "Other Audio";
	case UPowerDeviceType::RemoteControl: return "Remote Control";
	case UPowerDeviceType::Printer: return "Printer";
	case UPowerDeviceType::Scanner: return "Scanner";
	case UPowerDeviceType::Camera: return "Camera";
	case UPowerDeviceType::Wearable: return "Wearable";
	case UPowerDeviceType::Toy: return "Toy";
	case UPowerDeviceType::BluetoothGeneric: return "Bluetooth Generic";
	default: return "Invalid Type";
	}
}

namespace {

struct BatteryReading {
	bool ok = false;
	qreal energyWh = 0;
	qreal fullWh = 0;
	qreal designWh = 0;
	qreal rateW = 0;
};

void readBattery(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA& data, BatteryReading& reading) {
	DWORD size = 0;
	SetupDiGetDeviceInterfaceDetailW(devices, &data, nullptr, 0, &size, nullptr);
	if (size == 0) return;
	std::vector<BYTE> buffer(size);
	auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
	detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
	if (!SetupDiGetDeviceInterfaceDetailW(devices, &data, detail, size, &size, nullptr)) return;

	auto* battery = CreateFileW(
	    detail->DevicePath,
	    GENERIC_READ | GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    nullptr,
	    OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL,
	    nullptr
	);
	if (battery == INVALID_HANDLE_VALUE) return;

	DWORD wait = 0;
	DWORD written = 0;
	BATTERY_QUERY_INFORMATION query {};
	if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &query.BatteryTag, sizeof(query.BatteryTag), &written, nullptr)
	    && query.BatteryTag != 0)
	{
		BATTERY_INFORMATION info {};
		query.InformationLevel = BatteryInformation;
		if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &info, sizeof(info), &written, nullptr)
		    && (info.Capabilities & BATTERY_SYSTEM_BATTERY) != 0 && (info.Capabilities & BATTERY_CAPACITY_RELATIVE) == 0)
		{
			BATTERY_WAIT_STATUS waitStatus {};
			waitStatus.BatteryTag = query.BatteryTag;
			BATTERY_STATUS status {};
			if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_STATUS, &waitStatus, sizeof(waitStatus), &status, sizeof(status), &written, nullptr)) {
				reading.ok = true;
				reading.designWh += info.DesignedCapacity / 1000.0;
				reading.fullWh += info.FullChargedCapacity / 1000.0;
				if (status.Capacity != BATTERY_UNKNOWN_CAPACITY) reading.energyWh += status.Capacity / 1000.0;
				if (static_cast<ULONG>(status.Rate) != BATTERY_UNKNOWN_RATE) reading.rateW += status.Rate / 1000.0;
			}
		}
	}
	CloseHandle(battery);
}

BatteryReading readBatteries() {
	static const GUID batteryClass = {0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};
	auto reading = BatteryReading();
	auto* devices = SetupDiGetClassDevsW(&batteryClass, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devices == INVALID_HANDLE_VALUE) return reading;
	for (DWORD index = 0; index < 8; index++) {
		SP_DEVICE_INTERFACE_DATA data {};
		data.cbSize = sizeof(data);
		if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &batteryClass, index, &data)) break;
		readBattery(devices, data, reading);
	}
	SetupDiDestroyDeviceInfoList(devices);
	return reading;
}

} // namespace

UPowerDevice::UPowerDevice(QObject* parent): QObject(parent) {
	this->bIsLaptopBattery.setBinding([this]() {
		return this->bType == UPowerDeviceType::Battery && this->bPowerSupply.value();
	});

	this->bHealthSupported = false;
	this->bHealthPercentage = 100.0;
	this->bReady = true;
}

void UPowerDevice::updateFromSystemPowerStatus() {
	SYSTEM_POWER_STATUS status {};
	if (!GetSystemPowerStatus(&status)) {
		this->bIsPresent = false;
		this->bType = UPowerDeviceType::Unknown;
		return;
	}

	auto present = status.BatteryFlag != 128 && status.BatteryFlag != 255;

	this->bIsPresent = present;
	this->bPowerSupply = present;
	this->bType = present ? UPowerDeviceType::Battery : UPowerDeviceType::Unknown;
	this->bNativePath = present ? QStringLiteral("WindowsBattery") : QString();
	this->bModel = present ? QStringLiteral("System Battery") : QString();

	auto percent =
	    status.BatteryLifePercent == 255 ? 0.0 : qreal(status.BatteryLifePercent) / 100.0;
	this->bPercentage = present ? percent : 0.0;

	auto charging = (status.BatteryFlag & 8) != 0;
	auto onAc = status.ACLineStatus == 1;

	UPowerDeviceState::Enum state = UPowerDeviceState::Unknown;
	if (present) {
		if (charging) {
			state = UPowerDeviceState::Charging;
		} else if (onAc) {
			state = percent >= 0.99 ? UPowerDeviceState::FullyCharged : UPowerDeviceState::Charging;
		} else {
			state = UPowerDeviceState::Discharging;
		}
	}
	this->bState = state;

	auto lifeTimeUnknown = status.BatteryLifeTime == static_cast<DWORD>(-1);
	this->bTimeToEmpty =
	    (present && !onAc && !lifeTimeUnknown) ? qreal(status.BatteryLifeTime) : 0.0;
	this->bTimeToFull = 0.0;

	auto battery = present ? readBatteries() : BatteryReading();
	if (battery.ok) {
		this->bEnergy = battery.energyWh;
		this->bEnergyCapacity = battery.fullWh;
		this->bChangeRate = std::abs(battery.rateW);
		this->bHealthSupported = battery.designWh > 0;
		this->bHealthPercentage = battery.designWh > 0 ? battery.fullWh / battery.designWh * 100.0 : 100.0;
		if (state == UPowerDeviceState::Charging && battery.rateW > 0.1 && battery.fullWh > battery.energyWh) {
			this->bTimeToFull = (battery.fullWh - battery.energyWh) / battery.rateW * 3600.0;
		}
		if (state == UPowerDeviceState::Discharging && battery.rateW < -0.1 && this->bTimeToEmpty.value() <= 0) {
			this->bTimeToEmpty = battery.energyWh / -battery.rateW * 3600.0;
		}
	} else {
		this->bChangeRate = 0.0;
		this->bHealthSupported = false;
	}

	this->bIconName = present
	    ? (charging ? QStringLiteral("battery-good-charging-symbolic")
	                : QStringLiteral("battery-good-symbolic"))
	    : QString();
}

} // namespace qs::service::upower
