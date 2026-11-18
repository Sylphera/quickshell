#include "plugin.hpp"
#include <algorithm>

#include <qelapsedtimer.h>
#include <qvector.h> // NOLINT (what??)

#include "generation.hpp"
#include "logcat.hpp"

namespace {
QS_LOGGING_CATEGORY(logStartup, "quickshell.startup", QtWarningMsg);
}

// Registrars may run before static objects in other translation units.
static QVector<QsEnginePlugin*>& registeredPlugins() {
	static QVector<QsEnginePlugin*> plugins; // NOLINT
	return plugins;
}

void QsEnginePlugin::registerPlugin(QsEnginePlugin& plugin) { registeredPlugins().push_back(&plugin); }

void QsEnginePlugin::preinitPluginsOnly() {
	auto& plugins = registeredPlugins();
	plugins.removeIf([](QsEnginePlugin* plugin) { return !plugin->applies(); });

	std::ranges::sort(plugins, [](QsEnginePlugin* a, QsEnginePlugin* b) {
		return b->dependencies().contains(a->name());
	});

	for (QsEnginePlugin* plugin: plugins) {
		plugin->preinit();
	}
}

void QsEnginePlugin::initPlugins() {
	auto& plugins = registeredPlugins();
	plugins.removeIf([](QsEnginePlugin* plugin) { return !plugin->applies(); });

	std::ranges::sort(plugins, [](QsEnginePlugin* a, QsEnginePlugin* b) {
		return b->dependencies().contains(a->name());
	});

	for (QsEnginePlugin* plugin: plugins) {
		plugin->preinit();
	}

	auto timer = QElapsedTimer();
	timer.start();

	for (QsEnginePlugin* plugin: plugins) {
		plugin->init();
		qCDebug(logStartup) << "Initialized plugin" << plugin->name() << "in" << timer.restart() << "ms";
	}

	for (QsEnginePlugin* plugin: plugins) {
		plugin->registerTypes();
	}

	qCDebug(logStartup) << "Registered plugin types in" << timer.restart() << "ms";
}

void QsEnginePlugin::runConstructGeneration(EngineGeneration& generation) {
	auto& plugins = registeredPlugins();
	auto timer = QElapsedTimer();
	timer.start();

	for (QsEnginePlugin* plugin: plugins) {
		plugin->constructGeneration(generation);
		qCDebug(logStartup) << "Constructed generation for plugin" << plugin->name() << "in" << timer.restart() << "ms";
	}
}

void QsEnginePlugin::runOnReload() {
	auto& plugins = registeredPlugins();
	for (QsEnginePlugin* plugin: plugins) {
		plugin->onReload();
	}
}
