#include "obs-websocket-api.h"
#include "pmg-record.hpp"
#include "version.h"
#include <media-io/media-remux.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QAction>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QTabBar>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

static bool rename_record_enabled = true;
static bool rename_replay_enabled = true;
static bool user_confirm = true;
static bool auto_remux = false;
static bool hide_non_record_controls = true;
static bool capture_mode = true;
static bool lock_docks = true;
static bool capture_layout_applied = false;
static bool need_set_vertical = false;
static std::map<obs_output_t *, std::vector<std::string>> output_files;
static std::string filename_format;

static std::string vendor_filename_format;
static bool vendor_force = false;

static std::string hook_source;
static std::string hook_title;
static std::string hook_class;
static std::string hook_executable;

obs_websocket_vendor vendor = nullptr;

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("pmg-record", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PMG Record\nBased on Record Rename by Exeldro";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PMGRecord");
}

std::string hook_format(std::string format)
{
	struct dstr f;
	dstr_init_copy(&f, format.c_str());
	dstr_replace(&f, "%TITLE", hook_title.c_str());
	dstr_replace(&f, "%EXECUTABLE", hook_executable.c_str());
	std::string hf = f.array;
	dstr_free(&f);
	return hf;
}

void *remux_thread(void *param)
{
	media_remux_job_t mr_job = (media_remux_job_t)param;
	media_remux_job_process(mr_job, nullptr, nullptr);
	media_remux_job_destroy(mr_job);
	return nullptr;
}

static void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

void ask_rename_file_UI(void *param)
{
	std::string path = (char *)param;
	bfree(param);
	const auto main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	size_t extension_pos = -1;
	size_t slash_pos = -1;
	for (size_t pos = path.length(); pos > 0; pos--) {
		auto c = path[pos - 1];
		if (c == '.' && extension_pos == (size_t)-1) {
			extension_pos = pos;
		} else if (c == '/' || c == '\\') {
			slash_pos = pos;
			break;
		}
	}
	std::string filename;
	std::string extension;
	std::string folder;
	if (extension_pos != (size_t)-1) {
		filename = path.substr(0, extension_pos - 1);
		extension = path.substr(extension_pos - 1);
	} else {
		filename = path;
	}
	if (slash_pos != (size_t)-1) {
		folder = filename.substr(0, slash_pos);
		filename = filename.substr(slash_pos);
	}
	std::string orig_filename = filename;
	bool force = false;
	if (!vendor_filename_format.empty()) {
		std::string hf = hook_format(vendor_filename_format);
		char *formatted = os_generate_formatted_filename(nullptr, true, hf.c_str());
		if (formatted) {
			filename = formatted;
			bfree(formatted);
		}
		force = vendor_force;
		vendor_filename_format.clear();
	} else if (!filename_format.empty()) {
		std::string hf = hook_format(filename_format);
		char *formatted = os_generate_formatted_filename(nullptr, true, hf.c_str());
		if (formatted) {
			filename = formatted;
			bfree(formatted);
		}
	}
	std::replace(filename.begin(), filename.end(), '<', '_');
	std::replace(filename.begin(), filename.end(), '>', '_');
	std::replace(filename.begin(), filename.end(), ':', '_');
	std::replace(filename.begin(), filename.end(), '"', '_');
	std::replace(filename.begin(), filename.end(), '|', '_');
	std::replace(filename.begin(), filename.end(), '?', '_');
	std::replace(filename.begin(), filename.end(), '*', '_');

	std::string new_path = folder + filename + extension;
	if ((!force || os_file_exists(new_path.c_str())) && user_confirm) {
		do {
			std::string title = obs_module_text("RenameFile");
			if (filename != orig_filename && os_file_exists(new_path.c_str())) {
				title += ": ";
				title += obs_module_text("FileExists");
			}
			if (!RenameFileDialog::AskForName(main_window, title, filename))
				filename = orig_filename;
			new_path = folder + filename + extension;
		} while (filename != orig_filename && os_file_exists(new_path.c_str()));
	}
	if (filename != orig_filename) {
		struct dstr dir_path;
		dstr_init_copy(&dir_path, new_path.c_str());
		ensure_directory(dir_path.array);
		dstr_free(&dir_path);
		os_rename(path.c_str(), new_path.c_str());
	}

	if (auto_remux && extension != ".mp4") {
		media_remux_job_t mr_job = nullptr;
		std::string target = folder + filename + ".mp4";
		if (media_remux_job_create(&mr_job, new_path.c_str(), target.c_str())) {
			pthread_t thread;
			pthread_create(&thread, nullptr, remux_thread, mr_job);
		}
	}
}

void *remux_multiple_thread(void *param)
{
	std::vector<std::string> *remux = (std::vector<std::string> *)param;
	for (const std::string &fp : *remux) {
		media_remux_job_t mr_job = nullptr;
		std::string target = fp.substr(0, fp.find_last_of('.')) + ".mp4";
		if (media_remux_job_create(&mr_job, fp.c_str(), target.c_str())) {
			media_remux_job_process(mr_job, nullptr, nullptr);
			media_remux_job_destroy(mr_job);
		}
	}
	delete remux;
	return nullptr;
}

void ask_rename_files_UI(void *param)
{
	obs_output_t *output = (obs_output_t *)param;
	auto t = output_files.find(output);
	if (t == output_files.end() || t->second.empty())
		return;
	const auto main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	std::string path = t->second.front();
	size_t extension_pos = -1;
	size_t slash_pos = -1;
	for (size_t pos = path.length(); pos > 0; pos--) {
		auto c = path[pos - 1];
		if (c == '.' && extension_pos == (size_t)-1) {
			extension_pos = pos;
		} else if (c == '/' || c == '\\') {
			slash_pos = pos;
			break;
		}
	}
	std::string filename;
	std::string extension;
	std::string folder;
	if (extension_pos != (size_t)-1) {
		filename = path.substr(0, extension_pos - 1);
		extension = path.substr(extension_pos - 1);
	} else {
		filename = path;
	}
	if (slash_pos != (size_t)-1) {
		folder = filename.substr(0, slash_pos);
		filename = filename.substr(slash_pos);
	}
	std::string orig_filename = filename;
	bool force = false;
	if (!vendor_filename_format.empty()) {
		std::string hf = hook_format(vendor_filename_format);
		char *formatted = os_generate_formatted_filename(nullptr, true, hf.c_str());
		if (formatted) {
			filename = formatted;
			bfree(formatted);
		}
		force = vendor_force;
		vendor_filename_format.clear();
	} else if (!filename_format.empty()) {
		std::string hf = hook_format(filename_format);
		char *formatted = os_generate_formatted_filename(nullptr, true, hf.c_str());
		if (formatted) {
			filename = formatted;
			bfree(formatted);
		}
	}
	std::string new_path = folder + filename + " (1)" + extension;
	if (!force || os_file_exists(new_path.c_str())) {
		do {
			std::string title = obs_module_text("RenameFiles");
			title += " (";
			title += std::to_string(t->second.size());
			title += " ";
			title += obs_module_text("Files");
			title += ")";
			if (filename != orig_filename && os_file_exists(new_path.c_str())) {
				title += ": ";
				title += obs_module_text("FileExists");
			}
			if (!RenameFileDialog::AskForName(main_window, title, filename))
				filename = orig_filename;

			new_path = folder + filename + " (1)" + extension;
		} while (filename != orig_filename && os_file_exists(new_path.c_str()));
	}

	std::vector<std::string> *remux = (auto_remux && extension != ".mp4") ? new std::vector<std::string>() : nullptr;

	if (filename != orig_filename) {
		size_t i = 1;
		for (const std::string &fp : t->second) {
			new_path = folder + filename + " (" + std::to_string(i) + ")" + extension;
			if (remux)
				remux->emplace_back(new_path);
			os_rename(fp.c_str(), new_path.c_str());
			i++;
		}
	}

	if (remux) {
		pthread_t thread;
		pthread_create(&thread, nullptr, remux_multiple_thread, remux);
	}
	output_files.erase(output);
}

void ask_rename_file(std::string path)
{
	if (os_get_path_extension(path.c_str()) == nullptr) {
		return;
	}
	bool autoRemux = config_get_bool(obs_frontend_get_profile_config(), "Video", "AutoRemux");
	if (autoRemux) {
		blog(LOG_INFO, "[PMG Record] AutoRemux is enabled, skipping rename.");
		return;
	}
	if (!os_file_exists(path.c_str())) {
		blog(LOG_ERROR, "[PMG Record] File not found: %s", path.c_str());
		return;
	}
	obs_queue_task(OBS_TASK_UI, ask_rename_file_UI, (void *)bstrdup(path.c_str()), false);
}

void replay_saved(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	if (!rename_replay_enabled)
		return;
	obs_output_t *output = (obs_output_t *)data;
	calldata_t cd = {0};
	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_call(ph, "get_last_replay", &cd);
	const char *path = calldata_string(&cd, "path");
	if (path)
		ask_rename_file(path);
	calldata_free(&cd);
}

void record_stop(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	if (!rename_record_enabled)
		return;
	obs_output_t *output = (obs_output_t *)data;
	auto t = output_files.find(output);
	if (t == output_files.end()) {
		obs_data_t *settings = obs_output_get_settings(output);
		const char *path = obs_data_get_string(settings, "path");
		if (path && strlen(path) && os_file_exists(path)) {
			ask_rename_file(path);
		} else {
			const char *url = obs_data_get_string(settings, "url");
			if (url && strlen(url) && os_file_exists(url)) {
				ask_rename_file(url);
			}
		}
		obs_data_release(settings);
	} else {
		obs_queue_task(OBS_TASK_UI, ask_rename_files_UI, (void *)output, false);
	}
}

void file_changed(void *data, calldata_t *calldata)
{
	if (!rename_record_enabled)
		return;
	obs_output_t *output = (obs_output_t *)data;
	auto t = output_files.find(output);
	if (t == output_files.end()) {

		obs_data_t *settings = obs_output_get_settings(output);
		const char *path = obs_data_get_string(settings, "path");
		if (path && strlen(path) && os_file_exists(path)) {
			output_files[output].push_back(path);
		} else {
			const char *url = obs_data_get_string(settings, "url");
			if (url && strlen(url) && os_file_exists(url)) {
				output_files[output].push_back(url);
			}
		}
		obs_data_release(settings);
	}
	const char *next_file = calldata_string(calldata, "next_file");
	output_files[output].push_back(next_file);
}
std::vector<obs_output_t *> connected_outputs;
bool loadOutput(void *data, obs_output_t *output)
{
	bool unload = *((bool *)data);

	auto it = std::find(connected_outputs.begin(), connected_outputs.end(), output);
	if (!unload && it != connected_outputs.end())
			return true;

	if (strcmp("replay_buffer", obs_output_get_id(output)) == 0) {
		auto sh = obs_output_get_signal_handler(output);
		if (unload) {
			signal_handler_disconnect(sh, "saved", replay_saved, output);
			if (it != connected_outputs.end())
				connected_outputs.erase(it);
		} else {

			signal_handler_connect(sh, "saved", replay_saved, output);
			connected_outputs.push_back(output);
		}
	} else {
		auto sh = obs_output_get_signal_handler(output);
		if (unload) {
			signal_handler_disconnect(sh, "stop", record_stop, output);
			signal_handler_disconnect(sh, "file_changed", file_changed, output);
			if (it != connected_outputs.end())
				connected_outputs.erase(it);
		} else {
			signal_handler_connect(sh, "stop", record_stop, output);
			signal_handler_connect(sh, "file_changed", file_changed, output);
			connected_outputs.push_back(output);
		}
	}
	return true;
}

void loadOutputs()
{
	bool unload = false;
	obs_enum_outputs(loadOutput, &unload);
}

void unloadOutputs()
{
	bool unload = true;
	obs_enum_outputs(loadOutput, &unload);
}

static void add_pmg_logo(QMainWindow *main_window)
{
	if (main_window->findChild<QLabel *>("pmgLogo"))
		return;

	QPushButton *recordButton = main_window->findChild<QPushButton *>("recordButton");
	if (!recordButton)
		return;

	QLayout *layout = recordButton->parentWidget() ? recordButton->parentWidget()->layout() : nullptr;
	if (!layout)
		return;

	char *logo_path = obs_module_file("images/pmg-logo.svg");
	if (!logo_path)
		return;

	QPixmap pixmap(logo_path);
	bfree(logo_path);

	if (pixmap.isNull())
		return;

	QPixmap scaled = pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	QLabel *label = new QLabel(recordButton->parentWidget());
	label->setObjectName("pmgLogo");
	label->setPixmap(scaled);
	label->setAlignment(Qt::AlignCenter);
	label->setContentsMargins(0, 12, 0, 12);

	QBoxLayout *box = qobject_cast<QBoxLayout *>(layout);
	if (box)
		box->insertWidget(0, label);
	else
		layout->addWidget(label);
}

static config_t *get_user_or_global_config()
{
	typedef config_t *(*get_user_config_fn)(void);
#if defined(_WIN32)
	auto fn = (get_user_config_fn)GetProcAddress(GetModuleHandle(NULL), "obs_frontend_get_user_config");
#else
	auto fn = (get_user_config_fn)dlsym(RTLD_DEFAULT, "obs_frontend_get_user_config");
#endif
	if (fn) {
		config_t *c = fn();
		if (c)
			return c;
	}
	return obs_frontend_get_global_config();
}

static void apply_record_button_style(bool recording)
{
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;
	QPushButton *recordButton = main_window->findChild<QPushButton *>("recordButton");
	if (!recordButton)
		return;
	if (recording)
		recordButton->setStyleSheet("background-color: #cc0000; color: white;");
	else
		recordButton->setStyleSheet("");
}

static void apply_dock_lock()
{
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	QList<QDockWidget *> docks = main_window->findChildren<QDockWidget *>();
	for (QDockWidget *dock : docks) {
		if (lock_docks)
			dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
		else
			dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
					  QDockWidget::DockWidgetFloatable);
	}
}

static void apply_preview_lock()
{
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	QAction *lockPreview = main_window->findChild<QAction *>("actionLockPreview");
	if (!lockPreview)
		return;

	if (capture_mode && !lockPreview->isChecked()) {
		lockPreview->setChecked(true);
		lockPreview->trigger();
	} else if (!capture_mode && lockPreview->isChecked()) {
		lockPreview->setChecked(false);
		lockPreview->trigger();
	}
}

static void apply_controls_visibility()
{
	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	add_pmg_logo(main_window);

	QPushButton *streamButton = main_window->findChild<QPushButton *>("streamButton");
	if (streamButton)
		streamButton->setVisible(!hide_non_record_controls);

	QPushButton *virtualCamButton = main_window->findChild<QPushButton *>("virtualCamButton");
	if (!virtualCamButton)
		virtualCamButton = main_window->findChild<QPushButton *>("vcamButton");
	if (virtualCamButton) {
		virtualCamButton->setVisible(!hide_non_record_controls);
		QWidget *container = virtualCamButton->parentWidget();
		if (container && container->layout()) {
			QLayout *parentLayout = container->layout();
			for (int i = 0; i < parentLayout->count(); i++) {
				QLayout *sub = parentLayout->itemAt(i)->layout();
				if (!sub)
					continue;
				bool hasVcam = false;
				for (int j = 0; j < sub->count(); j++) {
					if (sub->itemAt(j)->widget() == virtualCamButton) {
						hasVcam = true;
						break;
					}
				}
				if (hasVcam) {
					for (int j = 0; j < sub->count(); j++) {
						QWidget *w = sub->itemAt(j)->widget();
						if (w)
							w->setVisible(!hide_non_record_controls);
					}
					break;
				}
			}
		}
	}

	QPushButton *modeSwitch = main_window->findChild<QPushButton *>("modeSwitch");
	if (modeSwitch)
		modeSwitch->setVisible(!hide_non_record_controls);

	QDockWidget *scenesDock = main_window->findChild<QDockWidget *>("scenesDock");
	if (scenesDock)
		scenesDock->setVisible(!capture_mode);

	QDockWidget *transitionsDock = main_window->findChild<QDockWidget *>("transitionsDock");
	if (transitionsDock)
		transitionsDock->setVisible(!capture_mode);

	if (capture_mode && obs_frontend_preview_program_mode_active())
		obs_frontend_set_preview_program_mode(false);

	apply_dock_lock();
	apply_preview_lock();
}

static void apply_capture_layout()
{
	if (!capture_mode || capture_layout_applied)
		return;

	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	QDockWidget *controlsDock = main_window->findChild<QDockWidget *>("controlsDock");
	QDockWidget *sourcesDock = main_window->findChild<QDockWidget *>("sourcesDock");
	QDockWidget *mixerDock = main_window->findChild<QDockWidget *>("mixerDock");

	blog(LOG_INFO, "[PMG Record] Applying capture layout: controlsDock=%p sourcesDock=%p mixerDock=%p",
	     (void *)controlsDock, (void *)sourcesDock, (void *)mixerDock);

	if (!controlsDock || !sourcesDock)
		return;

	controlsDock->setVisible(true);
	controlsDock->setFloating(false);
	main_window->addDockWidget(Qt::RightDockWidgetArea, controlsDock);

	if (mixerDock) {
		mixerDock->setVisible(true);
		mixerDock->setFloating(false);
		main_window->splitDockWidget(controlsDock, mixerDock, Qt::Vertical);
	}

	sourcesDock->setVisible(true);
	sourcesDock->setFloating(false);
	main_window->tabifyDockWidget(controlsDock, sourcesDock);
	controlsDock->raise();

	QList<QTabBar *> tabBars = main_window->findChildren<QTabBar *>();
	for (QTabBar *tb : tabBars) {
		if (tb->count() >= 2) {
			tb->setDrawBase(false);
			tb->setAutoFillBackground(false);
			QWidget *parent = tb->parentWidget();
			if (parent)
				parent->setAutoFillBackground(false);
		}
	}

	capture_layout_applied = true;
	apply_dock_lock();
}

void frontend_event(obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		loadOutputs();
		apply_record_button_style(true);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		apply_record_button_style(false);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		loadOutputs();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		if (capture_mode)
			obs_frontend_set_preview_program_mode(false);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (need_set_vertical) {
			config_t *vc = get_user_or_global_config();
			if (vc)
				config_set_bool(vc, "BasicWindow", "VerticalVolControl", true);
			config_t *pc = obs_frontend_get_profile_config();
			if (pc)
				config_set_bool(pc, "PMGRecord", "VerticalVolControlSet", true);
			need_set_vertical = false;
			blog(LOG_INFO, "[PMG Record] Set VerticalVolControl=true on exit (takes effect on restart)");
		}
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		capture_layout_applied = false;
		config_t *config = obs_frontend_get_profile_config();
		if (config) {
			config_set_default_bool(config, "PMGRecord", "RenameRecord", true);
			config_set_default_bool(config, "PMGRecord", "RenameReplay", true);
			config_set_default_bool(config, "PMGRecord", "UserConfirm", true);
			config_set_default_bool(config, "PMGRecord", "HideNonRecordControls", true);
			config_set_default_bool(config, "PMGRecord", "CaptureMode", true);
			config_set_default_bool(config, "PMGRecord", "LockDocks", true);
			rename_record_enabled = config_get_bool(config, "PMGRecord", "RenameRecord");
			rename_replay_enabled = config_get_bool(config, "PMGRecord", "RenameReplay");
			user_confirm = config_get_bool(config, "PMGRecord", "UserConfirm");
			auto_remux = config_get_bool(config, "PMGRecord", "AutoRemux");
			hide_non_record_controls = config_get_bool(config, "PMGRecord", "HideNonRecordControls");
			capture_mode = config_get_bool(config, "PMGRecord", "CaptureMode");
			lock_docks = config_get_bool(config, "PMGRecord", "LockDocks");
			const char *ff = config_get_string(config, "PMGRecord", "FilenameFormat");
			if (ff)
				filename_format = ff;
		}
		if (config && !config_get_bool(config, "PMGRecord", "VerticalVolControlSet"))
			need_set_vertical = true;
		loadOutputs();
		apply_controls_visibility();
		QTimer::singleShot(1000, [] { apply_capture_layout(); });
		break;
	}
	default:
		break;
	}
}

void save_config()
{
	config_t *config = obs_frontend_get_profile_config();
	if (config) {
		config_set_bool(config, "PMGRecord", "RenameRecord", rename_record_enabled);
		config_set_bool(config, "PMGRecord", "RenameReplay", rename_replay_enabled);
		config_set_bool(config, "PMGRecord", "UserConfirm", user_confirm);
		config_set_string(config, "PMGRecord", "FilenameFormat", filename_format.c_str());
		config_set_bool(config, "PMGRecord", "AutoRemux", auto_remux);
		config_set_bool(config, "PMGRecord", "HideNonRecordControls", hide_non_record_controls);
		config_set_bool(config, "PMGRecord", "CaptureMode", capture_mode);
		config_set_bool(config, "PMGRecord", "LockDocks", lock_docks);
	}
	config_save(config);
	blog(LOG_INFO,
	     "[PMG Record] Config saved: record=%s replay=%s confirm=%s remux=%s hide_controls=%s capture_mode=%s lock_docks=%s",
	     rename_record_enabled ? "true" : "false", rename_replay_enabled ? "true" : "false",
	     user_confirm ? "true" : "false", auto_remux ? "true" : "false",
	     hide_non_record_controls ? "true" : "false", capture_mode ? "true" : "false",
	     lock_docks ? "true" : "false");
}

void hooked(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(data);
	obs_source_t *source = (obs_source_t *)calldata_ptr(calldata, "source");
	hook_source = obs_source_get_name(source);
	hook_title = calldata_string(calldata, "title");
	hook_class = calldata_string(calldata, "class");
	hook_executable = calldata_string(calldata, "executable");
}

void source_create(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(data);
	obs_source_t *source = (obs_source_t *)calldata_ptr(calldata, "source");
	const char *id = obs_source_get_unversioned_id(source);
	if (strcmp(id, "game_capture") == 0 || strcmp(id, "window_capture") == 0) {
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_connect(sh, "hooked", hooked, nullptr);
	}
}

static QTimer *timer;

bool obs_module_load()
{
	blog(LOG_INFO, "[PMG Record] loaded version %s", PROJECT_VERSION);

	obs_frontend_add_event_callback(frontend_event, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_create", source_create, nullptr);

	timer = new QTimer();
	timer->setInterval(10000);
	QObject::connect(timer, &QTimer::timeout, []() { loadOutputs(); });
	timer->start();

	QAction *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("PMGRecord")));
	QMenu *menu = new QMenu();
	auto recordAction = menu->addAction(QString::fromUtf8(obs_module_text("Record")), [] {
		rename_record_enabled = !rename_record_enabled;
		save_config();
	});
	recordAction->setCheckable(true);
	auto replayAction = menu->addAction(QString::fromUtf8(obs_module_text("ReplayBuffer")), [] {
		rename_replay_enabled = !rename_replay_enabled;
		save_config();
	});
	replayAction->setCheckable(true);
	menu->addSeparator();
	auto confirmAction = menu->addAction(QString::fromUtf8(obs_module_text("UserConfirm")), [] {
		user_confirm = !user_confirm;
		save_config();
	});
	confirmAction->setCheckable(true);
	menu->addAction(QString::fromUtf8(obs_module_text("FilenameFormat")), [] {
		const auto main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
		FilenameFormatDialog dialog(main_window);
		dialog.userText->setMaxLength(170);
		dialog.userText->setText(QString::fromUtf8(filename_format.c_str()));
		dialog.userText->selectAll();
		dialog.userText->setFocus();
		if (dialog.exec() == QDialog::DialogCode::Accepted) {
			filename_format = dialog.userText->text().toUtf8().constData();
			save_config();
		}
	});
	auto remuxAction = menu->addAction(QString::fromUtf8(obs_module_text("AutoRemux")), [] {
		auto_remux = !auto_remux;
		save_config();
	});
	remuxAction->setCheckable(true);
	menu->addSeparator();
	auto hideControlsAction =
		menu->addAction(QString::fromUtf8(obs_module_text("HideNonRecordControls")), [] {
			hide_non_record_controls = !hide_non_record_controls;
			save_config();
			apply_controls_visibility();
		});
	hideControlsAction->setCheckable(true);
	auto captureModeAction = menu->addAction(QString::fromUtf8(obs_module_text("CaptureMode")), [] {
		capture_mode = !capture_mode;
		capture_layout_applied = false;
		save_config();
		apply_controls_visibility();
		if (capture_mode)
			QTimer::singleShot(500, [] { apply_capture_layout(); });
	});
	captureModeAction->setCheckable(true);
	auto lockDocksAction = menu->addAction(QString::fromUtf8(obs_module_text("LockDocks")), [] {
		lock_docks = !lock_docks;
		save_config();
		apply_dock_lock();
	});
	lockDocksAction->setCheckable(true);

	menu->addSeparator();
	menu->addAction(QString::fromUtf8("PMG Record (v0.1.0)"));
	menu->addAction(QString::fromUtf8("based on Record Rename 0.1.3 by Exeldro"),
			[] { QDesktopServices::openUrl(QUrl("https://exeldro.com")); });
	action->setMenu(menu);
	QObject::connect(menu, &QMenu::aboutToShow,
			 [recordAction, replayAction, remuxAction, confirmAction, hideControlsAction,
			  captureModeAction, lockDocksAction] {
				 recordAction->setChecked(rename_record_enabled);
				 replayAction->setChecked(rename_replay_enabled);
				 confirmAction->setChecked(user_confirm);
				 remuxAction->setChecked(auto_remux);
				 hideControlsAction->setChecked(hide_non_record_controls);
				 captureModeAction->setChecked(capture_mode);
				 lockDocksAction->setChecked(lock_docks);
			 });
	return true;
}

void vendor_set_filename(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *filename = obs_data_get_string(request_data, "filename");
	if (!filename || !strlen(filename)) {
		obs_data_set_string(response_data, "error", "'filename' not set");
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	vendor_filename_format = filename;
	vendor_force = obs_data_get_bool(request_data, "force");
	obs_data_set_bool(response_data, "success", true);
}

void obs_module_post_load()
{
	vendor = obs_websocket_register_vendor("pmg-record");
	if (!vendor)
		return;
	obs_websocket_vendor_register_request(vendor, "set_filename", vendor_set_filename, nullptr);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	if (timer) {
		timer->stop();
		delete timer;
		timer = nullptr;
	}
	unloadOutputs();
}

RenameFileDialog::RenameFileDialog(QWidget *parent, std::string title) : QDialog(parent)
{
	setWindowTitle(QString::fromUtf8(title));
	setModal(true);
	setWindowModality(Qt::WindowModality::WindowModal);
	setMinimumWidth(300);
	setMinimumHeight(100);
	QVBoxLayout *layout = new QVBoxLayout;
	setLayout(layout);

	QLabel *titleLabel = new QLabel(QString::fromUtf8(title), this);
	layout->addWidget(titleLabel);

	userText = new QLineEdit(this);
	layout->addWidget(userText);

	QDialogButtonBox *buttonbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget(buttonbox);
	buttonbox->setCenterButtons(true);
	connect(buttonbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttonbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool RenameFileDialog::AskForName(QWidget *parent, std::string title, std::string &name)
{
	RenameFileDialog dialog(parent, title);
	dialog.userText->setMaxLength(170);
	dialog.userText->setText(QString::fromUtf8(name.c_str()));
	dialog.userText->selectAll();
	dialog.userText->setFocus();

	if (dialog.exec() != DialogCode::Accepted) {
		return false;
	}
	name = dialog.userText->text().toUtf8().constData();
	return true;
}

FilenameFormatDialog::FilenameFormatDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QString::fromUtf8(obs_module_text("FilenameFormat")));
	setModal(true);
	setWindowModality(Qt::WindowModality::WindowModal);
	setMinimumWidth(300);
	setMinimumHeight(100);
	QVBoxLayout *layout = new QVBoxLayout;
	setLayout(layout);

	userText = new QLineEdit(this);
	QStringList specList =
		QString::fromUtf8(obs_frontend_get_locale_string("FilenameFormatting.completer")).split(QRegularExpression("\n"));
	specList.append("%TITLE");
	specList.append("%EXECUTABLE");
	QCompleter *specCompleter = new QCompleter(specList);
	specCompleter->setCaseSensitivity(Qt::CaseSensitive);
	specCompleter->setFilterMode(Qt::MatchContains);
	userText->setCompleter(specCompleter);

	layout->addWidget(userText);

	QDialogButtonBox *buttonbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget(buttonbox);
	buttonbox->setCenterButtons(true);
	connect(buttonbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttonbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
