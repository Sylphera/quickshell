#include <algorithm>
#include <random>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qrect.h>
#include <qset.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qtypes.h>

#include "tiling_layout.hpp"
#include "tiling_target.hpp"

using qs::windows::tiling::cursorInsertionTarget;
using qs::windows::tiling::DwindleLayout;
using qs::windows::tiling::Edge;
using qs::windows::tiling::LayoutEngine;
using qs::windows::tiling::Options;

namespace {

using Id = LayoutEngine::Id;

constexpr int SEQUENCES = 3000;
constexpr int OPERATIONS = 80;
constexpr int MAX_WINDOWS = 14;

class Random {
public:
	explicit Random(quint32 seed): engine(seed) {}
	int upTo(int limit) { return std::uniform_int_distribution<int>(0, limit - 1)(this->engine); }
	bool chance(int percent) { return this->upTo(100) < percent; }
	double real(double low, double high) {
		return std::uniform_real_distribution<double>(low, high)(this->engine);
	}
	Edge edge() {
		constexpr Edge EDGES[] = {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom};
		return EDGES[this->upTo(4)];
	}

private:
	std::mt19937 engine;
};

qint64 areaOf(const QRect& rect) { return qint64(rect.width()) * rect.height(); }

QString problem(const DwindleLayout& layout, const QList<Id>& model, const QSet<Id>& hidden, const QRect& area) {
	auto ids = layout.ids();
	if (QSet<Id>(ids.begin(), ids.end()) != QSet<Id>(model.begin(), model.end())) return "ids differ";
	if (ids.size() != model.size()) return "an id is repeated";
	if (layout.isEmpty() != model.isEmpty()) return "emptiness differs";

	qint64 covered = 0;
	QList<QRect> boxes;
	for (auto id: ids) {
		auto box = layout.box(id);
		if (layout.isHidden(id) != hidden.contains(id)) return "hidden differs";
		if (hidden.contains(id)) {
			if (!box.isEmpty()) return "a hidden window has a box";
			continue;
		}
		if (box.width() < 0 || box.height() < 0) return "a box is inside out";
		if (box.isEmpty()) continue;
		if (!area.contains(box)) return "a box leaves the area";
		for (const auto& other: boxes) {
			if (other.intersects(box)) return "two boxes overlap";
		}
		boxes.append(box);
		covered += areaOf(box);
	}
	if (!boxes.isEmpty() && covered != areaOf(area)) return "the area is not filled";
	return {};
}

}

class TestDwindleFuzz: public QObject {
	Q_OBJECT;

private slots:
	void theTreeAlwaysFillsItsArea() {
		for (auto seed = 1; seed <= SEQUENCES; seed++) {
			Random random(seed);
			DwindleLayout layout;
			auto options = Options();
			options.preserveSplit = random.chance(70);
			options.smartSplit = random.chance(20);
			options.forceSplit = random.upTo(3);
			options.defaultSplitRatio = random.real(0.1, 1.9);
			layout.configure(options);

			auto area = QRect(random.upTo(3000) - 1500, random.upTo(400), 1920, 1017);
			layout.compute(area);
			QList<Id> model;
			QSet<Id> hidden;
			Id next = 1;

			for (auto step = 0; step < OPERATIONS; step++) {
				auto pick = [&] { return model.isEmpty() ? Id(0) : model[random.upTo(static_cast<int>(model.size()))]; };
				auto point = QPoint(
				    area.left() + random.upTo(area.width()),
				    area.top() + random.upTo(area.height())
				);

				switch (random.upTo(12)) {
				case 0:
				case 1:
				case 2: {
					if (model.size() >= MAX_WINDOWS) break;
					auto byCursor = random.chance(50);
					auto target = byCursor ? cursorInsertionTarget(layout, point) : pick();
					if (target != 0 && layout.isHidden(target)) target = 0;
					layout.insert(next, target, byCursor ? &point : nullptr);
					model.append(next++);
					break;
				}
				case 3: {
					auto id = pick();
					layout.remove(id);
					model.removeOne(id);
					hidden.remove(id);
					break;
				}
				case 4: layout.swap(pick(), pick()); break;
				case 5: {
					auto id = pick();
					if (id == 0) break;
					layout.replace(id, next);
					model.replace(model.indexOf(id), next++);
					hidden.remove(id);
					break;
				}
				case 6: layout.toggleSplit(pick()); break;
				case 7: layout.swapSplit(pick()); break;
				case 8: layout.splitRatio(pick(), random.real(-3, 3), random.chance(50)); break;
				case 9: layout.moveEdge(pick(), random.edge(), random.upTo(5000) - 2500); break;
				case 10: layout.resize(pick(), random.upTo(800) - 400, random.upTo(800) - 400); break;
				case 11: {
					auto id = pick();
					if (id == 0) break;
					auto hide = random.chance(50);
					layout.setHidden(id, hide);
					if (hide) hidden.insert(id);
					else hidden.remove(id);
					break;
				}
				}

				layout.compute(area);
				auto wrong = problem(layout, model, hidden, area);
				if (!wrong.isEmpty()) {
					QFAIL(qPrintable(QString("seed %1 step %2 with %3 windows: %4")
					                     .arg(seed)
					                     .arg(step)
					                     .arg(model.size())
					                     .arg(wrong)));
				}
			}

			while (!model.isEmpty()) layout.remove(model.takeFirst());
			QVERIFY(layout.isEmpty());
		}
	}

	void movingAWindowAwayAndBackKeepsEveryWindow() {
		for (auto seed = 1; seed <= 500; seed++) {
			Random random(seed);
			DwindleLayout a;
			DwindleLayout b;
			auto area = QRect(0, 0, 1920, 1017);
			a.compute(area);
			b.compute(area);
			for (Id id = 1; id <= 6; id++) a.insert(id, id - 1, nullptr);

			for (auto step = 0; step < 60; step++) {
				auto& from = random.chance(50) ? a : b;
				auto& to = &from == &a ? b : a;
				auto ids = from.ids();
				if (ids.isEmpty()) continue;
				auto id = ids[random.upTo(static_cast<int>(ids.size()))];
				from.remove(id);
				auto there = to.ids();
				to.insert(id, there.isEmpty() ? 0 : there[random.upTo(static_cast<int>(there.size()))], nullptr);
				a.compute(area);
				b.compute(area);

				auto all = a.ids() + b.ids();
				std::ranges::sort(all);
				QCOMPARE(all, (QList<Id> {1, 2, 3, 4, 5, 6}));
				QCOMPARE(problem(a, a.ids(), {}, area), QString());
				QCOMPARE(problem(b, b.ids(), {}, area), QString());
			}
		}
	}

	void recomputingChangesNothing() {
		for (auto seed = 1; seed <= 500; seed++) {
			Random random(seed);
			DwindleLayout layout;
			auto area = QRect(0, 0, 1920, 1017);
			layout.compute(area);
			for (Id id = 1; id <= 8; id++) layout.insert(id, Id(random.upTo(static_cast<int>(id))), nullptr);
			for (Id id = 1; id <= 8; id++) layout.resize(id, random.upTo(300) - 150, random.upTo(300) - 150);
			layout.compute(area);
			QList<QRect> first;
			for (auto id: layout.ids()) first.append(layout.box(id));
			layout.compute(QRect(0, 0, 1600, 900));
			layout.compute(area);
			QList<QRect> second;
			for (auto id: layout.ids()) second.append(layout.box(id));
			QCOMPARE(second, first);
		}
	}
};

QTEST_APPLESS_MAIN(TestDwindleFuzz);

#include "dwindle_fuzz_test.moc"
