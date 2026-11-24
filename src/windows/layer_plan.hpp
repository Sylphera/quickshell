#pragma once

#include <qlist.h>
#include <qtypes.h>

namespace qs::windows {

struct LayerWindow {
	quintptr id = 0;
	bool topmost = false;
};

struct LayerMove {
	quintptr id = 0;
	quintptr after = 0;

	[[nodiscard]] bool operator==(const LayerMove& other) const = default;
};

[[nodiscard]] QList<LayerMove>
planLayers(const QList<LayerWindow>& current, quintptr anchor, const QList<quintptr>& chain);

}
