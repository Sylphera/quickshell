#pragma once

#include <utility>

#include <qlist.h>
#include <qset.h>

namespace qs::windows {

// WinEvent callbacks arrive in an ordered queue. Deduplicate repeated events
// without losing the first-observed order when a batch is registered with the
// tiling manager (whose split tree depends on insertion order).
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

} // namespace qs::windows
