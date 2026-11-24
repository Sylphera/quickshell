#pragma once

#include <memory>

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

class InactiveDim: public QObject {
	Q_OBJECT;

public:
	static InactiveDim* instance();
	Q_DISABLE_COPY_MOVE(InactiveDim);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

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
	[[nodiscard]] static bool animating();

	std::unique_ptr<Scene> scene;
	QTimer timer;
	HWND focused = nullptr;
	bool mEnabled = false;
	bool watching = false;
};

}
