/******************************************************************************
    Copyright (C) 2013-2015 by Hugh Bailey <obs.jim@gmail.com>
                               Zachary Lund <admin@computerquip.com>
                               Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <time.h>
#include <obs.hpp>
#include <QMessageBox>
#include <QShowEvent>
#include <QDesktopServices>
#include <QFileDialog>

#include <util/dstr.h>
#include <util/util.hpp>
#include <util/platform.h>
#include <graphics/math-defs.h>

#include "obs-app.hpp"
#include "platform.hpp"
#include "visibility-item-widget.hpp"
#include "window-basic-settings.hpp"
#include "window-namedialog.hpp"
#include "window-basic-source-select.hpp"
#include "window-basic-main.hpp"
#include "window-basic-main-outputs.hpp"
#include "window-basic-properties.hpp"
#include "window-log-reply.hpp"
#include "window-projector.hpp"
#include "window-remux.hpp"
#include "qt-wrappers.hpp"
#include "display-helpers.hpp"
#include "volume-control.hpp"
#include "remote-text.hpp"

#include "ui_OBSBasic.h"

#include <fstream>
#include <sstream>

#include <QScreen>
#include <QWindow>

#define PREVIEW_EDGE_SIZE 10

using namespace std;

Q_DECLARE_METATYPE(OBSScene);
Q_DECLARE_METATYPE(OBSSceneItem);
Q_DECLARE_METATYPE(OBSSource);
Q_DECLARE_METATYPE(obs_order_movement);

static void AddExtraModulePaths()
{
	char base_module_dir[512];
	int ret = os_get_config_path(base_module_dir, sizeof(base_module_dir),
			"obs-studio/plugins/%module%");

	if (ret <= 0)
		return;

	string path = (char*)base_module_dir;
	obs_add_module_path((path + "/bin").c_str(), (path + "/data").c_str());
}

static QList<QKeySequence> DeleteKeys;

OBSBasic::OBSBasic(QWidget *parent)
	: OBSMainWindow  (parent),
	  ui             (new Ui::OBSBasic)
{
	ui->setupUi(this);
	ui->previewDisabledLabel->setVisible(false);

	copyActionsDynamicProperties();

	ui->sources->setItemDelegate(new VisibilityItemDelegate(ui->sources));

	int width = config_get_int(App()->GlobalConfig(), "BasicWindow", "cx");

	// Check if no values are saved (new installation).
	if (width != 0) {
		int height = config_get_int(App()->GlobalConfig(),
				"BasicWindow", "cy");
		int posx = config_get_int(App()->GlobalConfig(), "BasicWindow",
				"posx");
		int posy = config_get_int(App()->GlobalConfig(), "BasicWindow",
				"posy");

		setGeometry(posx, posy, width, height);
	}

	char styleSheetPath[512];
	int ret = os_get_config_path(styleSheetPath, sizeof(styleSheetPath),
			"obs-studio/basic/stylesheet.qss");
	if (ret > 0) {
		if (QFile::exists(styleSheetPath)) {
			QString path = QString("file:///") +
				QT_UTF8(styleSheetPath);
			App()->setStyleSheet(path);
		}
	}

	qRegisterMetaType<OBSScene>    ("OBSScene");
	qRegisterMetaType<OBSSceneItem>("OBSSceneItem");
	qRegisterMetaType<OBSSource>   ("OBSSource");
	qRegisterMetaType<obs_hotkey_id>("obs_hotkey_id");

	ui->scenes->setAttribute(Qt::WA_MacShowFocusRect, false);
	ui->sources->setAttribute(Qt::WA_MacShowFocusRect, false);

	connect(windowHandle(), &QWindow::screenChanged, [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi))
			ResizePreview(ovi.base_width, ovi.base_height);
	});

	stringstream name;
	name << "OBS " << App()->GetVersionString();

	installEventFilter(CreateShortcutFilter());

	blog(LOG_INFO, "%s", name.str().c_str());
	setWindowTitle(QT_UTF8(name.str().c_str()));

	connect(ui->scenes->itemDelegate(),
			SIGNAL(closeEditor(QWidget*,
					QAbstractItemDelegate::EndEditHint)),
			this,
			SLOT(SceneNameEdited(QWidget*,
					QAbstractItemDelegate::EndEditHint)));

	connect(ui->sources->itemDelegate(),
			SIGNAL(closeEditor(QWidget*,
					QAbstractItemDelegate::EndEditHint)),
			this,
			SLOT(SceneItemNameEdited(QWidget*,
					QAbstractItemDelegate::EndEditHint)));

	cpuUsageInfo = os_cpu_usage_info_start();
	cpuUsageTimer = new QTimer(this);
	connect(cpuUsageTimer, SIGNAL(timeout()),
			ui->statusbar, SLOT(UpdateCPUUsage()));
	cpuUsageTimer->start(3000);

	DeleteKeys =
#ifdef __APPLE__
		QList<QKeySequence>{{Qt::Key_Backspace}} <<
#endif
		QKeySequence::keyBindings(QKeySequence::Delete);

#ifdef __APPLE__
	ui->actionRemoveSource->setShortcuts(DeleteKeys);
	ui->actionRemoveScene->setShortcuts(DeleteKeys);

	ui->action_Settings->setMenuRole(QAction::PreferencesRole);
	ui->actionE_xit->setMenuRole(QAction::QuitRole);
#endif

	auto addNudge = [this](const QKeySequence &seq, const char *s)
	{
		QAction *nudge = new QAction(ui->preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		ui->preview->addAction(nudge);
		connect(nudge, SIGNAL(triggered()), this, s);
	};

	addNudge(Qt::Key_Up, SLOT(NudgeUp()));
	addNudge(Qt::Key_Down, SLOT(NudgeDown()));
	addNudge(Qt::Key_Left, SLOT(NudgeLeft()));
	addNudge(Qt::Key_Right, SLOT(NudgeRight()));
}

static void SaveAudioDevice(const char *name, int channel, obs_data_t *parent)
{
	obs_source_t *source = obs_get_output_source(channel);
	if (!source)
		return;

	obs_data_t *data = obs_save_source(source);

	obs_data_set_obj(parent, name, data);

	obs_data_release(data);
	obs_source_release(source);
}

static obs_data_t *GenerateSaveData()
{
	obs_data_t       *saveData     = obs_data_create();
	obs_data_array_t *sourcesArray = obs_save_sources();
	obs_source_t     *currentScene = obs_get_output_source(0);
	const char       *sceneName   = obs_source_get_name(currentScene);

	SaveAudioDevice(DESKTOP_AUDIO_1, 1, saveData);
	SaveAudioDevice(DESKTOP_AUDIO_2, 2, saveData);
	SaveAudioDevice(AUX_AUDIO_1,     3, saveData);
	SaveAudioDevice(AUX_AUDIO_2,     4, saveData);
	SaveAudioDevice(AUX_AUDIO_3,     5, saveData);

	obs_data_set_string(saveData, "current_scene", sceneName);
	obs_data_set_array(saveData, "sources", sourcesArray);
	obs_data_array_release(sourcesArray);
	obs_source_release(currentScene);

	return saveData;
}

void OBSBasic::copyActionsDynamicProperties()
{
	// Themes need the QAction dynamic properties
	for (QAction *x : ui->scenesToolbar->actions()) {
		QWidget* temp = ui->scenesToolbar->widgetForAction(x);

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y, x->property(y));
		}
	}

	for (QAction *x : ui->sourcesToolbar->actions()) {
		QWidget* temp = ui->sourcesToolbar->widgetForAction(x);

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y, x->property(y));
		}
	}
}

void OBSBasic::ClearVolumeControls()
{
	VolControl *control;

	for (size_t i = 0; i < volumes.size(); i++) {
		control = volumes[i];
		delete control;
	}

	volumes.clear();
}

void OBSBasic::Save(const char *file)
{
	obs_data_t *saveData  = GenerateSaveData();
	const char *jsonData = obs_data_get_json(saveData);

	if (!!jsonData) {
		/* TODO: maybe a message box here? */
		bool success = os_quick_write_utf8_file(file, jsonData,
				strlen(jsonData), false);
		if (!success)
			blog(LOG_ERROR, "Could not save scene data to %s",
					file);
	}

	obs_data_release(saveData);
}

static void LoadAudioDevice(const char *name, int channel, obs_data_t *parent)
{
	obs_data_t *data = obs_data_get_obj(parent, name);
	if (!data)
		return;

	obs_source_t *source = obs_load_source(data);
	if (source) {
		obs_set_output_source(channel, source);
		obs_source_release(source);
	}

	obs_data_release(data);
}

void OBSBasic::CreateDefaultScene()
{
	obs_scene_t  *scene  = obs_scene_create(Str("Basic.Scene"));
	obs_source_t *source = obs_scene_get_source(scene);

	obs_add_source(source);

#ifdef __APPLE__
	source = obs_source_create(OBS_SOURCE_TYPE_INPUT, "display_capture",
			Str("Basic.DisplayCapture"), NULL, nullptr);

	if (source) {
		obs_scene_add(scene, source);
		obs_add_source(source);
		obs_source_release(source);
	}
#endif

	obs_set_output_source(0, obs_scene_get_source(scene));
	obs_scene_release(scene);
}

void OBSBasic::Load(const char *file)
{
	if (!file) {
		blog(LOG_ERROR, "Could not find file %s", file);
		return;
	}

	BPtr<char> jsonData = os_quick_read_utf8_file(file);
	if (!jsonData) {
		CreateDefaultScene();
		return;
	}

	obs_data_t       *data       = obs_data_create_from_json(jsonData);
	obs_data_array_t *sources    = obs_data_get_array(data, "sources");
	const char       *sceneName = obs_data_get_string(data,
			"current_scene");
	obs_source_t     *curScene;

	LoadAudioDevice(DESKTOP_AUDIO_1, 1, data);
	LoadAudioDevice(DESKTOP_AUDIO_2, 2, data);
	LoadAudioDevice(AUX_AUDIO_1,     3, data);
	LoadAudioDevice(AUX_AUDIO_2,     4, data);
	LoadAudioDevice(AUX_AUDIO_3,     5, data);

	obs_load_sources(sources);

	curScene = obs_get_source_by_name(sceneName);
	obs_set_output_source(0, curScene);
	obs_source_release(curScene);

	obs_data_array_release(sources);
	obs_data_release(data);
}

static inline bool HasAudioDevices(const char *source_id)
{
	const char *output_id = source_id;
	obs_properties_t *props = obs_get_source_properties(
			OBS_SOURCE_TYPE_INPUT, output_id);
	size_t count = 0;

	if (!props)
		return false;

	obs_property_t *devices = obs_properties_get(props, "device_id");
	if (devices)
		count = obs_property_list_item_count(devices);

	obs_properties_destroy(props);

	return count != 0;
}

#define SERVICE_PATH "obs-studio/basic/service.json"

void OBSBasic::SaveService()
{
	if (!service)
		return;

	char serviceJsonPath[512];
	int ret = os_get_config_path(serviceJsonPath, sizeof(serviceJsonPath),
			SERVICE_PATH);
	if (ret <= 0)
		return;

	obs_data_t *data     = obs_data_create();
	obs_data_t *settings = obs_service_get_settings(service);

	obs_data_set_string(data, "type", obs_service_get_type(service));
	obs_data_set_obj(data, "settings", settings);

	const char *json = obs_data_get_json(data);

	os_quick_write_utf8_file(serviceJsonPath, json, strlen(json), false);

	obs_data_release(settings);
	obs_data_release(data);
}

bool OBSBasic::LoadService()
{
	const char *type;

	char serviceJsonPath[512];
	int ret = os_get_config_path(serviceJsonPath, sizeof(serviceJsonPath),
			SERVICE_PATH);
	if (ret <= 0)
		return false;

	BPtr<char> jsonText = os_quick_read_utf8_file(serviceJsonPath);
	if (!jsonText)
		return false;

	obs_data_t *data = obs_data_create_from_json(jsonText);

	obs_data_set_default_string(data, "type", "rtmp_common");
	type = obs_data_get_string(data, "type");

	obs_data_t *settings = obs_data_get_obj(data, "settings");
	obs_data_t *hotkey_data = obs_data_get_obj(data, "hotkeys");

	service = obs_service_create(type, "default_service", settings,
			hotkey_data);
	obs_service_release(service);

	obs_data_release(hotkey_data);
	obs_data_release(settings);
	obs_data_release(data);

	return !!service;
}

bool OBSBasic::InitService()
{
	if (LoadService())
		return true;

	service = obs_service_create("rtmp_common", "default_service", nullptr,
			nullptr);
	if (!service)
		return false;
	obs_service_release(service);

	return true;
}

bool OBSBasic::InitBasicConfigDefaults()
{
	bool hasDesktopAudio = HasAudioDevices(App()->OutputAudioSource());
	bool hasInputAudio   = HasAudioDevices(App()->InputAudioSource());

	vector<MonitorInfo> monitors;
	GetMonitors(monitors);

	if (!monitors.size()) {
		OBSErrorBox(NULL, "There appears to be no monitors.  Er, this "
		                  "technically shouldn't be possible.");
		return false;
	}

	uint32_t cx = monitors[0].cx;
	uint32_t cy = monitors[0].cy;

	config_set_default_string(basicConfig, "Output", "Type", "Simple");

	config_set_default_string(basicConfig, "SimpleOutput", "FilePath",
			GetDefaultVideoSavePath().c_str());
	config_set_default_uint  (basicConfig, "SimpleOutput", "VBitrate",
			2500);
	config_set_default_uint  (basicConfig, "SimpleOutput", "ABitrate", 128);
	config_set_default_bool  (basicConfig, "SimpleOutput", "Reconnect",
			true);
	config_set_default_uint  (basicConfig, "SimpleOutput", "RetryDelay",
			10);
	config_set_default_uint  (basicConfig, "SimpleOutput", "MaxRetries",
			20);
	config_set_default_bool  (basicConfig, "SimpleOutput", "UseAdvanced",
			false);
	config_set_default_bool  (basicConfig, "SimpleOutput", "UseCBR", true);
	config_set_default_bool  (basicConfig, "SimpleOutput", "UseBufsize",
			false);
	config_set_default_int   (basicConfig, "SimpleOutput", "Bufsize", 2500);
	config_set_default_string(basicConfig, "SimpleOutput", "Preset",
			"veryfast");

	config_set_default_bool  (basicConfig, "AdvOut", "Reconnect", true);
	config_set_default_uint  (basicConfig, "AdvOut", "RetryDelay", 10);
	config_set_default_uint  (basicConfig, "AdvOut", "MaxRetries", 20);
	config_set_default_bool  (basicConfig, "AdvOut", "ApplyServiceSettings",
			true);
	config_set_default_bool  (basicConfig, "AdvOut", "UseRescale", false);
	config_set_default_bool  (basicConfig, "AdvOut", "Multitrack", false);
	config_set_default_uint  (basicConfig, "AdvOut", "TrackIndex", 1);
	config_set_default_uint  (basicConfig, "AdvOut", "TrackCount", 1);
	config_set_default_string(basicConfig, "AdvOut", "Encoder", "obs_x264");

	config_set_default_string(basicConfig, "AdvOut", "RecType", "Standard");

	config_set_default_string(basicConfig, "AdvOut", "RecFilePath",
			GetDefaultVideoSavePath().c_str());
	config_set_default_bool  (basicConfig, "AdvOut", "RecUseRescale",
			false);
	config_set_default_bool  (basicConfig, "AdvOut", "RecMultitrack",
			false);
	config_set_default_uint  (basicConfig, "AdvOut", "RecTrackIndex", 1);
	config_set_default_uint  (basicConfig, "AdvOut", "RecTrackCount", 1);
	config_set_default_string(basicConfig, "AdvOut", "RecEncoder",
			"none");

	config_set_default_uint  (basicConfig, "AdvOut", "FFVBitrate", 2500);
	config_set_default_bool  (basicConfig, "AdvOut", "FFUseRescale",
			false);
	config_set_default_uint  (basicConfig, "AdvOut", "FFABitrate", 160);
	config_set_default_uint  (basicConfig, "AdvOut", "FFAudioTrack", 1);

	config_set_default_uint  (basicConfig, "AdvOut", "Track1Bitrate", 160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track2Bitrate", 160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track3Bitrate", 160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track4Bitrate", 160);

	config_set_default_uint  (basicConfig, "Video", "BaseCX",   cx);
	config_set_default_uint  (basicConfig, "Video", "BaseCY",   cy);

	cx = cx * 10 / 15;
	cy = cy * 10 / 15;
	config_set_default_uint  (basicConfig, "Video", "OutputCX", cx);
	config_set_default_uint  (basicConfig, "Video", "OutputCY", cy);

	config_set_default_uint  (basicConfig, "Video", "FPSType", 0);
	config_set_default_string(basicConfig, "Video", "FPSCommon", "30");
	config_set_default_uint  (basicConfig, "Video", "FPSInt", 30);
	config_set_default_uint  (basicConfig, "Video", "FPSNum", 30);
	config_set_default_uint  (basicConfig, "Video", "FPSDen", 1);
	config_set_default_string(basicConfig, "Video", "ScaleType", "bicubic");
	config_set_default_string(basicConfig, "Video", "ColorFormat", "NV12");
	config_set_default_string(basicConfig, "Video", "ColorSpace", "709");
	config_set_default_string(basicConfig, "Video", "ColorRange",
			"Partial");

	config_set_default_uint  (basicConfig, "Audio", "SampleRate", 44100);
	config_set_default_string(basicConfig, "Audio", "ChannelSetup",
			"Stereo");
	config_set_default_uint  (basicConfig, "Audio", "BufferingTime", 1000);

	config_set_default_string(basicConfig, "Audio", "DesktopDevice1",
			hasDesktopAudio ? "default" : "disabled");
	config_set_default_string(basicConfig, "Audio", "DesktopDevice2",
			"disabled");
	config_set_default_string(basicConfig, "Audio", "AuxDevice1",
			hasInputAudio ? "default" : "disabled");
	config_set_default_string(basicConfig, "Audio", "AuxDevice2",
			"disabled");
	config_set_default_string(basicConfig, "Audio", "AuxDevice3",
			"disabled");

	return true;
}

bool OBSBasic::InitBasicConfig()
{
	char configPath[512];
	int ret = os_get_config_path(configPath, sizeof(configPath),
			"obs-studio/basic/basic.ini");
	if (ret <= 0) {
		OBSErrorBox(nullptr, "Failed to get base.ini path");
		return false;
	}

	int code = basicConfig.Open(configPath, CONFIG_OPEN_ALWAYS);
	if (code != CONFIG_SUCCESS) {
		OBSErrorBox(NULL, "Failed to open basic.ini: %d", code);
		return false;
	}

	return InitBasicConfigDefaults();
}

void OBSBasic::InitOBSCallbacks()
{
	signal_handler_connect(obs_get_signal_handler(), "source_add",
			OBSBasic::SourceAdded, this);
	signal_handler_connect(obs_get_signal_handler(), "source_remove",
			OBSBasic::SourceRemoved, this);
	signal_handler_connect(obs_get_signal_handler(), "channel_change",
			OBSBasic::ChannelChanged, this);
	signal_handler_connect(obs_get_signal_handler(), "source_activate",
			OBSBasic::SourceActivated, this);
	signal_handler_connect(obs_get_signal_handler(), "source_deactivate",
			OBSBasic::SourceDeactivated, this);
	signal_handler_connect(obs_get_signal_handler(), "source_rename",
			OBSBasic::SourceRenamed, this);
}

void OBSBasic::InitPrimitives()
{
	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 0.0f);
	box = gs_render_save();

	gs_render_start(true);
	for (int i = 0; i <= 360; i += (360/20)) {
		float pos = RAD(float(i));
		gs_vertex2f(cosf(pos), sinf(pos));
	}
	circle = gs_render_save();

	obs_leave_graphics();
}

void OBSBasic::ResetOutputs()
{
	const char *mode = config_get_string(basicConfig, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if (!outputHandler || !outputHandler->Active()) {
		outputHandler.reset();
		outputHandler.reset(advOut ?
			CreateAdvancedOutputHandler(this) :
			CreateSimpleOutputHandler(this));
	} else {
		outputHandler->Update();
	}
}

void OBSBasic::OBSInit()
{
	char savePath[512];
	int ret = os_get_config_path(savePath, sizeof(savePath),
			"obs-studio/basic/scenes.json");
	if (ret <= 0)
		throw "Failed to get scenes.json file path";

	/* make sure it's fully displayed before doing any initialization */
	show();
	App()->processEvents();

	if (!obs_startup(App()->GetLocale()))
		throw "Failed to initialize libobs";
	if (!InitBasicConfig())
		throw "Failed to load basic.ini";
	if (!ResetAudio())
		throw "Failed to initialize audio";

	ret = ResetVideo();

	switch (ret) {
	case OBS_VIDEO_MODULE_NOT_FOUND:
		throw "Failed to initialize video:  Graphics module not found";
	case OBS_VIDEO_NOT_SUPPORTED:
		throw "Failed to initialize video:  Required graphics API "
		      "functionality not found on these drivers or "
		      "unavailable on this equipment";
	case OBS_VIDEO_INVALID_PARAM:
		throw "Failed to initialize video:  Invalid parameters";
	default:
		if (ret != OBS_VIDEO_SUCCESS)
			throw "Failed to initialize video:  Unspecified error";
	}

	InitOBSCallbacks();
	InitHotkeys();

	AddExtraModulePaths();
	obs_load_all_modules();

	ResetOutputs();
	CreateHotkeys();

	if (!InitService())
		throw "Failed to initialize service";

	InitPrimitives();

	Load(savePath);
	ResetAudioDevices();

	TimedCheckForUpdates();
	loaded = true;

	saveTimer = new QTimer(this);
	connect(saveTimer, SIGNAL(timeout()), this, SLOT(SaveProject()));
	saveTimer->start(20000);

	bool previewEnabled = config_get_bool(App()->GlobalConfig(),
			"BasicWindow", "PreviewEnabled");
	if (!previewEnabled)
		QMetaObject::invokeMethod(this, "TogglePreview",
				Qt::QueuedConnection);
}

void OBSBasic::InitHotkeys()
{
	struct obs_hotkeys_translations t = {};
	t.insert                       = Str("Hotkeys.Insert");
	t.del                          = Str("Hotkeys.Delete");
	t.home                         = Str("Hotkeys.Home");
	t.end                          = Str("Hotkeys.End");
	t.page_up                      = Str("Hotkeys.PageUp");
	t.page_down                    = Str("Hotkeys.PageDown");
	t.num_lock                     = Str("Hotkeys.NumLock");
	t.scroll_lock                  = Str("Hotkeys.ScrollLock");
	t.caps_lock                    = Str("Hotkeys.CapsLock");
	t.backspace                    = Str("Hotkeys.Backspace");
	t.tab                          = Str("Hotkeys.Tab");
	t.print                        = Str("Hotkeys.Print");
	t.pause                        = Str("Hotkeys.Pause");
	t.left                         = Str("Hotkeys.Left");
	t.right                        = Str("Hotkeys.Right");
	t.up                           = Str("Hotkeys.Up");
	t.down                         = Str("Hotkeys.Down");
#ifdef _WIN32
	t.meta                         = Str("Hotkeys.Windows");
#else
	t.meta                         = Str("Hotkeys.Super");
#endif
	t.menu                         = Str("Hotkeys.Menu");
	t.space                        = Str("Hotkeys.Space");
	t.numpad_num                   = Str("Hotkeys.NumpadNum");
	t.numpad_multiply              = Str("Hotkeys.NumpadMultiply");
	t.numpad_divide                = Str("Hotkeys.NumpadDivide");
	t.numpad_plus                  = Str("Hotkeys.NumpadAdd");
	t.numpad_minus                 = Str("Hotkeys.NumpadSubtract");
	t.numpad_decimal               = Str("Hotkeys.NumpadDecimal");
	t.apple_keypad_num             = Str("Hotkeys.AppleKeypadNum");
	t.apple_keypad_multiply        = Str("Hotkeys.AppleKeypadMultiply");
	t.apple_keypad_divide          = Str("Hotkeys.AppleKeypadDivide");
	t.apple_keypad_plus            = Str("Hotkeys.AppleKeypadAdd");
	t.apple_keypad_minus           = Str("Hotkeys.AppleKeypadSubtract");
	t.apple_keypad_decimal         = Str("Hotkeys.AppleKeypadDecimal");
	t.apple_keypad_equal           = Str("Hotkeys.AppleKeypadEqual");
	t.mouse_num                    = Str("Hotkeys.MouseButton");
	obs_hotkeys_set_translations(&t);

	obs_hotkeys_set_audio_hotkeys_translations(Str("Mute"), Str("Unmute"),
			Str("Push-to-mute"), Str("Push-to-talk"));

	obs_hotkeys_set_sceneitem_hotkeys_translations(
			Str("SceneItemShow"), Str("SceneItemHide"));

	obs_hotkey_enable_callback_rerouting(true);
	obs_hotkey_set_callback_routing_func(OBSBasic::HotkeyTriggered, this);
}

void OBSBasic::ProcessHotkey(obs_hotkey_id id, bool pressed)
{
	obs_hotkey_trigger_routed_callback(id, pressed);
}

void OBSBasic::HotkeyTriggered(void *data, obs_hotkey_id id, bool pressed)
{
	OBSBasic &basic = *static_cast<OBSBasic*>(data);
	QMetaObject::invokeMethod(&basic, "ProcessHotkey",
			Q_ARG(obs_hotkey_id, id), Q_ARG(bool, pressed));
}

void OBSBasic::CreateHotkeys()
{
	auto LoadHotkeyData = [&](const char *name) -> OBSData
	{
		const char *info = config_get_string(basicConfig,
				"Hotkeys", name);
		if (!info)
			return {};

		obs_data_t *data = obs_data_create_from_json(info);
		if (!data)
			return {};

		OBSData res = data;
		obs_data_release(data);
		return res;
	};

	auto LoadHotkeyPair = [&](obs_hotkey_pair_id id, const char *name0,
			const char *name1)
	{
		obs_data_array_t *array0 =
			obs_data_get_array(LoadHotkeyData(name0), "bindings");
		obs_data_array_t *array1 =
			obs_data_get_array(LoadHotkeyData(name1), "bindings");

		obs_hotkey_pair_load(id, array0, array1);
		obs_data_array_release(array0);
		obs_data_array_release(array1);
	};

#define MAKE_CALLBACK(pred, method) \
	[](void *data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) \
	{ \
		OBSBasic &basic = *static_cast<OBSBasic*>(data); \
		if (pred && pressed) { \
			method(); \
			return true; \
		} \
		return false; \
	}

	streamingHotkeys = obs_hotkey_pair_register_frontend(
			"OBSBasic.StartStreaming",
			Str("Basic.Hotkeys.StartStreaming"),
			"OBSBasic.StopStreaming",
			Str("Basic.Hotkeys.StopStreaming"),
			MAKE_CALLBACK(!basic.outputHandler->StreamingActive(),
				basic.StartStreaming),
			MAKE_CALLBACK(basic.outputHandler->StreamingActive(),
				basic.StopStreaming),
			this, this);
	LoadHotkeyPair(streamingHotkeys,
			"OBSBasic.StartStreaming", "OBSBasic.StopStreaming");

	recordingHotkeys = obs_hotkey_pair_register_frontend(
			"OBSBasic.StartRecording",
			Str("Basic.Hotkeys.StartRecording"),
			"OBSBasic.StopRecording",
			Str("Basic.Hotkeys.StopRecording"),
			MAKE_CALLBACK(!basic.outputHandler->RecordingActive(),
				basic.StartRecording),
			MAKE_CALLBACK(basic.outputHandler->RecordingActive(),
				basic.StopRecording),
			this, this);
	LoadHotkeyPair(recordingHotkeys,
			"OBSBasic.StartRecording", "OBSBasic.StopRecording");

#undef MAKE_CALLBACK
}

OBSBasic::~OBSBasic()
{
	bool previewEnabled = obs_preview_enabled();

	/* XXX: any obs data must be released before calling obs_shutdown.
	 * currently, we can't automate this with C++ RAII because of the
	 * delicate nature of obs_shutdown needing to be freed before the UI
	 * can be freed, and we have no control over the destruction order of
	 * the Qt UI stuff, so we have to manually clear any references to
	 * libobs. */
	delete cpuUsageTimer;
	os_cpu_usage_info_destroy(cpuUsageInfo);

	obs_hotkey_set_callback_routing_func(nullptr, nullptr);
	obs_hotkey_pair_unregister(streamingHotkeys);
	obs_hotkey_pair_unregister(recordingHotkeys);

	service = nullptr;
	outputHandler.reset();

	if (interaction)
		delete interaction;

	if (properties)
		delete properties;

	if (filters)
		delete filters;

	if (transformWindow)
		delete transformWindow;

	if (advAudioWindow)
		delete advAudioWindow;

	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	gs_vertexbuffer_destroy(circle);
	obs_leave_graphics();

	/* When shutting down, sometimes source references can get in to the
	 * event queue, and if we don't forcibly process those events they
	 * won't get processed until after obs_shutdown has been called.  I
	 * really wish there were a more elegant way to deal with this via C++,
	 * but Qt doesn't use C++ in a normal way, so you can't really rely on
	 * normal C++ behavior for your data to be freed in the order that you
	 * expect or want it to. */
	QApplication::sendPostedEvents(this);

	obs_shutdown();

	config_set_int(App()->GlobalConfig(), "General", "LastVersion",
			LIBOBS_API_VER);

	QRect lastGeom = normalGeometry();

	config_set_int(App()->GlobalConfig(), "BasicWindow", "cx",
			lastGeom.width());
	config_set_int(App()->GlobalConfig(), "BasicWindow", "cy",
			lastGeom.height());
	config_set_int(App()->GlobalConfig(), "BasicWindow", "posx",
			lastGeom.x());
	config_set_int(App()->GlobalConfig(), "BasicWindow", "posy",
			lastGeom.y());
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "PreviewEnabled",
			previewEnabled);
	config_save(App()->GlobalConfig());
}

void OBSBasic::SaveProject()
{
	char savePath[512];
	int ret = os_get_config_path(savePath, sizeof(savePath),
			"obs-studio/basic/scenes.json");
	if (ret <= 0)
		return;

	Save(savePath);
}

OBSScene OBSBasic::GetCurrentScene()
{
	QListWidgetItem *item = ui->scenes->currentItem();
	return item ? item->data(Qt::UserRole).value<OBSScene>() : nullptr;
}

OBSSceneItem OBSBasic::GetSceneItem(QListWidgetItem *item)
{
	return item ? item->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
}

OBSSceneItem OBSBasic::GetCurrentSceneItem()
{
	return GetSceneItem(ui->sources->currentItem());
}

void OBSBasic::UpdateSources(OBSScene scene)
{
	ui->sources->clear();

	obs_scene_enum_items(scene,
			[] (obs_scene_t *scene, obs_sceneitem_t *item, void *p)
			{
				OBSBasic *window = static_cast<OBSBasic*>(p);
				window->InsertSceneItem(item);

				UNUSED_PARAMETER(scene);
				return true;
			}, this);
}

void OBSBasic::InsertSceneItem(obs_sceneitem_t *item)
{
	obs_source_t *source = obs_sceneitem_get_source(item);

	QListWidgetItem *listItem = new QListWidgetItem();
	listItem->setData(Qt::UserRole,
			QVariant::fromValue(OBSSceneItem(item)));

	ui->sources->insertItem(0, listItem);
	ui->sources->setCurrentRow(0);

	SetupVisibilityItem(ui->sources, listItem, item);

	/* if the source was just created, open properties dialog */
	if (sourceSceneRefs[source] == 0 && loaded)
		CreatePropertiesWindow(source);
}

void OBSBasic::CreateInteractionWindow(obs_source_t *source)
{
	if (interaction)
		interaction->close();

	interaction = new OBSBasicInteraction(this, source);
	interaction->Init();
	interaction->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreatePropertiesWindow(obs_source_t *source)
{
	if (properties)
		properties->close();

	properties = new OBSBasicProperties(this, source);
	properties->Init();
	properties->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreateFiltersWindow(obs_source_t *source)
{
	if (filters)
		filters->close();

	filters = new OBSBasicFilters(this, source);
	filters->Init();
	filters->setAttribute(Qt::WA_DeleteOnClose, true);
}

/* Qt callbacks for invokeMethod */

void OBSBasic::AddScene(OBSSource source)
{
	const char *name  = obs_source_get_name(source);
	obs_scene_t *scene = obs_scene_from_source(source);

	QListWidgetItem *item = new QListWidgetItem(QT_UTF8(name));
	item->setData(Qt::UserRole, QVariant::fromValue(OBSScene(scene)));
	ui->scenes->addItem(item);

	obs_hotkey_register_source(source, "OBSBasic.SelectScene",
			Str("Basic.Hotkeys.SelectScene"),
			[](void *data,
				obs_hotkey_id, obs_hotkey_t*, bool pressed)
	{
		auto potential_source = static_cast<obs_source_t*>(data);
		auto source = obs_source_get_ref(potential_source);
		if (source && pressed)
			obs_set_output_source(0, source);
		obs_source_release(source);
	}, static_cast<obs_source_t*>(source));

	signal_handler_t *handler = obs_source_get_signal_handler(source);
	signal_handler_connect(handler, "item_add",
			OBSBasic::SceneItemAdded, this);
	signal_handler_connect(handler, "item_remove",
			OBSBasic::SceneItemRemoved, this);
	signal_handler_connect(handler, "item_select",
			OBSBasic::SceneItemSelected, this);
	signal_handler_connect(handler, "item_deselect",
			OBSBasic::SceneItemDeselected, this);
	signal_handler_connect(handler, "reorder",
			OBSBasic::SceneReordered, this);
}

void OBSBasic::RemoveScene(OBSSource source)
{
	const char *name = obs_source_get_name(source);

	QListWidgetItem *sel = ui->scenes->currentItem();
	QList<QListWidgetItem*> items = ui->scenes->findItems(QT_UTF8(name),
			Qt::MatchExactly);

	if (sel != nullptr) {
		if (items.contains(sel))
			ui->sources->clear();
		delete sel;
	}
}

void OBSBasic::AddSceneItem(OBSSceneItem item)
{
	obs_scene_t  *scene  = obs_sceneitem_get_scene(item);
	obs_source_t *source = obs_sceneitem_get_source(item);

	if (GetCurrentScene() == scene)
		InsertSceneItem(item);

	sourceSceneRefs[source] = sourceSceneRefs[source] + 1;
}

void OBSBasic::RemoveSceneItem(OBSSceneItem item)
{
	obs_scene_t *scene = obs_sceneitem_get_scene(item);

	if (GetCurrentScene() == scene) {
		for (int i = 0; i < ui->sources->count(); i++) {
			QListWidgetItem *listItem = ui->sources->item(i);
			QVariant userData = listItem->data(Qt::UserRole);

			if (userData.value<OBSSceneItem>() == item) {
				delete listItem;
				break;
			}
		}
	}

	obs_source_t *source = obs_sceneitem_get_source(item);

	int scenes = sourceSceneRefs[source] - 1;
	sourceSceneRefs[source] = scenes;

	if (scenes == 0) {
		obs_source_remove(source);
		sourceSceneRefs.erase(source);
	}
}

void OBSBasic::UpdateSceneSelection(OBSSource source)
{
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		const char *name = obs_source_get_name(source);

		if (!scene)
			return;

		QList<QListWidgetItem*> items =
			ui->scenes->findItems(QT_UTF8(name), Qt::MatchExactly);

		if (items.count()) {
			sceneChanging = true;
			ui->scenes->setCurrentItem(items.first());
			sceneChanging = false;

			UpdateSources(scene);
		}
	}
}

static void RenameListValues(QListWidget *listWidget, const QString &newName,
		const QString &prevName)
{
	QList<QListWidgetItem*> items =
		listWidget->findItems(prevName, Qt::MatchExactly);

	for (int i = 0; i < items.count(); i++)
		items[i]->setText(newName);
}

void OBSBasic::RenameSources(QString newName, QString prevName)
{
	RenameListValues(ui->scenes,  newName, prevName);

	for (size_t i = 0; i < volumes.size(); i++) {
		if (volumes[i]->GetName().compare(prevName) == 0)
			volumes[i]->SetName(newName);
	}
}

void OBSBasic::SelectSceneItem(OBSScene scene, OBSSceneItem item, bool select)
{
	if (scene != GetCurrentScene())
		return;

	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem *witem = ui->sources->item(i);
		QVariant data = witem->data(Qt::UserRole);
		if (!data.canConvert<OBSSceneItem>())
			continue;

		if (item != data.value<OBSSceneItem>())
			continue;

		if (select) {
			ui->sources->setCurrentItem(witem);
		} else if (ui->sources->currentItem() == witem) {
			ui->sources->setCurrentItem(nullptr);
		}

		break;
	}
}

void OBSBasic::MoveSceneItem(OBSSceneItem item, obs_order_movement movement)
{
	OBSScene scene = obs_sceneitem_get_scene(item);
	if (scene != GetCurrentScene())
		return;

	int curRow = ui->sources->currentRow();
	if (curRow == -1)
		return;

	QListWidgetItem *listItem = ui->sources->takeItem(curRow);

	switch (movement) {
	case OBS_ORDER_MOVE_UP:
		if (curRow > 0)
			curRow--;
		break;

	case OBS_ORDER_MOVE_DOWN:
		if (curRow < ui->sources->count())
			curRow++;
		break;

	case OBS_ORDER_MOVE_TOP:
		curRow = 0;
		break;

	case OBS_ORDER_MOVE_BOTTOM:
		curRow = ui->sources->count();
		break;
	}

	ui->sources->insertItem(curRow, listItem);
	ui->sources->setCurrentRow(curRow);
}

void OBSBasic::ActivateAudioSource(OBSSource source)
{
	VolControl *vol = new VolControl(source);

	volumes.push_back(vol);
	ui->volumeWidgets->layout()->addWidget(vol);
}

void OBSBasic::DeactivateAudioSource(OBSSource source)
{
	for (size_t i = 0; i < volumes.size(); i++) {
		if (volumes[i]->GetSource() == source) {
			delete volumes[i];
			volumes.erase(volumes.begin() + i);
			break;
		}
	}
}

bool OBSBasic::QueryRemoveSource(obs_source_t *source)
{
	const char *name  = obs_source_get_name(source);

	QString text = QTStr("ConfirmRemove.Text");
	text.replace("$1", QT_UTF8(name));

	QMessageBox remove_source(this);
	remove_source.setText(text);
	QAbstractButton *Yes = remove_source.addButton(QTStr("Yes"),
			QMessageBox::YesRole);
	remove_source.addButton(QTStr("No"), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QTStr("ConfirmRemove.Title"));
	remove_source.exec();

	return Yes == remove_source.clickedButton();
}

#define UPDATE_CHECK_INTERVAL (60*60*24*4) /* 4 days */

#ifdef UPDATE_SPARKLE
void init_sparkle_updater(bool update_to_undeployed);
void trigger_sparkle_update();
#endif

void OBSBasic::TimedCheckForUpdates()
{
#ifdef UPDATE_SPARKLE
	init_sparkle_updater(config_get_bool(App()->GlobalConfig(), "General",
				"UpdateToUndeployed"));
#else
	long long lastUpdate = config_get_int(App()->GlobalConfig(), "General",
			"LastUpdateCheck");
	uint32_t lastVersion = config_get_int(App()->GlobalConfig(), "General",
			"LastVersion");

	if (lastVersion < LIBOBS_API_VER) {
		lastUpdate = 0;
		config_set_int(App()->GlobalConfig(), "General",
				"LastUpdateCheck", 0);
	}

	long long t    = (long long)time(nullptr);
	long long secs = t - lastUpdate;

	if (secs > UPDATE_CHECK_INTERVAL)
		CheckForUpdates();
#endif
}

void OBSBasic::CheckForUpdates()
{
#ifdef UPDATE_SPARKLE
	trigger_sparkle_update();
#else
	ui->actionCheckForUpdates->setEnabled(false);

	if (updateCheckThread) {
		updateCheckThread->wait();
		delete updateCheckThread;
	}

	RemoteTextThread *thread = new RemoteTextThread(
			"https://obsproject.com/obs2_update/basic.json");
	updateCheckThread = thread;
	connect(thread, &RemoteTextThread::Result,
			this, &OBSBasic::updateFileFinished);
	updateCheckThread->start();
#endif
}

#ifdef __APPLE__
#define VERSION_ENTRY "mac"
#elif _WIN32
#define VERSION_ENTRY "windows"
#else
#define VERSION_ENTRY "other"
#endif

void OBSBasic::updateFileFinished(const QString &text, const QString &error)
{
	ui->actionCheckForUpdates->setEnabled(true);

	if (text.isEmpty()) {
		blog(LOG_WARNING, "Update check failed: %s", QT_TO_UTF8(error));
		return;
	}

	obs_data_t *returnData  = obs_data_create_from_json(QT_TO_UTF8(text));
	obs_data_t *versionData = obs_data_get_obj(returnData, VERSION_ENTRY);
	const char *description = obs_data_get_string(returnData,
			"description");
	const char *download    = obs_data_get_string(versionData, "download");

	if (returnData && versionData && description && download) {
		long major   = obs_data_get_int(versionData, "major");
		long minor   = obs_data_get_int(versionData, "minor");
		long patch   = obs_data_get_int(versionData, "patch");
		long version = MAKE_SEMANTIC_VERSION(major, minor, patch);

		blog(LOG_INFO, "Update check: latest version is: %ld.%ld.%ld",
				major, minor, patch);

		if (version > LIBOBS_API_VER) {
			QString     str = QTStr("UpdateAvailable.Text");
			QMessageBox messageBox(this);

			str = str.arg(QString::number(major),
			              QString::number(minor),
			              QString::number(patch),
			              download);

			messageBox.setWindowTitle(QTStr("UpdateAvailable"));
			messageBox.setTextFormat(Qt::RichText);
			messageBox.setText(str);
			messageBox.setInformativeText(QT_UTF8(description));
			messageBox.exec();

			long long t = (long long)time(nullptr);
			config_set_int(App()->GlobalConfig(), "General",
					"LastUpdateCheck", t);
			config_save(App()->GlobalConfig());
		}
	} else {
		blog(LOG_WARNING, "Bad JSON file received from server");
	}

	obs_data_release(versionData);
	obs_data_release(returnData);
}

void OBSBasic::RemoveSelectedScene()
{
	OBSScene scene = GetCurrentScene();
	if (scene) {
		obs_source_t *source = obs_scene_get_source(scene);
		if (QueryRemoveSource(source))
			obs_source_remove(source);
	}
}

void OBSBasic::RemoveSelectedSceneItem()
{
	OBSSceneItem item = GetCurrentSceneItem();
	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (QueryRemoveSource(source))
			obs_sceneitem_remove(item);
	}
}

struct ReorderInfo {
	int idx = 0;
	OBSBasic *window;

	inline ReorderInfo(OBSBasic *window_) : window(window_) {}
};

void OBSBasic::ReorderSceneItem(obs_sceneitem_t *item, size_t idx)
{
	int count = ui->sources->count();
	int idx_inv = count - (int)idx - 1;

	for (int i = 0; i < count; i++) {
		QListWidgetItem *listItem = ui->sources->item(i);
		QVariant v = listItem->data(Qt::UserRole);
		OBSSceneItem sceneItem = v.value<OBSSceneItem>();

		if (sceneItem == item) {
			if ((int)idx_inv != i) {
				bool sel = (ui->sources->currentRow() == i);

				listItem = ui->sources->takeItem(i);
				if (listItem)  {
					ui->sources->insertItem(idx_inv,
							listItem);
					SetupVisibilityItem(ui->sources,
							listItem, item);

					if (sel)
						ui->sources->setCurrentRow(
								idx_inv);
				}
			}

			break;
		}
	}
}

void OBSBasic::ReorderSources(OBSScene scene)
{
	ReorderInfo info(this);

	if (scene != GetCurrentScene())
		return;

	obs_scene_enum_items(scene,
			[] (obs_scene_t*, obs_sceneitem_t *item, void *p)
			{
				ReorderInfo *info =
					reinterpret_cast<ReorderInfo*>(p);

				info->window->ReorderSceneItem(item,
					info->idx++);
				return true;
			}, &info);
}

/* OBS Callbacks */

void OBSBasic::SceneReordered(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t *scene = (obs_scene_t*)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "ReorderSources",
			Q_ARG(OBSScene, OBSScene(scene)));
}

void OBSBasic::SceneItemAdded(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "AddSceneItem",
			Q_ARG(OBSSceneItem, OBSSceneItem(item)));
}

void OBSBasic::SceneItemRemoved(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "RemoveSceneItem",
			Q_ARG(OBSSceneItem, OBSSceneItem(item)));
}

void OBSBasic::SceneItemSelected(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t     *scene = (obs_scene_t*)calldata_ptr(params, "scene");
	obs_sceneitem_t *item  = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "SelectSceneItem",
			Q_ARG(OBSScene, scene), Q_ARG(OBSSceneItem, item),
			Q_ARG(bool, true));
}

void OBSBasic::SceneItemDeselected(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t     *scene = (obs_scene_t*)calldata_ptr(params, "scene");
	obs_sceneitem_t *item  = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "SelectSceneItem",
			Q_ARG(OBSScene, scene), Q_ARG(OBSSceneItem, item),
			Q_ARG(bool, false));
}

void OBSBasic::SourceAdded(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");

	if (obs_scene_from_source(source) != NULL)
		QMetaObject::invokeMethod(window,
				"AddScene",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceRemoved(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");

	if (obs_scene_from_source(source) != NULL)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"RemoveScene",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceActivated(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	uint32_t     flags  = obs_source_get_output_flags(source);

	if (flags & OBS_SOURCE_AUDIO)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"ActivateAudioSource",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceDeactivated(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	uint32_t     flags  = obs_source_get_output_flags(source);

	if (flags & OBS_SOURCE_AUDIO)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"DeactivateAudioSource",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceRenamed(void *data, calldata_t *params)
{
	const char *newName  = calldata_string(params, "new_name");
	const char *prevName = calldata_string(params, "prev_name");

	QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
			"RenameSources",
			Q_ARG(QString, QT_UTF8(newName)),
			Q_ARG(QString, QT_UTF8(prevName)));
}

void OBSBasic::ChannelChanged(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	uint32_t channel = (uint32_t)calldata_int(params, "channel");

	if (channel == 0)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"UpdateSceneSelection",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::DrawBackdrop(float cx, float cy)
{
	if (!box)
		return;

	gs_effect_t    *solid = obs_get_solid_effect();
	gs_eparam_t    *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech  = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_scale3f(float(cx), float(cy), 1.0f);

	gs_load_vertexbuffer(box);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_load_vertexbuffer(nullptr);
}

void OBSBasic::RenderMain(void *data, uint32_t cx, uint32_t cy)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);
	obs_video_info ovi;

	obs_get_video_info(&ovi);

	window->previewCX = int(window->previewScale * float(ovi.base_width));
	window->previewCY = int(window->previewScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height),
			-100.0f, 100.0f);
	gs_set_viewport(window->previewX, window->previewY,
			window->previewCX, window->previewCY);

	window->DrawBackdrop(float(ovi.base_width), float(ovi.base_height));

	obs_render_main_view();
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	QSize previewSize = GetPixelSize(window->ui->preview);
	float right  = float(previewSize.width())  - window->previewX;
	float bottom = float(previewSize.height()) - window->previewY;

	gs_ortho(-window->previewX, right,
	         -window->previewY, bottom,
	         -100.0f, 100.0f);
	gs_reset_viewport();

	window->ui->preview->DrawSceneEditing();

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
}

/* Main class functions */

obs_service_t *OBSBasic::GetService()
{
	if (!service) {
		service = obs_service_create("rtmp_common", NULL, NULL,
				nullptr);
		obs_service_release(service);
	}
	return service;
}

void OBSBasic::SetService(obs_service_t *newService)
{
	if (newService)
		service = newService;
}

bool OBSBasic::StreamingActive()
{
	if (!outputHandler)
		return false;
	return outputHandler->StreamingActive();
}

#ifdef _WIN32
#define IS_WIN32 1
#else
#define IS_WIN32 0
#endif

static inline int AttemptToResetVideo(struct obs_video_info *ovi)
{
	int ret = obs_reset_video(ovi);
	if (ret == OBS_VIDEO_INVALID_PARAM) {
		struct obs_video_info new_params = *ovi;

		if (new_params.window_width == 0)
			new_params.window_width = 512;
		if (new_params.window_height == 0)
			new_params.window_height = 512;

		new_params.output_width  = new_params.window_width;
		new_params.output_height = new_params.window_height;
		new_params.base_width    = new_params.window_width;
		new_params.base_height   = new_params.window_height;
		ret = obs_reset_video(&new_params);
	}

	return ret;
}

static inline enum obs_scale_type GetScaleType(ConfigFile &basicConfig)
{
	const char *scaleTypeStr = config_get_string(basicConfig,
			"Video", "ScaleType");

	if (astrcmpi(scaleTypeStr, "bilinear") == 0)
		return OBS_SCALE_BILINEAR;
	else if (astrcmpi(scaleTypeStr, "lanczos") == 0)
		return OBS_SCALE_LANCZOS;
	else
		return OBS_SCALE_BICUBIC;
}

static inline enum video_format GetVideoFormatFromName(const char *name)
{
	if (astrcmpi(name, "I420") == 0)
		return VIDEO_FORMAT_I420;
	else if (astrcmpi(name, "NV12") == 0)
		return VIDEO_FORMAT_NV12;
	else if (astrcmpi(name, "I444") == 0)
		return VIDEO_FORMAT_I444;
#if 0 //currently unsupported
	else if (astrcmpi(name, "YVYU") == 0)
		return VIDEO_FORMAT_YVYU;
	else if (astrcmpi(name, "YUY2") == 0)
		return VIDEO_FORMAT_YUY2;
	else if (astrcmpi(name, "UYVY") == 0)
		return VIDEO_FORMAT_UYVY;
#endif
	else
		return VIDEO_FORMAT_RGBA;
}

int OBSBasic::ResetVideo()
{
	struct obs_video_info ovi;
	int ret;

	GetConfigFPS(ovi.fps_num, ovi.fps_den);

	const char *colorFormat = config_get_string(basicConfig, "Video",
			"ColorFormat");
	const char *colorSpace = config_get_string(basicConfig, "Video",
			"ColorSpace");
	const char *colorRange = config_get_string(basicConfig, "Video",
			"ColorRange");

	ovi.graphics_module = App()->GetRenderModule();
	ovi.base_width     = (uint32_t)config_get_uint(basicConfig,
			"Video", "BaseCX");
	ovi.base_height    = (uint32_t)config_get_uint(basicConfig,
			"Video", "BaseCY");
	ovi.output_width   = (uint32_t)config_get_uint(basicConfig,
			"Video", "OutputCX");
	ovi.output_height  = (uint32_t)config_get_uint(basicConfig,
			"Video", "OutputCY");
	ovi.output_format  = GetVideoFormatFromName(colorFormat);
	ovi.colorspace     = astrcmpi(colorSpace, "601") == 0 ?
		VIDEO_CS_601 : VIDEO_CS_709;
	ovi.range          = astrcmpi(colorRange, "Full") == 0 ?
		VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	ovi.adapter        = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type     = GetScaleType(basicConfig);

	QTToGSWindow(ui->preview->winId(), ovi.window);

	//required to make opengl display stuff on osx(?)
	ResizePreview(ovi.base_width, ovi.base_height);

	QSize size = GetPixelSize(ui->preview);
	ovi.window_width  = size.width();
	ovi.window_height = size.height();

	ret = AttemptToResetVideo(&ovi);
	if (IS_WIN32 && ret != OBS_VIDEO_SUCCESS) {
		/* Try OpenGL if DirectX fails on windows */
		if (astrcmpi(ovi.graphics_module, DL_OPENGL) != 0) {
			blog(LOG_WARNING, "Failed to initialize obs video (%d) "
					  "with graphics_module='%s', retrying "
					  "with graphics_module='%s'",
					  ret, ovi.graphics_module,
					  DL_OPENGL);
			ovi.graphics_module = DL_OPENGL;
			ret = AttemptToResetVideo(&ovi);
		}
	}

	if (ret == OBS_VIDEO_SUCCESS)
		obs_add_draw_callback(OBSBasic::RenderMain, this);

	return ret;
}

bool OBSBasic::ResetAudio()
{
	struct obs_audio_info ai;
	ai.samples_per_sec = config_get_uint(basicConfig, "Audio",
			"SampleRate");

	const char *channelSetupStr = config_get_string(basicConfig,
			"Audio", "ChannelSetup");

	if (strcmp(channelSetupStr, "Mono") == 0)
		ai.speakers = SPEAKERS_MONO;
	else
		ai.speakers = SPEAKERS_STEREO;

	ai.buffer_ms = config_get_uint(basicConfig, "Audio", "BufferingTime");

	return obs_reset_audio(&ai);
}

void OBSBasic::ResetAudioDevice(const char *sourceId, const char *deviceName,
		const char *deviceDesc, int channel)
{
	const char *deviceId = config_get_string(basicConfig, "Audio",
			deviceName);
	obs_source_t *source;
	obs_data_t *settings;
	bool same = false;

	source = obs_get_output_source(channel);
	if (source) {
		settings = obs_source_get_settings(source);
		const char *curId = obs_data_get_string(settings, "device_id");

		same = (strcmp(curId, deviceId) == 0);

		obs_data_release(settings);
		obs_source_release(source);
	}

	if (!same)
		obs_set_output_source(channel, nullptr);

	if (!same && strcmp(deviceId, "disabled") != 0) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "device_id", deviceId);
		source = obs_source_create(OBS_SOURCE_TYPE_INPUT,
				sourceId, deviceDesc, settings, nullptr);
		obs_data_release(settings);

		obs_set_output_source(channel, source);
		obs_source_release(source);
	}
}

void OBSBasic::ResetAudioDevices()
{
	ResetAudioDevice(App()->OutputAudioSource(), "DesktopDevice1",
			Str("Basic.DesktopDevice1"), 1);
	ResetAudioDevice(App()->OutputAudioSource(), "DesktopDevice2",
			Str("Basic.DesktopDevice2"), 2);
	ResetAudioDevice(App()->InputAudioSource(),  "AuxDevice1",
			Str("Basic.AuxDevice1"), 3);
	ResetAudioDevice(App()->InputAudioSource(),  "AuxDevice2",
			Str("Basic.AuxDevice2"), 4);
	ResetAudioDevice(App()->InputAudioSource(),  "AuxDevice3",
			Str("Basic.AuxDevice3"), 5);
}

void OBSBasic::ResizePreview(uint32_t cx, uint32_t cy)
{
	QSize  targetSize;

	/* resize preview panel to fix to the top section of the window */
	targetSize = GetPixelSize(ui->preview);
	GetScaleAndCenterPos(int(cx), int(cy),
			targetSize.width()  - PREVIEW_EDGE_SIZE * 2,
			targetSize.height() - PREVIEW_EDGE_SIZE * 2,
			previewX, previewY, previewScale);

	previewX += float(PREVIEW_EDGE_SIZE);
	previewY += float(PREVIEW_EDGE_SIZE);

	if (isVisible()) {
		if (resizeTimer)
			killTimer(resizeTimer);
		resizeTimer = startTimer(100);
	}
}

void OBSBasic::closeEvent(QCloseEvent *event)
{
	if (outputHandler && outputHandler->Active()) {
		QMessageBox::StandardButton button = QMessageBox::question(
				this, QTStr("ConfirmExit.Title"),
				QTStr("ConfirmExit.Text"));

		if (button == QMessageBox::No) {
			event->ignore();
			return;
		}
	}

	QWidget::closeEvent(event);
	if (!event->isAccepted())
		return;

	if (updateCheckThread)
		updateCheckThread->wait();
	if (logUploadThread)
		logUploadThread->wait();

	/* Check all child dialogs and ensure they run their proper closeEvent
	 * methods before exiting the application.  Otherwise Qt doesn't send
	 * the proper QCloseEvent messages. */
	QList<QDialog*> childDialogs = this->findChildren<QDialog *>();
	if (!childDialogs.isEmpty()) {
		for (int i = 0; i < childDialogs.size(); ++i) {
			childDialogs.at(i)->close();
		}
	}

	for (QPointer<QWidget> &projector : projectors) {
		delete projector;
		projector.clear();
	}

	// remove draw callback in case our drawable surfaces go away before
	// the destructor gets called
	obs_remove_draw_callback(OBSBasic::RenderMain, this);

	/* Delete the save timer so it doesn't trigger after this point while
	 * the program data is being freed */
	delete saveTimer;
	SaveProject();

	/* Clear the list boxes in ::closeEvent to ensure that we can process
	 * any ->deleteLater events in this window created by Qt in relation to
	 * their internal data */
	ClearVolumeControls();
	ui->sources->clear();
	ui->scenes->clear();
}

void OBSBasic::changeEvent(QEvent *event)
{
	/* TODO */
	UNUSED_PARAMETER(event);
}

void OBSBasic::resizeEvent(QResizeEvent *event)
{
	struct obs_video_info ovi;

	if (obs_get_video_info(&ovi))
		ResizePreview(ovi.base_width, ovi.base_height);

	UNUSED_PARAMETER(event);
}

void OBSBasic::timerEvent(QTimerEvent *event)
{
	if (event->timerId() == resizeTimer) {
		killTimer(resizeTimer);
		resizeTimer = 0;

		QSize size = GetPixelSize(ui->preview);
		obs_resize(size.width(), size.height());
	}
}

void OBSBasic::on_action_New_triggered()
{
	/* TODO */
}

void OBSBasic::on_action_Open_triggered()
{
	/* TODO */
}

void OBSBasic::on_action_Save_triggered()
{
	/* TODO */
}

void OBSBasic::on_actionShow_Recordings_triggered()
{
	const char *path = config_get_string(basicConfig,
			"SimpleOutput", "FilePath");
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OBSBasic::on_actionRemux_triggered()
{
	const char *path = config_get_string(basicConfig,
			"SimpleOutput", "FilePath");
	OBSRemux remux(path, this);
	remux.exec();
}

void OBSBasic::on_action_Settings_triggered()
{
	OBSBasicSettings settings(this);
	settings.exec();
}

void OBSBasic::on_actionAdvAudioProperties_triggered()
{
	if (advAudioWindow != nullptr) {
		advAudioWindow->raise();
		return;
	}

	advAudioWindow = new OBSBasicAdvAudio(this);
	advAudioWindow->show();
	advAudioWindow->setAttribute(Qt::WA_DeleteOnClose, true);

	connect(advAudioWindow, SIGNAL(destroyed()),
		this, SLOT(on_advAudioProps_destroyed()));
}

void OBSBasic::on_advAudioProps_clicked()
{
	on_actionAdvAudioProperties_triggered();
}

void OBSBasic::on_advAudioProps_destroyed()
{
	advAudioWindow = nullptr;
}

void OBSBasic::on_scenes_currentItemChanged(QListWidgetItem *current,
		QListWidgetItem *prev)
{
	obs_source_t *source = NULL;

	if (sceneChanging)
		return;

	if (current) {
		obs_scene_t *scene;

		scene = current->data(Qt::UserRole).value<OBSScene>();
		source = obs_scene_get_source(scene);
	}

	/* TODO: allow transitions */
	obs_set_output_source(0, source);

	UNUSED_PARAMETER(prev);
}

void OBSBasic::EditSceneName()
{
	QListWidgetItem *item = ui->scenes->currentItem();
	Qt::ItemFlags flags   = item->flags();

	item->setFlags(flags | Qt::ItemIsEditable);
	ui->scenes->editItem(item);
	item->setFlags(flags);
}

static void AddProjectorMenuMonitors(QMenu *parent, QObject *target,
		const char *slot)
{
	QAction *action;
	std::vector<MonitorInfo> monitors;
	GetMonitors(monitors);

	for (int i = 0; (size_t)i < monitors.size(); i++) {
		const MonitorInfo &monitor = monitors[i];

		QString str = QString("%1 %2: %3x%4 @ %5,%6").
			arg(QTStr("Display"),
			    QString::number(i),
			    QString::number((int)monitor.cx),
			    QString::number((int)monitor.cy),
			    QString::number((int)monitor.x),
			    QString::number((int)monitor.y));

		action = parent->addAction(str, target, slot);
		action->setProperty("monitor", i);
	}
}

void OBSBasic::on_scenes_customContextMenuRequested(const QPoint &pos)
{
	QListWidgetItem *item = ui->scenes->itemAt(pos);
	QPointer<QMenu> sceneProjectorMenu;

	QMenu popup(this);
	popup.addAction(QTStr("Add"),
			this, SLOT(on_actionAddScene_triggered()));

	if (item) {
		popup.addSeparator();
		popup.addAction(QTStr("Rename"),
				this, SLOT(EditSceneName()));
		popup.addAction(QTStr("Remove"),
				this, SLOT(RemoveSelectedScene()),
				DeleteKeys.front());
		popup.addSeparator();
		sceneProjectorMenu = new QMenu(QTStr("SceneProjector"));
		AddProjectorMenuMonitors(sceneProjectorMenu, this,
				SLOT(OpenSceneProjector()));
		popup.addMenu(sceneProjectorMenu);
		popup.addSeparator();
		popup.addAction(QTStr("Filters"), this,
				SLOT(OpenSceneFilters()));
	}

	popup.exec(QCursor::pos());
}

void OBSBasic::on_actionAddScene_triggered()
{
	string name;
	QString format{QTStr("Basic.Main.DefaultSceneName.Text")};

	int i = 1;
	QString placeHolderText = format.arg(i);
	obs_source_t *source = nullptr;
	while ((source = obs_get_source_by_name(QT_TO_UTF8(placeHolderText)))) {
		obs_source_release(source);
		placeHolderText = format.arg(++i);
	}

	bool accepted = NameDialog::AskForName(this,
			QTStr("Basic.Main.AddSceneDlg.Title"),
			QTStr("Basic.Main.AddSceneDlg.Text"),
			name,
			placeHolderText);

	if (accepted) {
		if (name.empty()) {
			QMessageBox::information(this,
					QTStr("NoNameEntered.Title"),
					QTStr("NoNameEntered.Text"));
			on_actionAddScene_triggered();
			return;
		}

		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (source) {
			QMessageBox::information(this,
					QTStr("NameExists.Title"),
					QTStr("NameExists.Text"));

			obs_source_release(source);
			on_actionAddScene_triggered();
			return;
		}

		obs_scene_t *scene = obs_scene_create(name.c_str());
		source = obs_scene_get_source(scene);
		obs_add_source(source);
		obs_scene_release(scene);

		obs_set_output_source(0, source);
	}
}

void OBSBasic::on_actionRemoveScene_triggered()
{
	OBSScene     scene  = GetCurrentScene();
	obs_source_t *source = obs_scene_get_source(scene);

	if (source && QueryRemoveSource(source))
		obs_source_remove(source);
}

void OBSBasic::on_actionSceneProperties_triggered()
{
	/* TODO */
}

void OBSBasic::on_actionSceneUp_triggered()
{
	/* TODO */
}

void OBSBasic::on_actionSceneDown_triggered()
{
	/* TODO */
}

void OBSBasic::on_sources_currentItemChanged(QListWidgetItem *current,
		QListWidgetItem *prev)
{
	auto select_one = [] (obs_scene_t *scene, obs_sceneitem_t *item,
			void *param)
	{
		obs_sceneitem_t *selectedItem =
			*reinterpret_cast<OBSSceneItem*>(param);
		obs_sceneitem_select(item, (selectedItem == item));

		UNUSED_PARAMETER(scene);
		return true;
	};

	if (!current)
		return;

	OBSSceneItem item = current->data(Qt::UserRole).value<OBSSceneItem>();
	obs_source_t *source = obs_sceneitem_get_source(item);
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
		return;

	obs_scene_enum_items(GetCurrentScene(), select_one, &item);

	UNUSED_PARAMETER(prev);
}

void OBSBasic::EditSceneItemName()
{
	QListWidgetItem *item = ui->sources->currentItem();
	Qt::ItemFlags flags   = item->flags();
	OBSSceneItem sceneItem= item->data(Qt::UserRole).value<OBSSceneItem>();
	obs_source_t *source  = obs_sceneitem_get_source(sceneItem);
	const char *name      = obs_source_get_name(source);

	item->setText(QT_UTF8(name));
	item->setFlags(flags | Qt::ItemIsEditable);
	ui->sources->removeItemWidget(item);
	ui->sources->editItem(item);
	item->setFlags(flags);
}

void OBSBasic::CreateSourcePopupMenu(QListWidgetItem *item, bool preview)
{
	QMenu popup(this);
	QPointer<QMenu> previewProjector;
	QPointer<QMenu> sourceProjector;

	if (preview) {
		QAction *action = popup.addAction(
				QTStr("Basic.Main.PreviewConextMenu.Enable"),
				this, SLOT(TogglePreview()));
		action->setCheckable(true);
		action->setChecked(obs_preview_enabled());

		previewProjector = new QMenu(QTStr("PreviewProjector"));
		AddProjectorMenuMonitors(previewProjector, this,
				SLOT(OpenPreviewProjector()));

		popup.addMenu(previewProjector);

		popup.addSeparator();
	}

	QPointer<QMenu> addSourceMenu = CreateAddSourcePopupMenu();
	if (addSourceMenu)
		popup.addMenu(addSourceMenu);

	if (item) {
		if (addSourceMenu)
			popup.addSeparator();

		OBSSceneItem sceneItem = GetSceneItem(item);
		obs_source_t *source = obs_sceneitem_get_source(sceneItem);
		QAction *action;

		popup.addAction(QTStr("Rename"), this,
				SLOT(EditSceneItemName()));
		popup.addAction(QTStr("Remove"), this,
				SLOT(on_actionRemoveSource_triggered()),
				DeleteKeys.front());
		popup.addSeparator();
		popup.addMenu(ui->orderMenu);
		popup.addMenu(ui->transformMenu);

		sourceProjector = new QMenu(QTStr("SourceProjector"));
		AddProjectorMenuMonitors(sourceProjector, this,
				SLOT(OpenSourceProjector()));

		popup.addSeparator();
		popup.addMenu(sourceProjector);
		popup.addSeparator();

		action = popup.addAction(QTStr("Interact"), this,
				SLOT(on_actionInteract_triggered()));

		action->setEnabled(obs_source_get_output_flags(source) &
				OBS_SOURCE_INTERACTION);

		popup.addAction(QTStr("Filters"), this,
				SLOT(OpenFilters()));
		popup.addAction(QTStr("Properties"), this,
				SLOT(on_actionSourceProperties_triggered()));
	}

	popup.exec(QCursor::pos());
}

void OBSBasic::on_sources_customContextMenuRequested(const QPoint &pos)
{
	CreateSourcePopupMenu(ui->sources->itemAt(pos), false);
}

void OBSBasic::on_sources_itemDoubleClicked(QListWidgetItem *witem)
{
	if (!witem)
		return;

	OBSSceneItem item = GetSceneItem(witem);
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::AddSource(const char *id)
{
	if (id && *id) {
		OBSBasicSourceSelect sourceSelect(this, id);
		sourceSelect.exec();
	}
}

QMenu *OBSBasic::CreateAddSourcePopupMenu()
{
	const char *type;
	bool foundValues = false;
	size_t idx = 0;

	QMenu *popup = new QMenu(QTStr("Add"), this);
	while (obs_enum_input_types(idx++, &type)) {
		const char *name = obs_source_get_display_name(
				OBS_SOURCE_TYPE_INPUT, type);

		if (strcmp(type, "scene") == 0)
			continue;

		QAction *popupItem = new QAction(QT_UTF8(name), this);
		popupItem->setData(QT_UTF8(type));
		connect(popupItem, SIGNAL(triggered(bool)),
				this, SLOT(AddSourceFromAction()));
		popup->addAction(popupItem);

		foundValues = true;
	}

	if (!foundValues) {
		delete popup;
		popup = nullptr;
	}

	return popup;
}

void OBSBasic::AddSourceFromAction()
{
	QAction *action = qobject_cast<QAction*>(sender());
	if (!action)
		return;

	AddSource(QT_TO_UTF8(action->data().toString()));
}

void OBSBasic::AddSourcePopupMenu(const QPoint &pos)
{
	if (!GetCurrentScene()) {
		// Tell the user he needs a scene first (help beginners).
		QMessageBox::information(this,
				QTStr("Basic.Main.AddSourceHelp.Title"),
				QTStr("Basic.Main.AddSourceHelp.Text"));
		return;
	}

	QPointer<QMenu> popup = CreateAddSourcePopupMenu();
	if (popup)
		popup->exec(pos);
}

void OBSBasic::on_actionAddSource_triggered()
{
	AddSourcePopupMenu(QCursor::pos());
}

void OBSBasic::on_actionRemoveSource_triggered()
{
	OBSSceneItem item   = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(item);

	if (source && QueryRemoveSource(source))
		obs_sceneitem_remove(item);
}

void OBSBasic::on_actionInteract_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreateInteractionWindow(source);
}

void OBSBasic::on_actionSourceProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::on_actionSourceUp_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
}

void OBSBasic::on_actionSourceDown_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
}

void OBSBasic::on_actionMoveUp_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
}

void OBSBasic::on_actionMoveDown_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
}

void OBSBasic::on_actionMoveToTop_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
}

void OBSBasic::on_actionMoveToBottom_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
}

static BPtr<char> ReadLogFile(const char *log)
{
	char logDir[512];
	if (os_get_config_path(logDir, sizeof(logDir), "obs-studio/logs") <= 0)
		return nullptr;

	string path = (char*)logDir;
	path += "/";
	path += log;

	BPtr<char> file = os_quick_read_utf8_file(path.c_str());
	if (!file)
		blog(LOG_WARNING, "Failed to read log file %s", path.c_str());

	return file;
}

void OBSBasic::UploadLog(const char *file)
{
	BPtr<char> fileString{ReadLogFile(file)};

	if (!fileString)
		return;

	if (!*fileString)
		return;

	ui->menuLogFiles->setEnabled(false);

	auto data_deleter = [](obs_data_t *d) { obs_data_release(d); };
	using data_t = unique_ptr<struct obs_data, decltype(data_deleter)>;

	data_t content{obs_data_create(), data_deleter};
	data_t files{obs_data_create(), data_deleter};
	data_t request{obs_data_create(), data_deleter};

	obs_data_set_string(content.get(), "content", fileString);

	obs_data_set_obj(files.get(), file, content.get());

	stringstream ss;
	ss << "OBS " << App()->GetVersionString()
	   << " log file uploaded at " << CurrentDateTimeString();
	obs_data_set_string(request.get(), "description", ss.str().c_str());
	obs_data_set_bool(request.get(), "public", false);
	obs_data_set_obj(request.get(), "files", files.get());

	const char *json = obs_data_get_json(request.get());
	if (!json) {
		blog(LOG_ERROR, "Failed to get JSON data for log upload");
		return;
	}

	QBuffer *postData = new QBuffer();
	postData->setData(json, (int) strlen(json));

	if (logUploadThread) {
		logUploadThread->wait();
		delete logUploadThread;
	}

	RemoteTextThread *thread = new RemoteTextThread(
			"https://api.github.com/gists",
			"application/json", json);
	logUploadThread = thread;
	connect(thread, &RemoteTextThread::Result,
			this, &OBSBasic::logUploadFinished);
	logUploadThread->start();
}

void OBSBasic::on_actionShowLogs_triggered()
{
	char logDir[512];
	if (os_get_config_path(logDir, sizeof(logDir), "obs-studio/logs") <= 0)
		return;

	QUrl url = QUrl::fromLocalFile(QT_UTF8(logDir));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionUploadCurrentLog_triggered()
{
	UploadLog(App()->GetCurrentLog());
}

void OBSBasic::on_actionUploadLastLog_triggered()
{
	UploadLog(App()->GetLastLog());
}

void OBSBasic::on_actionViewCurrentLog_triggered()
{
	char logDir[512];
	if (os_get_config_path(logDir, sizeof(logDir), "obs-studio/logs") <= 0)
		return;

	const char* log = App()->GetCurrentLog();

	string path = (char*)logDir;
	path += "/";
	path += log;

	QUrl url = QUrl::fromLocalFile(QT_UTF8(path.c_str()));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionCheckForUpdates_triggered()
{
	CheckForUpdates();
}

void OBSBasic::logUploadFinished(const QString &text, const QString &error)
{
	ui->menuLogFiles->setEnabled(true);

	if (text.isEmpty()) {
		QMessageBox::information(this,
				QTStr("LogReturnDialog.ErrorUploadingLog"),
				error);
		return;
	}

	obs_data_t *returnData = obs_data_create_from_json(QT_TO_UTF8(text));
	QString logURL         = obs_data_get_string(returnData, "html_url");
	obs_data_release(returnData);

	OBSLogReply logDialog(this, logURL);
	logDialog.exec();
}

static void RenameListItem(OBSBasic *parent, QListWidget *listWidget,
		obs_source_t *source, const string &name)
{
	const char *prevName = obs_source_get_name(source);
	if (name == prevName)
		return;

	obs_source_t    *foundSource = obs_get_source_by_name(name.c_str());
	QListWidgetItem *listItem    = listWidget->currentItem();

	if (foundSource || name.empty()) {
		listItem->setText(QT_UTF8(prevName));

		if (foundSource) {
			QMessageBox::information(parent,
				QTStr("NameExists.Title"),
				QTStr("NameExists.Text"));
		} else if (name.empty()) {
			QMessageBox::information(parent,
				QTStr("NoNameEntered.Title"),
				QTStr("NoNameEntered.Text"));
		}

		obs_source_release(foundSource);
	} else {
		listItem->setText(QT_UTF8(name.c_str()));
		obs_source_set_name(source, name.c_str());
	}
}

void OBSBasic::SceneNameEdited(QWidget *editor,
		QAbstractItemDelegate::EndEditHint endHint)
{
	OBSScene  scene = GetCurrentScene();
	QLineEdit *edit = qobject_cast<QLineEdit*>(editor);
	string    text  = QT_TO_UTF8(edit->text().trimmed());

	if (!scene)
		return;

	obs_source_t *source = obs_scene_get_source(scene);
	RenameListItem(this, ui->scenes, source, text);

	UNUSED_PARAMETER(endHint);
}

void OBSBasic::SceneItemNameEdited(QWidget *editor,
		QAbstractItemDelegate::EndEditHint endHint)
{
	OBSSceneItem item  = GetCurrentSceneItem();
	QLineEdit    *edit = qobject_cast<QLineEdit*>(editor);
	string       text  = QT_TO_UTF8(edit->text().trimmed());

	if (!item)
		return;

	obs_source_t *source = obs_sceneitem_get_source(item);
	RenameListItem(this, ui->sources, source, text);

	QListWidgetItem *listItem = ui->sources->currentItem();
	listItem->setText(QString());
	SetupVisibilityItem(ui->sources, listItem, item);

	UNUSED_PARAMETER(endHint);
}

void OBSBasic::OpenFilters()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	CreateFiltersWindow(source);
}

void OBSBasic::OpenSceneFilters()
{
	OBSScene scene = GetCurrentScene();
	OBSSource source = obs_scene_get_source(scene);

	CreateFiltersWindow(source);
}

void OBSBasic::StartStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		return;

	if (outputHandler->StartStreaming(service)) {
		ui->streamButton->setEnabled(false);
		ui->streamButton->setText(QTStr("Basic.Main.Connecting"));
	}
}

void OBSBasic::StopStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		outputHandler->StopStreaming();
}

void OBSBasic::StreamingStart()
{
	ui->streamButton->setText(QTStr("Basic.Main.StopStreaming"));
	ui->streamButton->setEnabled(true);
	ui->statusbar->StreamStarted(outputHandler->streamOutput);
}

void OBSBasic::StreamingStop(int code)
{
	const char *errorMessage;

	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		errorMessage = Str("Output.ConnectFail.BadPath");
		break;

	case OBS_OUTPUT_CONNECT_FAILED:
		errorMessage = Str("Output.ConnectFail.ConnectFailed");
		break;

	case OBS_OUTPUT_INVALID_STREAM:
		errorMessage = Str("Output.ConnectFail.InvalidStream");
		break;

	default:
	case OBS_OUTPUT_ERROR:
		errorMessage = Str("Output.ConnectFail.Error");
		break;

	case OBS_OUTPUT_DISCONNECTED:
		/* doesn't happen if output is set to reconnect.  note that
		 * reconnects are handled in the output, not in the UI */
		errorMessage = Str("Output.ConnectFail.Disconnected");
	}

	ui->statusbar->StreamStopped();

	ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));
	ui->streamButton->setEnabled(true);

	if (code != OBS_OUTPUT_SUCCESS)
		QMessageBox::information(this,
				QTStr("Output.ConnectFail.Title"),
				QT_UTF8(errorMessage));
}

void OBSBasic::StartRecording()
{
	SaveProject();

	if (!outputHandler->RecordingActive())
		outputHandler->StartRecording();
}

void OBSBasic::StopRecording()
{
	SaveProject();

	if (outputHandler->RecordingActive())
		outputHandler->StopRecording();
}

void OBSBasic::RecordingStart()
{
	ui->statusbar->RecordingStarted(outputHandler->fileOutput);
	ui->recordButton->setText(QTStr("Basic.Main.StopRecording"));
}

void OBSBasic::RecordingStop()
{
	ui->statusbar->RecordingStopped();
	ui->recordButton->setText(QTStr("Basic.Main.StartRecording"));
}

void OBSBasic::on_streamButton_clicked()
{
	if (outputHandler->StreamingActive()) {
		StopStreaming();
	} else {
		StartStreaming();
	}
}

void OBSBasic::on_recordButton_clicked()
{
	if (outputHandler->RecordingActive())
		StopRecording();
	else
		StartRecording();
}

void OBSBasic::on_settingsButton_clicked()
{
	OBSBasicSettings settings(this);
	settings.exec();
}

void OBSBasic::on_preview_customContextMenuRequested(const QPoint &pos)
{
	CreateSourcePopupMenu(ui->sources->currentItem(), true);

	UNUSED_PARAMETER(pos);
}

void OBSBasic::on_previewDisabledLabel_customContextMenuRequested(
		const QPoint &pos)
{
	QMenu popup(this);
	QPointer<QMenu> previewProjector;

	QAction *action = popup.addAction(
			QTStr("Basic.Main.PreviewConextMenu.Enable"),
			this, SLOT(TogglePreview()));
	action->setCheckable(true);
	action->setChecked(obs_preview_enabled());

	previewProjector = new QMenu(QTStr("PreviewProjector"));
	AddProjectorMenuMonitors(previewProjector, this,
			SLOT(OpenPreviewProjector()));

	popup.addMenu(previewProjector);
	popup.exec(QCursor::pos());

	UNUSED_PARAMETER(pos);
}

void OBSBasic::GetFPSCommon(uint32_t &num, uint32_t &den) const
{
	const char *val = config_get_string(basicConfig, "Video", "FPSCommon");

	if (strcmp(val, "10") == 0) {
		num = 10;
		den = 1;
	} else if (strcmp(val, "20") == 0) {
		num = 20;
		den = 1;
	} else if (strcmp(val, "25") == 0) {
		num = 25;
		den = 1;
	} else if (strcmp(val, "29.97") == 0) {
		num = 30000;
		den = 1001;
	} else if (strcmp(val, "48") == 0) {
		num = 48;
		den = 1;
	} else if (strcmp(val, "59.94") == 0) {
		num = 60000;
		den = 1001;
	} else if (strcmp(val, "60") == 0) {
		num = 60;
		den = 1;
	} else {
		num = 30;
		den = 1;
	}
}

void OBSBasic::GetFPSInteger(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(basicConfig, "Video", "FPSInt");
	den = 1;
}

void OBSBasic::GetFPSFraction(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(basicConfig, "Video", "FPSNum");
	den = (uint32_t)config_get_uint(basicConfig, "Video", "FPSDen");
}

void OBSBasic::GetFPSNanoseconds(uint32_t &num, uint32_t &den) const
{
	num = 1000000000;
	den = (uint32_t)config_get_uint(basicConfig, "Video", "FPSNS");
}

void OBSBasic::GetConfigFPS(uint32_t &num, uint32_t &den) const
{
	uint32_t type = config_get_uint(basicConfig, "Video", "FPSType");

	if (type == 1) //"Integer"
		GetFPSInteger(num, den);
	else if (type == 2) //"Fraction"
		GetFPSFraction(num, den);
	else if (false) //"Nanoseconds", currently not implemented
		GetFPSNanoseconds(num, den);
	else
		GetFPSCommon(num, den);
}

config_t *OBSBasic::Config() const
{
	return basicConfig;
}

void OBSBasic::on_actionEditTransform_triggered()
{
	if (transformWindow)
		transformWindow->close();

	transformWindow = new OBSBasicTransform(this);
	transformWindow->show();
	transformWindow->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::on_actionResetTransform_triggered()
{
	auto func = [] (obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		if (!obs_sceneitem_selected(item))
			return true;

		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.rot = 0.0f;
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.bounds, 0.0f, 0.0f);
		obs_sceneitem_set_info(item, &info);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
}

static void GetItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&] (float x, float y)
	{
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);
}

static vec3 GetItemTL(obs_sceneitem_t *item)
{
	vec3 tl, br;
	GetItemBox(item, tl, br);
	return tl;
}

static void SetItemTL(obs_sceneitem_t *item, const vec3 &tl)
{
	vec3 newTL;
	vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	newTL = GetItemTL(item);
	pos.x += tl.x - newTL.x;
	pos.y += tl.y - newTL.y;
	obs_sceneitem_set_pos(item, &pos);
}

static bool RotateSelectedSources(obs_scene_t *scene, obs_sceneitem_t *item,
		void *param)
{
	if (!obs_sceneitem_selected(item))
		return true;

	float rot = *reinterpret_cast<float*>(param);

	vec3 tl = GetItemTL(item);

	rot += obs_sceneitem_get_rot(item);
	if (rot >= 360.0f)       rot -= 360.0f;
	else if (rot <= -360.0f) rot += 360.0f;
	obs_sceneitem_set_rot(item, rot);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	UNUSED_PARAMETER(param);
	return true;
};

void OBSBasic::on_actionRotate90CW_triggered()
{
	float f90CW = 90.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CW);
}

void OBSBasic::on_actionRotate90CCW_triggered()
{
	float f90CCW = -90.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CCW);
}

void OBSBasic::on_actionRotate180_triggered()
{
	float f180 = 180.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f180);
}

static bool MultiplySelectedItemScale(obs_scene_t *scene, obs_sceneitem_t *item,
		void *param)
{
	vec2 &mul = *reinterpret_cast<vec2*>(param);

	if (!obs_sceneitem_selected(item))
		return true;

	vec3 tl = GetItemTL(item);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	vec2_mul(&scale, &scale, &mul);
	obs_sceneitem_set_scale(item, &scale);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	return true;
}

void OBSBasic::on_actionFlipHorizontal_triggered()
{
	vec2 scale;
	vec2_set(&scale, -1.0f, 1.0f);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale,
			&scale);
}

void OBSBasic::on_actionFlipVertical_triggered()
{
	vec2 scale;
	vec2_set(&scale, 1.0f, -1.0f);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale,
			&scale);
}

static bool CenterAlignSelectedItems(obs_scene_t *scene, obs_sceneitem_t *item,
		void *param)
{
	obs_bounds_type boundsType = *reinterpret_cast<obs_bounds_type*>(param);

	if (!obs_sceneitem_selected(item))
		return true;

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	obs_transform_info itemInfo;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.rot = 0.0f;

	vec2_set(&itemInfo.bounds,
			float(ovi.base_width), float(ovi.base_height));
	itemInfo.bounds_type = boundsType;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;

	obs_sceneitem_set_info(item, &itemInfo);

	UNUSED_PARAMETER(scene);
	return true;
}

void OBSBasic::on_actionFitToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_SCALE_INNER;
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems,
			&boundsType);
}

void OBSBasic::on_actionStretchToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_STRETCH;
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems,
			&boundsType);
}

void OBSBasic::on_actionCenterToScreen_triggered()
{
	auto func = [] (obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		vec3 tl, br, itemCenter, screenCenter, offset;
		obs_video_info ovi;

		if (!obs_sceneitem_selected(item))
			return true;

		obs_get_video_info(&ovi);

		vec3_set(&screenCenter, float(ovi.base_width),
				float(ovi.base_height), 0.0f);
		vec3_mulf(&screenCenter, &screenCenter, 0.5f);

		GetItemBox(item, tl, br);

		vec3_sub(&itemCenter, &br, &tl);
		vec3_mulf(&itemCenter, &itemCenter, 0.5f);
		vec3_add(&itemCenter, &itemCenter, &tl);

		vec3_sub(&offset, &screenCenter, &itemCenter);
		vec3_add(&tl, &tl, &offset);

		SetItemTL(item, tl);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
}

void OBSBasic::TogglePreview()
{
	bool enabled = !obs_preview_enabled();
	obs_preview_set_enabled(enabled);
	ui->preview->setVisible(enabled);
	ui->previewDisabledLabel->setVisible(!enabled);
}

void OBSBasic::Nudge(int dist, MoveDir dir)
{
	struct MoveInfo {
		float dist;
		MoveDir dir;
	} info = {(float)dist, dir};

	auto func = [] (obs_scene_t*, obs_sceneitem_t *item, void *param)
	{
		MoveInfo *info = reinterpret_cast<MoveInfo*>(param);
		struct vec2 dir;
		struct vec2 pos;

		vec2_set(&dir, 0.0f, 0.0f);

		if (!obs_sceneitem_selected(item))
			return true;

		switch (info->dir) {
		case MoveDir::Up:    dir.y = -info->dist; break;
		case MoveDir::Down:  dir.y =  info->dist; break;
		case MoveDir::Left:  dir.x = -info->dist; break;
		case MoveDir::Right: dir.x =  info->dist; break;
		}

		obs_sceneitem_get_pos(item, &pos);
		vec2_add(&pos, &pos, &dir);
		obs_sceneitem_set_pos(item, &pos);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, &info);
}

void OBSBasic::NudgeUp()       {Nudge(1,  MoveDir::Up);}
void OBSBasic::NudgeDown()     {Nudge(1,  MoveDir::Down);}
void OBSBasic::NudgeLeft()     {Nudge(1,  MoveDir::Left);}
void OBSBasic::NudgeRight()    {Nudge(1,  MoveDir::Right);}

void OBSBasic::OpenProjector(obs_source_t *source, int monitor)
{
	/* seriously?  10 monitors? */
	if (monitor > 9)
		return;

	delete projectors[monitor];
	projectors[monitor].clear();

	OBSProjector *projector = new OBSProjector(this, source);
	projector->Init(monitor);

	projectors[monitor] = projector;
}

void OBSBasic::OpenPreviewProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(nullptr, monitor);
}

void OBSBasic::OpenSourceProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OBSSceneItem item = GetCurrentSceneItem();
	if (!item)
		return;

	OpenProjector(obs_sceneitem_get_source(item), monitor);
}

void OBSBasic::OpenSceneProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OBSScene scene = GetCurrentScene();
	if (!scene)
		return;

	OpenProjector(obs_scene_get_source(scene), monitor);
}
