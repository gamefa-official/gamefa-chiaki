
// Use the native entry point instead of SDL's WinMain wrapper.
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#endif
#ifdef main
#undef main
#endif

// ugly workaround because Windows does weird things and ENOTIME
int real_main(int argc, char *argv[]);
int main(int argc, char *argv[]) { return real_main(argc, argv); }

#include <streamsession.h>
#include <settings.h>
#include <host.h>
#include <controllermanager.h>
#include <discoverymanager.h>
#include <qmlmainwindow.h>
#include <QApplication>
#include <QtTypes>

#ifdef CHIAKI_ENABLE_CLI
#include <chiaki-cli.h>
#endif

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>

#include <stdio.h>
#include <string.h>

#ifdef CHIAKI_HAVE_WEBENGINE
#include <QtWebEngineQuick>
#endif

#include <QCommandLineParser>
#include <QMap>
#include <QSurfaceFormat>

Q_DECLARE_METATYPE(ChiakiLogLevel)
Q_DECLARE_METATYPE(ChiakiRegistEventType)

#if defined(CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE) && defined(Q_OS_LINUX)
#include <QtPlugin>
Q_IMPORT_PLUGIN(SDInputContextPlugin)
#endif

#ifdef CHIAKI_ENABLE_CLI
struct CLICommand
{
	int (*cmd)(ChiakiLog *log, int argc, char *argv[]);
};

static const QMap<QString, CLICommand> cli_commands = {
	{ "discover", { chiaki_cli_cmd_discover } },
	{ "wakeup", { chiaki_cli_cmd_wakeup } }
};
#endif

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info);
static int GamefaRegist(const QString &host, const QString &account_b64, const QString &pin, bool ps5, const QString &out);
static int GamefaWakeup(const QString &host, const QString &regist_key_hex, bool ps5);
int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit);

int real_main(int argc, char *argv[])
{
	qRegisterMetaType<DiscoveryHost>();
	qRegisterMetaType<RegisteredHost>();
	qRegisterMetaType<HostMAC>();
	qRegisterMetaType<ChiakiQuitReason>();
	qRegisterMetaType<ChiakiRegistEventType>();
	qRegisterMetaType<ChiakiLogLevel>();

	QGuiApplication::setOrganizationName("Chiaki");
	QGuiApplication::setApplicationName("Chiaki");
	QGuiApplication::setApplicationVersion(CHIAKI_VERSION);
	QGuiApplication::setApplicationDisplayName("chiaki-ng");
#if defined(Q_OS_MACOS)
	// Use a plain CAMetalLayer for Vulkan windows. Qt's own layer presents in step with each
	// live resize, but our swapchain presents from the render thread, so every resize step
	// stalled and resizing the window became very slow.
	qputenv("QT_MTL_NO_TRANSACTION", "1");
	// The Vulkan loader also reads Homebrew's driver folder, so with Homebrew's molten-vk installed
	// the app loaded a second MoltenVK beside its own and listed every GPU twice. Use only the
	// bundled one. Setting any of these variables yourself keeps your choice.
	const QString moltenvk_icd = QFileInfo(QString::fromLocal8Bit(argv[0])).canonicalPath()
		+ "/../Resources/vulkan/icd.d/MoltenVK_icd.json";
	if(QFileInfo::exists(moltenvk_icd) && !qEnvironmentVariableIsSet("VK_DRIVER_FILES")
		&& !qEnvironmentVariableIsSet("VK_ICD_FILENAMES") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT")
		&& !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE"))
		qputenv("VK_DRIVER_FILES", QFileInfo(moltenvk_icd).canonicalFilePath().toUtf8());
#endif
#if defined(Q_OS_LINUX)
	if(qEnvironmentVariableIsSet("FLATPAK_ID"))
		QGuiApplication::setDesktopFileName(qEnvironmentVariable("FLATPAK_ID"));
	else
#endif
		QGuiApplication::setDesktopFileName("chiaki-ng");

	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
#if defined(Q_OS_WIN)
	const size_t cSize = strlen(argv[0])+1;
	wchar_t wc[cSize];
	mbstowcs (wc, argv[0], cSize);
	QString import_path = QFileInfo(QString::fromWCharArray(wc)).dir().absolutePath() + "/qml";
	qputenv("QML_IMPORT_PATH", import_path.toUtf8());
	// Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack adds the Dozen driver (Vulkan on
	// D3D12) beside the GPU's own, and loading it can crash the app even when it isn't used. Where
	// Dozen is the only Vulkan driver, Vulkan then finds no device and chiaki-ng uses OpenGL,
	// which the pack also provides. Setting either variable yourself keeps your choice.
	if(!qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT"))
		qputenv("VK_LOADER_DRIVERS_DISABLE", "*dzn*");
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
	qputenv("ANV_VIDEO_DECODE", "1");
	qputenv("RADV_PERFTEST", "video_decode");
#endif
#ifdef CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (qEnvironmentVariableIsSet("SteamDeck"))
		qputenv("QT_IM_MODULE", "sdinput");
#endif

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "Chiaki lib init failed: %s\n", chiaki_error_string(err));
		return 1;
	}

    SDL_SetHint(SDL_HINT_APP_NAME, "chiaki-ng");

	if(SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		fprintf(stderr, "SDL Audio init failed: %s\n", SDL_GetError());
		return 1;
	}

	QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef CHIAKI_HAVE_WEBENGINE
	QtWebEngineQuick::initialize();
#endif
	QApplication app(argc, argv);

#ifdef Q_OS_MACOS
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking_macos.svg"));
#else
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking.svg"));
#endif

	QCommandLineParser parser;
	parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
	parser.addHelpOption();
	
	QStringList cmds;
	cmds.append("stream");
	cmds.append("list");
#ifdef CHIAKI_ENABLE_CLI
	cmds.append(cli_commands.keys());
#endif

	parser.addPositionalArgument("command", cmds.join(", "));
	parser.addPositionalArgument("nickname", "Needed for stream command to get credentials for connecting. "
			"Use 'list' to get the nickname.");
	parser.addPositionalArgument("host", "Address to connect to (when using the stream command).");

	QCommandLineOption profile_option("profile", "", "profile", "Configuration profile");
	parser.addOption(profile_option);

	QCommandLineOption stream_exit_option("exit-app-on-stream-exit", "Exit the GUI application when the stream session ends.");
	parser.addOption(stream_exit_option);

	QCommandLineOption regist_key_option("registkey", "", "registkey");
	parser.addOption(regist_key_option);

	QCommandLineOption morning_option("morning", "", "morning");
	parser.addOption(morning_option);

	QCommandLineOption fullscreen_option("fullscreen", "Start window in fullscreen mode [maintains aspect ratio, adds black bars to fill unsused parts of screen if applicable] (only for use with stream command).");
	parser.addOption(fullscreen_option);

	QCommandLineOption dualsense_option("dualsense", "Enable DualSense haptics and adaptive triggers (PS5 and DualSense connected via USB only).");
	parser.addOption(dualsense_option);

	QCommandLineOption zoom_option("zoom", "Start window in fullscreen zoomed in to fit screen [maintains aspect ratio, cutting off edges of image to fill screen] (only for use with stream command)");
	parser.addOption(zoom_option);

	QCommandLineOption stretch_option("stretch", "Start window in fullscreen stretched to fit screen [distorts aspect ratio to fill screen] (only for use with stream command).");
	parser.addOption(stretch_option);

	QCommandLineOption passcode_option("passcode", "Automatically send your PlayStation login passcode (only affects users with a login passcode set on their PlayStation console).", "passcode");
	parser.addOption(passcode_option);

	QCommandLineOption opengl_option("opengl", "Switch the renderer to OpenGL from now on, for systems where Vulkan shows a blank window or crashes (switch back under Settings > Video > Renderer Backend).");
	parser.addOption(opengl_option);

	// Gamefa (2026-11-22): the Gamefa app's own «اتصال به PS5» drives these, as its phone app does:
	//   chiaki regist --host <ip> --accountid <base64> --pin <8 digits> [--ps5] --out <file.json>
	//   chiaki wakeup --host <ip> --registkeyhex <hex> [--ps5]
	//   chiaki stream --ps5 --registkeyhex <hex> --morning <base64> [--resolution 720|1080] [--fps 30|60] <nickname> <host>
	QCommandLineOption gf_ps5_option("ps5", "The console is a PS5 (stream with --registkey/--registkeyhex, regist, wakeup).");
	parser.addOption(gf_ps5_option);
	QCommandLineOption gf_regist_key_hex_option("registkeyhex", "The regist key, as hex (as regist prints it).", "hex");
	parser.addOption(gf_regist_key_hex_option);
	QCommandLineOption gf_host_option("host", "The console's address (regist, wakeup).", "host");
	parser.addOption(gf_host_option);
	QCommandLineOption gf_account_option("accountid", "The PSN Account-ID, base64 (regist).", "accountid");
	parser.addOption(gf_account_option);
	QCommandLineOption gf_pin_option("pin", "The 8-digit pairing PIN the console shows (regist).", "pin");
	parser.addOption(gf_pin_option);
	QCommandLineOption gf_out_option("out", "Where regist writes its result (JSON).", "file");
	parser.addOption(gf_out_option);
	// (Gamefa: the picture the PS5 sends - 720 or 1080, 30 or 60 frames a second - chosen in the Gamefa app)
	QCommandLineOption gf_resolution_option("resolution", "PS5 stream resolution: 720 or 1080 (stream).", "p");
	parser.addOption(gf_resolution_option);
	QCommandLineOption gf_fps_option("fps", "PS5 stream frames per second: 30 or 60 (stream).", "fps");
	parser.addOption(gf_fps_option);

	parser.process(app);
	QStringList args = parser.positionalArguments();

	Settings settings(parser.isSet(profile_option) ? parser.value(profile_option) : QString());
	bool exit_app_on_stream_exit = parser.isSet(stream_exit_option);
	if(parser.isSet(profile_option))
		settings.SetCurrentProfile(parser.value(profile_option));
	Settings alt_settings(parser.isSet(profile_option) ? "" : settings.GetCurrentProfile());
	if(!settings.GetCurrentProfile().isEmpty())
		QGuiApplication::setApplicationDisplayName(QString("chiaki-ng:%1").arg(settings.GetCurrentProfile()));
	bool use_alt_settings = false;
	if(!parser.isSet(profile_option))
		use_alt_settings = true;

	if(parser.isSet(opengl_option))
	{
		Settings *active_settings = use_alt_settings ? &alt_settings : &settings;
		active_settings->SetRenderBackend(RenderBackend::OpenGL);
		active_settings->SetVulkanStartupPending(false);
		active_settings->Sync();
	}

	if(args.length() == 0)
		return RunMain(app, use_alt_settings ? &alt_settings : &settings, exit_app_on_stream_exit);

	if(args[0] == "list")
	{
		for(const auto &host : settings.GetRegisteredHosts())
			printf("Host: %s \n", host.GetServerNickname().toLocal8Bit().constData());
		return 0;
	}
	if(args[0] == "regist")
		return GamefaRegist(parser.value(gf_host_option), parser.value(gf_account_option), parser.value(gf_pin_option), parser.isSet(gf_ps5_option), parser.value(gf_out_option));
	if(args[0] == "wakeup")
		return GamefaWakeup(parser.value(gf_host_option), parser.value(gf_regist_key_hex_option), parser.isSet(gf_ps5_option));

	if(args[0] == "stream")
	{
		if(args.length() < 2)
			parser.showHelp(1);

		//QString host = args[sizeof(args) -1]; //the ip is always the last param for stream
		QString host = args[args.size()-1];
		QByteArray morning;
		QByteArray regist_key;
		QString initial_login_passcode;
		ChiakiTarget target = CHIAKI_TARGET_PS4_10;

		if(parser.value(regist_key_option).isEmpty() && parser.value(gf_regist_key_hex_option).isEmpty() && parser.value(morning_option).isEmpty())
		{
			if(args.length() < 3)
				parser.showHelp(1);

			bool found = false;
			for(const auto &temphost : settings.GetRegisteredHosts())
			{
				if(temphost.GetServerNickname() == args[1])
				{
					found = true;
					morning = temphost.GetRPKey();
					regist_key = temphost.GetRPRegistKey();
					target = temphost.GetTarget();
					break;
				}
			}
			if(!found)
			{
				printf("No configuration found for '%s'\n", args[1].toLocal8Bit().constData());
				return 1;
			}
		}
		else
		{
			// TODO: explicit option for target
			// (Gamefa: --ps5 says it; the key may come as hex, as regist prints it)
			if(parser.isSet(gf_ps5_option))
				target = CHIAKI_TARGET_PS5_1;
			regist_key = parser.value(gf_regist_key_hex_option).isEmpty()
				? parser.value(regist_key_option).toUtf8()
				: QByteArray::fromHex(parser.value(gf_regist_key_hex_option).toUtf8());
			while(regist_key.endsWith('\0'))
				regist_key.chop(1);
			if(regist_key.length() > sizeof(ChiakiConnectInfo::regist_key))
			{
				printf("Given regist key is too long (expected size <=%llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::regist_key),
					regist_key.length());
				return 1;
			}
			regist_key += QByteArray(sizeof(ChiakiConnectInfo::regist_key) - regist_key.length(), 0);
			morning = QByteArray::fromBase64(parser.value(morning_option).toUtf8());
			if(morning.length() != sizeof(ChiakiConnectInfo::morning))
			{
				printf("Given morning has invalid size (expected %llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::morning),
					morning.length());
				printf("Given morning has invalid size (expected %llu)", (unsigned long long)sizeof(ChiakiConnectInfo::morning));
				return 1;
			}
		}
		if ((parser.isSet(stretch_option) && (parser.isSet(zoom_option) || parser.isSet(fullscreen_option))) || (parser.isSet(zoom_option) && parser.isSet(fullscreen_option)))
		{
			printf("Must choose between fullscreen, zoom or stretch option.");
			return 1;
		}
		if(parser.value(passcode_option).isEmpty())
		{
			//Set to empty if it wasn't given by user.
			initial_login_passcode = QString("");
		}
		else
		{
			initial_login_passcode = parser.value(passcode_option);
			if(initial_login_passcode.length() != 4)
			{
				printf("Login passcode must be 4 digits. You entered %" PRIdQSIZETYPE "digits)\n", initial_login_passcode.length());
				return 1;
			}
		}
		
		// Gamefa: the quality asked for, kept in this profile's settings (read by the session's video profile)
		{
			Settings *gs = use_alt_settings ? &alt_settings : &settings;
			if(parser.isSet(gf_resolution_option))
			{
				ChiakiVideoResolutionPreset r = parser.value(gf_resolution_option) == "720" ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
				gs->SetResolutionLocalPS5(r);
				gs->SetResolutionRemotePS5(r);
			}
			if(parser.isSet(gf_fps_option))
			{
				ChiakiVideoFPSPreset f = parser.value(gf_fps_option) == "30" ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
				gs->SetFPSLocalPS5(f);
				gs->SetFPSRemotePS5(f);
			}
		}
		StreamSessionConnectInfo connect_info(
				use_alt_settings ? &alt_settings : &settings,
				target,
				std::move(host),
				QString(),
				std::move(regist_key),
				std::move(morning),
				std::move(initial_login_passcode),
				QString(),
				false,
				parser.isSet(fullscreen_option),
				parser.isSet(zoom_option),
				parser.isSet(stretch_option));

		return RunStream(app, connect_info);
	}
#ifdef CHIAKI_ENABLE_CLI
	else if(cli_commands.contains(args[0]))
	{
		ChiakiLog log;
		// TODO: add verbose arg
		chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);

		const auto &cmd = cli_commands[args[0]];
		int sub_argc = args.count();
		QVector<QByteArray> sub_argv_b(sub_argc);
		QVector<char *> sub_argv(sub_argc);
		for(size_t i=0; i<sub_argc; i++)
		{
			sub_argv_b[i] = args[i].toLocal8Bit();
			sub_argv[i] = sub_argv_b[i].data();
		}
		return cmd.cmd(&log, sub_argc, sub_argv.data());
	}
#endif
	else
	{
		parser.showHelp(1);
	}
}

int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit)
{
	QmlMainWindow main_window(settings, exit_app_on_stream_exit);
	main_window.show();
	return app.exec();
}

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info)
{
	QmlMainWindow main_window(connect_info);
	main_window.show();
	return app.exec();
}

// ---- Gamefa (2026-11-22): pairing and wake-up from the command line

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <chiaki/discovery.h>

namespace {
struct GamefaRegistWait
{
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	bool ok = false;
	ChiakiRegisteredHost host = {};
};

void GamefaRegistCb(ChiakiRegistEvent *event, void *user)
{
	auto w = static_cast<GamefaRegistWait *>(user);
	std::lock_guard<std::mutex> l(w->m);
	w->ok = event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host;
	if(w->ok)
		w->host = *event->registered_host;
	w->done = true;
	w->cv.notify_all();
}

void GamefaWrite(const QString &out, const QJsonObject &o)
{
	QByteArray j = QJsonDocument(o).toJson(QJsonDocument::Compact);
	if(!out.isEmpty())
	{
		QFile f(out);
		if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
			f.write(j);
	}
	printf("%s\n", j.constData());
	fflush(stdout);
}
}

/** Pair with the console (its Pair Device PIN) by the PSN Account-ID: the keys to stream with, as JSON. */
static int GamefaRegist(const QString &host, const QString &account_b64, const QString &pin, bool ps5, const QString &out)
{
	QByteArray account = QByteArray::fromBase64(account_b64.toUtf8());
	if(host.isEmpty() || account.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE || pin.length() != 8)
	{
		GamefaWrite(out, {{"ok", false}, {"error", "args"}});
		return 2;
	}
	ChiakiRegistInfo info = {};
	QByteArray hostb = host.toUtf8();
	info.host = hostb.constData();
	info.target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.broadcast = false;
	info.pin = (uint32_t)pin.toULong();
	info.console_pin = 0;
	info.holepunch_info = nullptr;
	info.rudp = nullptr;
	info.psn_online_id = nullptr;
	memcpy(info.psn_account_id, account.constData(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);
	ChiakiRegist regist;
	GamefaRegistWait w;
	if(chiaki_regist_start(&regist, &log, &info, GamefaRegistCb, &w) != CHIAKI_ERR_SUCCESS)
	{
		GamefaWrite(out, {{"ok", false}, {"error", "start"}});
		return 1;
	}
	{
		std::unique_lock<std::mutex> l(w.m);
		w.cv.wait_for(l, std::chrono::seconds(60), [&] { return w.done; });
	}
	if(!w.done)
		chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	if(!w.ok)
	{
		GamefaWrite(out, {{"ok", false}, {"error", w.done ? "failed" : "timeout"}});
		return 1;
	}
	QJsonObject o;
	o["ok"] = true;
	o["target"] = (int)w.host.target;
	o["ps5"] = chiaki_target_is_ps5(w.host.target);
	o["nickname"] = QString::fromUtf8(w.host.server_nickname, (int)strnlen(w.host.server_nickname, sizeof(w.host.server_nickname)));
	o["mac"] = QString::fromLatin1(QByteArray((const char *)w.host.server_mac, sizeof(w.host.server_mac)).toHex(':'));
	o["regist_key_hex"] = QString::fromLatin1(QByteArray(w.host.rp_regist_key, sizeof(w.host.rp_regist_key)).toHex());
	o["regist_key"] = QString::fromLatin1(QByteArray(w.host.rp_regist_key, (int)strnlen(w.host.rp_regist_key, sizeof(w.host.rp_regist_key))));
	o["rp_key_type"] = (int)w.host.rp_key_type;
	o["morning"] = QString::fromLatin1(QByteArray((const char *)w.host.rp_key, sizeof(w.host.rp_key)).toBase64());
	GamefaWrite(out, o);
	return 0;
}

/** Wake a console in rest mode (the regist key, as regist printed it). */
static int GamefaWakeup(const QString &host, const QString &regist_key_hex, bool ps5)
{
	QByteArray key = QByteArray::fromHex(regist_key_hex.toUtf8());
	while(key.endsWith('\0'))
		key.chop(1);
	bool ok = false;
	uint64_t credential = (uint64_t)QString::fromUtf8(key).toULongLong(&ok, 16);
	if(host.isEmpty() || key.size() > 8 || !ok)
	{
		printf("{\"ok\":false,\"error\":\"args\"}\n");
		return 2;
	}
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);
	ChiakiErrorCode err = chiaki_discovery_wakeup(&log, nullptr, host.toUtf8().constData(), credential, ps5);
	printf("{\"ok\":%s}\n", err == CHIAKI_ERR_SUCCESS ? "true" : "false");
	return err == CHIAKI_ERR_SUCCESS ? 0 : 1;
}
