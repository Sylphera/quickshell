#pragma once

#include <utility>

#include <qlist.h>
#include <qset.h>

namespace qs::windows {

template <typename Handle>
class OrderedWindowCandidates {
public:
	void insert(Handle handle) {
		if (this->seen.contains(handle)) return;
		this->seen.insert(handle);
		this->ordered.append(handle);
	}

	[[nodiscard]] QList<Handle> takeAll() {
		auto candidates = std::move(this->ordered);
		this->ordered.clear();
		this->seen.clear();
		return candidates;
	}

private:
	QList<Handle> ordered;
	QSet<Handle> seen;
};

}
