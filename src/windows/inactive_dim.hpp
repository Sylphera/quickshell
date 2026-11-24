#pragma once

#include <map>
#include <memory>

#include <qt_windows.h>

#include <dcomp.h>
#include <dwmapi.h>
#include <qlist.h>
#include <qobject.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

struct DimLevel {
	float from = 0;
	float to = 0;
	qint64 start = 0;

	[[nodiscard]] float now() const;
	[[nodiscard]] DimLevel toward(float goal) const;
	[[nodiscard]] bool operator==(const DimLevel& other) const = default;
};

class InactiveDim: public QObject {
	Q_OBJECT;

public:
	static InactiveDim* instance();
	Q_DISABLE_COPY_MOVE(InactiveDim);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

	[[nodiscard]] static DimLevel levelOf(HWND hwnd);
	static bool applyLevel(
	    IDCompositionDevice* device,
	    IDCompositionEffectGroup* effect,
	    const DimLevel& level
	);
	[[nodiscard]] static HTHUMBNAIL shadeThumbnail(HWND destination, float level);
	void stepAside();
	void comeBack();

signals:
	void enabledChanged();

private:
	struct Scene;

	explicit InactiveDim(QObject* parent);
	~InactiveDim() override;

	void watch();
	void schedule(int delay);
	void refresh();
	void hide();
	void updateLevels(const QList<HWND>& shown);

	std::unique_ptr<Scene> scene;
	std::map<HWND, DimLevel> levels;
	QTimer timer;
	HWND focused = nullptr;
	bool mEnabled = false;
	bool watching = false;
};

}
